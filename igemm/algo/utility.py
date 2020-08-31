################################################################################
# 
#  MIT License
# 
#  Copyright (c) 2020 Advanced Micro Devices, Inc.
# 
#  Permission is hereby granted, free of charge, to any person obtaining a copy
#  of this software and associated documentation files (the "Software"), to deal
#  in the Software without restriction, including without limitation the rights
#  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#  copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
# 
#  The above copyright notice and this permission notice shall be included in all
#  copies or substantial portions of the Software.
# 
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
#  SOFTWARE.
# 
################################################################################

from ..codegen import *
import math

class macro_int_div_vv_t(mc_base_t):
    '''
    integer divide to compute `v_q = v_n / v_d`, v_q, v_n, v_d all vgpr
    '''
    def name(self):
        return '.v_u32_div'
    def __init__(self, mc):
        mc_base_t.__init__(self, mc)
    def __call__(self, v_q, v_n, v_d, v_tmp4, s_tmp4):
        return '{} {}, {}, {}, {}, {}'.format(self.name(), v_q, v_n, v_d, v_tmp4, s_tmp4)
    def emit(self):
        with self._emit_macro_indented(".macro {} v_q, v_n, v_d, v_tmp4, s_tmp4".format(self.name())):
            self._emit("v_cvt_f32_u32     v[\\v_tmp4+0],   v[\\v_d]")
            self._emit("v_rcp_f32         v[\\v_tmp4+0],   v[\\v_tmp4+0]")
            self._emit("v_mul_f32         v[\\v_tmp4+0],   0x4f800000, v[\\v_tmp4+0]")
            self._emit("v_cvt_u32_f32     v[\\v_tmp4+0],   v[\\v_tmp4+0]")
            self._emit("v_mul_lo_u32      v[\\v_tmp4+1],   v[\\v_d],      v[\\v_tmp4+0]")
            self._emit("v_mul_hi_u32      v[\\v_tmp4+2],   v[\\v_d],      v[\\v_tmp4+0]")
            self._emit("v_sub_co_u32      v[\\v_tmp4+3],   vcc, 0,     v[\\v_tmp4+1]")
            self._emit("v_cmp_ne_i32      s[\\s_tmp4:\\s_tmp4+1], 0,          v[\\v_tmp4+2]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+1],   v[\\v_tmp4+3],   v[\\v_tmp4+1],   s[\\s_tmp4:\\s_tmp4+1]")
            self._emit("v_mul_hi_u32      v[\\v_tmp4+1],   v[\\v_tmp4+1],   v[\\v_tmp4+0]")
            self._emit("v_sub_co_u32      v[\\v_tmp4+2],   vcc,        v[\\v_tmp4+0],   v[\\v_tmp4+1]")
            self._emit("v_add_co_u32      v[\\v_tmp4+0],   vcc,        v[\\v_tmp4+0],   v[\\v_tmp4+1]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+0],   v[\\v_tmp4+0],   v[\\v_tmp4+2],   s[\\s_tmp4:\\s_tmp4+1]")
            self._emit("v_mul_hi_u32      v[\\v_tmp4+0],   v[\\v_tmp4+0],   v[\\v_n]")
            self._emit("v_mul_lo_u32      v[\\v_tmp4+1],   v[\\v_tmp4+0],   v[\\v_d]")
            self._emit("v_sub_co_u32      v[\\v_tmp4+2],   vcc,        v[\\v_n],      v[\\v_tmp4+1]")
            self._emit("v_cmp_ge_u32      s[\\s_tmp4:\\s_tmp4+1], v[\\v_n],      v[\\v_tmp4+1]")
            self._emit("v_cmp_ge_u32      s[\\s_tmp4+2:\\s_tmp4+3], v[\\v_tmp4+2],   v[\\v_d]")
            self._emit("v_add_co_u32      v[\\v_tmp4+2],   vcc, 1, v[\\v_tmp4+0]")
            self._emit("s_and_b64         s[\\s_tmp4+2:\\s_tmp4+3], s[\\s_tmp4:\\s_tmp4+1], s[\\s_tmp4+2:\\s_tmp4+3]")
            self._emit("v_add_co_u32      v[\\v_tmp4+1],   vcc, -1,    v[\\v_tmp4+0]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+2],   v[\\v_tmp4+0],   v[\\v_tmp4+2],      s[\\s_tmp4+2:\\s_tmp4+3]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+2],   v[\\v_tmp4+1],   v[\\v_tmp4+2],      s[\\s_tmp4:\\s_tmp4+1]")
            self._emit("v_cmp_ne_i32      vcc,          0,          v[\\v_d]")
            self._emit("v_cndmask_b32     v[\\v_q],      -1,         v[\\v_tmp4+2],      vcc")

