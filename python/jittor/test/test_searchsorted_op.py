# ***************************************************************
# Copyright (c) 2020 Jittor. Authors: 
#     Guowei Yang <471184555@qq.com>
#     Dun Liang <randonlang@gmail.com>. 
# All Rights Reserved.
# This file is subject to the terms and conditions defined in
# file 'LICENSE.txt', which is part of this source code package.
# ***************************************************************
import unittest
import jittor as jt
import numpy as np
import ctypes
import sys
import torch
from torch.autograd import Variable

class TestSearchsorted(unittest.TestCase):
    def test_searchsorted_cpu(self):
        jt.flags.use_cuda = 0

        for i in range(1,3):
            s = np.sort(np.random.rand(*((10,)*i)),-1)
            v = np.random.rand(*((10,)*i))
            s_jt = jt.array(s)
            v_jt = jt.array(v)
            s_tc = torch.from_numpy(s)
            v_tc = torch.from_numpy(v)

            y_jt = jt.searchsorted(s_jt, v_jt, right=True)
            y_tc = torch.searchsorted(s_tc, v_tc, right=True)
            assert np.allclose(y_jt.numpy(), y_tc.data)
            y_jt = jt.searchsorted(s_jt, v_jt, right=False)
            y_tc = torch.searchsorted(s_tc, v_tc, right=False)
            assert np.allclose(y_jt.numpy(), y_tc.data)

    def test_searchsorted_gpu(self):
        jt.flags.use_cuda = 1
        
        for i in range(1,3):
            s = np.sort(np.random.rand(*((10,)*i)),-1)
            v = np.random.rand(*((10,)*i))
            s_jt = jt.array(s)
            v_jt = jt.array(v)
            s_tc = torch.from_numpy(s)
            v_tc = torch.from_numpy(v)

            y_jt = jt.searchsorted(s_jt, v_jt, right=True)
            y_tc = torch.searchsorted(s_tc, v_tc, right=True)
            assert np.allclose(y_jt.numpy(), y_tc.data)
            y_jt = jt.searchsorted(s_jt, v_jt, right=False)
            y_tc = torch.searchsorted(s_tc, v_tc, right=False)
            assert np.allclose(y_jt.numpy(), y_tc.data)

if __name__ == "__main__":
    unittest.main()