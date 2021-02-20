.globl entry

entry:
    syscall

.loop:
    jmp .loop