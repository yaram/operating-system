.set protection_bit_mask, 1 << 0

.set paging_bit_mask, 1 << 31

.set pae_bit_mask, 1 << 5

.set long_mode_bit_mask, 1 << 8

.equ kernel_load_location, 0x200000

.section .text.entry

.global entry

entry:
    ljmp $0x00, $.continued
.continued:

    // Enable extended memory
    mov %cr4, %eax
    or $pae_bit_mask, %eax
    mov %eax, %cr4

    // Load page table pointer
    mov $pml4t, %edi
    mov %edi, %cr3

    // Clear all 3 coarse page tables
    mov $0, %eax
    mov $(4096 * 3), %ecx
    rep stosb

    // Set up page table entries

    mov $pdpt, %eax
    and $~0xFFF, %eax
    or $0b11, %eax
    mov %eax, (pml4t)

    mov $pdt, %eax
    and $~0xFFF, %eax
    or $0b11, %eax
    mov %eax, (pdpt)

    mov $pt, %eax
    and $~0xFFF, %eax
    or $0b11, %eax
    mov %eax, (pdt)

    mov $pt, %eax
    add $4096, %eax
    and $~0xFFF, %eax
    or $0b11, %eax
    mov %eax, (pdt + 8)

    mov $pt, %eax
    add $(4096 * 2), %eax
    and $~0xFFF, %eax
    or $0b11, %eax
    mov %eax, (pdt + 16)

    mov $pt, %eax
    add $(4096 * 3), %eax
    and $~0xFFF, %eax
    or $0b11, %eax
    mov %eax, (pdt + 24)

    mov $pt, %edi
    mov $0b11, %ebx
    mov $(512 * 4), %ecx

.page_table_loop:
    mov %ebx, (%edi)
    add $4096, %ebx
    add $8, %edi
    loop .page_table_loop

    // Enable long mode
    mov $0xC0000080, %ecx
    rdmsr
    or $long_mode_bit_mask, %eax
    wrmsr

    // Enable paging and protection
    mov %cr0, %eax
    or $paging_bit_mask, %eax
    or $protection_bit_mask, %eax
    mov %eax, %cr0

    // Load bootstrap gdt
    lgdt gdt_descriptor

    // Load bootstrap stack
    lea stack_top, %esp

    // Set data segments
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    // Load kernel main parameters
    mov $0, %edi

    // Switch from 32-bit to 64-bit mode code segment & jump to 64-bit kernel code
    ljmpl $0x18, $kernel_load_location

.section .rodata

gdt_descriptor:
    .word 5 * 8 - 1
    .long gdt_entries

gdt_entries:
    .long 0
    .long 0

    .long 0
    .long 0

    .long 0
    .long 0

    .word 0
    .word 0
    .byte 0
    .byte 0b10011010
    .byte 0b10101111
    .byte 0

    .word 0
    .word 0
    .byte 0
    .byte 0b10010010
    .byte 0b00000000
    .byte 0

.section .bss

.align 4096

pml4t:
    .skip 4096

pdpt:
    .skip 4096

pdt:
    .skip 4096

pt:
    .skip 4096 * 4

.section .bss

.align 16

stack_bottom:
    .skip 16 * 1024
stack_top: