.globl entry

entry:
    mov $2, %rbx
    syscall

    mov $1, %rbx
    syscall

    mov $0, %rbx
    syscall