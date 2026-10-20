// What the game expects of its runtime (sm_rtl.h), for the GBA: in place
// of src/sm_rtl.c and src/sm_cpu_infra.c, which also run the SNES
// emulator the PC build checks the game against.
#include <stdio.h>
#include <string.h>

#include "gba_hw.h"
#include "sm_rtl.h"
#include "types.h"
#include "variables.h"

uint8 g_ram[0x20000] RAM_ALIGNED;
static uint8 sram[0x2000];
uint8 *g_sram = sram;
const uint8 *g_rom;
bool g_use_my_apu_code = true;
bool g_debug_flag;
int snes_frame_counter;

extern const uint8 snes_rom[];

void RtlInit(void) {
  g_rom = snes_rom;
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

const uint8 *RomPtr(uint32_t addr) {
  if (!(addr & 0x8000))
    printf("RomPtr: invalid access 0x%x\n", (unsigned)addr);
  return RomFixedPtr(addr);
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

uint16 SnesDivide(uint16 a, uint8 b) {
  return (b == 0) ? 0xffff : a / b;
}

uint16 SnesModulus(uint16 a, uint8 b) {
  return (b == 0) ? a : a % b;
}

// No sound yet.
void RtlApuWrite(uint32 adr, uint8 val) {
  (void)adr, (void)val;
}

void RtlApuUpload(const uint8 *p) {
  (void)p;
}

// The GBA's SRAM is 8 bits wide: a byte at a time.
void RtlWriteSram(void) {
  for (int i = 0; i < 0x2000; i++)
    GBA_SRAM[i] = sram[i];
}
