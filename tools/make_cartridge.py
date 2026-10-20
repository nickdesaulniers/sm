#!/usr/bin/env python3
"""Makes the playable GBA cartridge from the engine and your Super Metroid ROM.

    make_cartridge.py sm-engine.gba sm.smc sm.gba

The engine (the build's sm-engine.gba, or a release's) has none of the
game's data: this appends it, from your ROM, as gba/cart_data.h describes.
The ROM must be the US release (sha1 da957f0d63d14cb441d215462904c4fa8519c613),
with or without a copier's 512-byte header.
"""
import hashlib
import struct
import sys

SNES_ROM_SHA1 = 'da957f0d63d14cb441d215462904c4fa8519c613'
SNES_ROM_SIZE = 0x300000
MAGIC = b'SMGBA\0\0\0'
VERSION = 1
HEADER_SIZE = 256


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip())
    engine_path, rom_path, out_path = sys.argv[1:]
    engine = open(engine_path, 'rb').read()
    rom = open(rom_path, 'rb').read()
    if len(rom) == SNES_ROM_SIZE + 512:
        rom = rom[512:]
    if hashlib.sha1(rom).hexdigest() != SNES_ROM_SHA1:
        sys.exit(f'{rom_path}: not the Super Metroid ROM this needs '
                 f'(sha1 {SNES_ROM_SHA1})')
    if len(engine) % 256:
        sys.exit(f'{engine_path}: not an engine image (its size is not a multiple of 256)')
    header = MAGIC + struct.pack('<II', VERSION, len(rom))
    header += bytes(HEADER_SIZE - len(header))
    with open(out_path, 'wb') as f:
        f.write(engine + header + rom)


if __name__ == '__main__':
    main()
