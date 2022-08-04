ENTRY(entry)

MEMORY {
    main (rwax) : ORIGIN = 0x1000, LENGTH = 0x80000 - 0x1000
}

SECTIONS {
    . = 0x1000;

    .text : {
        *(.text.entry)
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