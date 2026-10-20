// The game's data, which tools/make_cartridge.py appends to the engine's
// image (sm-engine.gba) from the player's own copy of the SNES ROM: the ROM
// can't be distributed, so neither can a cartridge with it built in.
//
// The data starts at the end of the image (gba.ld's __cart_data), with this
// header, then the ROM, then the audio samples, decoded. Keep this in step
// with make_cartridge.py.
#pragma once

#include <stdint.h>

#define CART_DATA_MAGIC "SMGBA\0\0\0"
enum {
  kCartDataVersion = 2,
  kCartDataHeaderSize = 256,
  kSnesRomSize = 0x300000,
};

typedef struct CartData {
  char magic[8];
  uint32_t version;
  uint32_t snes_rom_size;
  // From the start of the header: a uint32_t count of AudioSamples, them,
  // then their PCM.
  uint32_t audio_offset, audio_size;
} CartData;

// A sample the game's uploads to the APU put in the sample directory, as
// 8-bit PCM. In order of where its BRR is in the ROM.
typedef struct AudioSample {
  uint32_t rom_offset;  // of its BRR
  uint32_t pcm_offset;  // from the start of the audio
  uint32_t length;
  uint32_t loops;
} AudioSample;

extern const CartData __cart_data;
