ENTRY(entry)

MEMORY {
    main (rwax) : ORIGIN = 0x100000, LENGTH = 0x200000 - 0x100000
}

SECTIONS {
    . = 0x100000;

    .text : {
        *(.text.multiboot)
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .bss : ALIGN(4096) {
        *(.bss)
        *(COMMON)
    }

    .data : {
        *(.data)
    }
}