@ The SNES ROM, in the cartridge after the code. The game reads all its
@ data from it, through g_rom.
    .section .snes_rom, "a", %progbits
    .balign 4
    .global snes_rom
snes_rom:
    .incbin SM_ROM_PATH
    .global snes_rom_end
snes_rom_end:
