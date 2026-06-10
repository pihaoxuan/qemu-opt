/*
 *  x86 misc helpers
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/cputlb.h"
#include "helper-tcg.h"


/* MXCSR bits that affect FMA computation results */
#define MXCSR_FP_NONDEFAULT  0xE040U   /* RC[14:13], FTZ[15], DAZ[6] */

/*
 * NOTE: the translator must set DisasContext.cc_op to CC_OP_EFLAGS
 * after generating a call to a helper that uses this.
 */
void cpu_load_eflags(CPUX86State *env, int eflags, int update_mask)
{
    CC_SRC = eflags & (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
    CC_OP = CC_OP_EFLAGS;
    env->df = 1 - (2 * ((eflags >> 10) & 1));
    env->eflags = (env->eflags & ~update_mask) |
        (eflags & update_mask) | 0x2;
}

void helper_into(CPUX86State *env, int next_eip_addend)
{
    int eflags;

    eflags = cpu_cc_compute_all(env);
    if (eflags & CC_O) {
        raise_interrupt(env, EXCP04_INTO, next_eip_addend);
    }
}

void helper_cpuid(CPUX86State *env)
{
    uint32_t eax, ebx, ecx, edx;

    cpu_svm_check_intercept_param(env, SVM_EXIT_CPUID, 0, GETPC());

    cpu_x86_cpuid(env, (uint32_t)env->regs[R_EAX], (uint32_t)env->regs[R_ECX],
                  &eax, &ebx, &ecx, &edx);
    env->regs[R_EAX] = eax;
    env->regs[R_EBX] = ebx;
    env->regs[R_ECX] = ecx;
    env->regs[R_EDX] = edx;
}

void helper_rdtsc(CPUX86State *env)
{
    uint64_t val;

    if ((env->cr[4] & CR4_TSD_MASK) && ((env->hflags & HF_CPL_MASK) != 0)) {
        raise_exception_ra(env, EXCP0D_GPF, GETPC());
    }
    cpu_svm_check_intercept_param(env, SVM_EXIT_RDTSC, 0, GETPC());

    val = cpu_get_tsc(env) + env->tsc_offset;
    env->regs[R_EAX] = (uint32_t)(val);
    env->regs[R_EDX] = (uint32_t)(val >> 32);
}

G_NORETURN void helper_rdpmc(CPUX86State *env)
{
    if (((env->cr[4] & CR4_PCE_MASK) == 0 ) &&
        ((env->hflags & HF_CPL_MASK) != 0)) {
        raise_exception_ra(env, EXCP0D_GPF, GETPC());
    }
    cpu_svm_check_intercept_param(env, SVM_EXIT_RDPMC, 0, GETPC());

    /* currently unimplemented */
    qemu_log_mask(LOG_UNIMP, "x86: unimplemented rdpmc\n");
    raise_exception_err(env, EXCP06_ILLOP, 0);
}

G_NORETURN void helper_pause(CPUX86State *env)
{
    CPUState *cs = env_cpu(env);

    /* Do gen_eob() tasks before going back to the main loop.  */
    do_end_instruction(env);
    helper_rechecking_single_step(env);

    /* Just let another CPU run.  */
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
}

uint64_t helper_rdpkru(CPUX86State *env, uint32_t ecx)
{
    if ((env->cr[4] & CR4_PKE_MASK) == 0) {
        raise_exception_err_ra(env, EXCP06_ILLOP, 0, GETPC());
    }
    if (ecx != 0) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
    }

    return env->pkru;
}

void helper_wrpkru(CPUX86State *env, uint32_t ecx, uint64_t val)
{
    CPUState *cs = env_cpu(env);

    if ((env->cr[4] & CR4_PKE_MASK) == 0) {
        raise_exception_err_ra(env, EXCP06_ILLOP, 0, GETPC());
    }
    if (ecx != 0 || (val & 0xFFFFFFFF00000000ull)) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
    }

    env->pkru = val;
    tlb_flush(cs);
}

target_ulong HELPER(rdpid)(CPUX86State *env)
{
#if !defined CONFIG_USER_ONLY
    return env->tsc_aux;
#elif defined CONFIG_LINUX && defined CONFIG_GETCPU
    unsigned cpu, node;
    getcpu(&cpu, &node);
    return (node << 12) | (cpu & 0xfff);
#elif defined CONFIG_SCHED_GETCPU
    return sched_getcpu();
#else
    return 0;
#endif
}


#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-ldst.h"
#include <math.h>

