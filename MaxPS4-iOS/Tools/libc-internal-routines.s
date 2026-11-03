    .intel_syntax noprefix
    .text
# SysV x86-64 routines for libSceLibcInternal's internal_* functions, embedded
# as internal_*_code in Native/maxps4_ps4_libkernel.cpp. On an x86-64 host:
#   c++ -std=c++17 Tools/libc-internal-check.cpp Tools/libc-internal-routines.s
# checks every routine against the host libc/libm, then prints the arrays.
    .globl r_memcpy
r_memcpy:
    mov rax, rdi
    mov rcx, rdx
    rep movsb
    ret
    .globl r_memcpy_end
r_memcpy_end:

    .globl r_memmove
r_memmove:
    mov rax, rdi
    mov rcx, rdx
    cmp rdi, rsi
    jbe 1f
    lea r8, [rsi+rdx]
    cmp rdi, r8
    jae 1f
    lea rsi, [rsi+rdx-1]
    lea rdi, [rdi+rdx-1]
    std
    rep movsb
    cld
    ret
1:  rep movsb
    ret
    .globl r_memmove_end
r_memmove_end:

    .globl r_memset
r_memset:
    mov r9, rdi
    mov eax, esi
    mov rcx, rdx
    rep stosb
    mov rax, r9
    ret
    .globl r_memset_end
r_memset_end:

    .globl r_memcmp
r_memcmp:
    xor eax, eax
1:  test rdx, rdx
    jz 2f
    movzx eax, byte ptr [rdi]
    movzx ecx, byte ptr [rsi]
    sub eax, ecx
    jnz 2f
    inc rdi
    inc rsi
    dec rdx
    jmp 1b
2:  ret
    .globl r_memcmp_end
r_memcmp_end:

    .globl r_strlen
r_strlen:
    mov rax, rdi
1:  cmp byte ptr [rax], 0
    je 2f
    inc rax
    jmp 1b
2:  sub rax, rdi
    ret
    .globl r_strlen_end
r_strlen_end:

    .globl r_strcmp
r_strcmp:
1:  movzx eax, byte ptr [rdi]
    movzx ecx, byte ptr [rsi]
    sub eax, ecx
    jnz 2f
    test ecx, ecx
    jz 2f
    inc rdi
    inc rsi
    jmp 1b
2:  ret
    .globl r_strcmp_end
r_strcmp_end:

    .globl r_strncmp
r_strncmp:
    xor eax, eax
1:  test rdx, rdx
    jz 2f
    movzx eax, byte ptr [rdi]
    movzx ecx, byte ptr [rsi]
    sub eax, ecx
    jnz 2f
    test ecx, ecx
    jz 2f
    inc rdi
    inc rsi
    dec rdx
    jmp 1b
2:  ret
    .globl r_strncmp_end
r_strncmp_end:

# strncpy(dest, src, n): copies up to n bytes, pads with zeros.
    .globl r_strncpy
r_strncpy:
    mov rax, rdi
1:  test rdx, rdx
    jz 3f
    movzx ecx, byte ptr [rsi]
    mov byte ptr [rdi], cl
    inc rdi
    dec rdx
    test ecx, ecx
    jz 2f
    inc rsi
    jmp 1b
2:  test rdx, rdx
    jz 3f
    mov byte ptr [rdi], 0
    inc rdi
    dec rdx
    jmp 2b
3:  ret
    .globl r_strncpy_end
r_strncpy_end:

# memcpy_s(dest, destsz, src, count): 0, EINVAL (22) for a null pointer,
# ERANGE (34) when count > destsz (dest is then zeroed).
    .globl r_memcpy_s
r_memcpy_s:
    test rdi, rdi
    jz 3f
    test rdx, rdx
    jz 2f
    cmp rcx, rsi
    ja 1f
    xchg rsi, rdx
    mov rax, rcx
    mov rcx, rax
    rep movsb
    xor eax, eax
    ret
1:  mov rcx, rsi
    xor eax, eax
    rep stosb
    mov eax, 34
    ret
2:  mov rcx, rsi
    xor eax, eax
    rep stosb
