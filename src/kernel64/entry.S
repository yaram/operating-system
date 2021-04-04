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

    lea stack_top, %rsp

    call main

    cli
.loop:
    hlt
    jmp .loop

.section .bss
.align 16

.globl stack_top

stack_bottom:
.skip 1024 * 16
stack_top:

.section .rodata

.globl embedded_init_binary
.globl embedded_init_binary_end

embedded_init_binary:
.incbin "build/init.elf"
embedded_init_binary_end:

.globl multiprocessor_binary
.globl multiprocessor_binary_end

multiprocessor_binary:
.incbin "build/multiprocessor.bin"
multiprocessor_binary_end: