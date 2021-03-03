.extern exception_handler

#define exception_thunk(index) ;\
.globl exception_handler_thunk_##index ;\
exception_handler_thunk_##index: ;\
    cli ;\
;\
    sub $8, %rsp ;\
;\
    sub $512, %rsp ;\
    fxsave64 (%rsp) ;\
;\
    sub $8, %rsp ;\
;\
    push %rbp ;\
    push %r15 ;\
    push %r14 ;\
    push %r13 ;\
    push %r12 ;\
    push %r11 ;\
    push %r10 ;\
    push %r9 ;\
    push %r8 ;\
    push %rdi ;\
    push %rsi ;\
    push %rdx ;\
    push %rcx ;\
    push %rbx ;\
    push %rax ;\
;\
    mov $index, %rdi ;\
    lea 0x288(%rsp), %rsi ;\
    mov $0, %rdx ;\
;\
    call exception_handler

#define exception_thunk_error_code(index) ;\
.globl exception_handler_thunk_##index ;\
exception_handler_thunk_##index: ;\
    cli ;\
;\
    sub $8, %rsp ;\
;\
    sub $512, %rsp ;\
    fxsave64 (%rsp) ;\
;\
    sub $8, %rsp ;\
;\
    push %rbp ;\
    push %r15 ;\
    push %r14 ;\
    push %r13 ;\
    push %r12 ;\
    push %r11 ;\
    push %r10 ;\
    push %r9 ;\
    push %r8 ;\
    push %rdi ;\
    push %rsi ;\
    push %rdx ;\
    push %rcx ;\
    push %rbx ;\
    push %rax ;\
;\
    mov $index, %rdi ;\
    lea 0x290(%rsp), %rsi ;\
    mov 0x288(%rsp), %rdx ;\
;\
    call exception_handler

exception_thunk(0)
exception_thunk(1)
exception_thunk(2)
exception_thunk(3)
exception_thunk(4)
exception_thunk(5)
exception_thunk(6)
exception_thunk(7)
exception_thunk_error_code(8)
exception_thunk_error_code(10)
exception_thunk_error_code(11)
exception_thunk_error_code(12)
exception_thunk_error_code(13)
exception_thunk_error_code(14)
exception_thunk(15)
exception_thunk(16)
exception_thunk_error_code(17)
exception_thunk(18)
exception_thunk(19)
exception_thunk(20)
exception_thunk_error_code(30)