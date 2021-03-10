.extern preempt_timer_handler

.globl preempt_timer_thunk
preempt_timer_thunk:
    pushq $0

    sub $512, %rsp
    fxsave64 (%rsp)

    sub $8, %rsp

    push %rbp
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    mov %rsp, %rdi

    call preempt_timer_handler

    hlt

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