class macro_int_div_rem_vv_t(mc_base_t):
    '''
    integer divide to compute `v_q = v_n / v_d, v_r = v_n % v_d`, v_r, v_q, v_n, v_d all vgpr
    '''
    def name(self):
        return '.v_u32_div_rem'
    def __init__(self, mc):
        mc_base_t.__init__(self, mc)
    def __call__(self, v_r, v_q, v_n, v_d, v_tmp4, s_tmp4):
        return '{} {}, {}, {}, {}, {}, {}'.format(self.name(), v_r, v_q, v_n, v_d, v_tmp4, s_tmp4)
    def emit(self):
        int_div_vv = macro_int_div_vv_t(self.mc)
        with self._emit_macro_indented(".macro {} v_r, v_q, v_n, v_d, v_tmp4, s_tmp4".format(self.name())):
            self._emit(int_div_vv("\\v_q", "\\v_n", "\\v_d", "\\v_tmp4", "\\s_tmp4"))
            self._emit(f"v_mul_lo_u32 v[\\v_tmp4], v[\\v_d], v[\\v_q]")
            self._emit(f"v_sub_u32 v[\\v_r], v[\\v_n], v[\\v_tmp4]")

class macro_int_div_vs_t(mc_base_t):
    '''
    integer divide to compute `v_q = v_n / s_d`, v_q, v_n are vgpr, s_d is sgpr
    '''
    def name(self):
        return '.v_u32_div_vs'
    def __init__(self, mc):
        mc_base_t.__init__(self, mc)
    def __call__(self, v_q, v_n, s_d, v_tmp4, s_tmp4):
        return '{} {}, {}, {}, {}, {}'.format(self.name(), v_q, v_n, s_d, v_tmp4, s_tmp4)
    def emit(self):
        with self._emit_macro_indented(".macro {} v_q, v_n, s_d, v_tmp4, s_tmp4".format(self.name())):
            self._emit("v_cvt_f32_u32     v[\\v_tmp4+0],   s[\\s_d]")
            self._emit("v_rcp_f32         v[\\v_tmp4+0],   v[\\v_tmp4+0]")
            self._emit("v_mul_f32         v[\\v_tmp4+0],   0x4f800000, v[\\v_tmp4+0]")
            self._emit("v_cvt_u32_f32     v[\\v_tmp4+0],   v[\\v_tmp4+0]")
            self._emit("v_mul_lo_u32      v[\\v_tmp4+1],   s[\\s_d],      v[\\v_tmp4+0]")
            self._emit("v_mul_hi_u32      v[\\v_tmp4+2],   s[\\s_d],      v[\\v_tmp4+0]")
            self._emit("v_sub_co_u32      v[\\v_tmp4+3],   vcc, 0,     v[\\v_tmp4+1]")
            self._emit("v_cmp_ne_i32      s[\\s_tmp4:\\s_tmp4+1], 0,          v[\\v_tmp4+2]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+1],   v[\\v_tmp4+3],   v[\\v_tmp4+1],   s[\\s_tmp4:\\s_tmp4+1]")
            self._emit("v_mul_hi_u32      v[\\v_tmp4+1],   v[\\v_tmp4+1],   v[\\v_tmp4+0]")
            self._emit("v_sub_co_u32      v[\\v_tmp4+2],   vcc,        v[\\v_tmp4+0],   v[\\v_tmp4+1]")
            self._emit("v_add_co_u32      v[\\v_tmp4+0],   vcc,        v[\\v_tmp4+0],   v[\\v_tmp4+1]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+0],   v[\\v_tmp4+0],   v[\\v_tmp4+2],   s[\\s_tmp4:\\s_tmp4+1]")
            self._emit("v_mul_hi_u32      v[\\v_tmp4+0],   v[\\v_tmp4+0],   v[\\v_n]")
            self._emit("v_mul_lo_u32      v[\\v_tmp4+1],   s[\\s_d],     v[\\v_tmp4+0]")
            self._emit("v_sub_co_u32      v[\\v_tmp4+2],   vcc,        v[\\v_n],      v[\\v_tmp4+1]")
            self._emit("v_cmp_ge_u32      s[\\s_tmp4:\\s_tmp4+1], v[\\v_n],      v[\\v_tmp4+1]")
            self._emit("v_cmp_le_u32      s[\\s_tmp4+2:\\s_tmp4+3],  s[\\s_d],    v[\\v_tmp4+2]")
            self._emit("v_add_co_u32      v[\\v_tmp4+2],   vcc, 1, v[\\v_tmp4+0]")
            self._emit("s_and_b64         s[\\s_tmp4+2:\\s_tmp4+3], s[\\s_tmp4:\\s_tmp4+1], s[\\s_tmp4+2:\\s_tmp4+3]")
            self._emit("v_add_co_u32      v[\\v_tmp4+1],   vcc, -1,    v[\\v_tmp4+0]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+2],   v[\\v_tmp4+0],   v[\\v_tmp4+2],      s[\\s_tmp4+2:\\s_tmp4+3]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+2],   v[\\v_tmp4+1],   v[\\v_tmp4+2],      s[\\s_tmp4:\\s_tmp4+1]")
            self._emit("v_cmp_ne_i32      vcc,          s[\\s_d],   0")
            self._emit("v_cndmask_b32     v[\\v_q],      -1,         v[\\v_tmp4+2],      vcc")

