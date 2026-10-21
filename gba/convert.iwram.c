// Converting SNES graphics to the GBA's formats, in ARM code in IWRAM, as
// it's done for every tile the game changes. (No division in here: the
// compiler-rt helpers are Thumb, see cmake/compiler-rt.cmake.)
// This file's functions (and the headers' inline ones it emits) are in
// IWRAM, as ARM (see CMakeLists.txt), but for those ROM_CODE puts in ROM.
#pragma clang section text = ".iwram.text"

#include "convert.h"

#include "gba_hw.h"

// Spreads a byte's bits a nibble apart, the leftmost pixel (bit 7) into
// the low nibble: a bitplane of a row of SNES pixels, as GBA 4bpp pixels.
static uint32_t expand[256] IWRAM_BSS;

ROM_CODE void Convert_Init(void) {
  for (int b = 0; b < 256; b++) {
    uint32_t v = 0;
    for (int k = 0; k < 8; k++) {
      if (b & (0x80 >> k))
        v |= 1u << (4 * k);
    }
    expand[b] = v;
  }
}

// A SNES 4bpp tile is 16 words: rows of bitplanes 0 and 1, then rows of
// bitplanes 2 and 3.
__attribute__((noinline)) void Convert_Tiles4bpp(volatile uint32_t *dst, const uint16_t *src, int count) {
  for (; count > 0; count--, src += 16, dst += 8) {
    for (int r = 0; r < 8; r++) {
      uint32_t p01 = src[r], p23 = src[r + 8];
      dst[r] = expand[p01 & 0xff] | expand[p01 >> 8] << 1 | expand[p23 & 0xff] << 2 |
               expand[p23 >> 8] << 3;
    }
  }
}

// A SNES 2bpp tile is 8 words: rows of bitplanes 0 and 1.
ROM_CODE void Convert_Tiles2bpp(volatile uint32_t *dst, const uint16_t *src, int count) {
  for (; count > 0; count--, src += 8, dst += 8) {
    for (int r = 0; r < 8; r++) {
      uint32_t p01 = src[r];
      dst[r] = expand[p01 & 0xff] | expand[p01 >> 8] << 1;
    }
  }
}

// SNES map entries are vhopppcc cccccccc (flips, priority, palette, tile);
// the GBA's are ppppvhcc cccccccc.
static inline uint32_t MapEntry(uint32_t e, uint32_t pal_add) {
  return (e & 0x3ff) | ((e >> 4) & 0xc00) | (((e & 0x1c00) << 2) + pal_add);
}

// Returns how many of the entries have the priority bit.
__attribute__((noinline)) int Convert_Map(volatile uint16_t *dst, const uint16_t *src, int count, uint32_t pal_add) {
  uint32_t hi = 0;
  for (int i = 0; i < count; i++) {
    dst[i] = MapEntry(src[i], pal_add);
    hi += src[i] & 0x2000;
  }
  return hi >> 13;
}

// Splits a map in two by the priority bit: tiles in front of the
// priority 2 sprites, and those behind, with |blank| (a transparent tile)
// in the other. Returns how many are in front.
__attribute__((noinline)) int Convert_MapSplit(volatile uint16_t *hi, volatile uint16_t *lo, const uint16_t *src,
                     int count, uint32_t blank) {
  int n = 0;
  for (int i = 0; i < count; i++) {
    uint32_t e = src[i];
    uint32_t g = MapEntry(e, 0);
    if (e & 0x2000) {
      hi[i] = g;
      lo[i] = blank;
      n++;
    } else {
      hi[i] = blank;
      lo[i] = g;
    }
  }
  return n;
}
