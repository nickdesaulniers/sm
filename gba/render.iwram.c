// See render.h.
//
// ARM code in IWRAM, as it runs every frame. (So no division: see
// cmake/compiler-rt.cmake.)
//
// The SNES's Mode 1 maps onto the GBA's Mode 0 like this, by layer:
//   GBA BG0: SNES BG3 (2bpp, the HUD and effects), priority 0 or 3
//   GBA BG1: SNES BG1's priority tiles, priority 1
//   GBA BG2: SNES BG1's other tiles, priority 2
//   GBA BG3: SNES BG2, priority 2
// and SNES sprites at priority 3, 2, 1, 0 become GBA priority 1, 2, 3, 3.
// The GBA draws a sprite in front of a background of the same priority, so
// this keeps SNES Mode 1's order (BG2's priority tiles aside). In a frame
// where BG2 shows more priority tiles than BG1 (split_bg2), it's BG2 that's
// split, and BG1's priority tiles go behind sprites of priority 2:
//   GBA BG1: SNES BG2's priority tiles, priority 1
//   GBA BG2: SNES BG1, priority 2
//   GBA BG3: SNES BG2's other tiles, priority 2
// In a frame that adds BG3 to BG2 but not BG1 (bg3_mid), BG1's layers and
// sprites go a priority higher (0 and 1; sprites 0-3), for BG3 to go
// between BG1 and BG2.
//
// GBA VRAM:
//   0x0000-0x7FFF  BG1/BG2 tiles (the SNES's 1024 at BG1's base)
//   0x8000-0xAFFF  BG3 tiles, converted to 4bpp (the first 384)
//   0xB000-0xBFFF  BG3 map, slot 0 (screen blocks 22-23)
//   0xC000-0xCFFF  BG1 priority tiles' map (24-25)
//   0xD000-0xDFFF  BG1 other tiles' map (26-27)
//   0xE000-0xEFFF  BG2 map (28-29)
//                  (with split_bg2: GBA BG1's, BG2's and BG3's, as above)
//   0xF000-0xFFFF  BG3 map, slot 1 (30-31): the HUD and the effects layer
//                  are different maps.
// In Mode 7, the SNES's one background is the GBA's affine BG2 in Mode 1,
// tiles at 0x0000 and its 128x128 map at 0x4000, in place of BG1 and BG2.
// A frame can have both: Ceres has Mode 7 under a Mode 1 HUD, which is BG3
// on GBA BG0 as usual.
// Sprite tiles keep the SNES's layout of 16 to a row, in the left half of
// the GBA's rows of 32 (2D mapping), with a copy in the right half, so that
// sprites whose tiles wrap around a row come out right.
#include "render.h"

#include <stdio.h>
#include <string.h>

#include "convert.h"
#include "display.h"
#include "gba_hw.h"
#include "snes_hw.h"
#include "sm_rtl.h"
#include "variables.h"

enum {
  // Lines at the top of the SNES screen with the HUD, the first 8 of
  // them blank, shown on the GBA as they are (but for those); the rest of
  // the GBA screen shows part of the SNES's gameplay area, following Samus.
  kHudLines = 32,
  kHudBlankLines = 8,
  kGbaHudLines = kHudLines - kHudBlankLines,
  kMaxYOffset = kSnesLines - kHudBlankLines - kGbaLines,
  // The SNES's 256 pixel lines lose 8 pixels at each end.
  kXOffset = 8,

  kBg3Tiles = 384,
  kSbBg3Slot0 = 22,
  kSbBg1Hi = 24,
  kSbBg1Lo = 26,
  kSbBg2 = 28,
  kSbBg3Slot1 = 30,
  kSbMode7 = 8,
};

#define VRAM_SB(n) (GBA_VRAM + (n) * 0x400)

// A line's GBA registers, before they're split into display.h's stream
// and segments.
typedef struct LineRegs {
  DisplaySegment ctl;  // but for first_line
  uint32_t stream[4];
} LineRegs;

// The PPU's registers for each SNES line: line_states[line_state[line]],
// with a state for each time they change down the screen.
static PpuRegs line_states[kSnesLines] __attribute__((aligned(4)));
static uint8_t line_state[kSnesLines];
static uint32_t line_wh[kSnesLines];  // WH0-WH3, by line
// BGnHOFS | BGnVOFS << 16, by line, in place of the states'.
static uint32_t line_ofs[kSnesLines][4];
#define LINE_REGS(s) (&line_states[line_state[s]])
#define HOFS(s, bg) (line_ofs[s][bg] & 0xffff)
#define VOFS(s, bg) (line_ofs[s][bg] >> 16)
// Two of each, for the frame on show and the next. (One of the streams in
// EWRAM, for room in IWRAM: HBlank DMA reads only 16 bytes a line.)
static uint32_t stream0[kGbaLines + 1][4] IWRAM_BSS, stream1[kGbaLines + 1][4];
static uint32_t (*const streams[2])[4] = {stream0, stream1};
// The streams of frames with windows (see display.h), and their windows
// line by line, for them.
static uint32_t window_streams[2][kGbaLines + 1][kWindowStreamWords];
static uint32_t line_windows[kGbaLines];
// Segments: a frame has a few, as a rule (each costs an interrupt). Past
// the most, the last goes on to the end.
enum { kMaxSegments = 32 };
static DisplaySegment segments[2][kMaxSegments];
static DisplayFrame frames[2];
static int back;

static bool hud;
// Outside gameplay (the title, menus, the pause screen), the SNES screen
// squashed into the GBA's: 6 of each 8 lines (the SNES's 224 lines make
// 168, less 4 at the top and bottom; see SetSquashLines). kSquashLine[g]
// is the SNES line on GBA line g, and sprites are scaled to match.
enum { kSquashTop = 4 };
static bool squash;
static uint8_t kSquashLine[kGbaLines];
// Whether lines on show use Mode 7, and one that uses another mode, or -1.
static bool frame_mode7;
static int text_line;
static int y_offset = kMaxYOffset / 2;
// SNES BG3 maps in GBA map slots 0 and 1, by BG3SC.
static uint8_t bg3_slot_sc[2] = {0xff, 0xff};
// How many of each slot's entries, by chunk of 16, have the priority bit.
// The GBA has no background to spare for BG3's priority tiles (in front of
// everything, with BGMODE's bit 3) and the others (at the back): lines
// whose rows have any go in front (Bg3Front).
static uint8_t bg3_hi[2][128];
static int bg3_hi_total[2];
// Likewise for BG1's and BG2's maps (by screen, 64 chunks each), as last
// converted; and whether to split BG2, not BG1, into the GBA's BG1 and BG3
// (see the top): Ceres's Ridley room has its walls and door in BG2's
// priority tiles, in front of a BG1 of all one tile (but for the escape's
// timer).
static uint8_t bg12_hi[2][128];
// Their sums by column of chunks: by screen, the left and right halves.
static uint16_t bg12_hi_cols[2][4];
static bool split_bg2;
// The GBA layers of BG1 and BG2, as split_bg2 has them.
static uint8_t bg12_layers[2] = {6, 8};
// Whether the frame adds BG3 (on the sub screen) to BG2 but not BG1, as in
// some of Brinstar's and the Wrecked Ship's rooms: then BG3 goes between
// them, BG1 a priority higher and sprites too (see ColorMath).
static bool bg3_mid;
// With the sub screen subtracted, how much the main screen keeps where the
// sub screen's layers are, in 16ths (see SubtractWeight).
static int subtract_weight;
static uint8_t subtract_layer = 4;
// Or, with bit 15 set, a color the palette subtracts, as it does the fixed
// color: the sub screen's average, when it's all over the screen; or when
// it's BG3 all over the screen (Maridia's shadows), the brightest of its
// colors, with the BG3 palettes it uses in subtract_palettes. Then BG3 goes
// on top with each of its colors c as that color less c, added: the SNES's
// subtraction, but for main screen colors darker than that color. (See
// SubtractWeight.)
static uint16_t subtract_color;
static uint8_t subtract_palettes;
// The palette's colors for that in the gameplay area (see DisplayFrame).
static int num_patches;
static uint8_t patch_index[24];
static uint16_t patch_color[24], restore_color[24];

// What render.c last converted, to see when it has to start over.
static struct {
  uint16_t bg12_base, bg3_base, obj_base[2];
  uint8_t bgsc[2];
  uint8_t bg_row[2];  // of 64x64 maps, the row of screens converted
  uint8_t bg3_slot_sc[2];
  int blank_tile;
  int brightness;
  uint32_t fixed_math;
  bool mode7, bg3_palettes, split_bg2, squashed;
  uint8_t subtract_palettes;
} conv = {.blank_tile = -1, .brightness = -1};

static inline __attribute__((always_inline)) int SnesLine(int g) {
  if (squash)
    return kSquashLine[g];
  if (!hud)
    return g + y_offset;
  return g < kGbaHudLines ? g + kHudBlankLines : g + kHudBlankLines + y_offset;
}

static int Luma(uint16_t color);

// The lines that go, squashed: those that show rows 0 and 4 of the text's
// layer's tiles (BG3's where it's on the main screen, or else BG1's), by
// each line's registers and scrolling, so that they move with the text.
// They're merged into rows 1 and 3 as the tiles are converted
// (Convert_Tiles4bppSquashed), and text loses none of its strokes. And
// kSquashTop of the rest off the top. Changes sprites' places, which go
// with the lines.
ROM_CODE static void SetSquashLines(void) {
  // Unless which of each 4 lines go changed, at a line of each 8, as is
  // usual, it's as it was.
  static uint32_t sampled[2];
  uint32_t now[2] = {0, 0};
  for (int s = 4; s < kSnesLines; s += 8) {
    int bg = line_states[line_state[s]].tm & 4 ? 2 : 0;
    now[s >> 7] = now[s >> 7] << 2 | ((s + 1 + (line_ofs[s][bg] >> 16)) & 3);
  }
  if (now[0] == sampled[0] && now[1] == sampled[1])
    return;
  sampled[0] = now[0], sampled[1] = now[1];
  bool changed = false;
  int state = -1, bg = 0;
  for (int s = 0, g = -kSquashTop; g < kGbaLines && s < kSnesLines; s++) {
    if (line_state[s] != state) {
      state = line_state[s];
      bg = line_states[state].tm & 4 ? 2 : 0;
    }
    // (Line s shows row (s + 1 + VOFS) & 7.)
    if (!((s + 1 + (line_ofs[s][bg] >> 16)) & 3))
      continue;
    if (g >= 0) {
      changed |= kSquashLine[g] != s;
      kSquashLine[g] = s;
    }
    g++;
  }
  if (changed)
    g_snes_hw.oam_dirty = true;
}

ROM_CODE static void SetSquashRanks(void) {
  // BG palette 0's colors by brightness, for merging rows: once a screen,
  // when its colors aren't dark (as they fade in; then they may change, as
  // the game animates them, but not their order, as a rule), and if it's
  // another order, the tiles again.
  static uint32_t rank_key;
  const PpuRegs *r = LINE_REGS(kSnesLines / 2);
  uint32_t key = r->tm | r->bgsc[0] << 8 | r->bgsc[1] << 16 | r->bgsc[2] << 24;
  if (key != rank_key) {
    static uint8_t rank[16];
    uint8_t luma[16], now[16];
    int most = 0;
    for (int c = 0; c < 16; c++) {
      luma[c] = Luma(g_cgram[c]);
      most = luma[c] > most ? luma[c] : most;
    }
    if (most >= 16) {
      rank_key = key;
      for (int c = 0; c < 16; c++) {
        now[c] = 0;
        for (int d = 0; d < 16; d++)
          now[c] += luma[d] < luma[c] || (luma[d] == luma[c] && d > c);
      }
      if (memcmp(now, rank, sizeof(rank))) {
        memcpy(rank, now, sizeof(rank));
        Convert_SetSquashRanks(rank);
        conv.bg12_base = conv.bg3_base = 0xffff;
      }
    }
  }
}

ROM_CODE void Render_Init(void) {
  for (int g = 0; g < kGbaLines; g++)
    kSquashLine[g] = g + kSquashTop;
  REG_DISPCNT = kDispcntForcedBlank;
  DmaFill32(GBA_VRAM, 0, 0x18000);
  for (int i = 0; i < 128; i++)
    GBA_OAM[i * 4] = 0x200;  // hidden
  REG_DISPSTAT = kDispstatVBlankIrq;
}

// Logs the SNES's screen layout when it changes, as what's supported grows.
ROM_CODE static void LogLayoutChange(const PpuRegs *r) {
  static uint32_t last[2];
  uint32_t now[2] = {
    r->bgmode | r->bgsc[0] << 8 | r->bgsc[1] << 16 | (uint32_t)r->bgsc[2] << 24,
    r->bgnba[0] | r->bgnba[1] << 8 | r->tm << 16 | (uint32_t)g_snes_hw.obsel << 24,
  };
  if (now[0] != last[0] || now[1] != last[1]) {
    printf("render: mode %02x bgsc %02x %02x %02x nba %02x %02x tm %02x obsel %02x\n", r->bgmode,
           r->bgsc[0], r->bgsc[1], r->bgsc[2], r->bgnba[0], r->bgnba[1], r->tm, g_snes_hw.obsel);
    last[0] = now[0];
    last[1] = now[1];
  }
}

static int Bg3Slot(uint8_t sc) {
  return sc == bg3_slot_sc[1] ? 1 : 0;
}

static uint16_t BgSize(uint8_t sc) {
  // 64x64 maps get half, the row of two screens on show (UpdateMaps12):
  // there are two screens per map.
  return (sc & 3) == 3 ? 1 : (sc & 3);
}

static int Sext13(int v) {
  return (int16_t)(v << 3) >> 3;
}

static void BuildLineMode7(LineRegs *l, const PpuRegs *r, int s) {
  uint16_t dispcnt = kDispcntMode1;
  if (!(r->inidisp & 0x80)) {
    if (r->tm & 1) dispcnt |= kDispcntBg0 << 2;
    if (r->tm & 16) dispcnt |= kDispcntObj;
  }
  l->ctl.dispcnt = dispcnt;
  l->ctl.bgcnt[0] = l->ctl.bgcnt[1] = l->ctl.bgcnt[3] = 0;
  // 1024x1024, wrapping around unless the SNES's field is "large".
  l->ctl.bgcnt[2] = 3 | kSbMode7 << 8 | 3 << 14 | (r->m7sel & 0x80 ? 0 : 1 << 13);
  l->ctl.bld[0] = l->ctl.bld[1] = l->ctl.bld[2] = 0;
  l->ctl.win[0] = l->ctl.win[1] = 0;
  for (int i = 0; i < 8; i++)
    l->ctl.ofs[i] = 0;
  // The SNES's screen pixel (sx, sy) shows the map's pixel
  //   ((a b) (c d)) (sx + hofs - x0, sy + vofs - y0) + (x0, y0),
  // and GBA pixel (gx, line) is SNES pixel (gx + kXOffset, s).
  int a = r->m7[0], b = r->m7[1], c = r->m7[2], d = r->m7[3];
  int x0 = r->m7[4], y0 = r->m7[5];
  int dx = kXOffset + Sext13(r->m7hofs) - x0;
  int dy = s + Sext13(r->m7vofs) - y0;
  l->stream[0] = (uint16_t)a | (uint32_t)(uint16_t)b << 16;
  l->stream[1] = (uint16_t)c | (uint32_t)(uint16_t)d << 16;
  l->stream[2] = a * dx + b * dy + (x0 << 8);
  l->stream[3] = c * dx + d * dy + (y0 << 8);
}

// The next line of a Mode 7 stream with the same registers: the reference
// point moves down the map by (b, d).
static void StepLineMode7(LineRegs *l) {
  l->stream[2] += (int16_t)(l->stream[0] >> 16);
  l->stream[3] += (int16_t)(l->stream[1] >> 16);
}

// The GBA layers (BLDCNT's bits) of the SNES's layers in |snes| (CGADSUB's
// or TM's bits: BG1-BG4, OBJ, backdrop).
static uint16_t BlendLayers(uint8_t snes) {
  return (snes & 1 ? bg12_layers[0] : 0) | (snes & 2 ? bg12_layers[1] : 0) | (snes & 4 ? 1 : 0) |
         (snes & 0x30);
}

// The SNES's color math, as the GBA's blending can do it, for a line with
// the tiled layers. In place of the SNES's sub screen added to the main
// screen, which the GBA can't do, its layers that aren't on the main
// screen show above the others, blended with the layer below them: the
// sum's the same. (The game's rooms add BG3 that way, for rain, fog, water
// and the like.) The fixed color, added or subtracted, is in the palette
// (UpdatePalette), the same for all the frame's lines. Without windows yet,
// color math "inside the window" is nowhere, and "outside", everywhere. The
// sub screen subtracted only darkens, by about what it takes away
// (SubtractWeight).
// Whether a layer is in the SNES's windows, as its |sel| (W12SEL's,
// W34SEL's or WOBJSEL's 4 bits for it) and |logic| (WBGLOG's or WOBJLOG's 2
// bits) have it, at a pixel in window 1 or not, and 2 or not.
ROM_CODE static bool InWindows(int sel, int logic, bool in1, bool in2) {
  bool on1 = sel & 2, on2 = sel & 8;
  bool w1 = in1 ^ (sel & 1), w2 = in2 ^ ((sel >> 2) & 1);
  if (!on1 || !on2)
    return on1 ? w1 : on2 ? w2 : false;
  switch (logic) {
  case 0: return w1 || w2;
  case 1: return w1 && w2;
  case 2: return w1 ^ w2;
  default: return !(w1 ^ w2);
  }
}

// Whether color math is off in a region (CGWSEL's bits 4-5, |region|), as
// the color window has it there.
ROM_CODE static bool NoMath(const PpuRegs *r, int region, bool in1, bool in2) {
  bool color = InWindows(r->wsel[2] >> 4, (r->wobjlog >> 2) & 3, in1, in2);
  return region == 3 || (region == 1 && !color) || (region == 2 && color);
}

// Whether the windows hide all the sub screen's layers in |windowed|
// somewhere color math's on: where the fixed color shows.
ROM_CODE static bool FixedShows(const PpuRegs *r, uint8_t windowed) {
  for (int k = 0; k < 3; k++) {
    bool in1 = k == 0, in2 = k == 1, hidden = true;
    for (int i = 0; i < 3; i++) {
      if (windowed & (1 << i))
        hidden &= InWindows((r->wsel[i >> 1] >> ((i & 1) * 4)) & 15, (r->wbglog >> (2 * i)) & 3,
                            in1, in2);
    }
    if (hidden && !NoMath(r, (r->cgwsel >> 4) & 3, in1, in2))
      return true;
  }
  return false;
}

// The sub screen's layers that the line |r|'s color math uses, as
// ColorMath has them. Subtracted, it's one of them, that SubtractWeight
// chose: the GBA can't have several on top, as they are on the SNES's sub
// screen, with nothing to blend between them (Tourian's darkness).
ROM_CODE static uint8_t SubLayers(const PpuRegs *r) {
  uint8_t sub = r->ts & ~r->tm & 7;
  return (r->cgadsub & 0x80) && (sub & subtract_layer) ? subtract_layer : sub;
}

