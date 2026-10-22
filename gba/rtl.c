// What the game expects of its runtime (sm_rtl.h), for the GBA: in place
// of src/sm_rtl.c and src/sm_cpu_infra.c, which also run the SNES
// emulator the PC build checks the game against.
#include <stdio.h>
#include <string.h>

#include "cart_data.h"
#include "gba_hw.h"
#include "sm_rtl.h"
#include "types.h"
#include "variables.h"

uint8 g_ram[0x20000] RAM_ALIGNED;
static uint8 sram[0x2000];
uint8 *g_sram = sram;
bool g_use_my_apu_code = true;
bool g_debug_flag;
int snes_frame_counter;

// No game data appended to the cartridge (see cart_data.h): there's nothing
// to run. A red screen, and why in mGBA's log.
static void MissingCartData(void) {
  printf("sm: no game data: make the cartridge with tools/make_cartridge.py\n");
  REG_DISPCNT = 0;
  GBA_BG_PALETTE[0] = 0x001f;
  for (;;) {}
}

void RtlInit(void) {
  if (memcmp(__cart_data.magic, CART_DATA_MAGIC, 8) || __cart_data.version != kCartDataVersion ||
      __cart_data.snes_rom_size != kSnesRomSize)
    MissingCartData();
  for (int i = 0; i < 0x2000; i++)
    sram[i] = GBA_SRAM[i];
}

void Die(const char *error) {
  printf("Die: %s\n", error);
  for (;;) {}
}

void Warning(const char *error) {
  printf("Warning: %s\n", error);
}

bool Unreachable(void) {
  printf("Unreachable!\n");
  return false;
}

void Call(uint32 addr) {
  printf("Call(0x%x): not decompiled\n", (unsigned)addr);
}

void DebugGameOverMenu(void) {
  printf("DebugGameOverMenu: not decompiled\n");
}

void MemCpy(void *dst, const void *src, int size) {
  memcpy(dst, src, size);
}

PairU16 MakePairU16(uint16 k, uint16 j) {
  PairU16 r = {k, j};
  return r;
}

void mov24(LongPtr *a, uint32 d) {
  a->addr = d & 0xffff;
  a->bank = d >> 16;
}

uint32 Load24(const LongPtr *src) {
  return src->addr | src->bank << 16;
}

uint16 Mult8x8(uint8 a, uint8 b) {
  return a * b;
}

// The SNES's divider, by multiplying: a / b is the top word of a times
// 2^32 / b rounded up, for any 16-bit a and b from 2 to 255 (it's less than
// 2^-16 over, and a / b's fraction is at most 1 - 1/b: checked for all of
// them). The fades between rooms divide some 1500 times a frame, which the
// ARM7TDMI does in software, slowly; this is a umull, in ARM. (The table is
// worked out by the compiler, in ROM.)
#define R(b) ((b) < 2 ? 0 : 0xffffffffu / (b) + 1)
#define R4(b) R(b), R((b) + 1), R((b) + 2), R((b) + 3)
#define R16(b) R4(b), R4((b) + 4), R4((b) + 8), R4((b) + 12)
#define R64(b) R16(b), R16((b) + 16), R16((b) + 32), R16((b) + 48)
static const uint32_t reciprocals[256] = {R64(0), R64(64), R64(128), R64(192)};
#undef R
#undef R4
#undef R16
#undef R64

GBA_IWRAM_CODE uint16 SnesDivide(uint16 a, uint8 b) {
  if (b < 2)
    return b ? a : 0xffff;
  return (uint64_t)a * reciprocals[b] >> 32;
}

GBA_IWRAM_CODE uint16 SnesModulus(uint16 a, uint8 b) {
  if (b < 2)
    return b ? 0 : a;
  return a - (uint16)((uint64_t)a * reciprocals[b] >> 32) * b;
}

// ClearUnusedOam (src/sm_80.c) hides the sprites a frame didn't use, by their
// Y: a byte of each four of the OAM's copy, in EWRAM, for up to 128 sprites,
// and a frame calls it. Unrolled, for the cartridge's prefetch to keep ahead
// of the stores (which take most of the time): the linker calls this in its
// place, from the game's other files (--wrap in CMakeLists.txt).
void __wrap_ClearUnusedOam(void) {
  uint32_t i = oam_next_ptr >> 2;
  uint8 *y = &g_ram[0x371 + i * 4];
#pragma clang loop unroll_count(8)
  for (; i < 0x80; i++, y += 4)
    *y = 0xf0;
  oam_next_ptr = 0;
}

// The GBA's SRAM is 8 bits wide: a byte at a time.
void RtlWriteSram(void) {
  for (int i = 0; i < 0x2000; i++)
    GBA_SRAM[i] = sram[i];
}
