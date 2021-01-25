.set MAGIC, 0x1BADB002
.set FLAG_ALIGN, 1 << 0
.set FLAGS, FLAG_ALIGN
.set CHECKSUM, -(MAGIC + FLAGS)

.section .text.multiboot
.align 4

.long MAGIC
.long FLAGS
.long CHECKSUM