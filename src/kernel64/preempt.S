.globl user_enter_thunk
user_enter_thunk:
    mov %rdi, %rsp

    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    pop %rbp

    add $8, %rsp

    fxrstor64 (%rsp)
    add $512, %rsp

    add $8, %rsp

    iretq