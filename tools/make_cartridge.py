#!/usr/bin/env python3
"""Makes the playable GBA cartridge from the engine and your Super Metroid ROM.

    make_cartridge.py sm-engine.gba sm.smc sm.gba

The engine (the build's sm-engine.gba, or a release's) has none of the
game's data: this appends it, from your ROM, as gba/cart_data.h describes.
The ROM must be the US release (sha1 da957f0d63d14cb441d215462904c4fa8519c613),
with or without a copier's 512-byte header.

Besides the ROM itself, the data has the game's audio samples, decoded from
the SNES's BRR to the 8-bit PCM the GBA plays (gba/audio.c), and the menu
fonts' tiles redrawn for the squashed screens (tools/squash_font.py).
"""
import hashlib
import os
import struct
import sys

sys.dont_write_bytecode = True  # (no tools/__pycache__ for squash_font)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import squash_font  # noqa: E402

SNES_ROM_SHA1 = 'da957f0d63d14cb441d215462904c4fa8519c613'
SNES_ROM_SIZE = 0x300000
MAGIC = b'SMGBA\0\0\0'
VERSION = 3
HEADER_SIZE = 256

# The SNES addresses of the game's uploads to the APU: the first is the
# sound driver with its common samples, the others the music for each
# part of the game (kMusicPointers, 0x8FE7E1, which also starts with the
# first).
MUSIC_POINTERS = 0x8FE7E1
NUM_MUSIC_BANKS = 25
# Where the driver keeps the sample directory (DSP register DIR).
SAMPLE_DIRECTORY = 0x6D00
NUM_SAMPLES = 64


def rom_offset(addr):
    """The offset in the ROM of a LoROM address."""
    return ((addr >> 16 & 0x7f) << 15 | addr & 0x7fff) % SNES_ROM_SIZE


def upload_blocks(rom, addr):
    """The blocks an upload writes: (APU address, length, offset in the ROM).

    An upload is a list of blocks (length, address, the bytes), ending with
    a length of 0. It reads the ROM in order, which in LoROM is in order in
    the file too.
    """
    o = rom_offset(addr)
    blocks = []
    while True:
        length, dest = struct.unpack_from('<HH', rom, o)
        if not length:
            return blocks
        blocks.append((dest, length, o + 4))
        o += 4 + length


def decode_brr(rom, o):
    """Decodes a BRR sample from the ROM at o, as src/snes/dsp.c does.

    Returns the samples (15-bit) and whether it loops.
    """
    out = []
    old = older = 0
    while o + 9 <= len(rom) and len(out) < 0x20000:
        header = rom[o]
        shift, filt = header >> 4, header >> 2 & 3
        for i in range(16):
            byte = rom[o + 1 + i // 2]
            s = byte >> 4 if i % 2 == 0 else byte & 0xf
            if s > 7:
                s -= 16
            s = (s << shift) >> 1 if shift <= 12 else (s >> 3) << 12
            if filt == 1:
                s += old + (-old >> 4)
            elif filt == 2:
                s += 2 * old + ((3 * -old) >> 5) - older + (older >> 4)
            elif filt == 3:
                s += 2 * old + ((13 * -old) >> 6) - older + ((3 * older) >> 4)
            s = max(-0x8000, min(0x7fff, s))
            s = ((s & 0x7fff) << 1)
            s = (s - 0x10000 if s & 0x8000 else s) >> 1
            older, old = old, s
            out.append(s)
        o += 9
        if header & 1:
            return out, bool(header & 2)
    return out, False


def audio_samples(rom):
    """The samples the game's uploads put in the sample directory, decoded.

    Returns {offset of the BRR in the ROM: (PCM bytes, loops)}.
    """
    pointers = rom_offset(MUSIC_POINTERS)
    banks = [int.from_bytes(rom[pointers + 3 * i:pointers + 3 * i + 3], 'little')
             for i in range(NUM_MUSIC_BANKS)]
    base = upload_blocks(rom, banks[0])
    samples = {}
    for bank in banks:
        blocks = base + (upload_blocks(rom, bank) if bank != banks[0] else [])

        def where(apu_addr):
            for dest, length, o in reversed(blocks):
                if dest <= apu_addr < dest + length:
                    return o + apu_addr - dest
            return None

        for i in range(NUM_SAMPLES):
            entry = where(SAMPLE_DIRECTORY + 4 * i)
            if entry is None:
                continue
            start = struct.unpack_from('<H', rom, entry)[0]
            o = where(start)
            if o is None or o in samples:
                continue
            pcm, loops = decode_brr(rom, o)
            samples[o] = (bytes((s >> 7) & 0xff for s in pcm), loops)
    return samples


def audio_section(rom):
    """The audio samples, as gba/cart_data.h's AudioSample table and PCM."""
    samples = audio_samples(rom)
    table = struct.pack('<I', len(samples))
    pcm = b''
    pcm_start = 4 + 16 * len(samples)
    for o in sorted(samples):
        data, loops = samples[o]
        table += struct.pack('<IIII', o, pcm_start + len(pcm), len(data), loops)
        pcm += data
        pcm += bytes(-len(pcm) % 4)
    return table + pcm


def squash_hash(data, bpp):
    """gba/cart_data.h's SquashHash of a tile's bytes."""
    h = bpp
    for w in struct.unpack('<%dH' % (bpp * 4), data[:bpp * 8]):
        h = ((h ^ w) * 0x9E3779B1) & 0xffffffff
    return h


def squash_section(rom):
    """The redrawn font tiles, as gba/cart_data.h's SquashHeader describes."""
    tiles = squash_font.tuned_tiles(rom, rom_offset)
    bits = 4
    while (1 << bits) < 4 * len(tiles):
        bits += 1
    slots = [0] * (1 << bits)
    entries = b''
    for n, (bpp, src, dst) in enumerate(tiles):
        i = squash_hash(src, bpp) >> (32 - bits)
        while slots[i]:
            i = (i + 1) & ((1 << bits) - 1)
        slots[i] = n + 1
        entries += struct.pack('<I16H', bpp, *struct.unpack('<%dH' % (bpp * 4), src),
                               *[0] * (16 - bpp * 4)) + dst
    return (struct.pack('<II', len(tiles), bits) + struct.pack('<%dH' % len(slots), *slots) +
            entries)


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
    audio = audio_section(rom)
    audio_offset = HEADER_SIZE + len(rom)
    squash = squash_section(rom)
    squash_offset = audio_offset + len(audio)
    header = MAGIC + struct.pack('<IIIIII', VERSION, len(rom), audio_offset, len(audio),
                                 squash_offset, len(squash))
    header += bytes(HEADER_SIZE - len(header))
    with open(out_path, 'wb') as f:
        f.write(engine + header + rom + audio + squash)


if __name__ == '__main__':
    main()
