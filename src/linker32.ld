ENTRY(entry)

SECTIONS {
    . = 0x100000;

    .text : {
        *(.text.multiboot)
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : ALIGN(4096) {
        *(.bss)
        *(COMMON)
    }
}