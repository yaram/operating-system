ENTRY(entry)

MEMORY {
    main (rwax) : ORIGIN = 0x200000, LENGTH = 0x800000 - 0x200000
}

SECTIONS {
    . = 0x200000;

    .text : {
        *(.text.entry)
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.eh_frame)
        *(.rodata)
        *(.rodata.*)

        *(.init_array_start)
        *(.init_array)
        *(.init_array_end)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(COMMON)
    }
}