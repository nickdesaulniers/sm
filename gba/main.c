// The GBA cartridge's main loop: a frame of the game, then its picture.
#include <stdio.h>

#include "audio.h"
#include "convert.h"
#include "gba_hw.h"
#include "input.h"
#include "render.h"
#include "snes_hw.h"
#include "sm_rtl.h"
#include "variables.h"

void RtlInit(void);
void RunOneFrameOfGame(void);
void IrqEntry(void);
extern volatile uint32_t g_vblank_count;
extern uint32_t g_audio_underruns;

static void VBlankIntrWait(void) {
  __asm__ volatile("swi 0x05" ::: "r0", "r1", "r2", "r3", "memory");
}

// The stack's low-water mark, to see that gba.ld reserves enough: the stack
// is painted at the start, and the paint left shows how deep it went.
extern char __sp_sys[], __stack_size[];
static void PaintStack(void) {
  volatile uint32_t *p = (volatile uint32_t *)(__sp_sys - (uintptr_t)__stack_size);
  uint32_t here = (uintptr_t)&p - 64;
  for (; (uintptr_t)p < here; p++)
    *p = 0xDEADBEEF;
}
static unsigned StackUsed(void) {
  const uint32_t *p = (const uint32_t *)(__sp_sys - (uintptr_t)__stack_size);
  while (*p == 0xDEADBEEF)
    p++;
  return (uintptr_t)__sp_sys - (uintptr_t)p;
}

void gba_main(void) {
  PaintStack();
  // Cartridge ROM: 3/1 wait states, prefetch. SRAM: 8.
  REG_WAITCNT = 0x4317;
  REG_TMCNT_H(3) = 0x84;  // cascade, enabled
  REG_TMCNT_H(2) = 0x80;

  RtlInit();
  SnesHw_Reset();
  Convert_Init();
  Render_Init();
  Audio_Init();

  REG_IRQ_VECTOR = (uintptr_t)IrqEntry;
  REG_IE = kIrqVBlank | kIrqVCount | kIrqTimer1;
  REG_IME = 1;

  printf("sm: starting\n");
  coroutine_state_0 = 1;
  bug_fix_counter = 1;

  // Per 60 frames: CPU cycles per frame of the game, its sound (per frame of
  // the GBA's), and render.c's work on its picture (not counting the wait
  // for VBlank); and how often the sound ran out.
  uint32_t frames = 0, game_cycles = 0, audio_cycles = 0, render_cycles = 0;
  uint32_t vblanks = g_vblank_count, audio_start = g_audio_cycles;
  for (;;) {
    g_snes_hw.joy1 = Input_ReadJoypad();
    uint16_t state = game_state;
#if SM_SKIP_INTRO
    // For testing: a new game's intro goes straight to its end, which fades
    // out and starts the game at Ceres, or with SM_START_ZEBES, as after
    // escaping Ceres, on Zebes.
    static uint16_t intro_frames;
    if (game_state == 30 && ++intro_frames == 60)
      cinematic_function = (uint16_t)0x8BC0C5;  // CinematicFunction_Intro_Func72
#if SM_START_ZEBES
    if (game_state == 31 && loading_game_state == 0x1f)
      loading_game_state = 0x22;  // kLoadingGameState_22_EscapingCeres
#endif
#endif
#if SM_TEST_ITEMS
    // For testing the windows, which the X-ray scope and power bombs use.
    if (game_state == 8 && !(collected_items & 0x8000)) {
      collected_items |= 0xc004;
      equipped_items |= 0xc004;
      samus_power_bombs = samus_max_power_bombs = 10;
    }
#endif
    // (Less the sound's, which interrupts them: see Audio_Background.)
    uint32_t t0 = Cycles(), a0 = g_audio_cycles;
    RunOneFrameOfGame();
    if (game_state != state)
      printf("sm: game state %u\n", (unsigned)game_state);
    uint32_t t1 = Cycles(), a1 = g_audio_cycles;
    Render_CaptureFrame();
    uint32_t t2 = Cycles(), a2 = g_audio_cycles;
    VBlankIntrWait();
    uint32_t t3 = Cycles(), a3 = g_audio_cycles;
    Render_Present();
    uint32_t t4 = Cycles(), a4 = g_audio_cycles;
    Audio_Kick();
    game_cycles += (t1 - t0) - (a1 - a0);
    render_cycles += (t2 - t1) - (a2 - a1) + (t4 - t3) - (a4 - a3);
    if (++frames == 60) {
      uint32_t v = g_vblank_count, audio = g_audio_cycles;
      audio_cycles = (audio - audio_start) / (v - vblanks);
      audio_start = audio;
      printf("sm: 60 frames in %u vblanks; cycles/frame: game %u, audio %u, render %u; "
             "state %u; stack %u; underruns %u\n",
             (unsigned)(v - vblanks), (unsigned)(game_cycles / 60), (unsigned)audio_cycles,
             (unsigned)(render_cycles / 60), (unsigned)game_state, StackUsed(), (unsigned)g_audio_underruns);
      vblanks = v;
      frames = game_cycles = audio_cycles = render_cycles = 0;
    }
  }
}
