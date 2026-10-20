// See render.h.
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
// tiles at 0x0000 and its 128x128 map at 0x4000.
// Sprite tiles keep the SNES's layout of 16 to a row, in the left half of
// the GBA's rows of 32 (2D mapping), with a copy in the right half, so that
// sprites whose tiles wrap around a row come out right.
#include "render.h"

#include <stdio.h>
#include <string.h>

#include "convert.h"
#include "gba_hw.h"
#include "snes_hw.h"
#include "sm_rtl.h"
#include "variables.h"

void Vector_IRQ(void);

enum {
  kSnesLines = 224,
  kGbaLines = 160,
  // Lines at the top of the SNES screen with the HUD, shown on the GBA
  // as they are; the rest of the GBA screen shows part of the SNES's
  // gameplay area, following Samus.
  kHudLines = 32,
  kMaxYOffset = kSnesLines - kGbaLines,
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

// The GBA's registers 0x04000000-0x0400002F for each line, which HBlank
// DMA copies in: DISPCNT, BG0CNT-BG3CNT, the scroll registers and BG2's
// affine transformation (and DISPSTAT and VCOUNT, in between, which this
// rewrites unchanged and can't write). See irq.iwram.c.
typedef struct LineIo {
  uint16_t dispcnt, green_swap, dispstat, vcount;
  uint16_t bgcnt[4];
  uint16_t ofs[8];
  int16_t bg2p[4];
  int32_t bg2x, bg2y;
} LineIo;
_Static_assert(sizeof(LineIo) == 48, "LineIo is the registers 0x00-0x2F");

static PpuRegs line_regs[kSnesLines];
// One more than the lines: DMA copies in an entry at the end of the last.
static LineIo line_io[2][kGbaLines + 1];
static int io_back;

static bool hud;
static int y_offset = kMaxYOffset / 2;
// SNES BG3 maps in GBA map slots 0 and 1, by BG3SC.
static uint8_t bg3_slot_sc[2] = {0xff, 0xff};

// What render.c last converted, to see when it has to start over.
static struct {
  uint16_t bg12_base, bg3_base, obj_base[2];
  uint8_t bgsc[2];
  uint8_t bg3_slot_sc[2];
  int blank_tile;
  int brightness;
  bool mode7;
} conv = {.blank_tile = -1, .brightness = -1};

static int SnesLine(int g) {
  return (hud && g < kHudLines) ? g : g + y_offset;
}

void Render_Init(void) {
  REG_DISPCNT = kDispcntForcedBlank;
  DmaFill32(GBA_VRAM, 0, 0x18000);
  for (int i = 0; i < 128; i++)
    GBA_OAM[i * 4] = 0x200;  // hidden
  REG_DISPSTAT = kDispstatVBlankIrq;
}

// Logs the SNES's screen layout when it changes, as what's supported grows.
static void LogLayoutChange(const PpuRegs *r) {
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

static void BuildLineIoMode7(LineIo *io, const PpuRegs *r, int s) {
  uint16_t dispcnt = kDispcntMode1;
  if (!(r->inidisp & 0x80)) {
    if (r->tm & 1) dispcnt |= kDispcntBg0 << 2;
    if (r->tm & 16) dispcnt |= kDispcntObj;
  }
  io->dispcnt = dispcnt;
  io->green_swap = 0;
  io->dispstat = kDispstatVBlankIrq;
  io->vcount = 0;
  io->bgcnt[0] = io->bgcnt[1] = io->bgcnt[3] = 0;
  // 1024x1024, wrapping around unless the SNES's field is "large".
  io->bgcnt[2] = 3 | kSbMode7 << 8 | 3 << 14 | (r->m7sel & 0x80 ? 0 : 1 << 13);
  for (int i = 0; i < 8; i++)
    io->ofs[i] = 0;
  // The SNES's screen pixel (sx, sy) shows the map's pixel
  //   ((a b) (c d)) (sx + hofs - x0, sy + vofs - y0) + (x0, y0),
  // and GBA pixel (gx, line) is SNES pixel (gx + kXOffset, s).
  int a = r->m7[0], b = r->m7[1], c = r->m7[2], d = r->m7[3];
  int x0 = r->m7[4], y0 = r->m7[5];
  int dx = kXOffset + Sext13(r->m7hofs) - x0;
  int dy = s + Sext13(r->m7vofs) - y0;
  io->bg2p[0] = a;
  io->bg2p[1] = b;
  io->bg2p[2] = c;
  io->bg2p[3] = d;
  io->bg2x = a * dx + b * dy + (x0 << 8);
  io->bg2y = c * dx + d * dy + (y0 << 8);
}

static void BuildLineIo(LineIo *io, const PpuRegs *r, int s, int g) {
  if ((r->bgmode & 7) == 7) {
    BuildLineIoMode7(io, r, s);
    return;
  }
  uint16_t dispcnt = kDispcntMode0;
  if (!(r->inidisp & 0x80)) {
    if (r->tm & 1) dispcnt |= kDispcntBg0 << 1 | kDispcntBg0 << 2;
    if (r->tm & 2) dispcnt |= kDispcntBg0 << 3;
    if (r->tm & 4) dispcnt |= kDispcntBg0;
    if (r->tm & 16) dispcnt |= kDispcntObj;
  }
  io->dispcnt = dispcnt;
  io->green_swap = 0;
  io->dispstat = kDispstatVBlankIrq;
  io->vcount = 0;
  int bg3_sb = Bg3Slot(r->bgsc[2]) ? kSbBg3Slot1 : kSbBg3Slot0;
  io->bgcnt[0] = ((r->bgmode & 8) ? 0 : 3) | 2 << 2 | bg3_sb << 8 | BgSize(r->bgsc[2]) << 14;
  io->bgcnt[1] = 1 | kSbBg1Hi << 8 | BgSize(r->bgsc[0]) << 14;
  io->bgcnt[2] = 2 | kSbBg1Lo << 8 | BgSize(r->bgsc[0]) << 14;
  io->bgcnt[3] = 2 | kSbBg2 << 8 | BgSize(r->bgsc[1]) << 14;
  int dy = s - g;
  io->ofs[0] = r->hofs[2] + kXOffset;
  io->ofs[1] = r->vofs[2] + dy;
  io->ofs[2] = io->ofs[4] = r->hofs[0] + kXOffset;
  io->ofs[3] = io->ofs[5] = r->vofs[0] + dy;
  io->ofs[6] = r->hofs[1] + kXOffset;
  io->ofs[7] = r->vofs[1] + dy;
  io->bg2p[0] = io->bg2p[3] = 0x100;
  io->bg2p[1] = io->bg2p[2] = 0;
  io->bg2x = io->bg2y = 0;
}

static void UpdateYOffset(void) {
  int target;
  if (hud) {
    // Samus in the middle of the gameplay area, as far as it goes.
    int samus_y = (int16_t)(samus_y_pos - layer1_y_pos);
    target = samus_y - (kHudLines + (kGbaLines - kHudLines) / 2);
    target = target < 0 ? 0 : target > kMaxYOffset ? kMaxYOffset : target;
  } else {
    target = kMaxYOffset / 2;
  }
  int d = target - y_offset;
  y_offset += d > 4 ? 4 : d < -4 ? -4 : d;
}

void Render_CaptureFrame(void) {
  SnesHw *h = &g_snes_hw;
  SnesHw_HdmaInit();
  for (int v = 0; v < kSnesLines; v++) {
    line_regs[v] = h->r;
    SnesHw_HdmaRunLine();
    if ((h->nmitimen & 0x30) && v == h->vtime)
      Vector_IRQ();
  }

  hud = line_regs[8].tm == 4 && line_regs[kHudLines + 8].tm != 4;
  UpdateYOffset();

  // The BG3 maps the lines use, in slots.
  uint8_t first = line_regs[SnesLine(0)].bgsc[2];
  uint8_t second = first;
  for (int g = 1; g < kGbaLines; g++) {
    uint8_t sc = line_regs[SnesLine(g)].bgsc[2];
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

  LineIo *io = line_io[io_back];
  for (int g = 0; g < kGbaLines; g++) {
    int s = SnesLine(g);
    BuildLineIo(&io[g], &line_regs[s], s, g);
  }
  io[kGbaLines] = io[kGbaLines - 1];
  LogLayoutChange(&line_regs[SnesLine(kGbaLines / 2)]);
}

static void UpdatePalette(const PpuRegs *r, bool mode7) {
  SnesHw *h = &g_snes_hw;
  int b = (r->inidisp & 0x80) ? 0 : (r->inidisp & 15);
  if (!h->cgram_dirty && b == conv.brightness && mode7 == conv.mode7)
    return;
  h->cgram_dirty = false;
  conv.brightness = b;
  uint16_t pal[256];
  if (b == 15) {
    memcpy(pal, h->cgram, sizeof(pal));
  } else {
    uint8_t scale[32];
    for (int i = 0; i < 32; i++)
      scale[i] = b ? (i * (b + 1)) >> 4 : 0;
    for (int i = 0; i < 256; i++) {
      uint16_t c = h->cgram[i];
      pal[i] = scale[c & 31] | scale[(c >> 5) & 31] << 5 | scale[(c >> 10) & 31] << 10;
    }
  }
  DmaCopy32(GBA_BG_PALETTE, pal, 256);
  DmaCopy32(GBA_OBJ_PALETTE, pal + 128, 256);
  // BG3's 2bpp palettes of 4 colors in GBA palettes 8-15. (Mode 7's
  // background has all 256 colors.)
  for (int p = 0; p < 8 && !mode7; p++) {
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

// 1024 words, aligned.
static bool ScreenDirty(const uint32_t *dirty, uint16_t addr) {
  uint32_t w = (addr & 0x7fff) >> (kVramChunkShift + 5);
  return dirty[w] | dirty[w + 1];
}

static void UpdateTilesMode7(const uint32_t *dirty) {
  SnesHw *h = &g_snes_hw;
  // Each word of the first 32 KB is a map entry (low byte) and a pixel
  // (high byte).
  volatile uint16_t *tiles = GBA_VRAM, *map = VRAM_SB(kSbMode7);
  for (uint32_t w = 0; w < 0x4000 >> (kVramChunkShift + 5); w++) {
    uint32_t bits = dirty[w];
    while (bits) {
      uint32_t c = w * 32 + __builtin_ctz(bits);
      bits &= bits - 1;
      uint32_t addr = c << kVramChunkShift;
      const uint16_t *src = &h->vram[addr];
      for (int i = 0; i < 16; i += 2) {
        uint16_t w0 = src[i], w1 = src[i + 1];
        map[(addr + i) >> 1] = (w0 & 0xff) | (w1 & 0xff) << 8;
        tiles[(addr + i) >> 1] = (w0 >> 8) | (w1 & 0xff00);
      }
    }
  }
}

static void UpdateTiles(const PpuRegs *r, uint32_t *dirty, bool mode7) {
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
      const uint16_t *src = &h->vram[addr];
      uint16_t off;
      if (!mode7) {
        off = (addr - bg12_base) & 0x7fff;
        if (off < 0x4000)
          Convert_Tiles4bpp(vram32 + (off >> 4) * 8, src, 1);
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
    if (blank < 0 || !TileIsBlank(&h->vram[(bg12_base + blank * 16) & 0x7fff])) {
      blank = 0;
      for (int i = 1023; i >= 0; i--) {
        if (TileIsBlank(&h->vram[(bg12_base + i * 16) & 0x7fff])) {
          blank = i;
          break;
        }
      }
    }
  }
  conv.blank_tile = blank;
}

static void UpdateMaps(const PpuRegs *r, const uint32_t *dirty, bool force_bg1) {
  SnesHw *h = &g_snes_hw;
  for (int bg = 0; bg < 2; bg++) {
    uint8_t sc = r->bgsc[bg];
    bool changed = sc != conv.bgsc[bg] || (bg == 0 && force_bg1);
    conv.bgsc[bg] = sc;
    int screens = (sc & 3) ? 2 : 1;
    for (int k = 0; k < screens; k++) {
      uint16_t addr = (((sc & 0xfc) << 8) + k * 0x400) & 0x7fff;
      if (!changed && !ScreenDirty(dirty, addr))
        continue;
      if (bg == 0)
        Convert_MapSplit(VRAM_SB(kSbBg1Hi + k), VRAM_SB(kSbBg1Lo + k), &h->vram[addr], 1024,
                         conv.blank_tile);
      else
        Convert_Map(VRAM_SB(kSbBg2 + k), &h->vram[addr], 1024, 0);
    }
  }
  for (int slot = 0; slot < 2; slot++) {
    uint8_t sc = bg3_slot_sc[slot];
    if (sc == 0xff)
      continue;
    bool changed = sc != conv.bg3_slot_sc[slot];
    conv.bg3_slot_sc[slot] = sc;
    int screens = (sc & 3) ? 2 : 1;
    for (int k = 0; k < screens; k++) {
      uint16_t addr = (((sc & 0xfc) << 8) + k * 0x400) & 0x7fff;
      if (changed || ScreenDirty(dirty, addr))
        Convert_Map(VRAM_SB((slot ? kSbBg3Slot1 : kSbBg3Slot0) + k), &h->vram[addr], 1024, 8 << 12);
    }
  }
}

// SNES sprite sizes by OBSEL, small and large, as the GBA's shape << 2 |
// size: 0-3 are squares of 8-64 pixels, 10 is 16x32 and 11 is 32x64.
static const uint8_t kObjShapes[8][2] = {
  {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}, {10, 11}, {10, 2},
};
static const uint8_t kObjPriority[4] = {3, 3, 2, 1};

static void UpdateOam(void) {
  SnesHw *h = &g_snes_hw;
  if (!h->oam_dirty)
    return;
  h->oam_dirty = false;
  const uint8_t *shapes = kObjShapes[h->obsel >> 5];
  int y_off = hud ? y_offset : kMaxYOffset / 2;
  for (int i = 0; i < 128; i++) {
    const uint8_t *e = &h->oam[i * 4];
    int hi = (h->oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
    int x = e[0] | (hi & 1) << 8;
    int shape = shapes[hi >> 1];
    uint8_t attr = e[3];
    int tile_index = e[2];
    int tile = (attr & 1) * 512 + (tile_index >> 4) * 32 + (tile_index & 15);
    volatile uint16_t *o = &GBA_OAM[i * 4];
    o[0] = ((e[1] - y_off) & 0xff) | (shape >> 2) << 14;
    o[1] = ((x - kXOffset) & 0x1ff) | (attr & 0xc0) << 6 | (shape & 3) << 14;
    o[2] = tile | kObjPriority[(attr >> 4) & 3] << 10 | ((attr >> 1) & 7) << 12;
  }
}


void Render_Present(void) {
  SnesHw *h = &g_snes_hw;
  const PpuRegs *r = &line_regs[SnesLine(kGbaLines / 2)];
  uint32_t dirty[kVramChunks / 32];
  memcpy(dirty, h->vram_dirty, sizeof(dirty));
  memset(h->vram_dirty, 0, sizeof(h->vram_dirty));

  bool mode7 = (r->bgmode & 7) == 7;
  if (mode7 != conv.mode7) {
    // Mode 7 and the other modes use the GBA's VRAM differently.
    memset(dirty, 0xff, sizeof(dirty));
    conv.bg12_base = conv.bg3_base = 0xffff;
    conv.bgsc[0] = conv.bgsc[1] = conv.bg3_slot_sc[0] = conv.bg3_slot_sc[1] = 0xff;
  }
  UpdatePalette(&line_regs[SnesLine(0)], mode7);
  conv.mode7 = mode7;
  int blank_before = conv.blank_tile;
  if (mode7)
    UpdateTilesMode7(dirty);
  UpdateTiles(r, dirty, mode7);
  if (!mode7)
    UpdateMaps(r, dirty, conv.blank_tile != blank_before);
  UpdateOam();

  // The VBlank interrupt sets up line 0 and starts HBlank DMA for the
  // rest, from now on with this frame's registers.
  extern const uint32_t *volatile g_line_regs;
  LineIo *io = line_io[io_back];
  g_line_regs = (const uint32_t *)io;
  volatile uint32_t *regs = (volatile uint32_t *)0x04000000;
  for (int i = 0; i < 12; i++)
    regs[i] = ((const uint32_t *)io)[i];
  io_back ^= 1;
}

