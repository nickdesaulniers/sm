// The GBA cartridge's main loop: a frame of the game, then its picture.
#include <stdio.h>

#include "convert.h"
#include "gba_hw.h"
#include "render.h"
#include "snes_hw.h"
#include "sm_rtl.h"
#include "variables.h"

void RtlInit(void);
void RunOneFrameOfGame(void);
void IrqHandler(void);
extern volatile uint32_t g_vblank_count;

static void VBlankIntrWait(void) {
  __asm__ volatile("swi 0x05" ::: "r0", "r1", "r2", "r3", "memory");
}

enum {
  kSnesB = 0x8000, kSnesY = 0x4000, kSnesSelect = 0x2000, kSnesStart = 0x1000,
  kSnesUp = 0x0800, kSnesDown = 0x0400, kSnesLeft = 0x0200, kSnesRight = 0x0100,
  kSnesA = 0x0080, kSnesX = 0x0040, kSnesL = 0x0020, kSnesR = 0x0010,
};

// The GBA's buttons as the SNES's joypad (JOY1H << 8 | JOY1L). The GBA has
// no X and Y, so in gameplay, with the game's default controls, much as in
// Metroid: Zero Mission:
//   A       jump (SNES A)
//   B       shoot (SNES X)
//   R       aim diagonally: up, or down with Down held (SNES R, L)
//   L       switch item (SNES Select)
//   Select  cancel item (SNES Y)
//   Start   pause
// and Samus always runs (SNES B held). Elsewhere, menus and the pause
// screen included, the buttons are the SNES's buttons of the same names.
static uint16_t ReadJoypad(void) {
  uint16_t keys = ~REG_KEYINPUT & 0x3ff;
  uint16_t joy = 0;
  if (keys & kKeyUp) joy |= kSnesUp;
  if (keys & kKeyDown) joy |= kSnesDown;
  if (keys & kKeyLeft) joy |= kSnesLeft;
  if (keys & kKeyRight) joy |= kSnesRight;
  if (keys & kKeyStart) joy |= kSnesStart;
  if (keys & kKeyA) joy |= kSnesA;
  bool gameplay = game_state >= 7 && game_state <= 11;
  if (!gameplay) {
    if (keys & kKeyB) joy |= kSnesB;
    if (keys & kKeySelect) joy |= kSnesSelect;
    if (keys & kKeyL) joy |= kSnesL;
    if (keys & kKeyR) joy |= kSnesR;
    return joy;
  }
  joy |= kSnesB;
  if (keys & kKeyB) joy |= kSnesX;
  if (keys & kKeyL) joy |= kSnesSelect;
  if (keys & kKeySelect) joy |= kSnesY;
  if (keys & kKeyR) {
    if (keys & kKeyDown)
      joy = (joy & ~kSnesDown) | kSnesL;
    else
      joy = (joy & ~kSnesUp) | kSnesR;
  }
  return joy;
}

void gba_main(void) {
  // Cartridge ROM: 3/1 wait states, prefetch. SRAM: 8.
  REG_WAITCNT = 0x4317;
  REG_TMCNT_H(3) = 0x84;  // cascade, enabled
  REG_TMCNT_H(2) = 0x80;

  RtlInit();
  SnesHw_Reset();
  Convert_Init();
  Render_Init();

  REG_IRQ_VECTOR = (uintptr_t)IrqHandler;
  REG_IE = kIrqVBlank;
  REG_IME = 1;

  printf("sm: starting\n");
  coroutine_state_0 = 1;
  bug_fix_counter = 1;

  // Per 60 frames: CPU cycles per frame of the game, and of render.c's
  // work on it (not counting the wait for VBlank).
  uint32_t frames = 0, game_cycles = 0, render_cycles = 0;
  uint32_t vblanks = g_vblank_count;
  for (;;) {
    g_snes_hw.joy1 = ReadJoypad();
    uint32_t t0 = Cycles();
    uint16_t state = game_state;
    RunOneFrameOfGame();
    if (game_state != state)
      printf("sm: game state %u\n", (unsigned)game_state);
    uint32_t t1 = Cycles();
    Render_CaptureFrame();
    uint32_t t2 = Cycles();
    VBlankIntrWait();
    uint32_t t3 = Cycles();
    Render_Present();
    uint32_t t4 = Cycles();
    game_cycles += t1 - t0;
    render_cycles += (t2 - t1) + (t4 - t3);
    if (++frames == 60) {
      uint32_t v = g_vblank_count;
      printf("sm: 60 frames in %u vblanks; game %u, render %u cycles/frame; state %u\n",
             (unsigned)(v - vblanks), (unsigned)(game_cycles / 60), (unsigned)(render_cycles / 60),
             (unsigned)game_state);
      vblanks = v;
      frames = game_cycles = render_cycles = 0;
    }
  }
}
