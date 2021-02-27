.set protection_bit_mask, 1 << 0

.set paging_bit_mask, 1 << 31

.set pae_bit_mask, 1 << 5

.set long_mode_bit_mask, 1 << 8

.equ kernel64_load_location, 0x200000

.section .text

.globl entry
entry:
    cli

    // Set up known GDT

    lgdt gdt_descriptor

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    ljmp $0x08, $.long_jump_target
.long_jump_target:

    // Fill in memory map from the multiboot memory map

    mov 44(%ebx), %eax
    mov 48(%ebx), %esi
    mov %esi, %edi
    add %eax, %edi

    mov $0, %eax

.memory_map_loop:
    mov 4(%esi), %ebx
    mov %ebx, memory_map_buffer(%eax)

    mov 8(%esi), %ebx
    mov %ebx, memory_map_buffer + 4(%eax)

    mov 12(%esi), %ebx
    mov %ebx, memory_map_buffer + 8(%eax)

    mov 16(%esi), %ebx
    mov %ebx, memory_map_buffer + 12(%eax)

    mov $0, %ebx
    mov 20(%esi), %bl
    cmp $1, %bl
    sete %bl
    mov %bl, memory_map_buffer + 16(%eax)

    mov (%esi), %ebx
    add %ebx, %esi
    add $4, %esi

    add $24, %eax

    cmp %edi, %esi
    jl .memory_map_loop

    mov $0, %edx
    mov $24, %ebx
    div %ebx
    mov %eax, (memory_map_size)

    // Copy 64-bit kernel to it's final memory location
    mov $kernel64, %esi
    mov $kernel64_load_location, %edi
    mov $(kernel64_end - kernel64), %ecx
    rep movsb

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
    rep stosl

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

    // Enable paging
    mov %cr0, %eax
    or $paging_bit_mask, %eax
    mov %eax, %cr0

    // Load main parameters
    mov $memory_map_buffer, %edi
    mov (memory_map_size), %esi

    // Switch from 32-bit to 64-bit mode code segment & jump to 64-bit kernel code

    ljmp $0x18, $kernel64_load_location

.section .rodata

gdt_descriptor:
    .word 5 * 8 - 1
    .long gdt_entries

gdt_entries:
    .long 0
    .long 0

    .word 0xFFFF
    .word 0
    .byte 0
    .byte 0b10011010
    .byte 0b11001111
    .byte 0

    .word 0xFFFF
    .word 0
    .byte 0
    .byte 0b10010010
    .byte 0b11001111
    .byte 0

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

memory_map_size:
    .skip 4

memory_map_buffer:
    .skip 4096

.align 4096

pml4t:
    .skip 4096

pdpt:
    .skip 4096

pdt:
    .skip 4096

pt:
    .skip 4096 * 4

.section .data

kernel64:
.incbin "build/kernel64.bin"
kernel64_end: