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
// this keeps SNES Mode 1's order (BG2's priority tiles aside).
//
// GBA VRAM:
//   0x0000-0x7FFF  BG1/BG2 tiles (the SNES's 1024 at BG1's base)
//   0x8000-0xAFFF  BG3 tiles, converted to 4bpp (the first 384)
//   0xB000-0xBFFF  BG3 map, slot 0 (screen blocks 22-23)
//   0xC000-0xCFFF  BG1 priority tiles' map (24-25)
//   0xD000-0xDFFF  BG1 other tiles' map (26-27)
//   0xE000-0xEFFF  BG2 map (28-29)
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
static PpuRegs line_states[kSnesLines];
static uint8_t line_state[kSnesLines];
#define LINE_REGS(s) (&line_states[line_state[s]])
// Two of each, for the frame on show and the next.
static uint32_t streams[2][kGbaLines + 1][4] IWRAM_BSS;
static DisplaySegment segments[2][kGbaLines];
static DisplayFrame frames[2];
static int back;

static bool hud;
// Outside gameplay (the title, menus, the pause screen), the SNES screen
// squashed into the GBA's: 6 of each 8 lines, so that the same two rows of
// each row of tiles go, and text stays legible (the SNES's 224 lines make
// 168, less 4 at the top and bottom). kSquashLine[g] is the SNES line on
// GBA line g, and sprites are scaled to match.
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
// everything, with BGMODE's bit 3) and the others (at the back): a slot
// with any goes in front.
static uint8_t bg3_hi[2][128];
static int bg3_hi_total[2];

// What render.c last converted, to see when it has to start over.
static struct {
  uint16_t bg12_base, bg3_base, obj_base[2];
  uint8_t bgsc[2];
  uint8_t bg3_slot_sc[2];
  int blank_tile;
  int brightness;
  bool mode7, bg3_palettes;
} conv = {.blank_tile = -1, .brightness = -1};

static int SnesLine(int g) {
  if (squash)
    return kSquashLine[g];
  if (!hud)
    return g + y_offset;
  return g < kGbaHudLines ? g + kHudBlankLines : g + kHudBlankLines + y_offset;
}