//void helper_custom_fast_fma(CPUX86State *env, uint32_t dest, uint32_t src1, uint32_t src2, target_ulong vaddr, uint32_t flags) {
//    // 获取宿主机的指令返回地址，极速访问内存发生缺页时，能精准恢复虚拟机上下文
//    uintptr_t ra = GETPC(); 
//    
//    // 解析标志位
//    uint32_t oprsz = flags & 0xFFFF;
//    uint32_t opcode = (flags >> 16) & 0xFF;
//    uint32_t w_bit = (flags >> 24) & 1;
//    
//    int element_count = oprsz / ((w_bit == 1) ? 8 : 4);
//
//    if (w_bit == 1) { // 64位 double (pd)
//        double * __restrict dest_ptr = (double *)&env->xmm_regs[dest];
//        double * __restrict src1_ptr = (double *)&env->xmm_regs[src1];
//        double * __restrict src2_ptr;
//        double mem_src2_pd[4] = {0};
//
//        if (src2 != 0xFFFFFFFF) {
//            src2_ptr = (double *)&env->xmm_regs[src2];
//        } else {
//            for (int i = 0; i < element_count; i++) {
//                union { uint64_t i; double d; } conv;
//                conv.i = cpu_ldq_data_ra(env, vaddr + i * 8, ra);
//                mem_src2_pd[i] = conv.d;
//            }
//            src2_ptr = mem_src2_pd;
//        }
//
//        for (int i = 0; i < element_count; i++) {
//            if (opcode == 0xB8) dest_ptr[i] = (src1_ptr[i] * src2_ptr[i]) + dest_ptr[i];
//            else if (opcode == 0xA8) dest_ptr[i] = (dest_ptr[i] * src1_ptr[i]) + src2_ptr[i];
//            else if (opcode == 0x98) dest_ptr[i] = (dest_ptr[i] * src2_ptr[i]) + src1_ptr[i];
//        }
//    } else { // 32位 float (ps)
//        float * __restrict dest_ptr = (float *)&env->xmm_regs[dest];
//        float * __restrict src1_ptr = (float *)&env->xmm_regs[src1];
//        float * __restrict src2_ptr;
//        float mem_src2_ps[8] = {0};
//
//        if (src2 != 0xFFFFFFFF) {
//            src2_ptr = (float *)&env->xmm_regs[src2];
//        } else {
//            for (int i = 0; i < element_count; i++) {
//                union { uint32_t i; float f; } conv;
//                conv.i = cpu_ldl_data_ra(env, vaddr + i * 4, ra);
//                mem_src2_ps[i] = conv.f;
//            }
//            src2_ptr = mem_src2_ps;
//        }
//	
//#pragma GCC ivdep
//        for (int i = 0; i < element_count; i++) {
//            if (opcode == 0xB8) dest_ptr[i] = (src1_ptr[i] * src2_ptr[i]) + dest_ptr[i];
//            else if (opcode == 0xA8) dest_ptr[i] = (dest_ptr[i] * src1_ptr[i]) + src2_ptr[i];
//            else if (opcode == 0x98) dest_ptr[i] = (dest_ptr[i] * src2_ptr[i]) + src1_ptr[i];
//        }
//    }
//
//    // 清除 YMM 高 128 位
//    if (oprsz == 16) {
//        env->xmm_regs[dest].ZMM_Q(2) = 0;
//        env->xmm_regs[dest].ZMM_Q(3) = 0;
//    }
//}
//
//
//
//
//
//uint64_t helper_custom_fma_chunk(uint64_t d, uint64_t a, uint64_t b, uint32_t flags) {
//    uint32_t opcode = flags & 0xFF;
//    uint32_t w_bit = (flags >> 8) & 1;
//    uint32_t chunk_idx = (flags >> 16) & 0xFF;
//
//    bool is_scalar = (opcode == 0x99 || opcode == 0xA9 || opcode == 0xB9);
//
//    if (w_bit == 1) { 
//        // 64位数据包含 1 个 double
//        union { uint64_t i; double f; } vd, va, vb, vres;
//        vd.i = d; va.i = a; vb.i = b;
//        
//	if(is_scalar && chunk_idx > 0){
//	    vres.f = vd.f;
//	}else{
//            if (opcode == 0xB8) vres.f = fma(va.f, vb.f, vd.f);
//            else if (opcode == 0xA8) vres.f = fma(vd.f, va.f, vb.f);
//            else if (opcode == 0x98) vres.f = fma(vd.f, vb.f, va.f);
//            else vres.f = vd.f;
//	}
//        return vres.i;
//    } else { 
//        // 64位数据包含 2 个 float
//        union { uint64_t i; float f[2]; } vd, va, vb, vres;
//        vd.i = d; va.i = a; vb.i = b;
//        
//        for (int j = 0; j < 2; j++) {
//	    if(is_scalar && (chunk_idx > 0 || j > 0)){
//		vres.f[j] = vd.f[j];
//	    }else{
//                if (opcode == 0xB8) vres.f[j] = fmaf(va.f[j], vb.f[j], vd.f[j]);
//                else if (opcode == 0xA8) vres.f[j] = fmaf(vd.f[j], va.f[j], vb.f[j]);
//                else if (opcode == 0x98) vres.f[j] = fmaf(vd.f[j], vb.f[j], va.f[j]);
//                else vres.f[j] = vd.f[j];
//	    }
//        }
//        
//        return vres.i;
//    }
//}


