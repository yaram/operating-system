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