class macro_int_div_rem_vs_t(mc_base_t):
    '''
    integer divide to compute `v_q = v_n / s_d, v_r = v_n % s_d`, v_r, v_q, v_n are vgpr, s_d is sgpr
    '''
    def name(self):
        return '.v_u32_div_rem_vs'
    def __init__(self, mc):
        mc_base_t.__init__(self, mc)
    def __call__(self, v_r, v_q, v_n, s_d, v_tmp4, s_tmp4):
        return '{} {}, {}, {}, {}, {}, {}'.format(self.name(), v_r, v_q, v_n, s_d, v_tmp4, s_tmp4)
    def emit(self):
        int_div_vs = macro_int_div_vs_t(self.mc)
        with self._emit_macro_indented(".macro {} v_r, v_q, v_n, s_d, v_tmp4, s_tmp4".format(self.name())):
            self._emit(int_div_vs("\\v_q", "\\v_n", "\\s_d", "\\v_tmp4", "\\s_tmp4"))
            self._emit(f"v_mul_lo_u32 v[\\v_tmp4], s[\\s_d], v[\\v_q]")
            self._emit(f"v_sub_u32 v[\\v_r], v[\\v_n], v[\\v_tmp4]")

class macro_int_div_ss_t(mc_base_t):
    '''
    integer divide to compute `s_q = s_n / s_d`, s_q, s_n, s_d all sgpr
    '''
    def name(self):
        return '.v_u32_div_ss'
    def __init__(self, mc):
        mc_base_t.__init__(self, mc)
    def __call__(self, v_q, s_n, s_d, v_tmp4, s_tmp4):
        return '{} {}, {}, {}, {}, {}'.format(self.name(), v_q, s_n, s_d, v_tmp4, s_tmp4)
    def emit(self):
        with self._emit_macro_indented(".macro .v_u32_div_ss v_q, s_n, s_d, v_tmp4, s_tmp4"):
            self._emit("v_cvt_f32_u32     v[\\v_tmp4+0],   s[\\s_d]")
            self._emit("v_rcp_f32         v[\\v_tmp4+0],   v[\\v_tmp4+0]")
            self._emit("v_mul_f32         v[\\v_tmp4+0],   0x4f800000, v[\\v_tmp4+0]")
            self._emit("v_cvt_u32_f32     v[\\v_tmp4+0],   v[\\v_tmp4+0]")
            self._emit("v_mul_lo_u32      v[\\v_tmp4+1],   s[\\s_d],      v[\\v_tmp4+0]")
            self._emit("v_mul_hi_u32      v[\\v_tmp4+2],   s[\\s_d],      v[\\v_tmp4+0]")
            self._emit("v_sub_co_u32      v[\\v_tmp4+3],   vcc, 0,     v[\\v_tmp4+1]")
            self._emit("v_cmp_ne_i32      s[\\s_tmp4:\\s_tmp4+1], 0,          v[\\v_tmp4+2]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+1],   v[\\v_tmp4+3],   v[\\v_tmp4+1],   s[\\s_tmp4:\\s_tmp4+1]")
            self._emit("v_mul_hi_u32      v[\\v_tmp4+1],   v[\\v_tmp4+1],   v[\\v_tmp4+0]")
            self._emit("v_sub_co_u32      v[\\v_tmp4+2],   vcc,        v[\\v_tmp4+0],   v[\\v_tmp4+1]")
            self._emit("v_add_co_u32      v[\\v_tmp4+0],   vcc,        v[\\v_tmp4+0],   v[\\v_tmp4+1]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+0],   v[\\v_tmp4+0],   v[\\v_tmp4+2],   s[\\s_tmp4:\\s_tmp4+1]")
            self._emit("v_mul_hi_u32      v[\\v_tmp4+0],   s[\\s_n],   v[\\v_tmp4+0]")
            self._emit("v_mul_lo_u32      v[\\v_tmp4+1],   s[\\s_d],     v[\\v_tmp4+0]")
            self._emit("v_sub_co_u32      v[\\v_tmp4+2],   vcc,        s[\\s_n],      v[\\v_tmp4+1]")
            self._emit("v_cmp_ge_u32      s[\\s_tmp4:\\s_tmp4+1], s[\\s_n],      v[\\v_tmp4+1]")
            self._emit("v_cmp_le_u32      s[\\s_tmp4+2:\\s_tmp4+3],  s[\\s_d],    v[\\v_tmp4+2]")
            self._emit("v_add_co_u32      v[\\v_tmp4+2],   vcc, 1, v[\\v_tmp4+0]")
            self._emit("s_and_b64         s[\\s_tmp4+2:\\s_tmp4+3], s[\\s_tmp4:\\s_tmp4+1], s[\\s_tmp4+2:\\s_tmp4+3]")
            self._emit("v_add_co_u32      v[\\v_tmp4+1],   vcc, -1,    v[\\v_tmp4+0]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+2],   v[\\v_tmp4+0],   v[\\v_tmp4+2],      s[\\s_tmp4+2:\\s_tmp4+3]")
            self._emit("v_cndmask_b32     v[\\v_tmp4+2],   v[\\v_tmp4+1],   v[\\v_tmp4+2],      s[\\s_tmp4:\\s_tmp4+1]")
            self._emit("v_cmp_ne_i32      vcc,          s[\\s_d],   0")
            self._emit("v_cndmask_b32     v[\\v_q],      -1,         v[\\v_tmp4+2],      vcc")

