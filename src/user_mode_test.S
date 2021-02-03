.globl entry

entry:
    syscall

    pushq $42
    popq %rax

    syscall

.loop:
    syscall

    pushq $42
    popq %rax

    jmp .loop