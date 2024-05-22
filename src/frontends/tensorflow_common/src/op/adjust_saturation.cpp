// Copyright (C) 2018-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "common_op_table.hpp"
#include "openvino/op/add.hpp"
#include "openvino/op/constant.hpp"
#include "openvino/op/convert_like.hpp"
#include "openvino/op/convert.hpp"
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
#include "openvino/op/gather.hpp"

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

// shared_ptr<Node> convert_rgb_to_hsv(shared_ptr<Node> images) {
shared_ptr<tuple<shared_ptr<Node>, shared_ptr<Node>, shared_ptr<Node>>> convert_rgb_to_hsv(shared_ptr<Node> images, element::Type type) {
    // Assume images are in shape [batch, height, width, 3] and dtype float

    // Reduce operations to find max and min across the channel axis
    auto max_rgb = make_shared<v1::ReduceMax>(images, make_shared<v0::Constant>(element::i64, Shape{1}, vector<int64_t>{-1}), true);
    auto vv = max_rgb;
    auto min_rgb = make_shared<v1::ReduceMin>(images, make_shared<v0::Constant>(element::i64, Shape{1}, vector<int64_t>{-1}), true);
    auto range = make_shared<v1::Subtract>(max_rgb, min_rgb);

    // auto greater_than_zero = make_shared<v1::Greater>(max_rgb, make_shared<v0::Constant>(type, max_rgb->get_shape(), std::vector<float>{0}));

    // auto safe_vv = make_shared<v1::Select>(greater_than_zero, max_rgb, make_shared<v0::Constant>(type, max_rgb->get_shape(), std::vector<float>{1}));

    // // Now compute saturation where safe_vv is used to avoid division by zero
    // auto saturation = make_shared<v1::Divide>(range, safe_vv);

    // // Use the original max_rgb to set saturation to 0 where vv is 0
    // auto corrected_saturation = make_shared<v1::Select>(greater_than_zero, saturation, make_shared<v0::Constant>(type, saturation->get_shape(), std::vector<float>{0}));

    // Compute Saturation (S)
    auto ss = make_shared<v1::Divide>(range, vv);

    // Compute normalization factor (for Hue calculation)
    auto norm = make_shared<v1::Divide>(
        make_shared<v0::Constant>(type, range->get_shape(), vector<float>{1.0f}),
        make_shared<v1::Multiply>(
            make_shared<v0::Constant>(type, range->get_shape(), vector<float>{6.0f}),
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
        make_shared<v0::Constant>(type, norm->get_shape(), vector<float>{2.0f / 6.0f})
    );
    auto hue_case_b = make_shared<v1::Add>(
        make_shared<v1::Multiply>(norm, make_shared<v1::Subtract>(r, g)),
        make_shared<v0::Constant>(type, norm->get_shape(), vector<float>{4.0f / 6.0f})
    );

    // Select the correct hue based on the maximum component
    auto hue_temp = make_shared<v1::Select>(r_eq_v, hue_case_r, hue_case_g);
    auto hh = make_shared<v1::Select>(g_eq_v, hue_case_g, hue_case_b);

    // return make_shared<v0::Concat>(NodeVector{hh, vv, s}, -1); 
    return make_shared<tuple<shared_ptr<Node>, shared_ptr<Node>, shared_ptr<Node>>>(hh, ss, vv);
}

shared_ptr<Node> hsv_to_rgb(shared_ptr<Node> h, shared_ptr<Node> s, shared_ptr<Node> v, element::Type type) {
    // c = s * v;
    auto c = make_shared<v1::Multiply>(s, v);
    // m = v - c;
    auto m = make_shared<v1::Subtract>(v, c);
    // dh = h * 6;
    auto dh = make_shared<v1::Multiply>(h, make_shared<v0::Constant>(type, Shape{}, 6.0f));

    // fmodu rounded to within [0, 2)
    auto fmodu = make_shared<v1::FloorMod>(dh, make_shared<v0::Constant>(type, Shape{}, 2.0f));

    //  x = c * (1 - std::abs(fmodu - 1));
    auto x = make_shared<v1::Multiply>(c, make_shared<v1::Subtract>(make_shared<v0::Constant>(element::f32, Shape{}, 1.0f), make_shared<v0::Abs>(make_shared<v1::Subtract>(fmodu, make_shared<v0::Constant>(type, Shape{}, 1.0f)))));

    
    // h_category = static_cast<int>(dh);
    auto h_category = make_shared<v0::Convert>(make_shared<v0::Floor>(dh), element::i32);

    auto zeros = make_shared<v0::Constant>(type, x->get_shape(), 0.0f);

    auto rr_options = NodeVector{c, x, zeros, zeros, x, c};
    auto gg_options = NodeVector{x, c, c, x, zeros, zeros};
    auto bb_options = NodeVector{zeros, zeros, x, c, c, x};

    std::ofstream logFile("debug.log", std::ios_base::app);
    logFile << "Shape of x: " << x->get_shape().to_string() << std::endl;
    logFile << "Shape of c: " << c->get_shape().to_string() << std::endl;
    logFile << "Shape of zero: " << zeros->get_shape().to_string() << std::endl;
    logFile << "Shape of h: " << h_category->get_shape().to_string() << std::endl;

    auto rr_concat = make_shared<v0::Concat>(rr_options, -1); 
    auto gg_concat = make_shared<v0::Concat>(gg_options, -1);
    auto bb_concat = make_shared<v0::Concat>(bb_options, -1);

    logFile << "Shape of rr_concat: " << rr_concat->get_shape().to_string() << std::endl;
    
    // Use a gather operation to select the correct channel values based on h_category
    auto axis = make_shared<v0::Constant>(element::i32, Shape{}, -1);
    auto rr = make_shared<v8::Gather>(rr_concat, h_category, axis, 3);
    auto gg = make_shared<v8::Gather>(gg_concat, h_category, axis, 3);
    auto bb = make_shared<v8::Gather>(bb_concat, h_category, axis, 3);

    logFile << "Shape of rr: " << rr->get_shape().to_string() << std::endl;
    logFile.close();

    // Adding m to each component
    auto r = make_shared<v1::Add>(rr, m);
    auto g = make_shared<v1::Add>(gg, m);
    auto b = make_shared<v1::Add>(bb, m);

    // Return concatenated RGB 
    return make_shared<v0::Concat>(NodeVector{r, g, b}, -1); 
}


OutputVector translate_adjust_saturation_op(const NodeContext& node) {
    default_op_checks(node, 2, {"AdjustSaturation"});
    auto images = node.get_input(0);
    auto scale = node.get_input(1);
    auto node_name = node.get_name();

    auto type = images.get_element_type();

    auto hsv_components = convert_rgb_to_hsv(images.get_node_shared_ptr(), type);
    auto hh = get<0>(*hsv_components);
    auto ss = get<1>(*hsv_components);
    auto vv = get<2>(*hsv_components);

    scale = make_shared<v1::ConvertLike>(scale, images);

    auto ss_adjust = make_shared<v1::Multiply>(ss, scale);

    auto new_images = hsv_to_rgb(hh, ss_adjust, vv, type);

    // std::ofstream logFile("debug.log", std::ios_base::app);
    // logFile << "Shape of images: " << images.get_shape().to_string() << std::endl;
    // logFile << "Shape of news: " << new_images->get_shape().to_string() << std::endl;
    // logFile.close();

    auto adjust_saturation_ = new_images->output(0);
    // auto adjust_saturation_ = make_shared<v1::Multiply>(images, scale)->output(0);

    set_node_name(node_name, adjust_saturation_.get_node_shared_ptr());
    return {adjust_saturation_};
}


}  // namespace op
}  // namespace tensorflow
}  // namespace frontend
}  // namespace ov