class macro_int_div_rem_ss_t(mc_base_t):
    '''
    integer divide to compute `s_q = s_n / s_d, s_r = s_n % s_d`, s_r, s_q, s_n, s_d all sgpr
    '''
    def name(self):
        return '.v_u32_div_rem_ss'
    def __init__(self, mc):
        mc_base_t.__init__(self, mc)

    def __call__(self, s_r, s_q, s_n, s_d, v_q, v_tmp4, s_tmp4):
        return '{} {}, {}, {}, {}, {}, {}, {}'.format(self.name(), s_r, s_q, s_n, s_d, v_q, v_tmp4, s_tmp4) 

    def emit(self):
        int_div_ss = macro_int_div_ss_t(self.mc)
        with self._emit_macro_indented(".macro {} s_r, s_q, s_n, s_d, v_q, v_tmp4, s_tmp4".format(self.name())):
            self._emit(int_div_ss("\\v_q", "\\s_n", "\\s_d", "\\v_tmp4", "\\s_tmp4"))
            self._emit(f"v_readfirstlane_b32 s[\\s_q], v[\\v_q]")
            self._emit(f"s_mul_i32 s[\\s_tmp4], s[\\s_d], s[\\s_q]")
            self._emit(f"s_sub_i32 s[\\s_r], s[\\s_n], s[\\s_tmp4]")

class macro_c_clear_t(mc_base_t):
    def name(self):
        return '.v_clear_nc'
    def __init__(self, mc):
        mc_base_t.__init__(self, mc)
    def __call__(self, vid, num):
        return '{} {}, {}'.format(self.name(), vid, num)
    def emit(self):
        with self._emit_macro_indented(".macro {} vid, num".format(self.name())):
            self._emit("_v = \\vid")
            self._emit(".rept \\num")
            with self._indent_context():
                self._emit("v_mov_b32 v[_v], 0")
                self._emit("_v = _v + 1")
            self._emit(".endr")

class macro_acc_c_clear_t(mc_base_t):
    '''
    gfx908 RAW harzard attention!
    '''
    def name(self):
        return '.v_clear_acc_c'
    def __init__(self, mc):
        mc_base_t.__init__(self, mc)
    def __call__(self, a, num):
        return '{} {}, {}'.format(self.name(), a, num)
    def emit(self):
        with self._emit_macro_indented(".macro {} a, num".format(self.name())):
            self._emit("_a = \\a")
            self._emit(".rept \\num")
            with self._indent_context():
                self._emit("v_accvgpr_write_b32 a[_a], 0")
                self._emit("_a = _a + 1")
            self._emit(".endr")

class gpr_sequencer_t(object):
    def __init__(self, cnt = 0):
        self.cnt = cnt
    def __call__(self, step = 0, alignment = 0):
        previous_cnt = self.cnt
        if alignment:
            aligned_cnt = ((previous_cnt + alignment - 1) // alignment) * alignment
            self.cnt = aligned_cnt
            previous_cnt = aligned_cnt
        self.cnt += step
        return previous_cnt
    def get(self):
        return self.cnt


def utility_list_to_string(arr):
    assert type(arr) is list
    return 'x'.join(f'{itm}' for itm in arr)

class utility_dict_with_default_t(object):
    def __init__(self, d):
        self.d = d
    def __call__(self, key, default_value):
        if self.d is None:
            return default_value
        if key in self.d:
            return self.d[key]
        return default_value

# compute next power of 2
def utility_next_pow2(n):
    if n == 0:
        return 1
    if n & (n - 1) == 0:
        return n
    while n & (n - 1) > 0:
        n &= (n - 1)
    return n << 1

def utility_next_mul(n, mul):
    d = n // mul
    d = d + (1 if (n % mul != 0) else 0)
    return d * mul

def utility_is_pow2(v):
    return v and (not(v & (v - 1)))

def utility_log2(v):
    assert (v and (not(v & (v - 1)))), 'v:{} must be power of 2'.format(v)
    return int(math.log2(v))

def utility_get_epack_length(precision):
        # GetEPackLength
        epack = 1
        if precision == AMDGPU_PRECISION_FP16:
            # todo: xdlops check
            epack = 2
        elif precision == AMDGPU_PRECISION_BF16:
            epack = 2
        return epack

def utility_gcd(a, b):
    # math.gcd new in python 3.5
    return math.gcd(a, b)

def utility_lcm(a, b):
    return abs(a * b) // math.gcd(a, b)

def utility_flatten_list_product(x):
    assert type(x) is list
    from functools import reduce
    return reduce(lambda a, b: a*b, x, 1)

def utility_flatten_list_accumulate(x):
    assert type(x) is list
    from functools import reduce
    return reduce(lambda a, b: a+b, x, 0)