// The game's data, which tools/make_cartridge.py appends to the engine's
// image (sm-engine.gba) from the player's own copy of the SNES ROM: the ROM
// can't be distributed, so neither can a cartridge with it built in.
//
// The data starts at the end of the image (gba.ld's __cart_data), with this
// header, then the ROM. Keep this in step with make_cartridge.py.
#pragma once

#include <stdint.h>

#define CART_DATA_MAGIC "SMGBA\0\0\0"
enum {
  kCartDataVersion = 1,
  kCartDataHeaderSize = 256,
  kSnesRomSize = 0x300000,
};

typedef struct CartData {
  char magic[8];
  uint32_t version;
  uint32_t snes_rom_size;
} CartData;

extern const CartData __cart_data;