3:  mov eax, 22
    ret
    .globl r_memcpy_s_end
r_memcpy_s_end:

# strncpy_s(dest, destsz, src, count): copies at most count characters and
# always terminates; ERANGE (34) with dest[0] = 0 when it does not fit.
    .globl r_strncpy_s
r_strncpy_s:
    test rdi, rdi
    jz 4f
    test rsi, rsi
    jz 4f
    test rdx, rdx
    jz 5f
    mov r8, rdi
1:  test rcx, rcx
    jz 2f
    movzx eax, byte ptr [rdx]
    test eax, eax
    jz 2f
    cmp rsi, 1
    jbe 3f
    mov byte ptr [rdi], al
    inc rdi
    inc rdx
    dec rsi
    dec rcx
    jmp 1b
2:  mov byte ptr [rdi], 0
    xor eax, eax
    ret
3:  mov byte ptr [r8], 0
    mov eax, 34
    ret
5:  mov byte ptr [rdi], 0
4:  mov eax, 22
    ret
    .globl r_strncpy_s_end
r_strncpy_s_end:

# Floats in xmm0 (and xmm1), through x87.
    .globl r_sinf
r_sinf:
    sub rsp, 8
    movss dword ptr [rsp], xmm0
    fld dword ptr [rsp]
    fsin
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 8
    ret
    .globl r_sinf_end
r_sinf_end:

    .globl r_cosf
r_cosf:
    sub rsp, 8
    movss dword ptr [rsp], xmm0
    fld dword ptr [rsp]
    fcos
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 8
    ret
    .globl r_cosf_end
r_cosf_end:

    .globl r_tanf
r_tanf:
    sub rsp, 8
    movss dword ptr [rsp], xmm0
    fld dword ptr [rsp]
    fptan
    fstp st(0)
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 8
    ret
    .globl r_tanf_end
r_tanf_end:

    .globl r_atanf
r_atanf:
    sub rsp, 8
    movss dword ptr [rsp], xmm0
    fld dword ptr [rsp]
    fld1
    fpatan
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 8
    ret
    .globl r_atanf_end
r_atanf_end:

# atan2f(y, x): fpatan gives atan(st1 / st0) in the right quadrant.
    .globl r_atan2f
r_atan2f:
    sub rsp, 8
    movss dword ptr [rsp], xmm0
    fld dword ptr [rsp]
    movss dword ptr [rsp], xmm1
    fld dword ptr [rsp]
    fpatan
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 8
    ret
    .globl r_atan2f_end
r_atan2f_end:

# expf(x) = 2^(x log2 e), split into integer and fraction for f2xm1/fscale.
    .globl r_expf
r_expf:
    sub rsp, 8
    movss dword ptr [rsp], xmm0
    fld dword ptr [rsp]
    fldl2e
    fmulp st(1), st
    fld st(0)
    frndint
    fsub st(1), st
    fxch st(1)
    f2xm1
    fld1
    faddp st(1), st
    fscale
    fstp st(1)
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 8
    ret
    .globl r_expf_end
r_expf_end:

    .globl r_logf
r_logf:
    sub rsp, 8
    movss dword ptr [rsp], xmm0
    fldln2
    fld dword ptr [rsp]
    fyl2x
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 8
    ret
    .globl r_logf_end
r_logf_end:

# sincosf(x, float* sine, float* cosine): fsincos leaves cos in st0, sin in st1.
    .globl r_sincosf
r_sincosf:
    sub rsp, 8
    movss dword ptr [rsp], xmm0
    fld dword ptr [rsp]
    fsincos
    fstp dword ptr [rsi]
    fstp dword ptr [rdi]
    add rsp, 8
    ret
    .globl r_sincosf_end
r_sincosf_end:

    .globl r_atan
r_atan:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fld1
    fpatan
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
    .globl r_atan_end
r_atan_end:

    .globl r_cos
r_cos:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fcos
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
    .globl r_cos_end
r_cos_end:

    .globl r_tan
r_tan:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fptan
    fstp st(0)
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
    .globl r_tan_end
r_tan_end:
