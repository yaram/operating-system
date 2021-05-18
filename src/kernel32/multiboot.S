.set FLAG_MEMINFO, 1 << 1

.set MAGIC, 0x1BADB002
.set FLAGS, FLAG_MEMINFO
.set CHECKSUM, -(MAGIC + FLAGS)

.section .text.multiboot
.align 4

.long MAGIC
.long FLAGS
.long CHECKSUM