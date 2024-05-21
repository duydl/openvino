// Copyright (C) 2018-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "common_op_table.hpp"
#include "openvino/op/add.hpp"
#include "openvino/op/constant.hpp"
#include "openvino/op/convert_like.hpp"
#include "openvino/op/multiply.hpp"
#include "openvino/op/reduce_mean.hpp"
#include "openvino/op/subtract.hpp"
#include "openvino/op/maximum.hpp"
#include "openvino/op/minimum.hpp"
#include "openvino/op/reduce_max.hpp"
#include "openvino/op/reduce_min.hpp"
#include "openvino/op/greater.hpp"
#include "openvino/op/select.hpp"
#include "openvino/op/divide.hpp"
#include "openvino/op/equal.hpp"
#include "openvino/op/split.hpp"
#include "openvino/op/concat.hpp"

#include "openvino/op/floor_mod.hpp"
#include "openvino/op/floor.hpp"

#include "openvino/op/abs.hpp"

#include <fstream>

using namespace std;
using namespace ov;
using namespace ov::op;

namespace ov {
namespace frontend {
namespace tensorflow {
namespace op {

    shared_ptr<Node> convert_rgb_to_hsv(shared_ptr<Node> images) {
        // Assume images are in shape [batch, height, width, 3] and dtype float

        // Reduce operations to find max and min across the channel axis
        auto max_rgb = make_shared<v1::ReduceMax>(images, make_shared<v0::Constant>(element::i64, Shape{1}, vector<int64_t>{-1}), true);
        auto vv = max_rgb;
        auto min_rgb = make_shared<v1::ReduceMin>(images, make_shared<v0::Constant>(element::i64, Shape{1}, vector<int64_t>{-1}), true);
        auto range = make_shared<v1::Subtract>(max_rgb, min_rgb);

        // Compute Saturation (S)
        auto s = make_shared<v1::Divide>(range, vv);

        // Compute normalization factor (for Hue calculation)
        auto norm = make_shared<v1::Divide>(
            make_shared<v0::Constant>(range->get_element_type(), range->get_shape(), vector<float>{1.0f}),
            make_shared<v1::Multiply>(
                make_shared<v0::Constant>(range->get_element_type(), range->get_shape(), vector<float>{6.0f}),
                range
            )
        );

        // Split the image tensor into R, G, B channels
        auto channels = make_shared<v1::Split>(images, make_shared<v0::Constant>(element::i64, Shape{}, 3), 3);
        auto r = channels->output(0);
        auto g = channels->output(1);
        auto b = channels->output(2);

        // Determine which component is the max (V) to compute Hue (H)
        auto r_eq_v = make_shared<v1::Equal>(r, vv);
        auto g_eq_v = make_shared<v1::Equal>(g, vv);

        auto hue_case_r = make_shared<v1::Multiply>(norm, make_shared<v1::Subtract>(g, b));
        auto hue_case_g = make_shared<v1::Add>(
            make_shared<v1::Multiply>(norm, make_shared<v1::Subtract>(b, r)),
            make_shared<v0::Constant>(norm->get_element_type(), norm->get_shape(), vector<float>{2.0f / 6.0f})
        );
        auto hue_case_b = make_shared<v1::Add>(
            make_shared<v1::Multiply>(norm, make_shared<v1::Subtract>(r, g)),
            make_shared<v0::Constant>(norm->get_element_type(), norm->get_shape(), vector<float>{4.0f / 6.0f})
        );

        // Select the correct hue based on the maximum component
        auto hue_temp = make_shared<v1::Select>(r_eq_v, hue_case_r, hue_case_g);
        auto hh = make_shared<v1::Select>(g_eq_v, hue_case_g, hue_case_b);

        // Return a single node that presumably could be used to extract individual components
        return make_shared<v0::Concat>(NodeVector{hh, vv, s}, 3); 
    }

    shared_ptr<Node> hsv_to_rgb(shared_ptr<Node> h, shared_ptr<Node> s, shared_ptr<Node> v) {
    auto c = make_shared<v1::Multiply>(s, v);
    auto m = make_shared<v1::Subtract>(v, c);
    auto dh = make_shared<v1::Multiply>(h, make_shared<v0::Constant>(h->get_element_type(), h->get_shape(), vector<float>{6.0f}));

    // Compute fmodu equivalent in a vectorized manner, ensuring it wraps within [0, 2)
    auto fmodu = make_shared<v1::FloorMod>(dh, make_shared<v0::Constant>(dh->get_element_type(), dh->get_shape(), vector<float>{2.0f}));

    auto x = make_shared<v1::Multiply>(c, make_shared<v1::Subtract>(make_shared<v0::Constant>(c->get_element_type(), c->get_shape(), vector<float>{1.0f}), make_shared<v0::Abs>(make_shared<v1::Subtract>(fmodu, make_shared<v0::Constant>(fmodu->get_element_type(), fmodu->get_shape(), vector<float>{1.0f})))));

    // Map fmodu to RGB categories
    auto rr = make_shared<v1::Select>(make_shared<v1::Equal>(make_shared<v0::Floor>(dh), make_shared<v0::Constant>(dh->get_element_type(), dh->get_shape(), vector<float>{0.0f})), c,
                 make_shared<v1::Select>(make_shared<v1::Equal>(make_shared<v0::Floor>(dh), make_shared<v0::Constant>(dh->get_element_type(), dh->get_shape(), vector<float>{1.0f})), x,
                    make_shared<v0::Constant>(dh->get_element_type(), dh->get_shape(), vector<float>{0.0f})));
    auto gg = make_shared<v1::Select>(make_shared<v1::Equal>(make_shared<v0::Floor>(dh), make_shared<v0::Constant>(dh->get_element_type(), dh->get_shape(), vector<float>{2.0f})), c,
                 make_shared<v1::Select>(make_shared<v1::Equal>(make_shared<v0::Floor>(dh), make_shared<v0::Constant>(dh->get_element_type(), dh->get_shape(), vector<float>{3.0f})), x,
                    make_shared<v0::Constant>(dh->get_element_type(), dh->get_shape(), vector<float>{0.0f})));
    auto bb = make_shared<v1::Select>(make_shared<v1::Equal>(make_shared<v0::Floor>(dh), make_shared<v0::Constant>(dh->get_element_type(), dh->get_shape(), vector<float>{4.0f})), c,
                 make_shared<v1::Select>(make_shared<v1::Equal>(make_shared<v0::Floor>(dh), make_shared<v0::Constant>(dh->get_element_type(), dh->get_shape(), vector<float>{5.0f})), x,
                    make_shared<v0::Constant>(dh->get_element_type(), dh->get_shape(), vector<float>{0.0f})));

    // Adding m to each component
    auto r = make_shared<v1::Add>(rr, m);
    auto g = make_shared<v1::Add>(gg, m);
    auto b = make_shared<v1::Add>(bb, m);

    // Return concatenated RGB or individual nodes depending on your system setup
    return make_shared<v0::Concat>(NodeVector{r, g, b}, -1);  // Assuming last axis concatenation
}


OutputVector translate_adjust_saturation_op(const NodeContext& node) {
    default_op_checks(node, 2, {"AdjustSaturation"});
    auto images = node.get_input(0);
    auto scale = node.get_input(1);
    auto node_name = node.get_name();
    // scale = make_shared<v1::ConvertLike>(scale, images);


    // START
    // reduce spatial dimensions of images in a format [batch, height, width, channel]
    auto max_rgb = make_shared<v1::ReduceMax>(images, make_shared<v0::Constant>(element::i64, Shape{1}, vector<int64_t>{-1}), true);
    auto vv = max_rgb;
    // std::cerr << "Shape of max_rgb: " << max_rgb->get_shape().to_string() << std::endl;
    // std::ofstream logFile("debug.log", std::ios_base::app);
    // logFile << "Shape of max_rgb: " << max_rgb->get_shape().to_string() << std::endl;
    // logFile.close();

    auto min_rgb = make_shared<v1::ReduceMin>(images, make_shared<v0::Constant>(element::i64, Shape{1}, vector<int64_t>{-1}), true);
    auto range = std::make_shared<v1::Subtract>(max_rgb, min_rgb);


    // auto greater_than_zero = make_shared<v1::Greater>(max_rgb, make_shared<v0::Constant>(max_rgb->get_element_type(), max_rgb->get_shape(), std::vector<float>{0}));

    // auto safe_vv = make_shared<v1::Select>(greater_than_zero, max_rgb, make_shared<v0::Constant>(max_rgb->get_element_type(), max_rgb->get_shape(), std::vector<float>{1}));

    // // Now compute saturation where safe_vv is used to avoid division by zero
    // auto saturation = make_shared<v1::Divide>(range, safe_vv);

    // // Use the original max_rgb to set saturation to 0 where vv is 0
    // auto corrected_saturation = make_shared<v1::Select>(greater_than_zero, saturation, make_shared<v0::Constant>(saturation->get_element_type(), saturation->get_shape(), std::vector<float>{0}));

    auto s = make_shared<v1::Divide>(range, max_rgb);
    auto norm = make_shared<v1::Divide>(
    make_shared<v0::Constant>(range->get_element_type(), range->get_shape(), std::vector<float>{1.0f}),
    make_shared<v1::Multiply>(
        make_shared<v0::Constant>(range->get_element_type(), range->get_shape(), std::vector<float>{6.0f}),
        range
        )
    );

    auto channel_axis_node = make_shared<v0::Constant>(element::i64, Shape{}, vector<int64_t>{-1});

    // Perform the split operation specifying the number of channels directly
    auto channels = make_shared<v1::Split>(images, channel_axis_node, 3); 


    // channels now contains three outputs: [R, G, B], we need to explicitly get each channel
    auto r = channels->output(0);
    auto g = channels->output(1);
    auto b = channels->output(2);

    auto r_eq_v = make_shared<v1::Equal>(r, vv);
    auto g_eq_v = make_shared<v1::Equal>(g, vv);

    // Calculate intermediate hue values based on the maximum component
    auto hue_case_r = make_shared<v1::Multiply>(norm, make_shared<v1::Subtract>(g, b));
    auto hue_case_g = make_shared<v1::Add>(
        make_shared<v1::Multiply>(norm, make_shared<v1::Subtract>(b, r)),
        make_shared<v0::Constant>(norm->get_element_type(), norm->get_shape(), std::vector<float>{2.0f / 6.0f})
    );
    auto hue_case_b = make_shared<v1::Add>(
        make_shared<v1::Multiply>(norm, make_shared<v1::Subtract>(r, g)),
        make_shared<v0::Constant>(norm->get_element_type(), norm->get_shape(), std::vector<float>{4.0f / 6.0f})
    );

    // Selecting based on conditions
    auto hue_temp = make_shared<v1::Select>(r_eq_v, hue_case_r, hue_case_g);
    auto hh = make_shared<v1::Select>(g_eq_v, hue_case_g, hue_case_b);

    // END

    scale = make_shared<v1::ConvertLike>(scale, images);

    auto adjust_saturation = make_shared<v1::Multiply>(s, scale);


    auto new_ = hsv_to_rgb(hh, adjust_saturation, vv);

    std::ofstream logFile("debug.log", std::ios_base::app);
    logFile << "Shape of images: " << images.get_shape().to_string() << std::endl;
    logFile << "Shape of news: " << new_->get_shape().to_string() << std::endl;
    logFile.close();

    auto adjust_saturation_ = new_->output(0);
    // auto adjust_saturation_ = make_shared<v1::Multiply>(images, scale)->output(0);

    set_node_name(node_name, adjust_saturation_.get_node_shared_ptr());
    return {adjust_saturation_};
}


}  // namespace op
}  // namespace tensorflow
}  // namespace frontend
}  // namespace ov