// Returns the sub screen's layers it shows (CGADSUB's bits), or with
// kFixedInWindows, the ones that windows hide, for the fixed color there.
enum { kFixedInWindows = 0x80 };
ROM_CODE static uint8_t ColorMath(DisplaySegment *c, const PpuRegs *r) {
  c->bld[0] = c->bld[1] = c->bld[2] = 0;
  uint8_t math = r->cgadsub & 0x3f;
  // Color math only inside the color window, or not at all; with the
  // window, Windows has the GBA's windows do it.
  int region = (r->cgwsel >> 4) & 3;
  if (!math || (region == 1 && !(r->wsel[2] & 0xa0)) || region == 3 || frame_mode7 ||
      (r->inidisp & 0x80))
    return 0;
  bool subtract = r->cgadsub & 0x80, half = r->cgadsub & 0x40;
  if (r->cgwsel & 2) {
    uint8_t sub = SubLayers(r);
    // (Or the palette subtracts, as SubtractWeight chose.)
    if (subtract && (subtract_color & 0x8000) && !subtract_palettes)
      return 0;
    if (subtract && (subtract_color & 0x8000)) {
      // The palette subtracts the brightest of BG3's colors, and BG3 adds
      // back what's too much (see subtract_color).
      c->dispcnt |= kDispcntBg0;
      c->bgcnt[0] &= ~3;
      c->bld[0] = 1 | BlendLayers(math & (r->tm | 0x20)) << 8 | 1 << 6;
      c->bld[1] = 16 | 16 << 8;
      return 4;
    }
    if (!sub || (subtract && !(r->tm & 3)))
      return 0;
    // Where it's transparent, the sub screen's the fixed color. Where
    // windows hide its layers (a power bomb's, all but its explosion), it's
    // that: the main screen brightens, by the brightest of the color's
    // channels (the GBA can't make it a color), where Windows has the
    // GBA's windows do it. Its layers don't show, here, elsewhere.
    uint8_t windowed = 0;
    for (int i = 0; i < 3; i++) {
      if ((sub & r->tsw & (1 << i)) && ((r->wsel[i >> 1] >> ((i & 1) * 4)) & 0xa))
        windowed |= 1 << i;
    }
    int fixed = r->coldata[0] > r->coldata[1] ? r->coldata[0] : r->coldata[1];
    fixed = fixed > r->coldata[2] ? fixed : r->coldata[2];
    if (windowed && fixed && !subtract && FixedShows(r, windowed)) {
      c->bld[0] = BlendLayers(math & ~sub) | 2 << 6;
      c->bld[2] = fixed * 33 >> 6;  // /31 * 16
      return windowed | kFixedInWindows;
    }
    if (!(r->tm & 3) && (math & r->tm & 4) && !subtract) {
      // Only BG3 on the main screen, and the room on the sub screen
      // (Crateria's and Maridia's fog, for one). BG3 is GBA BG0, which
      // would be in front of the others at the same priority: the room
      // shows as usual (and Samus), with BG3 on top added to it.
      uint8_t room = r->ts & ~r->tm & 0x13;
      c->dispcnt |= BlendLayers(room) << 8;
      c->bgcnt[0] &= ~3;
      c->bld[0] = 1 | BlendLayers(room | 0x20) << 8 | 1 << 6;
      c->bld[1] = half ? 8 | 8 << 8 : 16 | 16 << 8;
      return sub;
    }
    // The sub screen's layers on top.
    uint16_t on_top = BlendLayers(sub);
    c->dispcnt |= on_top << 8;
    for (int i = 0; i < 4; i++) {
      if (on_top & (1 << i))
        c->bgcnt[i] &= ~3;
    }
    if (bg3_mid)
      c->bgcnt[0] |= 2;  // in front of BG2 only
    c->bld[0] = BlendLayers(sub) | BlendLayers(math & ~sub) << 8 | 1 << 6;
    // Subtracted (Maridia's and Tourian's shadows): the GBA can't, but
    // where the sub screen's layers are, it darkens what's under them,
    // about as much as their colors do, and they don't show.
    c->bld[1] = subtract ? subtract_weight << 8 : half ? 8 | 8 << 8 : 16 | 16 << 8;
    return sub;
  }
  return 0;
}

// What the SNES's windows do in each of the GBA's regions (see Windows):
// WININ, WINOUT, or 0 if nothing.
ROM_CODE static uint32_t WindowRegions(const PpuRegs *r, uint8_t sub, bool blending) {
  bool fixed = sub & kFixedInWindows;
  sub &= 7;
  int math_region = (r->cgwsel >> 4) & 3, clip_region = (r->cgwsel >> 6) & 3;
  bool used = false;
  uint8_t bits[3];
  for (int k = 0; k < 3; k++) {
    bool in1 = k == 0, in2 = k == 1;
    // SNES BG1-BG4 and OBJ masked, by its main or sub screen.
    uint8_t masked = 0;
    for (int i = 0; i < 5; i++) {
      int sel = (i < 4 ? r->wsel[i >> 1] >> ((i & 1) * 4) : r->wsel[2]) & 15;
      int logic = i < 4 ? (r->wbglog >> (2 * i)) & 3 : r->wobjlog & 3;
      uint8_t w = sub & (1 << i) ? r->tsw : r->tmw;
      if ((w & (1 << i)) && InWindows(sel, logic, in1, in2))
        masked |= 1 << i;
    }
    bool color = InWindows(r->wsel[2] >> 4, (r->wobjlog >> 2) & 3, in1, in2);
    bool no_math = NoMath(r, math_region, in1, in2);
    bool clip = clip_region == 3 || (clip_region == 1 && !color) || (clip_region == 2 && color);
    if (clip)
      masked |= 0x1f & ~sub;
    if (fixed) {
      // The fixed color where the sub screen's layers are hidden.
      no_math |= (masked & sub) != sub;
      masked &= ~sub;
      used = true;
    }
    if (no_math)
      masked |= sub;  // as the sub screen doesn't show
    used |= masked || (no_math && blending);
    bits[k] = (uint8_t)(0x1f & ~BlendLayers(masked)) | (no_math ? 0 : 0x20);
  }
  return used ? (bits[0] | bits[1] << 8) | (uint32_t)bits[2] << 16 : 0;
}

// A window's GBA WINxH, from the SNES's edges (WH0-WH3). For none, from
// the right edge to it: the GBA's (0, 0) is all the line.
static uint16_t WindowH(int left, int right) {
  left -= kXOffset;
  right += 1 - kXOffset;
  left = left < 0 ? 0 : left;
  right = right > 240 ? 240 : right;
  return left < right ? left << 8 | right : 240 << 8 | 240;
}

// The SNES's windows, as the GBA's: window 1 is WIN0 and 2 is WIN1 (where
// they overlap, the GBA's in WIN0 only). For each GBA region, which layers
// show (the SNES's main screen layers but where TMW masks them, the sub
// screen's on top, |sub|, but where TSW does), and whether color math is
// on (CGWSEL's region, by the color window), or the main screen's black
// (it's clipped: showing the backdrop, here). Only with the tiled layers.
static void Windows(LineRegs *l, const PpuRegs *r, uint8_t sub) {
  DisplaySegment *c = &l->ctl;
  c->win[0] = c->win[1] = 0;
  // (As a rule, no window is on for anything.)
  if (!((r->wsel[0] | r->wsel[1] | r->wsel[2]) & 0xaa) || frame_mode7 || (r->inidisp & 0x80))
    return;
  // The regions, for the settings: they change seldom, unlike the edges.
  static uint32_t last_key[3] IWRAM_BSS, regions IWRAM_BSS;
  bool blending = c->bld[0];
  uint32_t key[3] = {
    r->wsel[0] | r->wsel[1] << 8 | r->wsel[2] << 16 | (uint32_t)r->wbglog << 24,
    r->wobjlog | r->tmw << 8 | r->tsw << 16 | (uint32_t)(r->cgwsel & 0xf0) << 24,
    sub | blending << 8 | split_bg2 << 9,
  };
  if (key[0] != last_key[0] || key[1] != last_key[1] || key[2] != last_key[2]) {
    last_key[0] = key[0];
    last_key[1] = key[1];
    last_key[2] = key[2];
    regions = WindowRegions(r, sub, blending);
  }
  if (!regions)
    return;
  c->dispcnt |= 3 << 13;
  c->win[0] = regions;
  c->win[1] = regions >> 16;
}

// The windows' GBA WIN0H and WIN1H, from the SNES's edges on its line |s|.
static uint32_t LineWindows(int s) {
  uint32_t wh = line_wh[s];
  return WindowH(wh & 0xff, (wh >> 8) & 0xff) | (uint32_t)WindowH((wh >> 16) & 0xff, wh >> 24) << 16;
}

// The stream of a line (not in frames with Mode 7): its scrolling, as the
// GBA's, for its SNES line |s|.
static void LineStream(LineRegs *l, int s, int g) {
  // (The SNES shows a background's row VOFS + 1 on its first line.)
  const uint32_t *ofs = line_ofs[s], add = kXOffset | (uint32_t)(s + 1 - g) << 16;
  l->stream[0] = ofs[2] + add;
  l->stream[1] = ofs[split_bg2] + add;
  l->stream[2] = ofs[0] + add;
  l->stream[3] = ofs[1] + add;
}

// Whether two lines' registers are the same, but for the scrolling: then
// so are their GBA registers but for the stream. (Maridia's water and
// Norfair's heat have different scrolling on every line.)
_Static_assert(sizeof(PpuRegs) == 64 && offsetof(PpuRegs, hofs) == 10 &&
                   offsetof(PpuRegs, vofs) + sizeof(((PpuRegs *)0)->vofs) == 26,
               "SameButScrolling's layout");
static bool SameButScrolling(const PpuRegs *a, const PpuRegs *b) {
  // A word at a time (line_states is aligned): halfwords 0-4 and 13-31.
  const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
  if (x[0] != y[0] || x[1] != y[1] || (x[2] ^ y[2]) & 0xffff || (x[6] ^ y[6]) >> 16)
    return false;
  for (int i = 7; i < 16; i++) {
    if (x[i] != y[i])
      return false;
  }
  return true;
}

