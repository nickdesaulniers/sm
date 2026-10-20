/* Linker script for the GBA cartridge.
 * https://problemkaputt.de/gbatek.htm#gbamemorymap
 *
 * Code runs from the cartridge, except *.iwram.c and .iwram sections, which
 * go to IWRAM. Data goes to EWRAM (the game's 128 KB of SNES RAM is most of
 * it), except .iwram.data and .iwram_bss. */
ENTRY(_start)

MEMORY {
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 32K
    rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 32M
}

/* Top of IWRAM: 0x03007FA0-0x03007FFF belongs to the BIOS. */
__sp_irq = 0x03007FA0;
__sp_sys = 0x03007F00;
/* Space reserved below __sp_sys for the System-mode stack: the game used
   968 bytes at most, measured on Zebes (main.c logs it). */
__stack_size = 2048;

SECTIONS {
    .crt0 : {
        KEEP(*(.crt0.entry))
        ASSERT(. == 0x08000004, "ROM header must follow the 4-byte entry branch");
        KEEP(*(.gba_header))
        ASSERT(. == 0x080000C0, "ROM header must be 0xBC bytes");
        KEEP(*(.crt0.text))
    } > rom

    .text : {
        *(EXCLUDE_FILE(*.iwram.c.o*) .text .text.*)
    } > rom
    .rodata : { *(.rodata .rodata.*) } > rom

    .iwram : ALIGN(4) {
        __iwram_start = .;
        *.iwram.c.o*(.text .text.*)
        *(.iwram .iwram.*)
        . = ALIGN(4);
        __iwram_end = .;
    } > iwram AT> rom
    __iwram_lma = LOADADDR(.iwram);

    .iwram_bss (NOLOAD) : ALIGN(4) {
        __iwram_bss_start = .;
        *(.iwram_bss .iwram_bss.*)
        . = ALIGN(4);
        __iwram_bss_end = .;
    } > iwram
    ASSERT(__iwram_bss_end <= __sp_sys - __stack_size, "IWRAM overflows into the stack")

    .data : ALIGN(4) {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > ewram AT> rom
    __data_lma = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        *(.ewram_bss .ewram_bss.*)
        . = ALIGN(4);
        __bss_end = .;
    } > ewram
    __heap_start = .;
    __heap_end = ORIGIN(ewram) + LENGTH(ewram);

    /* The end of the engine's image, padded to a multiple of 256 bytes:
       tools/make_cartridge.py appends the game's data, the SNES ROM among
       it, here (see cart_data.h). */
    .rom_end : {
        BYTE(0)
        . = ALIGN(256);
    } > rom
    __cart_data = LOADADDR(.rom_end) + SIZEOF(.rom_end);
    snes_rom = __cart_data + 256;

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
