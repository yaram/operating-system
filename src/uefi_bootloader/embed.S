.section .rodata

.globl kernel_binary
.globl kernel_binary_end

kernel_binary:
.incbin "build/kernel.bin"
kernel_binary_end: