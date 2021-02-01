.extern stack_top
.extern syscall_entrance

.globl user_enter_thunk
user_enter_thunk:
    mov $0, %r11
    mov %rdi, %rcx
    mov %rsi, %rsp

    sysretq

.globl syscall_thunk
syscall_thunk:
    mov %rsp, %rax

    mov $stack_top, %rsp

    push %rcx
    push %r11

    push %rax

    mov %rcx, %rdi
    mov %rax, %rsi

    call syscall_entrance

    pop %rax

    pop %r11
    pop %rcx

    mov %rax, %rsp

    sysretq