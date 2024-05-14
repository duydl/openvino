# Copyright (C) 2018-2024 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

import platform

import numpy as np
import pytest
import tensorflow as tf
from common.tf_layer_test_class import CommonTFLayerTest


class TestAdjustSaturation(CommonTFLayerTest):
    def _prepare_input(self, inputs_info):
        assert 'images:0' in inputs_info
        images_shape = inputs_info['images:0']
        inputs_data = {}
        inputs_data['images:0'] = np.random.rand(*images_shape).astype(self.input_type)
        inputs_data['contrast_factor:0'] = np.random.rand()
        return inputs_data

    def create_adjust_saturation_net(self, input_shape, input_type):
        self.input_type = input_type
        tf.compat.v1.reset_default_graph()
        # Create the graph and model
        with tf.compat.v1.Session() as sess:
            images = tf.compat.v1.placeholder(input_type, input_shape, 'images')
            contrast_factor = tf.compat.v1.placeholder(input_type, [], 'contrast_factor')
            tf.raw_ops.AdjustSaturation(images=images, contrast_factor=contrast_factor)
            tf.compat.v1.global_variables_initializer()
            tf_net = sess.graph_def

        return tf_net, None

    test_data_basic = [
        dict(input_shape=[10, 20, 3], input_type=np.float32),
        dict(input_shape=[5, 25, 15, 2], input_type=np.float32),
        dict(input_shape=[3, 4, 8, 10, 4], input_type=np.float32),
    ]

    @pytest.mark.parametrize("params", test_data_basic)
    @pytest.mark.precommit
    @pytest.mark.nightly
    @pytest.mark.xfail(condition=platform.system() == 'Darwin' and platform.machine() == 'arm64',
                       reason='Ticket - 122716')
    def test_adjust_saturation_basic(self, params, ie_device, precision, ir_version, temp_dir,
                                   use_legacy_frontend):
        self._test(*self.create_adjust_saturation_net(**params),
                   ie_device, precision, ir_version, temp_dir=temp_dir,
                   use_legacy_frontend=use_legacy_frontend)