ROM_CODE void Render_Init(void) {
  // Rows 3 and 7 of each 8 go: fonts keep their tops and bottoms.
  for (int s = 0, g = -kSquashTop; g < kGbaLines; s++) {
    if ((s & 3) == 3)
      continue;
    if (g >= 0)
      kSquashLine[g] = s;
    g++;
  }
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
  // 64x64 maps get only their top half: there are two screens per map.
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

static void BuildLine(LineRegs *l, const PpuRegs *r, int s, int g) {
  if ((r->bgmode & 7) == 7) {
    BuildLineMode7(l, r, s);
    return;
  }
  uint16_t dispcnt = frame_mode7 ? kDispcntMode1 : kDispcntMode0;
  if (!(r->inidisp & 0x80)) {
    // With Mode 7 lines in the frame, BG1 and BG2's VRAM holds Mode 7's.
    if ((r->tm & 1) && !frame_mode7) dispcnt |= kDispcntBg0 << 1 | kDispcntBg0 << 2;
    if ((r->tm & 2) && !frame_mode7) dispcnt |= kDispcntBg0 << 3;
    if (r->tm & 4) dispcnt |= kDispcntBg0;
    if (r->tm & 16) dispcnt |= kDispcntObj;
  }
  l->ctl.dispcnt = dispcnt;
  int bg3_sb = Bg3Slot(r->bgsc[2]) ? kSbBg3Slot1 : kSbBg3Slot0;
  int bg3_front = (r->bgmode & 8) && bg3_hi_total[Bg3Slot(r->bgsc[2])];
  l->ctl.bgcnt[0] = (bg3_front ? 0 : 3) | 2 << 2 | bg3_sb << 8 | BgSize(r->bgsc[2]) << 14;
  l->ctl.bgcnt[1] = 1 | kSbBg1Hi << 8 | BgSize(r->bgsc[0]) << 14;
  l->ctl.bgcnt[2] = 2 | kSbBg1Lo << 8 | BgSize(r->bgsc[0]) << 14;
  l->ctl.bgcnt[3] = 2 | kSbBg2 << 8 | BgSize(r->bgsc[1]) << 14;
  int dy = s - g;
  uint16_t ofs[8];
  ofs[0] = r->hofs[2] + kXOffset;
  ofs[1] = r->vofs[2] + dy;
  ofs[2] = ofs[4] = r->hofs[0] + kXOffset;
  ofs[3] = ofs[5] = r->vofs[0] + dy;
  ofs[6] = r->hofs[1] + kXOffset;
  ofs[7] = r->vofs[1] + dy;
  // The scrolling is the stream, but in frames with Mode 7, whose stream
  // is BG2's transformation.
  for (int i = 0; i < 4; i++) {
    uint32_t w = ofs[2 * i] | (uint32_t)ofs[2 * i + 1] << 16;
    l->stream[i] = frame_mode7 ? 0 : w;
    ((uint32_t *)l->ctl.ofs)[i] = frame_mode7 ? w : 0;
  }
  if (frame_mode7)
    l->stream[0] = l->stream[1] = 0x100;  // BG2's identity, for what it's worth
}

// But for first_line. (Not with memcmp: it's in Thumb code, and slow.)
static bool SameSegment(const DisplaySegment *a, const DisplaySegment *b) {
  const uint16_t *x = &a->dispcnt, *y = &b->dispcnt;
  for (int i = 0; i < 13; i++) {
    if (x[i] != y[i])
      return false;
  }
  return true;
}

static void UpdateYOffset(void) {
  int target;
  if (hud) {
    // Samus in the middle of the gameplay area, as far as it goes.
    int samus_y = (int16_t)(samus_y_pos - layer1_y_pos);
    target = samus_y - (kHudLines + (kGbaLines - kGbaHudLines) / 2);
    target = target < 0 ? 0 : target > kMaxYOffset ? kMaxYOffset : target;
  } else {
    target = kMaxYOffset / 2;
  }
  int d = target - y_offset;
  y_offset += d > 4 ? 4 : d < -4 ? -4 : d;
}

void Render_CaptureFrame(void) {
  SnesHw_RunDisplay(line_states, line_state);

  squash = game_state < 7 || game_state > 11;
  hud = !squash && LINE_REGS(8)->tm == 4 && LINE_REGS(kHudLines + 8)->tm != 4;
  UpdateYOffset();

  // The BG3 maps the lines use, in slots.
  uint8_t first = LINE_REGS(SnesLine(0))->bgsc[2];
  uint8_t second = first;
  for (int g = 1; g < kGbaLines; g++) {
    uint8_t sc = LINE_REGS(SnesLine(g))->bgsc[2];
    if (sc != first) {
      second = sc;
      break;
    }
  }
  if (first != bg3_slot_sc[0] && first != bg3_slot_sc[1]) {
    // Keep the other one where it is.
    bg3_slot_sc[second == bg3_slot_sc[0] ? 1 : 0] = first;
  }
  if (second != bg3_slot_sc[0] && second != bg3_slot_sc[1])
    bg3_slot_sc[first == bg3_slot_sc[0] ? 1 : 0] = second;

  frame_mode7 = false;
  text_line = -1;
  for (int g = 0; g < kGbaLines; g++) {
    if ((LINE_REGS(SnesLine(g))->bgmode & 7) == 7)
      frame_mode7 = true;
    else if (text_line < 0 || g == kGbaLines / 2)
      text_line = SnesLine(g);
  }

  // Lines with the same registers as the one before (and the same
  // vertical offset) have the same GBA registers, but for Mode 7's
  // reference point, which moves down the map a line at a time.
  uint32_t(*stream)[4] = streams[back];
  DisplaySegment *segs = segments[back];
  int num_segs = 0;
  LineRegs l;
  int prev_state = -1, prev_dy = 0;
  for (int g = 0; g < kGbaLines; g++) {
    int s = SnesLine(g), state = line_state[s];
    const PpuRegs *r = &line_states[state];
    if (state == prev_state && s - g == prev_dy) {
      if ((r->bgmode & 7) == 7)
        StepLineMode7(&l);
    } else {
      BuildLine(&l, r, s, g);
      if (!num_segs || !SameSegment(&segs[num_segs - 1], &l.ctl)) {
        segs[num_segs] = l.ctl;
        segs[num_segs++].first_line = g;
      }
    }
    for (int i = 0; i < 4; i++)
      stream[g][i] = l.stream[i];
    prev_state = state;
    prev_dy = s - g;
  }
  for (int i = 0; i < 4; i++)
    stream[kGbaLines][i] = stream[kGbaLines - 1][i];
  DisplayFrame *f = &frames[back];
  f->stream = (const uint32_t(*)[4])stream;
  f->stream_dest = frame_mode7 ? 0x04000020 : 0x04000010;
  f->num_segments = num_segs;
  f->segments = segs;
  LogLayoutChange(LINE_REGS(SnesLine(kGbaLines / 2)));
}

ROM_CODE static void UpdatePalette(const PpuRegs *r, bool bg3_palettes) {
  SnesHw *h = &g_snes_hw;
  int b = (r->inidisp & 0x80) ? 0 : (r->inidisp & 15);
  if (!h->cgram_dirty && b == conv.brightness && bg3_palettes == conv.bg3_palettes)
    return;
  h->cgram_dirty = false;
  conv.brightness = b;
  conv.bg3_palettes = bg3_palettes;
  uint16_t pal[256];
  if (b == 15) {
    memcpy(pal, g_cgram, sizeof(pal));
  } else {
    uint8_t scale[32];
    for (int i = 0; i < 32; i++)
      scale[i] = b ? (i * (b + 1)) >> 4 : 0;
    for (int i = 0; i < 256; i++) {
      uint16_t c = g_cgram[i];
      pal[i] = scale[c & 31] | scale[(c >> 5) & 31] << 5 | scale[(c >> 10) & 31] << 10;
    }
  }
  DmaCopy32(GBA_BG_PALETTE, pal, 256);
  DmaCopy32(GBA_OBJ_PALETTE, pal + 128, 256);
  // BG3's 2bpp palettes of 4 colors in GBA palettes 8-15. (Mode 7's
  // background has all 256 colors: frames with both are a compromise.)
  for (int p = 0; p < 8 && bg3_palettes; p++) {
    for (int k = 0; k < 4; k++)
      GBA_BG_PALETTE[128 + p * 16 + k] = pal[p * 4 + k];
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

// Runs the statement after it for each chunk k (declared here) of the 64
// in the map screen at addr (1024 words, aligned) that changed, or all.
#define FOR_EACH_DIRTY_CHUNK(dirty, addr, all, k)                                          \
  for (uint64_t bits_ = (all) ? ~0ull                                                      \
                              : (uint64_t)(dirty)[((addr) >> 9) + 1] << 32 |              \
                                    (dirty)[(addr) >> 9],                                  \
                k = 0;                                                                     \
       bits_; bits_ >>= 1, k++)                                                            \
    if (bits_ & 1)

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
      obj1 != conv.obj_base[1]) {
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
          Convert_Tiles4bpp(vram32 + (off >> 4) * 8, src, 1);
      }
      if (bg3) {
        off = (addr - bg3_base) & 0x7fff;
        if (off < kBg3Tiles * 8)
          Convert_Tiles2bpp(vram32 + 0x8000 / 4 + (off >> 3) * 8, src, 2);
      }
      for (int t = 0; t < 2; t++) {
        off = (addr - conv.obj_base[t]) & 0x7fff;
        if (off < 0x1000) {
          int i = off >> 4;
          int tile = t * 512 + (i >> 4) * 32 + (i & 15);
          Convert_Tiles4bpp(obj32 + tile * 8, src, 1);
          Convert_Tiles4bpp(obj32 + (tile + 16) * 8, src, 1);
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

static void UpdateMaps(const PpuRegs *r, const uint32_t *dirty, bool force_bg1, bool bg12) {
  for (int bg = 0; bg < 2 && bg12; bg++) {
    uint8_t sc = r->bgsc[bg];
    bool changed = sc != conv.bgsc[bg] || (bg == 0 && force_bg1);
    conv.bgsc[bg] = sc;
    int screens = (sc & 3) ? 2 : 1;
    for (int k = 0; k < screens; k++) {
      uint16_t addr = (((sc & 0xfc) << 8) + k * 0x400) & 0x7fff;
      FOR_EACH_DIRTY_CHUNK(dirty, addr, changed, c) {
        uint32_t o = c * 16;
        if (bg == 0)
          Convert_MapSplit(VRAM_SB(kSbBg1Hi + k) + o, VRAM_SB(kSbBg1Lo + k) + o,
                           &g_vram[addr + o], 16, conv.blank_tile);
        else
          Convert_Map(VRAM_SB(kSbBg2 + k) + o, &g_vram[addr + o], 16, 0);
      }
    }
  }
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
        Convert_Map(VRAM_SB((slot ? kSbBg3Slot1 : kSbBg3Slot0) + k) + o, &g_vram[addr + o], 16,
                    8 << 12);
        int hi = 0;
        for (int i = 0; i < 16; i++)
          hi += (g_vram[addr + o + i] >> 13) & 1;
        bg3_hi[slot][k * 64 + c] = hi;
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

// The GBA's sprite heights, by kObjShapes's codes.
static const uint8_t kObjHeight[12] = {8, 16, 32, 64, 0, 0, 0, 0, 0, 0, 32, 64};

// Squashed sprites' transformations, in OAM: no flip, horizontal, vertical,
// and both. Lines are 3/4 as many.
ROM_CODE static void SetSquashMatrices(void) {
  for (int m = 0; m < 4; m++) {
    int16_t p[4] = {m & 1 ? -256 : 256, 0, 0, m & 2 ? -341 : 341};
    for (int i = 0; i < 4; i++)
      GBA_OAM[(m * 4 + i) * 4 + 3] = p[i];
  }
}

static void UpdateOam(void) {
  SnesHw *h = &g_snes_hw;
  static int last_y_off = -1;
  static bool last_squash;
  int y_off = hud ? kHudBlankLines + y_offset : y_offset;
  if (!h->oam_dirty && y_off == last_y_off && squash == last_squash)
    return;
  h->oam_dirty = false;
  last_y_off = y_off;
  if (squash && !last_squash)
    SetSquashMatrices();
  last_squash = squash;
  const uint8_t *shapes = kObjShapes[h->obsel >> 5];
  for (int i = 0; i < 128; i++) {
    const uint8_t *e = &g_oam[i * 4];
    int hi = (g_oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
    int x = e[0] | (hi & 1) << 8;
    int shape = shapes[hi >> 1];
    uint8_t attr = e[3];
    int tile_index = e[2];
    int tile = (attr & 1) * 512 + (tile_index >> 4) * 32 + (tile_index & 15);
    volatile uint16_t *o = &GBA_OAM[i * 4];
    if (squash) {
      // Its middle where the SNES's would be, squashed: sprites below line
      // 224 are partly above the screen.
      int height = kObjHeight[shape], y = e[1] >= kSnesLines ? e[1] - 256 : e[1];
      int middle = (((y + height / 2) * 3) >> 2) - kSquashTop;
      o[0] = ((middle - height / 2) & 0xff) | 1 << 8 | (shape >> 2) << 14;
      o[1] = ((x - kXOffset) & 0x1ff) | (attr >> 6) << 9 | (shape & 3) << 14;
    } else {
      o[0] = ((e[1] - y_off) & 0xff) | (shape >> 2) << 14;
      o[1] = ((x - kXOffset) & 0x1ff) | (attr & 0xc0) << 6 | (shape & 3) << 14;
    }
    o[2] = tile | kObjPriority[(attr >> 4) & 3] << 10 | ((attr >> 1) & 7) << 12;
  }
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
  UpdatePalette(LINE_REGS(SnesLine(0)), text);
  conv.mode7 = mode7;
  int blank_before = conv.blank_tile;
  if (mode7)
    UpdateTilesMode7(dirty);
  UpdateTiles(r, dirty, text && !mode7, text);
  if (text)
    UpdateMaps(r, dirty, conv.blank_tile != blank_before, !mode7);
  UpdateOam();

  // From now on, the VBlank interrupt sets up this frame's registers.
  REG_IME = 0;
  g_display = &frames[back];
  Display_Start();
  REG_IME = 1;
  back ^= 1;
}



