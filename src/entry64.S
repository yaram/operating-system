.section .text.entry

.extern main

.globl entry

entry:
    // Set up segments not set up previously
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    lea stack_top(%rip), %rsp

    call main

    cli
.loop:
    hlt
    jmp .loop

.section .bss
.align 16

stack_bottom:
.skip 1024 * 16
stack_top: