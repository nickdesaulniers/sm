// The GBA cartridge's main loop: a frame of the game, then its picture.
#include <stdio.h>

#include "convert.h"
#include "gba_hw.h"
#include "input.h"
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
    g_snes_hw.joy1 = Input_ReadJoypad();
    uint32_t t0 = Cycles();
    uint16_t state = game_state;
#if SM_SKIP_INTRO
    // For testing: a new game's intro goes straight to its end, which fades
    // out and starts the game at Ceres.
    static uint16_t intro_frames;
    if (game_state == 30 && ++intro_frames == 60)
      cinematic_function = (uint16_t)0x8BC0C5;  // CinematicFunction_Intro_Func72
#endif
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