// Whether BG3 goes in front (see bg3_hi), for the line |r| on GBA line |g|
// (SNES line |s|): if the rows of its map that the lines with its state
// show from there on have priority tiles. (The HUD's do, and some rooms'
// BG3 below it in the same map, all behind, doesn't: Phantoon's room's,
// opaque black.)
ROM_CODE static bool Bg3Front(const PpuRegs *r, int s, int g) {
  int slot = Bg3Slot(r->bgsc[2]);
  if (!(r->bgmode & 8) || !bg3_hi_total[slot])
    return false;
  // 64 rows high, the second screen's rows 32-63; wider, both screens'
  // rows (as UpdateMaps has them: of 64x64, the top two).
  uint8_t sc = r->bgsc[2];
  bool tall = (sc & 3) == 2, wide = sc & 1;
  const uint8_t *hi = bg3_hi[slot];
  int state = line_state[s];
  // A line every 8 (a row's height), and the last.
  for (int t = s;;) {
    int row = ((t + 1 + VOFS(t, 2)) >> 3) & (tall ? 63 : 31);
    int c = (row & 32) * 2 + (row & 31) * 2;
    if (hi[c] | hi[c + 1] | (wide ? hi[64 + c] | hi[65 + c] : 0))
      return true;
    // (A state's lines follow each other, and SnesLine goes up: the line 8
    // on is the state's if those between are.)
    int n = g + 8 < kGbaLines ? 8 : kGbaLines - 1 - g;
    while (n && line_state[SnesLine(g + n)] != state)
      n--;
    if (!n)
      return false;
    g += n;
    t = SnesLine(g);
  }
}

// The registers BuildLine last made the GBA's of, but for the scrolling,
// in this frame.
static const PpuRegs *built_regs;

static void BuildLine(LineRegs *l, const PpuRegs *r, int s, int g) {
  if ((r->bgmode & 7) == 7) {
    BuildLineMode7(l, r, s);
    return;
  }
  int split = split_bg2;
  if (!frame_mode7 && built_regs && (built_regs == r || SameButScrolling(built_regs, r)))
    goto scrolling;
  built_regs = r;
  uint16_t dispcnt = frame_mode7 ? kDispcntMode1 : kDispcntMode0;
  if (!(r->inidisp & 0x80)) {
    // With Mode 7 lines in the frame, BG1 and BG2's VRAM holds Mode 7's.
    if (!frame_mode7) dispcnt |= BlendLayers(r->tm & 3) << 8;
    if (r->tm & 4) dispcnt |= kDispcntBg0;
    if (r->tm & 16) dispcnt |= kDispcntObj;
  }
  l->ctl.dispcnt = dispcnt;
  int bg3_sb = Bg3Slot(r->bgsc[2]) ? kSbBg3Slot1 : kSbBg3Slot0;
  int bg3_front = Bg3Front(r, s, g);
  l->ctl.bgcnt[0] = (bg3_front ? 0 : 3) | 2 << 2 | bg3_sb << 8 | BgSize(r->bgsc[2]) << 14;
  // The SNES background split into GBA BG1 and BG2 or BG3.
  l->ctl.bgcnt[1] = (1 - bg3_mid) | kSbBg1Hi << 8 | BgSize(r->bgsc[split]) << 14;
  l->ctl.bgcnt[2] = (2 - bg3_mid) | kSbBg1Lo << 8 | BgSize(r->bgsc[0]) << 14;
  l->ctl.bgcnt[3] = 2 | kSbBg2 << 8 | BgSize(r->bgsc[1]) << 14;
  Windows(l, r, ColorMath(&l->ctl, r));
scrolling:;
  // (The SNES shows a background's row VOFS + 1 on its first line.)
  // The scrolling is the stream, but in frames with Mode 7, whose stream
  // is BG2's transformation.
  if (!frame_mode7) {
    LineStream(l, s, g);
    return;
  }
  // (By halfwords: ofs isn't word aligned, and the ARM would store a word
  // at the word it's in.)
  const uint32_t *ofs = line_ofs[s], add = kXOffset | (uint32_t)(s + 1 - g) << 16;
  const uint32_t in[4] = {ofs[2] + add, ofs[split] + add, ofs[0] + add, ofs[1] + add};
  for (int i = 0; i < 4; i++) {
    l->ctl.ofs[2 * i] = (uint16_t)in[i];
    l->ctl.ofs[2 * i + 1] = in[i] >> 16;
  }
  // BG2's identity, for what it's worth.
  l->stream[0] = 0x100;
  l->stream[1] = 0x100 << 16;
  l->stream[2] = l->stream[3] = 0;
}

