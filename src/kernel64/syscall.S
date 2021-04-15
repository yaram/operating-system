.set PROCESSOR_STACK_SIZE, 1024 * 16

.set USER_PROCESSOR_AREA_ADDRESS, 0x800000

.set STACK_OFFSET, 0

.extern syscall_entrance

.globl syscall_thunk
syscall_thunk:
    mov %rsp, %rax

    mov $(USER_PROCESSOR_AREA_ADDRESS + STACK_OFFSET + PROCESSOR_STACK_SIZE), %rsp

    pushq $0x1b
    push %rax
    push %r11
    pushq $0x23
    push %rcx

    pushq $0

    sub $512, %rsp
    fxsave64 (%rsp)

    sub $8, %rsp

    push %rbp
    push %r15
    push %r14
    push %r13
    push %r12
    pushq $0
    push %r10
    push %r9
    push %r8
    push %rdi
    push %rsi
    push %rdx
    pushq $0
    push %rbx
    pushq $0

    mov %rsp, %rdi

    call syscall_entrance

    add $8, %rsp
    pop %rbx
    add $8, %rsp
    pop %rdx
    pop %rsi
    pop %rdi
    pop %r8
    pop %r9
    pop %r10
    add $8, %rsp
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    pop %rbp

    add $8, %rsp

    fxrstor64 (%rsp)
    add $512, %rsp

    add $8, %rsp

    pop %rcx
    add $8, %rsp
    pop %r11
    pop %rax
    add $8, %rsp

    mov %rax, %rsp

    sysretq