#include <lasxintrin.h> // 龙芯 256-bit 向量指令集 (LASX)
#include <lsxintrin.h>  // 龙芯 128-bit 向量指令集 (LSX)
// #include "exec/cpu_ldst.h" // 需要用到内存访问接口和 GETPC()

// ====================================================================
// 处理纯寄存器操作数的 Helper
// ====================================================================
void helper_custom_fma_vector_reg(CPUX86State *env, uint32_t dofs, uint32_t aofs, uint32_t bofs, uint32_t flags) {
    /* --- MXCSR 守卫：非默认 FP 环境 → 回退至软浮点 --- */
    if (unlikely(env->mxcsr & MXCSR_FP_NONDEFAULT)) {
        uint32_t opcode = flags & 0xFF;
        uint32_t w_bit  = (flags >> 8) & 1;
        uint32_t l_bit  = (flags >> 9) & 1;
        ZMMReg *dreg = (ZMMReg *)((uint8_t *)env + dofs);
        ZMMReg *areg = (ZMMReg *)((uint8_t *)env + aofs);
        ZMMReg *breg = (ZMMReg *)((uint8_t *)env + bofs);
        int n = l_bit ? (w_bit ? 4 : 8) : (w_bit ? 2 : 4);
        for (int i = 0; i < n; i++) {
            if (w_bit) {
                float64 d = dreg->ZMM_Q(i), a = areg->ZMM_Q(i), b = breg->ZMM_Q(i), r;
                if      (opcode == 0xB8) r = float64_muladd(a, b, d, 0, &env->sse_status);
                else if (opcode == 0xA8) r = float64_muladd(d, a, b, 0, &env->sse_status);
                else if (opcode == 0x98) r = float64_muladd(d, b, a, 0, &env->sse_status);
                else r = d;
                dreg->ZMM_Q(i) = r;
            } else {
                float32 d = dreg->ZMM_L(i), a = areg->ZMM_L(i), b = breg->ZMM_L(i), r;
                if      (opcode == 0xB8) r = float32_muladd(a, b, d, 0, &env->sse_status);
                else if (opcode == 0xA8) r = float32_muladd(d, a, b, 0, &env->sse_status);
                else if (opcode == 0x98) r = float32_muladd(d, b, a, 0, &env->sse_status);
                else r = d;
                dreg->ZMM_L(i) = r;
            }
        }
        return;   /* 跳过 LASX/LSX 快速路径 */
    }
    /* --- 守卫结束 --- */
    
    uint32_t opcode = flags & 0xFF;
    uint32_t w_bit = (flags >> 8) & 1;
    uint32_t l_bit = (flags >> 9) & 1;
    bool is_scalar = (opcode == 0x99 || opcode == 0xA9 || opcode == 0xB9);

    void *pd = (void *)env + dofs;
    void *pa = (void *)env + aofs;
    void *pb = (void *)env + bofs;

    if (l_bit == 1) { // 256-bit YMM (LASX)
        if (w_bit == 1) { // Double (4个双精度)
            __m256d vd = (__m256d)__lasx_xvld(pd, 0);
            __m256d va = (__m256d)__lasx_xvld(pa, 0);
            __m256d vb = (__m256d)__lasx_xvld(pb, 0);
            __m256d vres;

            if (opcode == 0xB8) vres = __lasx_xvfmadd_d(va, vb, vd);      // a*b+d
            else if (opcode == 0xA8) vres = __lasx_xvfmadd_d(vd, va, vb); // d*a+b
            else if (opcode == 0x98) vres = __lasx_xvfmadd_d(vd, vb, va); // d*b+a
            else vres = vd;
            
            // 兼容你的标量保留逻辑
            if (is_scalar) {
                double *res_f = (double *)&vres;
                double *d_f = (double *)pd;
                for (int j = 1; j < 4; j++) res_f[j] = d_f[j];
            }
            __lasx_xvst(vres, pd, 0);
            
        } else { // Float (8个单精度)
            __m256 vd = (__m256)__lasx_xvld(pd, 0);
            __m256 va = (__m256)__lasx_xvld(pa, 0);
            __m256 vb = (__m256)__lasx_xvld(pb, 0);
            __m256 vres;

            if (opcode == 0xB8) vres = __lasx_xvfmadd_s(va, vb, vd);
            else if (opcode == 0xA8) vres = __lasx_xvfmadd_s(vd, va, vb);
            else if (opcode == 0x98) vres = __lasx_xvfmadd_s(vd, vb, va);
            else vres = vd;

            if (is_scalar) {
                float *res_f = (float *)&vres;
                float *d_f = (float *)pd;
                for (int j = 1; j < 8; j++) res_f[j] = d_f[j];
            }
            __lasx_xvst(vres, pd, 0);
        }
    } else { // 128-bit XMM (LSX)
        if (w_bit == 1) { // Double (2个双精度)
            __m128d vd = (__m128d)__lsx_vld(pd, 0);
            __m128d va = (__m128d)__lsx_vld(pa, 0);
            __m128d vb = (__m128d)__lsx_vld(pb, 0);
            __m128d vres;

            if (opcode == 0xB8) vres = __lsx_vfmadd_d(va, vb, vd);
            else if (opcode == 0xA8) vres = __lsx_vfmadd_d(vd, va, vb);
            else if (opcode == 0x98) vres = __lsx_vfmadd_d(vd, vb, va);
            else vres = vd;

            if (is_scalar) {
                double *res_f = (double *)&vres;
                double *d_f = (double *)pd;
                res_f[1] = d_f[1];
            }
            __lsx_vst(vres, pd, 0);
        } else { // Float (4个单精度)
            __m128 vd = (__m128)__lsx_vld(pd, 0);
            __m128 va = (__m128)__lsx_vld(pa, 0);
            __m128 vb = (__m128)__lsx_vld(pb, 0);
            __m128 vres;

            if (opcode == 0xB8) vres = __lsx_vfmadd_s(va, vb, vd);
            else if (opcode == 0xA8) vres = __lsx_vfmadd_s(vd, va, vb);
            else if (opcode == 0x98) vres = __lsx_vfmadd_s(vd, vb, va);
            else vres = vd;

            if (is_scalar) {
                float *res_f = (float *)&vres;
                float *d_f = (float *)pd;
                for (int j = 1; j < 4; j++) res_f[j] = d_f[j];
            }
            __lsx_vst(vres, pd, 0);
        }
    }
}

// ====================================================================
// 处理带内存操作数的 Helper
// ====================================================================
void helper_custom_fma_vector_mem(CPUX86State *env, uint32_t dofs, uint32_t aofs, target_ulong vaddr, uint32_t flags) {
    uintptr_t ra = GETPC(); // 捕获异常返回地址，用于引发缺页异常时回溯
    uint32_t opcode = flags & 0xFF;
    uint32_t w_bit = (flags >> 8) & 1;
    uint32_t l_bit = (flags >> 9) & 1;
    bool is_scalar = (opcode == 0x99 || opcode == 0xA9 || opcode == 0xB9);

    // 核心安全点：使用 QEMU API 读取 Guest 内存并存放到宿主机的栈对齐数组中
    __attribute__((aligned(32))) uint64_t mem_buf[4] = {0};
    mem_buf[0] = cpu_ldq_data_ra(env, vaddr, ra);
    mem_buf[1] = cpu_ldq_data_ra(env, vaddr + 8, ra);
    if (l_bit == 1) {
        mem_buf[2] = cpu_ldq_data_ra(env, vaddr + 16, ra);
        mem_buf[3] = cpu_ldq_data_ra(env, vaddr + 24, ra);
    }

    void *pd = (void *)env + dofs;
    void *pa = (void *)env + aofs;
    void *pb = (void *)mem_buf; // 将读入的内存作为操作数 b

    /* --- MXCSR 守卫：内存读完后检查，非默认 FP 环境 → 软浮点回退 --- */
    if (unlikely(env->mxcsr & MXCSR_FP_NONDEFAULT)) {
        ZMMReg *dreg = (ZMMReg *)pd;
        ZMMReg *areg = (ZMMReg *)pa;
        ZMMReg *breg = (ZMMReg *)mem_buf;  /* 内存操作数已就位 */
        int n = l_bit ? (w_bit ? 4 : 8) : (w_bit ? 2 : 4);
        for (int i = 0; i < n; i++) {
            if (w_bit) {
                float64 d = dreg->ZMM_Q(i), a = areg->ZMM_Q(i),
                        b = breg->ZMM_Q(i), r;
                if      (opcode == 0xB8) r = float64_muladd(a, b, d, 0, &env->sse_status);
                else if (opcode == 0xA8) r = float64_muladd(d, a, b, 0, &env->sse_status);
                else if (opcode == 0x98) r = float64_muladd(d, b, a, 0, &env->sse_status);
                else r = d;
                dreg->ZMM_Q(i) = r;
            } else {
                float32 d = dreg->ZMM_L(i), a = areg->ZMM_L(i),
                        b = breg->ZMM_L(i), r;
                if      (opcode == 0xB8) r = float32_muladd(a, b, d, 0, &env->sse_status);
                else if (opcode == 0xA8) r = float32_muladd(d, a, b, 0, &env->sse_status);
                else if (opcode == 0x98) r = float32_muladd(d, b, a, 0, &env->sse_status);
                else r = d;
                dreg->ZMM_L(i) = r;
            }
        }
        return;   /* 跳过 LASX/LSX 快速路径 */
    }
    /* --- 守卫结束 --- */

    // -------- 接下来的计算逻辑与 Register Helper 完全一致 --------
    if (l_bit == 1) { // 256-bit YMM (LASX)
        if (w_bit == 1) {
            __m256d vd = (__m256d)__lasx_xvld(pd, 0);
            __m256d va = (__m256d)__lasx_xvld(pa, 0);
            __m256d vb = (__m256d)__lasx_xvld(pb, 0);
            __m256d vres;

            if (opcode == 0xB8) vres = __lasx_xvfmadd_d(va, vb, vd);
            else if (opcode == 0xA8) vres = __lasx_xvfmadd_d(vd, va, vb);
            else if (opcode == 0x98) vres = __lasx_xvfmadd_d(vd, vb, va);
            else vres = vd;
            
            if (is_scalar) {
                double *res_f = (double *)&vres;
                double *d_f = (double *)pd;
                for (int j = 1; j < 4; j++) res_f[j] = d_f[j];
            }
            __lasx_xvst(vres, pd, 0);
        } else {
            __m256 vd = (__m256)__lasx_xvld(pd, 0);
            __m256 va = (__m256)__lasx_xvld(pa, 0);
            __m256 vb = (__m256)__lasx_xvld(pb, 0);
            __m256 vres;

            if (opcode == 0xB8) vres = __lasx_xvfmadd_s(va, vb, vd);
            else if (opcode == 0xA8) vres = __lasx_xvfmadd_s(vd, va, vb);
            else if (opcode == 0x98) vres = __lasx_xvfmadd_s(vd, vb, va);
            else vres = vd;

            if (is_scalar) {
                float *res_f = (float *)&vres;
                float *d_f = (float *)pd;
                for (int j = 1; j < 8; j++) res_f[j] = d_f[j];
            }
            __lasx_xvst(vres, pd, 0);
        }
    } else { // 128-bit XMM (LSX)
        if (w_bit == 1) {
            __m128d vd = (__m128d)__lsx_vld(pd, 0);
            __m128d va = (__m128d)__lsx_vld(pa, 0);
            __m128d vb = (__m128d)__lsx_vld(pb, 0);
            __m128d vres;

            if (opcode == 0xB8) vres = __lsx_vfmadd_d(va, vb, vd);
            else if (opcode == 0xA8) vres = __lsx_vfmadd_d(vd, va, vb);
            else if (opcode == 0x98) vres = __lsx_vfmadd_d(vd, vb, va);
            else vres = vd;

            if (is_scalar) {
                double *res_f = (double *)&vres;
                double *d_f = (double *)pd;
                res_f[1] = d_f[1];
            }
            __lsx_vst(vres, pd, 0);
        } else {
            __m128 vd = (__m128)__lsx_vld(pd, 0);
            __m128 va = (__m128)__lsx_vld(pa, 0);
            __m128 vb = (__m128)__lsx_vld(pb, 0);
            __m128 vres;

            if (opcode == 0xB8) vres = __lsx_vfmadd_s(va, vb, vd);
            else if (opcode == 0xA8) vres = __lsx_vfmadd_s(vd, va, vb);
            else if (opcode == 0x98) vres = __lsx_vfmadd_s(vd, vb, va);
            else vres = vd;

            if (is_scalar) {
                float *res_f = (float *)&vres;
                float *d_f = (float *)pd;
                for (int j = 1; j < 4; j++) res_f[j] = d_f[j];
            }
            __lsx_vst(vres, pd, 0);
        }
    }
}