// Whether two lines' scrolling is the same.
static bool SameOfs(int a, int b) {
  const uint32_t *x = line_ofs[a], *y = line_ofs[b];
  return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

// But for first_line. (Not with memcmp: it's in Thumb code, and slow.)
static bool SameSegment(const DisplaySegment *a, const DisplaySegment *b) {
  const uint16_t *x = &a->dispcnt, *y = &b->dispcnt;
  for (int i = 0; i < 18; i++) {
    if (x[i] != y[i])
      return false;
  }
  return true;
}

ROM_CODE int Render_GameplayYOffset(void) {
  return hud ? y_offset : 0;
}

ROM_CODE static void UpdateYOffset(void) {
  int target;
  if (hud) {
    // Samus in the middle of the gameplay area, as far as it goes: by her
    // feet, standing's 21 lines below her middle (which moves as she
    // crouches or curls up), and only once she's 12 lines from where she
    // would be (not as she ducks, or walks over a bump).
    int feet = (int16_t)(samus_y_pos + samus_y_radius - layer1_y_pos);
    int middle = kHudLines + (kGbaLines - kGbaHudLines) / 2 + y_offset;
    int away = feet - 21 - middle;
    target = y_offset + (away > 12 ? away - 12 : away < -12 ? away + 12 : 0);
    target = target < 0 ? 0 : target > kMaxYOffset ? kMaxYOffset : target;
  } else {
    target = kMaxYOffset / 2;
  }
  int d = target - y_offset;
  y_offset += d > 4 ? 4 : d < -4 ? -4 : d;
}

// Runs the statement after it for each chunk k (declared here) of the 64
// in the map screen at addr (1024 words, aligned) that changed, or all.
#define FOR_EACH_DIRTY_CHUNK(dirty, addr, all, k)                                          \
  for (int w_ = 0; w_ < 2; w_++)                                                           \
    for (uint32_t bits_ = (all) ? ~0u : (dirty)[((addr) >> 9) + w_], k;                    \
         bits_ && ((k = w_ * 32 + __builtin_ctz(bits_)), 1); bits_ &= bits_ - 1)

ROM_CODE static void SetSplit(bool split) {
  split_bg2 = split;
  bg12_layers[0] = split ? 4 : 6;
  bg12_layers[1] = split ? 10 : 8;
}

// Chooses split_bg2 from BG1's and BG2's priority tiles (as the frame
// before converted their maps) and the line |r|'s scrolling.
ROM_CODE static void ChooseSplit(const PpuRegs *r) {
  int shown[2];
  for (int bg = 0; bg < 2; bg++) {
    uint8_t sc = r->bgsc[bg];
    int screens = (sc & 3) ? 2 : 1;
    // The columns the GBA shows, 128 pixels wide, two to a screen (in all
    // the rows, and both screens of a map two screens high).
    int wide = sc & 1, n = 0;
    int first = (HOFS(text_line, bg) + kXOffset) >> 7, last = (HOFS(text_line, bg) + kXOffset + 239) >> 7;
    for (int j = first; j <= last; j++) {
      int col = j & (wide ? 3 : 1);
      n += bg12_hi_cols[bg][col];
      if (!wide && screens == 2)
        n += bg12_hi_cols[bg][2 + col];
    }
    shown[bg] = n;
  }
  SetSplit(shown[1] > shown[0] && (r->tm & 2));
}

// The CGRAM index of background |bg|'s pixel at SNES (x, line), with the
// line |r|'s registers, or 0 where it's transparent.
ROM_CODE static int BgPixel(const PpuRegs *r, int bg, int x, int line) {
  uint8_t sc = r->bgsc[bg];
  int bx = (HOFS(line, bg) + x) & 0x3ff, by = (VOFS(line, bg) + line + 1) & 0x3ff;
  int screen = (bx >> 8) & (sc & 1);
  screen += (by >> 8) & (sc >> 1 & 1) ? (sc & 1) + 1 : 0;
  uint16_t e = g_vram[((sc & 0xfc) << 8 | screen << 10 | ((by >> 3) & 31) << 5 | ((bx >> 3) & 31)) &
                      0x7fff];
  int tx = e & 0x4000 ? 7 - (bx & 7) : bx & 7, ty = e & 0x8000 ? 7 - (by & 7) : by & 7;
  int nba = (bg == 2 ? r->bgnba[1] : r->bgnba[0] >> (bg * 4)) & 15;
  int words = bg == 2 ? 8 : 16;
  const uint16_t *t = &g_vram[((nba << 12) + (e & 0x3ff) * words + ty) & 0x7fff];
  int shift = 7 - tx;
  int c = (t[0] >> shift & 1) | (t[0] >> (shift + 8) & 1) << 1;
  if (bg == 2)
    return c ? ((e >> 10) & 7) * 4 + c : 0;
  c |= (t[8] >> shift & 1) << 2 | (t[8] >> (shift + 8) & 1) << 3;
  return c ? ((e >> 10) & 7) * 16 + c : 0;
}

ROM_CODE static int Luma(uint16_t color) {
  return (color & 31) + (color >> 5 & 31) + (color >> 10 & 31);
}

// What's left of |m|'s channels after |s|'s are taken from them.
ROM_CODE static int LumaLeft(uint16_t m, uint16_t s) {
  int left = 0;
  for (int i = 0; i < 15; i += 5) {
    int d = (m >> i & 31) - (s >> i & 31);
    left += d > 0 ? d : 0;
  }
  return left;
}

// SubtractWeight's samples, k of 40 in 5 rows of 8: staggered, so that
// they fall on all of a tile's rows and columns, not one (BG3's shadows
// can be a pattern of thin lines).
ROM_CODE static int SampleX(int k) {
  return kXOffset + 11 + (k & 7) * 29 + (k >> 3) * 5;
}

ROM_CODE static int SampleLine(int k) {
  return SnesLine(9 + (k >> 3) * 29 + (k & 7) * 3);
}

// What's left of each of |m|'s channels with |s|'s taken away, |add|'s
// added back (to 31 at most), against what the SNES makes of |m| less |c|:
// how far off, in all.
ROM_CODE static int SubtractError(uint16_t m, uint16_t s, uint16_t add, uint16_t c) {
  int error = 0;
  for (int i = 0; i < 15; i += 5) {
    int v = (m >> i & 31) - (s >> i & 31), want = (m >> i & 31) - (c >> i & 31);
    v = (v < 0 ? 0 : v) + (add >> i & 31);
    v = v > 31 ? 31 : v;
    want = want < 0 ? 0 : want;
    error += v > want ? v - want : want - v;
  }
  return error;
}

// How far off adding back would be, in a channel, with |v| subtracted in
// the palette, at SubtractWeight's 40 samples: their main screen's values
// (|m|), the sub screen's (|c|, or 32 where it's transparent, and nothing's
// added back) and what the SNES makes of them (|want|). (In IWRAM: a
// search runs it a dozen times a frame.)
static int AddBackError(const uint8_t *m, const uint8_t *c, const uint8_t *want, int v) {
  int error = 0;
  for (int k = 0; k < 40; k++) {
    int got = (m[k] > v ? m[k] - v : 0) + (v > c[k] ? v - c[k] : 0);
    got = got > 31 ? 31 : got;
    error += got > want[k] ? got - want[k] : want[k] - got;
  }
  return error;
}


// For the line |r|, which subtracts the sub screen, by a few dozen of its
// pixels on show and the main screen's there: the GBA's way that comes
// closest, of
// - darkening where the sub screen's layer with the most pixels
//   (subtract_layer) is, by how much of the main screen's brightness is left
//   there, in 16ths (the result);
// - if the sub screen is all over the screen, subtracting the average of
//   its colors in the palette (subtract_color, with subtract_palettes 0);
// - if it's BG3 all over the screen, subtracting a color in the palette,
//   and BG3 adding back what's too much, by its colors: the color, channel
//   by channel, that's closest (subtract_color, with the BG3 palettes it
//   uses, subtract_palettes).
// Over sixteen frames, a slice a frame: the colors and the scrolling change
// slowly. (No division: see cmake/compiler-rt.cmake.)
ROM_CODE static int SubtractWeight(const PpuRegs *r) {
  // A slice a frame, so that no frame does much (it's some 150k cycles in
  // all, from the cartridge): frames 0-4 take 8 samples each, 5 and 6 look
  // at them, 7 sees how far off darkening and the average would be, 8-13
  // search a channel each two, 14 says what's best; 15 rests. If the
  // registers change partway, it starts over.
  enum { kColumns = 8, kRows = 5, kSamples = kColumns * kRows, kPerFrame = 8 };
  enum {
    kLook = kSamples / kPerFrame,
    kErrors = kLook + 2,
    kSearch,
    kDecide = kSearch + 6,
  };
  static uint8_t phase;
  static uint32_t key;
  static uint8_t pixels[3][kSamples], mains[kSamples];
  static uint16_t main_colors[kSamples], sub_colors[kSamples];
  static uint8_t opaque_at[kSamples];
  static int best, weight, darkened, averaged, added_back, darkest[3], brightest[3];
  static int opaque, bg3, sums[3], left_luma, main_luma, lo, hi;
  static uint16_t mean, top;
  static bool add_back;
  uint8_t sub = r->ts & ~r->tm & 7, main = r->tm & 3;
  uint32_t now = r->ts | r->tm << 8 | (uint32_t)r->cgadsub << 16;
  // (Whether the errors are of these registers' first look, or the room's:
  // doors are game states 9-11.)
  static bool fresh = true;
  if (game_state != 8)
    fresh = true;
  if (now != key) {
    key = now;
    phase = 0;
    fresh = true;
  }
  int p = phase;
  phase = (phase + 1) & 15;
  if (!sub) {
    subtract_color = 0;
    return 16;
  }
  if (p < kSamples / kPerFrame) {
    for (int k = p * kPerFrame; k < (p + 1) * kPerFrame; k++) {
      int x = SampleX(k), line = SampleLine(k), m = 0;
      for (int bg = 0; bg < 3; bg++)
        pixels[bg][k] = sub & (1 << bg) ? BgPixel(r, bg, x, line) : 0;
      for (int bg = 0; bg < 2 && !m; bg++) {
        if (main & (1 << bg))
          m = BgPixel(r, bg, x, line);
      }
      mains[k] = m;
    }
    return subtract_weight;
  }
  if (p == kLook) {
    // The layer that takes the most away: with the most pixels on show (in
    // front of the others) that aren't black.
    int count[3] = {0, 0, 0};
    best = 2;
    for (int k = 0; k < kSamples; k++) {
      for (int bg = 0; bg < 3; bg++) {
        if ((sub & (1 << bg)) && pixels[bg][k]) {
          count[bg] += g_cgram[pixels[bg][k]] != 0;
          break;
        }
      }
    }
    for (int bg = 0; bg < 3; bg++) {
      if ((sub & (1 << bg)) && (count[bg] > count[best] || !(sub & (1 << best))))
        best = bg;
    }
    opaque = bg3 = left_luma = main_luma = 0;
    for (int c = 0; c < 3; c++)
      sums[c] = 0, darkest[c] = 31, brightest[c] = 0;
  }
  if (p <= kLook + 1) {
    // The main and sub screens' pixels; and of the sub screen's, how many,
    // how many BG3's, their sums, the darkest and the brightest: half the
    // samples a frame.
    int from_k = p == kLook ? 0 : kSamples / 2;
    for (int k = from_k; k < from_k + kSamples / 2; k++) {
      int m = mains[k], q = 0, from = 0;
      for (int bg = 0; bg < 3 && !q; bg++) {
        if (sub & (1 << bg))
          q = pixels[bg][k], from = bg;
      }
      main_colors[k] = g_cgram[m];
      sub_colors[k] = q ? g_cgram[q] : 0;
      opaque_at[k] = q != 0;
      if (pixels[best][k]) {
        left_luma += LumaLeft(g_cgram[m], g_cgram[pixels[best][k]]);
        main_luma += Luma(g_cgram[m]);
      }
      if (!q)
        continue;
      opaque++;
      bg3 += from == 2;
      for (int c = 0; c < 3; c++) {
        int v = g_cgram[q] >> (5 * c) & 31;
        sums[c] += v;
        brightest[c] = v > brightest[c] ? v : brightest[c];
        darkest[c] = v < darkest[c] ? v : darkest[c];
      }
    }
    if (p == kLook)
      return subtract_weight;
    // (left_luma << 4) / main_luma: at most 16, as what's left is at most
    // what was there.
    weight = 16;
    if (main_luma) {
      weight = 0;
      for (int left = left_luma << 4; left >= main_luma; left -= main_luma)
        weight++;
    }
    if (r->cgadsub & 0x40)
      weight >>= 1;
    // Where the sub screen's transparent, nothing's subtracted (it's the
    // fixed color, black as a rule), nor added back.
    if (opaque < kSamples)
      darkest[0] = darkest[1] = darkest[2] = 0;
    mean = top = 0;
    _Static_assert(kSamples == 40, "the division by 40");
    for (int c = 0; c < 3; c++)
      mean |= ((sums[c] + kSamples / 2) * 1639 >> 16) << (5 * c);
    add_back = opaque && bg3 == opaque;
    return subtract_weight;
  }
  if (p == kErrors) {
    // How far off each would be, at the pixels. (Not halved.)
    darkened = averaged = 0;
    added_back = add_back ? 0 : 0x7fffffff;
    for (int k = 0; k < kSamples; k++) {
      uint16_t m = main_colors[k], c = sub_colors[k];
      uint16_t dark = pixels[best][k] ? 0 : m;
      for (int i = 0; i < 15 && pixels[best][k]; i += 5)
        dark |= ((m >> i & 31) * weight >> 4) << i;
      darkened += SubtractError(dark, 0, 0, c);
      averaged += SubtractError(m, mean, 0, c);
    }
    return subtract_weight;
  }
  if (p < kDecide) {
    // Adding back: a channel each two frames, the color between BG3's
    // darkest and brightest that's closest. (A ternary search: the error
    // falls, then rises. Its first few steps in the first frame.)
    int ch = 5 * ((p - kSearch) >> 1);
    bool first = !((p - kSearch) & 1);
    if (!add_back)
      return subtract_weight;
    uint8_t m[kSamples], c[kSamples], want[kSamples];
    for (int k = 0; k < kSamples; k++) {
      m[k] = main_colors[k] >> ch & 31;
      c[k] = opaque_at[k] ? sub_colors[k] >> ch & 31 : 32;
      int w = m[k] - (opaque_at[k] ? c[k] : 0);
      want[k] = w > 0 ? w : 0;
    }
    if (first)
      lo = darkest[ch / 5], hi = brightest[ch / 5];
    for (int steps = 0; hi - lo > 2 && (!first || steps < 4); steps++) {
      int third = (hi - lo) * 11 >> 5;  // (hi - lo) / 3, as they're 5 bits
      if (AddBackError(m, c, want, lo + third) <= AddBackError(m, c, want, hi - third))
        hi = hi - third;
      else
        lo = lo + third;
    }
    if (first)
      return subtract_weight;
    int best_error = 0x7fffffff, best_s = lo;
    for (int v = lo; v <= hi; v++) {
      int error = AddBackError(m, c, want, v);
      if (error < best_error)
        best_error = error, best_s = v;
    }
    added_back += best_error;
    top |= best_s << ch;
    return subtract_weight;
  }
  if (p == kDecide) {
    // Darkening, the average or adding back: the one that's least off,
    // over the last few times (40 pixels are only a sample, and the fog
    // moves; they can be close, and the colors would flicker between
    // them).
    static int error[3];
    int now_error[3] = {darkened, averaged, add_back ? added_back : 0x1000000};
    for (int i = 0; i < 3; i++)
      error[i] = fresh ? now_error[i] : error[i] + (now_error[i] - error[i]) / 4;
    fresh = false;
    int way = error[1] <= error[0] && error[1] <= error[2] ? 1 : error[2] < error[0] ? 2 : 0;
    subtract_layer = 1 << best;
    subtract_color = 0;
    if (way == 1) {
      subtract_color = 0x8000 | mean;
      subtract_palettes = 0;
    } else if (way == 2 && add_back) {
      // All of BG3's palettes: those not among the pixels, too.
      subtract_color = 0x8000 | top;
      subtract_palettes = 0xff;
    }
    return weight;
  }
  return subtract_weight;
}

// Whether the line |r| adds BG3 alone, on the sub screen, to BG2 but not
// BG1 (see bg3_mid).
ROM_CODE static bool Bg3Between(const PpuRegs *r) {
  return (r->cgwsel & 2) && !(r->cgadsub & 0x80) && (r->ts & ~r->tm & 7) == 4 &&
         (r->tm & 3) == 3 && (r->cgadsub & 3) == 2;
}

// The GBA shows a background only three lines after DISPCNT turns it on
// partway down the screen (it was the backdrop at the top of the gameplay
// area, below the HUD): segments before one that turns on BG1-BG3 already
// have them on, but blacked out, with BG0 in front of them. (Not in
// segments with sprites, color math or windows: the HUD has none.)
ROM_CODE static void KeepLayersOn(DisplaySegment *segs, int n) {
  uint16_t later = 0;
  for (int i = n - 1; i >= 0; i--) {
    DisplaySegment *c = &segs[i];
    uint16_t hidden = later & ~c->dispcnt;
    if (hidden && !c->bld[0] && !(c->dispcnt & (kDispcntObj | 3 << 13))) {
      c->dispcnt |= hidden;
      c->bgcnt[0] &= ~3;
      c->bld[0] = hidden >> 8 | 3 << 6;
      c->bld[2] = 16;
    }
    later |= c->dispcnt & 0x0e00;
  }
}

// Squashed outside gameplay: switched while the screen's dark, between
// gameplay and the pause screen, the title or a cutscene (a frame of one
// shown as the other would be squashed, or not, wrongly), or at most a
// second after the game state did.
ROM_CODE static void SetSquash(void) {
  static uint8_t waited;
  bool want = game_state < 7 || game_state > 11;
  if (want == squash) {
    waited = 0;
    return;
  }
  const PpuRegs *r = LINE_REGS(kSnesLines / 2);
  if ((r->inidisp & 0x80) || !(r->inidisp & 15) || ++waited >= 60) {
    squash = want;
    waited = 0;
  }
}

void Render_CaptureFrame(void) {
  SnesHw_RunDisplay(line_states, line_state, line_wh, line_ofs);
  // (The palette's line, as it was last frame: see FixedMath.)
  SnesHw_HdmaColdata(line_states, line_state, SnesLine(kGbaLines / 2));

  SetSquash();
  if (squash) {
    SetSquashLines();
    if (g_snes_hw.cgram_dirty)
      SetSquashRanks();
  }
  // The HUD: BG3 alone, and below it, other layers or another BG3 (in
  // rooms with fog, BG3 alone again, the room on the sub screen, and
  // color math, unlike the HUD's).
  const PpuRegs *top = LINE_REGS(8), *below = LINE_REGS(kHudLines + 8);
  hud = !squash && top->tm == 4 &&
        (below->tm != 4 || below->cgadsub != top->cgadsub || below->bgsc[2] != top->bgsc[2] ||
         HOFS(kHudLines + 8, 2) != HOFS(8, 2) || VOFS(kHudLines + 8, 2) != VOFS(8, 2));
  UpdateYOffset();

  // The BG3 maps the lines use (in slots), whether any line is Mode 7's,
  // and a line of text (not Mode 7), the middle one if it can be. A line
  // with the state of the one before tells nothing new.
  uint8_t first = LINE_REGS(SnesLine(0))->bgsc[2];
  uint8_t second = first;
  frame_mode7 = false;
  text_line = -1;
  for (int g = 0, prev = -1; g < kGbaLines; g++) {
    int s = SnesLine(g), state = line_state[s];
    if (state == prev && g != kGbaLines / 2)
      continue;
    prev = state;
    const PpuRegs *r = &line_states[state];
    if (second == first)
      second = r->bgsc[2];
    if ((r->bgmode & 7) == 7)
      frame_mode7 = true;
    else if (text_line < 0 || g == kGbaLines / 2)
      text_line = s;
  }
  if (first != bg3_slot_sc[0] && first != bg3_slot_sc[1]) {
    // Keep the other one where it is.
    bg3_slot_sc[second == bg3_slot_sc[0] ? 1 : 0] = first;
  }
  if (second != bg3_slot_sc[0] && second != bg3_slot_sc[1])
    bg3_slot_sc[first == bg3_slot_sc[0] ? 1 : 0] = second;

  if (text_line >= 0 && !frame_mode7)
    ChooseSplit(LINE_REGS(text_line));
  else
    SetSplit(false);
  bg3_mid = text_line >= 0 && !frame_mode7 && !split_bg2 && Bg3Between(LINE_REGS(text_line));
  if (text_line >= 0 && !frame_mode7 && (LINE_REGS(text_line)->cgadsub & 0x80) &&
      (LINE_REGS(text_line)->cgwsel & 2))
    subtract_weight = SubtractWeight(LINE_REGS(text_line));
  else
    subtract_color = 0;

  // Lines with the same registers as the one before (and the same
  // vertical offset) have the same GBA registers, but for Mode 7's
  // reference point, which moves down the map a line at a time.
  uint32_t(*stream)[4] = streams[back];
  DisplaySegment *segs = segments[back];
  int num_segs = 0;
  LineRegs l;
  int prev_state = -1, prev_dy = 0, prev_s = 0;
  bool windows = false;
  built_regs = NULL;
  for (int g = 0; g < kGbaLines; g++) {
    int s = SnesLine(g), state = line_state[s];
    const PpuRegs *r = &line_states[state];
    bool mode7 = (r->bgmode & 7) == 7, same = state == prev_state && s - g == prev_dy;
    if (same && mode7) {
      StepLineMode7(&l);
    } else if (same && !frame_mode7) {
      LineStream(&l, s, g);
    } else if (!same || !SameOfs(s, prev_s)) {
      BuildLine(&l, r, s, g);
      if (!num_segs || (num_segs < kMaxSegments && !SameSegment(&segs[num_segs - 1], &l.ctl))) {
        segs[num_segs] = l.ctl;
        segs[num_segs++].first_line = g;
      }
    }
    for (int i = 0; i < 4; i++)
      stream[g][i] = l.stream[i];
    if (l.ctl.dispcnt & (3 << 13)) {
      line_windows[g] = LineWindows(s);
      windows = true;
    }
    prev_state = state;
    prev_dy = s - g;
    prev_s = s;
  }
  for (int i = 0; i < 4; i++)
    stream[kGbaLines][i] = stream[kGbaLines - 1][i];
  KeepLayersOn(segs, num_segs);
  DisplayFrame *f = &frames[back];
  f->stream = &stream[0][0];
  f->stream_words = 4;
  f->stream_dest = frame_mode7 ? 0x04000020 : 0x04000010;
  if (windows) {
    // (Not with Mode 7: see Windows.) The transformations stay 0.
    uint32_t(*w)[kWindowStreamWords] = window_streams[back];
    for (int g = 0; g <= kGbaLines; g++) {
      for (int i = 0; i < 4; i++)
        w[g][i] = stream[g][i];
      w[g][kWindowStreamWords - 1] = line_windows[g < kGbaLines ? g : kGbaLines - 1];
    }
    f->stream = &w[0][0];
    f->stream_words = kWindowStreamWords;
  }
  f->num_segments = num_segs;
  f->segments = segs;
  // The gameplay area's first segment, for the palette's patches.
  f->patch_segment = 0;
  while (hud && f->patch_segment < num_segs - 1 && segs[f->patch_segment].first_line < kGbaHudLines - 1)
    f->patch_segment++;
  LogLayoutChange(LINE_REGS(SnesLine(kGbaLines / 2)));
}

// The color math with the fixed color that line |m| does, which the
// palette does in its place (see ColorMath): its CGADSUB and the color, or
// 0 for none.
ROM_CODE static uint32_t FixedMath(const PpuRegs *m) {
  int region = (m->cgwsel >> 4) & 3;
  bool sub = m->cgwsel & 2;
  if ((sub && !((m->cgadsub & 0x80) && (subtract_color & 0x8000))) || !(m->cgadsub & 0x3f) ||
      region == 1 || region == 3)
    return 0;
  // The sub screen's one color (see subtract_color), or the fixed color.
  uint32_t color = sub ? subtract_color & 0x7fff
                       : m->coldata[0] | m->coldata[1] << 5 | m->coldata[2] << 10;
  if (!color && !(m->cgadsub & 0x40))
    return 0;
  return m->cgadsub | color << 8;
}

ROM_CODE static int Bright(int v, int brightness) {
  return brightness == 15 ? v : brightness ? (v * (brightness + 1)) >> 4 : 0;
}

// |n| colors from |src| to |dst|, by |t|'s channels, unless they're as they
// were (|same|).
static void Recolor(volatile uint16_t *dst, const uint16_t *src, int n, const uint8_t (*t)[32],
                    bool same) {
  if (same) {
    for (int i = 0; i < n; i++)
      dst[i] = src[i];
    return;
  }
  for (int i = 0; i < n; i++) {
    uint16_t c = src[i];
    dst[i] = t[0][c & 31] | t[1][(c >> 5) & 31] << 5 | t[2][(c >> 10) & 31] << 10;
  }
}

// The palette, at the brightness of line |r|, with line |m|'s color math
// with the fixed color.
ROM_CODE static void UpdatePalette(const PpuRegs *r, const PpuRegs *m, bool bg3_palettes) {
  SnesHw *h = &g_snes_hw;
  int b = (r->inidisp & 0x80) ? 0 : (r->inidisp & 15);
  uint32_t math = FixedMath(m);
  uint8_t subtracted = (subtract_color & 0x8000) && (m->cgwsel & 2) ? subtract_palettes : 0;
  if (!h->cgram_dirty && b == conv.brightness && bg3_palettes == conv.bg3_palettes &&
      math == conv.fixed_math && subtracted == conv.subtract_palettes)
    return;
  conv.subtract_palettes = subtracted;
  h->cgram_dirty = false;
  conv.brightness = b;
  conv.bg3_palettes = bg3_palettes;
  conv.fixed_math = math;
  // Each channel's value, by what it was, at the brightness, and with the
  // color math too. (Tables, as the palette often changes every frame.)
  static uint8_t plain[3][32], with[3][32];
  static int tables_b = -1;
  static uint32_t tables_math;
  if (b != tables_b || math != tables_math) {
    tables_b = b;
    tables_math = math;
    for (int c = 0; c < 3; c++) {
      int f = (math >> (8 + 5 * c)) & 31;
      for (int i = 0; i < 32; i++) {
        int v = math & 0x80 ? (i > f ? i - f : 0) : (i + f < 31 ? i + f : 31);
        plain[c][i] = Bright(i, b);
        with[c][i] = Bright(math & 0x40 ? v >> 1 : v, b);
      }
    }
  }
  // The backgrounds', the sprites', and BG3's 2bpp palettes of 4 colors in
  // GBA palettes 8-15 (Mode 7's background has all 256 colors: frames with
  // both are a compromise); with the color math on the layers it's on (the
  // sprites': in palettes 4-7). By tables on the stack, in IWRAM.
  uint8_t tables[2][3][32];
  memcpy(tables[0], plain, sizeof(plain));
  memcpy(tables[1], with, sizeof(with));
  const uint8_t(*same)[32] = tables[0];
  const uint8_t(*bd)[32] = tables[(math & 0x20) != 0], (*bg12)[32] = tables[(math & 3) != 0];
  const uint8_t(*o)[32] = tables[(math & 0x10) != 0], (*b3)[32] = tables[(math & 4) != 0];
  bool full = b == 15;
  Recolor(GBA_BG_PALETTE, g_cgram, 1, bd, full && bd == same);
  Recolor(GBA_BG_PALETTE + 1, g_cgram + 1, 127, bg12, full && bg12 == same);
  Recolor(GBA_OBJ_PALETTE, g_cgram + 128, 64, same, full);
  Recolor(GBA_OBJ_PALETTE + 64, g_cgram + 192, 64, o, full && o == same);
  for (int p = 0; p < 8 && bg3_palettes; p++)
    Recolor(GBA_BG_PALETTE + 128 + p * 16, g_cgram + p * 4, 4, b3, full && b3 == same);
  // BG3's colors c that add back the subtracted color less c (see
  // subtract_color), in the gameplay area only: the HUD has some of the
  // same palettes.
  num_patches = 0;
  for (int p = 0; p < 8 && bg3_palettes; p++) {
    if (!(subtracted & (1 << p)))
      continue;
    for (int i = 1; i < 4; i++) {
      uint16_t color = 0;
      for (int ch = 0; ch < 15; ch += 5) {
        int d = (subtract_color >> ch & 31) - (g_cgram[p * 4 + i] >> ch & 31);
        d = d < 0 ? 0 : (math & 0x40) ? d >> 1 : d;
        color |= Bright(d, b) << ch;
      }
      patch_index[num_patches] = p * 16 + i;
      patch_color[num_patches] = color;
      restore_color[num_patches++] = GBA_BG_PALETTE[128 + p * 16 + i];
    }
  }
}

static bool TileIsBlank(const uint16_t *t) {
  for (int i = 0; i < 16; i++) {
    if (t[i])
      return false;
  }
  return true;
}

static bool ChunkDirty(const uint32_t *dirty, uint32_t chunk) {
  return (dirty[chunk >> 5] >> (chunk & 31)) & 1;
}

ROM_CODE static void UpdateTilesMode7(const uint32_t *dirty) {
  // Each word of the first 32 KB is a map entry (low byte) and a pixel
  // (high byte).
  volatile uint16_t *tiles = GBA_VRAM, *map = VRAM_SB(kSbMode7);
  for (uint32_t w = 0; w < 0x4000 >> (kVramChunkShift + 5); w++) {
    uint32_t bits = dirty[w];
    while (bits) {
      uint32_t c = w * 32 + __builtin_ctz(bits);
      bits &= bits - 1;
      uint32_t addr = c << kVramChunkShift;
      const uint16_t *src = &g_vram[addr];
      for (int i = 0; i < 16; i += 2) {
        uint16_t w0 = src[i], w1 = src[i + 1];
        map[(addr + i) >> 1] = (w0 & 0xff) | (w1 & 0xff) << 8;
        tiles[(addr + i) >> 1] = (w0 >> 8) | (w1 & 0xff00);
      }
    }
  }
}

static void UpdateTiles(const PpuRegs *r, uint32_t *dirty, bool bg12, bool bg3) {
  SnesHw *h = &g_snes_hw;
  uint16_t bg12_base = (r->bgnba[0] & 0xf) << 12;
  uint16_t bg2_base = (r->bgnba[0] >> 4) << 12;
  uint16_t bg3_base = (r->bgnba[1] & 0xf) << 12;
  uint16_t obj0 = (h->obsel & 7) << 13;
  uint16_t obj1 = (obj0 + ((((h->obsel >> 3) & 3) + 1) << 12)) & 0x7fff;
  static uint16_t warned_bg2_base = 0xffff;
  if (bg2_base != bg12_base && (r->tm & 2) && warned_bg2_base != bg2_base) {
    printf("render: BG2 tiles at 0x%x, BG1's at 0x%x (not supported yet)\n", bg2_base, bg12_base);
    warned_bg2_base = bg2_base;
  }
  if (bg12_base != conv.bg12_base || bg3_base != conv.bg3_base || obj0 != conv.obj_base[0] ||
      obj1 != conv.obj_base[1] || squash != conv.squashed) {
    conv.squashed = squash;
    conv.bg12_base = bg12_base;
    conv.bg3_base = bg3_base;
    conv.obj_base[0] = obj0;
    conv.obj_base[1] = obj1;
    conv.blank_tile = -1;
    memset(dirty, 0xff, kVramChunks / 8);
  }

  volatile uint32_t *vram32 = (volatile uint32_t *)GBA_VRAM;
  volatile uint32_t *obj32 = (volatile uint32_t *)GBA_OBJ_VRAM;

  for (uint32_t w = 0; w < kVramChunks / 32; w++) {
    uint32_t bits = dirty[w];
    while (bits) {
      uint32_t c = w * 32 + __builtin_ctz(bits);
      bits &= bits - 1;
      uint16_t addr = c << kVramChunkShift;
      const uint16_t *src = &g_vram[addr];
      uint16_t off;
      if (bg12) {
        off = (addr - bg12_base) & 0x7fff;
        if (off < 0x4000)
          (squash ? Convert_Tiles4bppSquashed : Convert_Tiles4bpp)(vram32 + (off >> 4) * 8, src, 1);
      }
      if (bg3) {
        off = (addr - bg3_base) & 0x7fff;
        if (off < kBg3Tiles * 8)
          (squash ? Convert_Tiles2bppSquashed : Convert_Tiles2bpp)(vram32 + 0x8000 / 4 + (off >> 3) * 8,
                                                                 src, 2);
      }
      for (int t = 0; t < 2; t++) {
        off = (addr - conv.obj_base[t]) & 0x7fff;
        if (off < 0x1000) {
          int i = off >> 4;
          int tile = t * 512 + (i >> 4) * 32 + (i & 15);
          if (squash) {
            Convert_Tile4bppAndMirrored(obj32 + tile * 8, obj32 + (tile - 2 * (i & 15) + 31) * 8,
                                        src);
          } else {
            Convert_Tiles4bpp(obj32 + tile * 8, src, 1);
            Convert_Tiles4bpp(obj32 + (tile + 16) * 8, src, 1);
          }
        }
      }
    }
  }

  // A transparent tile, for the halves of BG1's map that don't show.
  int blank = conv.blank_tile;
  if (blank < 0 || ChunkDirty(dirty, ((bg12_base + blank * 16) & 0x7fff) >> kVramChunkShift)) {
    if (blank < 0 || !TileIsBlank(&g_vram[(bg12_base + blank * 16) & 0x7fff])) {
      blank = 0;
      for (int i = 1023; i >= 0; i--) {
        if (TileIsBlank(&g_vram[(bg12_base + i * 16) & 0x7fff])) {
          blank = i;
          break;
        }
      }
    }
  }
  conv.blank_tile = blank;
}

// (In ROM: what it calls does the work.)
ROM_CODE static void UpdateMaps12(const PpuRegs *r, const uint32_t *dirty, bool force_split) {
  bool split_changed = split_bg2 != conv.split_bg2;
  conv.split_bg2 = split_bg2;
  for (int bg = 0; bg < 2; bg++) {
    uint8_t sc = r->bgsc[bg];
    bool split = bg == split_bg2;
    // A 64x64 map's top or bottom row, by the middle line (Kraid's room).
    int row = (sc & 3) == 3 ? ((VOFS(text_line, bg) + text_line) >> 8) & 1 : 0;
    bool moved = sc != conv.bgsc[bg] || row != conv.bg_row[bg];
    bool changed = moved || (split && force_split) || split_changed;
    if (moved) {
      memset(bg12_hi[bg], 0, sizeof(bg12_hi[bg]));
      memset(bg12_hi_cols[bg], 0, sizeof(bg12_hi_cols[bg]));
    }
    conv.bgsc[bg] = sc;
    conv.bg_row[bg] = row;
    int screens = (sc & 3) ? 2 : 1;
    for (int k = 0; k < screens; k++) {
      uint16_t addr = (((sc & 0xfc) << 8) + (row * 2 + k) * 0x400) & 0x7fff;
      uint8_t *counts = &bg12_hi[bg][k * 64];
      uint16_t *cols = &bg12_hi_cols[bg][k * 2];
      FOR_EACH_DIRTY_CHUNK(dirty, addr, changed, c) {
        uint32_t o = c * 16;
        int hi;
        if (split)
          hi = Convert_MapSplit(VRAM_SB(kSbBg1Hi + k) + o,
                                VRAM_SB((bg ? kSbBg2 : kSbBg1Lo) + k) + o, &g_vram[addr + o], 16,
                                conv.blank_tile);
        else
          hi = Convert_Map(VRAM_SB((bg ? kSbBg2 : kSbBg1Lo) + k) + o, &g_vram[addr + o], 16, 0);
        cols[c & 1] += hi - counts[c];
        counts[c] = hi;
      }
    }
  }
}

static void UpdateMaps(const PpuRegs *r, const uint32_t *dirty, bool force_split, bool bg12) {
  if (bg12)
    UpdateMaps12(r, dirty, force_split);
  for (int slot = 0; slot < 2; slot++) {
    uint8_t sc = bg3_slot_sc[slot];
    if (sc == 0xff)
      continue;
    bool changed = sc != conv.bg3_slot_sc[slot];
    conv.bg3_slot_sc[slot] = sc;
    if (changed)
      memset(bg3_hi[slot], 0, sizeof(bg3_hi[slot]));
    int screens = (sc & 3) ? 2 : 1;
    for (int k = 0; k < screens; k++) {
      uint16_t addr = (((sc & 0xfc) << 8) + k * 0x400) & 0x7fff;
      FOR_EACH_DIRTY_CHUNK(dirty, addr, changed, c) {
        uint32_t o = c * 16;
        bg3_hi[slot][k * 64 + c] = Convert_Map(VRAM_SB((slot ? kSbBg3Slot1 : kSbBg3Slot0) + k) + o,
                                               &g_vram[addr + o], 16, 8 << 12);
      }
    }
    int total = 0;
    for (int i = 0; i < 128; i++)
      total += bg3_hi[slot][i];
    bg3_hi_total[slot] = total;
  }
}

// SNES sprite sizes by OBSEL, small and large, as the GBA's shape << 2 |
// size: 0-3 are squares of 8-64 pixels, 10 is 16x32 and 11 is 32x64.
static const uint8_t kObjShapes[8][2] = {
  {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}, {10, 11}, {10, 2},
};
static const uint8_t kObjPriority[4] = {3, 3, 2, 1};

// The GBA's sprite heights and widths, by kObjShapes's codes.
static const uint8_t kObjHeight[12] = {8, 16, 32, 64, 0, 0, 0, 0, 0, 0, 32, 64};
static const uint8_t kObjWidth[12] = {8, 16, 32, 64, 0, 0, 0, 0, 0, 0, 16, 32};

// Squashed sprites' transformations, in OAM: unflipped and flipped top to
// bottom. Lines are 3/4 as many.
ROM_CODE static void SetSquashMatrices(void) {
  for (int m = 0; m < 2; m++) {
    int16_t p[4] = {256, 0, 0, m ? -341 : 341};
    for (int i = 0; i < 4; i++)
      GBA_OAM[(m * 4 + i) * 4 + 3] = p[i];
  }
}

// Sprite |e| (at |x|, of GBA shape |shape|) squashed, in OAM entry |o|; and
// its first tile. Its middle goes where the SNES's would be, squashed
// (sprites below line 224 are partly above the screen).
// - 8 lines high (mostly text), as it is: squashed, its row 3 would go.
// - Higher, scaled by a matrix. Not flipped left to right by one, though:
//   the GBA flips by a scale of -1 about the middle, which takes columns
//   width..1, not width - 1..0. UpdateTiles puts each tile flipped at 31 -
//   its column, and those, in reverse order, are the sprite flipped.
ROM_CODE static int SquashSprite(volatile uint16_t *o, const uint8_t *e, int x, int shape) {
  uint8_t attr = e[3];
  int height = kObjHeight[shape], y = e[1] >= kSnesLines ? e[1] - 256 : e[1];
  int middle = (((y + height / 2) * 3) >> 2) - kSquashTop;
  int column = e[2] & 15;
  if (height == 8) {
    o[0] = ((middle - 4) & 0xff) | (shape >> 2) << 14;
    o[1] = ((x - kXOffset) & 0x1ff) | (attr & 0xc0) << 6 | (shape & 3) << 14;
  } else {
    if (attr & 0x40)
      column = 32 - column - kObjWidth[shape] / 8;
    o[0] = ((middle - height / 2) & 0xff) | 1 << 8 | (shape >> 2) << 14;
    o[1] = ((x - kXOffset) & 0x1ff) | (attr >> 7) << 9 | (shape & 3) << 14;
  }
  return (attr & 1) * 512 + (e[2] >> 4) * 32 + column;
}

static void UpdateOam(void) {
  SnesHw *h = &g_snes_hw;
  static int last_y_off = -1;
  static bool last_squash, last_bg3_mid;
  int y_off = hud ? kHudBlankLines + y_offset : y_offset;
  if (!h->oam_dirty && y_off == last_y_off && squash == last_squash && bg3_mid == last_bg3_mid)
    return;
  h->oam_dirty = false;
  last_y_off = y_off;
  last_bg3_mid = bg3_mid;
  if (squash && !last_squash)
    SetSquashMatrices();
  last_squash = squash;
  const uint8_t *shapes = kObjShapes[h->obsel >> 5];
  // The game puts the sprites it doesn't use at line 240, small, where
  // they don't show: most of them, most of the time.
  bool unused_hide = kObjHeight[shapes[0]] <= 256 - 0xf0;
  for (int i = 0; i < 128; i++) {
    const uint8_t *e = &g_oam[i * 4];
    int hi = (g_oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
    if (e[1] == 0xf0 && !(hi & 2) && unused_hide) {
      GBA_OAM[i * 4] = 1 << 9;  // hidden
      continue;
    }
    int x = e[0] | (hi & 1) << 8;
    int shape = shapes[hi >> 1];
    uint8_t attr = e[3];
    int tile_index = e[2];
    int tile = (attr & 1) * 512 + (tile_index >> 4) * 32 + (tile_index & 15);
    volatile uint16_t *o = &GBA_OAM[i * 4];
    if (squash) {
      tile = SquashSprite(o, e, x, shape);
    } else {
      o[0] = ((e[1] - y_off) & 0xff) | (shape >> 2) << 14;
      o[1] = ((x - kXOffset) & 0x1ff) | (attr & 0xc0) << 6 | (shape & 3) << 14;
    }
    int priority = (attr >> 4) & 3;
    o[2] = tile | (bg3_mid ? 3 - priority : kObjPriority[priority]) << 10 | ((attr >> 1) & 7) << 12;
  }
}

// VRAM's changes on frames with no line showing BG1-BG3 (as a door loads
// the next room's graphics, or in Mode 7), for when lines show them: then
// their tiles and maps are converted.
static uint32_t later_dirty[kVramChunks / 32];

ROM_CODE static void KeepForLater(const uint32_t *dirty) {
  for (int i = 0; i < kVramChunks / 32; i++)
    later_dirty[i] |= dirty[i];
}

ROM_CODE static void AddFromBefore(uint32_t *dirty) {
  for (int i = 0; i < kVramChunks / 32; i++)
    dirty[i] |= later_dirty[i];
  memset(later_dirty, 0, sizeof(later_dirty));
}

void Render_Present(void) {
  bool mode7 = frame_mode7, text = text_line >= 0;
  // The registers of a line with a tiled background mode, if any, for
  // where their tiles and maps are.
  const PpuRegs *r = LINE_REGS(text ? text_line : SnesLine(kGbaLines / 2));
  uint32_t dirty[kVramChunks / 32];
  memcpy(dirty, g_vram_dirty, sizeof(dirty));
  memset(g_vram_dirty, 0, sizeof(g_vram_dirty));

  if (mode7 != conv.mode7) {
    // Mode 7 and the other modes use the GBA's VRAM differently.
    memset(dirty, 0xff, sizeof(dirty));
    conv.bg12_base = conv.bg3_base = 0xffff;
    conv.bgsc[0] = conv.bgsc[1] = conv.bg3_slot_sc[0] = conv.bg3_slot_sc[1] = 0xff;
  }
  UpdatePalette(LINE_REGS(SnesLine(0)), LINE_REGS(SnesLine(kGbaLines / 2)), text);
  conv.mode7 = mode7;
  int blank_before = conv.blank_tile;
  if (mode7)
    UpdateTilesMode7(dirty);
  if (text)
    AddFromBefore(dirty);
  else
    KeepForLater(dirty);
  UpdateTiles(r, dirty, text && !mode7, text);
  if (text)
    UpdateMaps(r, dirty, conv.blank_tile != blank_before, !mode7);
  UpdateOam();

  // From now on, the VBlank interrupt sets up this frame's registers: from
  // now, if it's still VBlank. (Not partway down the screen.)
  DisplayFrame *f = &frames[back];
  f->num_patches = num_patches;
  for (int i = 0; i < num_patches; i++) {
    f->patch_index[i] = patch_index[i];
    f->patch_color[i] = patch_color[i];
    f->restore_color[i] = restore_color[i];
  }
  REG_IME = 0;
  g_display = &frames[back];
  if (REG_VCOUNT >= kGbaLines)
    Display_Start();
  REG_IME = 1;
  back ^= 1;
}



