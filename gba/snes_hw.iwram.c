// See snes_hw.h. Register behavior follows src/snes/ppu.c, snes.c and
// dma.c, the emulator the PC build checks the game against.
//
// ARM code in IWRAM, as the game's DMA and the SNES's display of each frame
// run through here. (So no division: see cmake/compiler-rt.cmake.)
#include "snes_hw.h"

#include <stddef.h>
#include <string.h>

#include "gba_hw.h"
#include "sm_rtl.h"

// (Aligned: SameRegs compares its registers' words.)
SnesHw g_snes_hw IWRAM_BSS __attribute__((aligned(4)));
uint16_t g_cgram[256] __attribute__((aligned(4)));
uint8_t g_oam[544] __attribute__((aligned(4)));
uint16_t g_vram[kVramWords];
uint32_t g_vram_dirty[kVramChunks / 32];
// The game wrote HDMAEN: HDMA's decoded channels are out of date.
static bool hdma_dirty IWRAM_BSS;

static uint32_t wram_port_addr;

ROM_CODE ROM_CODE void SnesHw_Reset(void) {
  memset(&g_snes_hw, 0, sizeof(g_snes_hw));
  g_snes_hw.vram_inc = 1;
  for (int i = 0; i < 8; i++) {
    SnesDmaChannel *c = &g_snes_hw.dma[i];
    c->dmap = 0xff;
    c->bbad = 0xff;
    c->a1t = 0xffff;
    c->a1b = 0xff;
    c->das = 0xffff;
    c->dasb = 0xff;
    c->a2a = 0xffff;
    c->ntrl = 0xff;
  }
  memset(g_vram_dirty, 0xff, sizeof(g_vram_dirty));
  g_snes_hw.vram_dirty = g_snes_hw.cgram_dirty = true;
  g_snes_hw.cgram_changed = g_snes_hw.oam_changed = ~0u;
}

static inline void MarkVramDirty(uint16_t addr) {
  uint32_t chunk = (addr & 0x7fff) >> kVramChunkShift;
  g_vram_dirty[chunk >> 5] |= 1u << (chunk & 31);
  g_snes_hw.vram_dirty = true;
}

// And counts the chunk (g_vram_dirty_marks) if it wasn't dirty: for the
// paths that go a word at a time. (A function: they're the rare ones.)
uint32_t g_vram_dirty_marks IWRAM_BSS;
__attribute__((noinline)) static void MarkVramDirtyCounted(uint16_t addr) {
  uint32_t chunk = (addr & 0x7fff) >> kVramChunkShift, bit = 1u << (chunk & 31);
  uint32_t *w = &g_vram_dirty[chunk >> 5];
  g_snes_hw.vram_dirty = true;
  if (!(*w & bit)) {
    *w |= bit;
    g_vram_dirty_marks++;
  }
}

ROM_CODE void SnesHw_WriteVram(uint16_t addr, const uint16_t *src, int words) {
  for (int i = 0; i < words; i++) {
    g_vram[(addr + i) & 0x7fff] = src[i];
    MarkVramDirtyCounted(addr + i);
  }
}

ROM_CODE uint8_t SnesHw_ReadA(uint32_t addr) {
  uint8_t bank = addr >> 16;
  uint16_t a = addr;
  if (bank == 0x7e || bank == 0x7f)
    return g_ram[((bank & 1) << 16) | a];
  if (a < 0x2000 && ((bank & 0x7f) < 0x40))
    return g_ram[a];
  return *RomFixedPtr(addr);
}

static void WriteA(uint32_t addr, uint8_t val) {
  uint8_t bank = addr >> 16;
  uint16_t a = addr;
  if (bank == 0x7e || bank == 0x7f)
    g_ram[((bank & 1) << 16) | a] = val;
  else if (a < 0x2000 && ((bank & 0x7f) < 0x40))
    g_ram[a] = val;
}

static uint16_t VramRemap(void) {
  uint16_t adr = g_snes_hw.vram_ptr;
  switch ((g_snes_hw.vmain >> 2) & 3) {
  case 0: return adr;
  case 1: return (adr & 0xff00) | ((adr & 0xe0) >> 5) | ((adr & 0x1f) << 3);
  case 2: return (adr & 0xfe00) | ((adr & 0x1c0) >> 6) | ((adr & 0x3f) << 3);
  default: return (adr & 0xfc00) | ((adr & 0x380) >> 7) | ((adr & 0x7f) << 3);
  }
}

// The PPU registers below 0x34 that aren't in PpuRegs's states, by bit:
// OBSEL, the ports to OAM, VRAM and CGRAM, and BG2-BG4's scrolling (in states
// of its own: BG1's is too, but Mode 7's shares its registers).
static const uint32_t kNotPpuRegs[2] = {
  1u << 0x01 | 1u << 0x02 | 1u << 0x03 | 1u << 0x04 | 1u << 0x0f | 1u << 0x10 | 1u << 0x11 |
      1u << 0x12 | 1u << 0x13 | 1u << 0x14 | 1u << 0x15 | 1u << 0x16 | 1u << 0x17 |
      1u << 0x18 | 1u << 0x19,
  1u << (0x21 - 32) | 1u << (0x22 - 32),
};

static void WriteBBus(uint8_t adr, uint8_t val) {
  SnesHw *h = &g_snes_hw;
  PpuRegs *r = &h->r;
  if (adr < 0x34 && !((kNotPpuRegs[adr >> 5] >> (adr & 31)) & 1))
    h->regs_dirty = true;
  switch (adr) {
  case 0x00: r->inidisp = val; break;
  case 0x01:
    if (val != h->obsel)
      h->oam_changed = ~0u;  // (the sprites' sizes)
    h->obsel = val;
    break;
  case 0x02:
    h->oam_addr_reload = (h->oam_addr_reload & 0x200) | (val << 1);
    h->oam_addr = h->oam_addr_reload;
    break;
  case 0x03:
    h->oam_addr_reload = (h->oam_addr_reload & 0x1fe) | ((val & 1) << 9);
    h->oam_addr = h->oam_addr_reload;
    break;
  case 0x04: {
    uint16_t a = h->oam_addr;
    if (a < 0x200) {
      if (!(a & 1)) {
        h->oam_latch = val;
      } else {
        g_oam[a - 1] = h->oam_latch;
        g_oam[a] = val;
      }
    } else {
      g_oam[0x200 + (a & 0x1f)] = val;
    }
    h->oam_changed = ~0u;
    h->oam_addr = (a + 1) & 0x3ff;
    break;
  }
  case 0x05: r->bgmode = val; break;
  case 0x06: r->mosaic = val; break;
  case 0x07: case 0x08: case 0x09: case 0x0a: r->bgsc[adr - 7] = val; break;
  case 0x0b: r->bgnba[0] = val; break;
  case 0x0c: r->bgnba[1] = val; break;
  case 0x0d:
    r->m7hofs = ((val << 8) | h->m7_latch) & 0x1fff;
    h->m7_latch = val;
    // fallthrough
  case 0x0f: case 0x11: case 0x13:
    r->hofs[(adr - 0xd) >> 1] = ((val << 8) | (h->bgofs_latch & 0xf8) | (h->bghofs_latch & 7)) & 0x3ff;
    h->bgofs_latch = val;
    h->bghofs_latch = val;
    h->scroll_dirty = true;
    h->scroll_written |= 1 << ((adr - 0xd) >> 1);
    break;
  case 0x0e:
    r->m7vofs = ((val << 8) | h->m7_latch) & 0x1fff;
    h->m7_latch = val;
    // fallthrough
  case 0x10: case 0x12: case 0x14:
    r->vofs[(adr - 0xe) >> 1] = ((val << 8) | h->bgofs_latch) & 0x3ff;
    h->bgofs_latch = val;
    h->scroll_dirty = true;
    h->scroll_written |= 0x10 << ((adr - 0xe) >> 1);
    break;
  case 0x15:
    h->vram_inc = (val & 3) == 0 ? 1 : (val & 3) == 1 ? 32 : 128;
    h->vmain = val;
    break;
  case 0x16:
    h->vram_ptr = (h->vram_ptr & 0xff00) | val;
    h->vram_read_buf = g_vram[VramRemap() & 0x7fff];
    break;
  case 0x17:
    h->vram_ptr = (h->vram_ptr & 0x00ff) | (val << 8);
    h->vram_read_buf = g_vram[VramRemap() & 0x7fff];
    break;
  case 0x18: {
    uint16_t a = VramRemap() & 0x7fff;

    g_vram[a] = (g_vram[a] & 0xff00) | val;
    MarkVramDirtyCounted(a);
    if (!(h->vmain & 0x80))
      h->vram_ptr += h->vram_inc;
    break;
  }
  case 0x19: {
    uint16_t a = VramRemap() & 0x7fff;

    g_vram[a] = (g_vram[a] & 0x00ff) | (val << 8);
    MarkVramDirtyCounted(a);
    if (h->vmain & 0x80)
      h->vram_ptr += h->vram_inc;
    break;
  }
  case 0x1a: r->m7sel = val; break;
  case 0x1b: case 0x1c: case 0x1d: case 0x1e:
    r->m7[adr - 0x1b] = (val << 8) | h->m7_latch;
    h->m7_latch = val;
    break;
  case 0x1f: case 0x20:
    // 13-bit signed.
    r->m7[adr - 0x1b] = (int16_t)(((val << 8) | h->m7_latch) << 3) >> 3;
    h->m7_latch = val;
    break;
  case 0x21:
    h->cgram_ptr = val;
    h->cgram_high = false;
    break;
  case 0x22:
    if (!h->cgram_high) {
      h->cgram_latch = val;
    } else {
      h->cgram_changed = ~0u;
      g_cgram[h->cgram_ptr++] = ((val & 0x7f) << 8) | h->cgram_latch;
      h->cgram_dirty = true;
    }
    h->cgram_high = !h->cgram_high;
    break;
  case 0x23: case 0x24: case 0x25: r->wsel[adr - 0x23] = val; break;
  case 0x26: case 0x27: case 0x28: case 0x29: r->wh[adr - 0x26] = val; break;
  case 0x2a: r->wbglog = val; break;
  case 0x2b: r->wobjlog = val; break;
  case 0x2c: r->tm = val; break;
  case 0x2d: r->ts = val; break;
  case 0x2e: r->tmw = val; break;
  case 0x2f: r->tsw = val; break;
  case 0x30: r->cgwsel = val; break;
  case 0x31: r->cgadsub = val; break;
  case 0x32:
    if (val & 0x20) r->coldata[0] = val & 0x1f;
    if (val & 0x40) r->coldata[1] = val & 0x1f;
    if (val & 0x80) r->coldata[2] = val & 0x1f;
    break;
  case 0x33: r->setini = val; break;
  case 0x40: case 0x41: case 0x42: case 0x43:
    RtlApuWrite(0x2100 + adr, val);
    break;
  case 0x80:
    g_ram[wram_port_addr] = val;
    wram_port_addr = (wram_port_addr + 1) & 0x1ffff;
    break;
  case 0x81: wram_port_addr = (wram_port_addr & 0x1ff00) | val; break;
  case 0x82: wram_port_addr = (wram_port_addr & 0x100ff) | (val << 8); break;
  case 0x83: wram_port_addr = (wram_port_addr & 0x0ffff) | ((val & 1) << 16); break;
  }
}

ROM_CODE static uint8_t ReadBBus(uint8_t adr) {
  SnesHw *h = &g_snes_hw;
  switch (adr) {
  case 0x34: case 0x35: case 0x36: {
    int32_t result = h->r.m7[0] * (int8_t)(h->r.m7[1] >> 8);
    return result >> (8 * (adr - 0x34));
  }
  case 0x39: {
    uint16_t val = h->vram_read_buf;
    if (!(h->vmain & 0x80)) {
      h->vram_read_buf = g_vram[VramRemap() & 0x7fff];
      h->vram_ptr += h->vram_inc;
    }
    return val;
  }
  case 0x3a: {
    uint16_t val = h->vram_read_buf;
    if (h->vmain & 0x80) {
      h->vram_read_buf = g_vram[VramRemap() & 0x7fff];
      h->vram_ptr += h->vram_inc;
    }
    return val >> 8;
  }
  case 0x80: {
    uint8_t v = g_ram[wram_port_addr];
    wram_port_addr = (wram_port_addr + 1) & 0x1ffff;
    return v;
  }
  default:
    return 0;
  }
}

static const uint8_t kBbadOffsets[8][4] = {
  {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
  {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
};
static const uint8_t kTransferLength[8] = {1, 2, 2, 4, 4, 4, 2, 4};

// Returns a pointer to |count| bytes at the A-bus address, if they're all
// in the same block of RAM or ROM.
static const uint8_t *ABusSpan(uint8_t bank, uint16_t a, uint32_t count) {
  if (a + count > 0x10000)
    return NULL;
  if (bank == 0x7e || bank == 0x7f)
    return &g_ram[((bank & 1) << 16) | a];
  if ((bank & 0x7f) < 0x40 && a + count <= 0x2000)
    return &g_ram[a];
  if (a >= 0x8000)
    return RomFixedPtr((bank << 16) | a);
  return NULL;
}

// Copies |bytes| (even) with the GBA's DMA when both are halfword-aligned:
// the cartridge then reads sequentially, which is several times faster
// than a load at a time.
static void CopyBytes(void *dst, const uint8_t *src, uint32_t bytes) {
  if (!(((uintptr_t)dst | (uintptr_t)src) & 1)) {
    REG_DMASAD(3) = (uintptr_t)src;
    REG_DMADAD(3) = (uintptr_t)dst;
    REG_DMACNT(3) = (bytes >> 1) | kDma16 | kDmaEnable;
  } else {
    memcpy(dst, src, bytes);
  }
}

// Copies |blocks| blocks of 16 bytes from |src| to |dst|, both aligned to
// words, but those that are as they were: the game uploads all of CGRAM and
// OAM every frame, mostly the same, and render.c converts only what's new.
// Returns the blocks that changed, as bits from |first| on. (Reading both
// takes about what DMA's reading and writing does.)
__attribute__((noinline)) static uint32_t CopyChanged(uint32_t *dst, const uint32_t *src,
                                                      uint32_t blocks, uint32_t first) {
  uint32_t changed = 0;
  for (uint32_t i = first; blocks; blocks--, i++, dst += 4, src += 4) {
    uint32_t s0 = src[0], s1 = src[1], s2 = src[2], s3 = src[3];
    uint32_t d0 = dst[0], d1 = dst[1], d2 = dst[2], d3 = dst[3];
    // (Loaded before they're compared, so that it's a block load of each.)
    __asm__("" : "+r"(d0), "+r"(d1), "+r"(d2), "+r"(d3));
    if ((s0 ^ d0) | (s1 ^ d1) | (s2 ^ d2) | (s3 ^ d3)) {
      dst[0] = s0, dst[1] = s1, dst[2] = s2, dst[3] = s3;
      changed |= 1u << i;
    }
  }
  return changed;
}

// Whether the |n| words at |src| (bytes) are those at |a|, a pair at a
// time; false (not known) when they aren't 4-byte aligned or n is odd.
static bool SameWords(const uint16_t *a, const uint8_t *src, uint32_t n) {
  if ((((uintptr_t)a | (uintptr_t)src) & 3) | (n & 1))
    return false;
  const uint32_t *a32 = (const uint32_t *)a, *b32 = (const uint32_t *)src;
  for (n >>= 1; n; n--) {
    if (*a32++ != *b32++)
      return false;
  }
  return true;
}

// The rest of FastDma's palettes and OAM: odd counts or places, a byte at
// a time through the latches, as seldom happens. (In ROM, for room in
// IWRAM.)
ROM_CODE static void PaletteOrOamBytes(uint8_t bbad, const uint8_t *src, uint32_t count) {
  SnesHw *h = &g_snes_hw;
  if (bbad == 0x22) {
    for (uint32_t i = 0; i < count; i++) {
      if (!h->cgram_high)
        h->cgram_latch = src[i];
      else
        g_cgram[h->cgram_ptr++] = ((src[i] & 0x7f) << 8) | h->cgram_latch;
      h->cgram_high = !h->cgram_high;
    }
    h->cgram_dirty = true;
    h->cgram_changed = ~0u;
    return;
  }
  uint16_t a = h->oam_addr;
  for (uint32_t i = 0; i < count; i++, a = (a + 1) & 0x3ff) {
    if (a >= 0x200)
      g_oam[0x200 + (a & 0x1f)] = src[i];
    else if (!(a & 1))
      h->oam_latch = src[i];
    else
      g_oam[a - 1] = h->oam_latch, g_oam[a] = src[i];
  }
  h->oam_addr = a;
  h->oam_changed = ~0u;
}

// DMA a byte at a time, through the B bus's registers: what FastDma
// doesn't do, as seldom happens. (In ROM, for room in IWRAM.)
ROM_CODE static void DmaBytes(SnesDmaChannel *c, uint32_t count) {
  uint8_t mode = c->dmap & 7;
  int step = (c->dmap & 0x08) ? 0 : (c->dmap & 0x10) ? -1 : 1;
  for (uint32_t j = 0; j < count; j++) {
    uint8_t b = c->bbad + kBbadOffsets[mode][j & 3];
    uint32_t a = (c->a1b << 16) | c->a1t;
    if (c->dmap & 0x80)
      WriteA(a, ReadBBus(b));
    else
      WriteBBus(b, SnesHw_ReadA(a));
    c->a1t += step;
  }
}

// DMA from the A bus to the PPU's memories without going through WriteBBus
// a byte at a time: palettes, OAM and VRAM, from a span of RAM or ROM, as
// the game does almost all its DMA. Returns false for anything else.
static bool FastDma(SnesDmaChannel *c, uint32_t count) {
  SnesHw *h = &g_snes_hw;
  // B to A, or a fixed or decrementing A address.
  if (c->dmap & 0x98)
    return false;
  const uint8_t *src = ABusSpan(c->a1b, c->a1t, count);
  if (!src)
    return false;
  uint8_t mode = c->dmap & 7;
  bool no_remap = !(h->vmain & 0x0c), inc_on_high = h->vmain & 0x80;
  if (c->bbad == 0x22 && (mode == 0 || mode == 2) && !h->cgram_high && !(count & 1) &&
      h->cgram_ptr + count / 2 <= 256) {
    // Whole colors (the unused top bit doesn't matter to the GBA).
    if (!(((uintptr_t)src & 3) | (h->cgram_ptr & 7) | (count & 15))) {
      h->cgram_changed |= CopyChanged((uint32_t *)&g_cgram[h->cgram_ptr], (const uint32_t *)src,
                                      count >> 4, h->cgram_ptr >> 3);
    } else {
      CopyBytes(&g_cgram[h->cgram_ptr], src, count);
      h->cgram_changed = ~0u;
    }
    h->cgram_ptr += count / 2;
    h->cgram_dirty = true;
  } else if (c->bbad == 0x04 && (mode == 0 || mode == 2) && !(h->oam_addr & 1) &&
             h->oam_addr + count <= 0x220) {
    // The low table and then the high table, as the game uploads OAM.
    if (!h->oam_addr && count == 0x220 && !((uintptr_t)src & 3)) {
      // All of it. (The high table's two blocks have the bits of half of
      // the low table's blocks each.)
      uint32_t high = CopyChanged((uint32_t *)&g_oam[0x200], (const uint32_t *)(src + 0x200), 2, 0);
      h->oam_changed |= CopyChanged((uint32_t *)g_oam, (const uint32_t *)src, 32, 0) |
                        (high & 1) * 0xffffu | (high >> 1 & 1) * 0xffff0000u;
    } else {
      uint32_t low = h->oam_addr < 0x200 ? 0x200 - h->oam_addr : 0;
      if (low > count)
        low = count;
      CopyBytes(&g_oam[h->oam_addr], src, low);
      CopyBytes(&g_oam[h->oam_addr + low], src + low, count - low);
      h->oam_changed = ~0u;
    }
    h->oam_addr = (h->oam_addr + count) & 0x3ff;
  } else if ((c->bbad == 0x22 || c->bbad == 0x04) && (mode == 0 || mode == 2)) {
    PaletteOrOamBytes(c->bbad, src, count);
  } else if (c->bbad == 0x18 && mode == 1 && no_remap && inc_on_high && !(count & 1)) {
    uint16_t p = h->vram_ptr, inc = h->vram_inc;
    if (inc == 1) {
      // In up to two pieces, as VRAM wraps around.
      uint32_t words = count / 2, marked = 0;
      while (words) {
        uint32_t n = 0x8000 - (p & 0x7fff);
        n = n < words ? n : words;
        // Only the chunks that change are dirty: the game uploads Samus's
        // and the enemies' tiles every frame, mostly the same as before.
        for (uint32_t q = p & ~15; q < (uint32_t)p + n; q += 16) {
          uint32_t lo = q < p ? p : q, hi = q + 16 < p + n ? q + 16 : p + n;
          if (!SameWords(&g_vram[lo & 0x7fff], src + (lo - p) * 2, hi - lo)) {
            MarkVramDirty(q);
            marked++;
          }
        }
        CopyBytes(&g_vram[p & 0x7fff], src, n * 2);
        src += n * 2;
        p += n;
        words -= n;
      }
      g_vram_dirty_marks += marked;
    } else {
      for (uint32_t i = 0; i < count; i += 2, p += inc) {
        g_vram[p & 0x7fff] = src[i] | (src[i + 1] << 8);
        MarkVramDirtyCounted(p);
      }
    }
    h->vram_ptr = p;
  } else if ((c->bbad == 0x18 || c->bbad == 0x19) && mode == 0 && no_remap &&
             inc_on_high == (c->bbad == 0x19)) {
    // Bytes to one half of each word: Mode 7's maps and tiles, say.
    bool high = c->bbad == 0x19;
    uint16_t p = h->vram_ptr, inc = h->vram_inc;
    for (uint32_t i = 0; i < count; i++, p += inc) {
      uint16_t *w = &g_vram[p & 0x7fff];
      *w = high ? (*w & 0x00ff) | (src[i] << 8) : (*w & 0xff00) | src[i];
      if (!(p & 15) || inc != 1)
        MarkVramDirtyCounted(p);
    }
    MarkVramDirtyCounted(h->vram_ptr);
    MarkVramDirtyCounted(p - 1);
    h->vram_ptr = p;
  } else {
    return false;
  }
  c->a1t += count;
  return true;
}

static void RunDma(uint8_t channels) {
  for (int i = 0; i < 8; i++) {
    if (!(channels & (1 << i)))
      continue;
    SnesDmaChannel *c = &g_snes_hw.dma[i];
    uint32_t count = c->das ? c->das : 0x10000;
    if (!FastDma(c, count))
      DmaBytes(c, count);
    c->das = 0;
  }
}

static void WriteDmaReg(uint16_t reg, uint8_t val) {
  SnesDmaChannel *c = &g_snes_hw.dma[(reg >> 4) & 7];
  switch (reg & 0xf) {
  case 0x0: c->dmap = val; break;
  case 0x1: c->bbad = val; break;
  case 0x2: c->a1t = (c->a1t & 0xff00) | val; break;
  case 0x3: c->a1t = (c->a1t & 0x00ff) | (val << 8); break;
  case 0x4: c->a1b = val; break;
  case 0x5: c->das = (c->das & 0xff00) | val; break;
  case 0x6: c->das = (c->das & 0x00ff) | (val << 8); break;
  case 0x7: c->dasb = val; break;
  case 0x8: c->a2a = (c->a2a & 0xff00) | val; break;
  case 0x9: c->a2a = (c->a2a & 0x00ff) | (val << 8); break;
  case 0xa: c->ntrl = val; break;
  case 0xb: case 0xf: c->unused = val; break;
  }
}

ROM_CODE static uint8_t ReadDmaReg(uint16_t reg) {
  SnesDmaChannel *c = &g_snes_hw.dma[(reg >> 4) & 7];
  switch (reg & 0xf) {
  case 0x0: return c->dmap;
  case 0x1: return c->bbad;
  case 0x2: return c->a1t;
  case 0x3: return c->a1t >> 8;
  case 0x4: return c->a1b;
  case 0x5: return c->das;
  case 0x6: return c->das >> 8;
  case 0x7: return c->dasb;
  case 0x8: return c->a2a;
  case 0x9: return c->a2a >> 8;
  case 0xa: return c->ntrl;
  default: return c->unused;
  }
}

void WriteReg(uint16 reg, uint8 value) {
  SnesHw *h = &g_snes_hw;
  if (reg >= 0x2100 && reg < 0x2200) {
    WriteBBus(reg & 0xff, value);
    return;
  }
  if (reg >= 0x4300 && reg < 0x4380) {
    WriteDmaReg(reg, value);
    return;
  }
  switch (reg) {
  case NMITIMEN: h->nmitimen = value; break;
  case WRMPYA: h->wrmpya = value; break;
  case WRMPYB: h->rdmpy = h->wrmpya * value; break;
  case WRDIVL: h->wrdiv = (h->wrdiv & 0xff00) | value; break;
  case WRDIVH: h->wrdiv = (h->wrdiv & 0x00ff) | (value << 8); break;
  case WRDIVB:
    // In Thumb code, which can call the division helpers.
    h->rddiv = SnesDivide(h->wrdiv, value);
    h->rdmpy = SnesModulus(h->wrdiv, value);
    break;
  case HTIMEL: h->htime = (h->htime & 0x100) | value; break;
  case HTIMEH: h->htime = (h->htime & 0xff) | ((value & 1) << 8); break;
  case VTIMEL: h->vtime = (h->vtime & 0x100) | value; break;
  case VTIMEH: h->vtime = (h->vtime & 0xff) | ((value & 1) << 8); break;
  case MDMAEN: RunDma(value); break;
  case HDMAEN:
    h->hdmaen = value;
    hdma_dirty = true;
    break;
  }
}

ROM_CODE uint8 ReadReg(uint16 reg) {
  SnesHw *h = &g_snes_hw;
  if (reg >= 0x2100 && reg < 0x2200)
    return ReadBBus(reg & 0xff);
  if (reg >= 0x4300 && reg < 0x4380)
    return ReadDmaReg(reg);
  switch (reg) {
  case RDNMI: return 0x02;
  case HVBJOY: return 0;
  case RDDIVL: return h->rddiv;
  case RDDIVH: return h->rddiv >> 8;
  case RDMPYL: return h->rdmpy;
  case RDMPYH: return h->rdmpy >> 8;
  case JOY1L: return h->joy1;
  case JOY1H: return h->joy1 >> 8;
  default: return 0;
  }
}

ROM_CODE uint16 ReadRegWord(uint16 reg) {
  uint16 lo = ReadReg(reg);
  return lo | (ReadReg(reg + 1) << 8);
}

void WriteRegWord(uint16 reg, uint16 value) {
  WriteReg(reg, (uint8)value);
  WriteReg(reg + 1, value >> 8);
}

// The channels HDMA is running, decoded once a frame (and if the game
// writes HDMAEN partway down the screen), in channel order.
typedef struct HdmaChannel {
  SnesDmaChannel *c;
  uint8_t bytes;    // per line
  bool indirect;
  // Writes whole words to scroll registers (see WriteScroll).
  bool scroll;
  uint8_t regs[4];  // the B-bus registers they go to, in order
  // For scroll: the registers its words go to, and Mode 7's (or NULL).
  uint16_t *ofs[2], *m7ofs[2];
  // Writes the windows' edges only (WH0-WH3), or the fixed color only
  // (COLDATA).
  bool window, coldata;
  // Pointers to the table's bank and the indirect data's: see BankPtr.
  const uint8_t *table, *data;
  bool table_ram, data_ram;
} HdmaChannel;

// The channels on: the first hdma_count run line by line; then up to
// hdma_scrolls, those that write only scroll registers (but BG1's, which
// Mode 7 shares); up to hdma_windows, those that write only the windows'
// edges; and last, those that write only the fixed color: those run on
// their own, for all the frame (HdmaScrolls, HdmaWindows,
// SnesHw_HdmaColdata).
static HdmaChannel hdma[8] IWRAM_BSS;
static int hdma_count IWRAM_BSS, hdma_scrolls IWRAM_BSS, hdma_windows IWRAM_BSS,
    hdma_total IWRAM_BSS;

// A pointer to a bank of the A bus, in which [addr] is the byte at addr: all
// of a bank of RAM (*ram), or the upper half of a LoROM bank's.
static const uint8_t *BankPtr(uint8_t bank, bool *ram) {
  *ram = bank == 0x7e || bank == 0x7f;
  if (*ram)
    return &g_ram[(bank & 1) << 16];
  return (const uint8_t *)((uintptr_t)RomFixedPtr(bank << 16 | 0x8000) - 0x8000);
}

static inline uint8_t ReadBank(const uint8_t *p, bool ram, uint8_t bank, uint16_t addr) {
  if (ram || addr >= 0x8000)
    return p[addr];
  // The first 8 KB of RAM, in the lower banks: HDMA tables, often.
  if (addr < 0x2000 && !(bank & 0x40))
    return g_ram[addr];
  return SnesHw_ReadA(bank << 16 | addr);
}

ROM_CODE static void HdmaDecode(void) {
  hdma_count = 0;
  hdma_dirty = false;
  for (int i = 0; i < 8; i++) {
    SnesDmaChannel *c = &g_snes_hw.dma[i];
    if (!(g_snes_hw.hdmaen & (1 << i)) || c->hdma_terminated)
      continue;
    HdmaChannel *d = &hdma[hdma_count++];
    uint8_t mode = c->dmap & 7;
    d->c = c;
    d->bytes = kTransferLength[mode];
    d->indirect = c->dmap & 0x40;
    for (int j = 0; j < 4; j++)
      d->regs[j] = c->bbad + kBbadOffsets[mode][j];
    // Modes 2 and 3 write each register twice: the scroll registers' two
    // halves, as most of the game's HDMA does.
    d->scroll = (mode == 2 || mode == 3) && c->bbad >= 0x0d && d->regs[3] <= 0x14;
    d->window = c->bbad >= 0x26 && c->bbad + kBbadOffsets[mode][3] <= 0x29 && !d->scroll;
    // (Modes 0, 2 and 6: a register.)
    d->coldata = c->bbad == 0x32 && !kBbadOffsets[mode][1] && !kBbadOffsets[mode][3];
    for (int k = 0; d->scroll && k < 2; k++) {
      PpuRegs *r = &g_snes_hw.r;
      uint8_t adr = d->regs[2 * k];
      d->ofs[k] = adr & 1 ? &r->hofs[(adr - 0xd) >> 1] : &r->vofs[(adr - 0xe) >> 1];
      d->m7ofs[k] = adr == 0x0d ? &r->m7hofs : adr == 0x0e ? &r->m7vofs : NULL;
    }
    d->table = BankPtr(c->a1b, &d->table_ram);

    d->data = BankPtr(c->dasb, &d->data_ram);
  }
  hdma_total = hdma_count;
  // The scrolling's, then the windows', then the fixed color's, last.
  HdmaChannel sorted[8];
  int n = 0;
  for (int group = 0; group < 4; group++) {
    if (group == 1)
      hdma_count = n;
    if (group == 2)
      hdma_scrolls = n;
    if (group == 3)
      hdma_windows = n;
    for (int k = 0; k < hdma_total; k++) {
      HdmaChannel *d = &hdma[k];
      bool scroll = d->scroll && !d->m7ofs[0] && !d->m7ofs[1];
      int in = scroll ? 1 : d->window ? 2 : d->coldata ? 3 : 0;
      if (in == group)
        sorted[n++] = *d;
    }
  }
  for (int k = 0; k < n; k++)
    hdma[k] = sorted[k];
}

// Reads a channel's next table entry: its line count, and the address of
// its data if indirect.
static void HdmaNextEntry(HdmaChannel *d) {
  SnesDmaChannel *c = d->c;
  uint16_t a = c->a2a;
  if (d->table_ram || (a >= 0x8000 && a <= 0xfffd)) {
    // The usual: all of it from a pointer.
    const uint8_t *p = d->table + a;
    c->ntrl = p[0];
    if (d->indirect) {
      c->das = p[1] | p[2] << 8;
      c->a2a = a + 3;
    } else {
      c->a2a = a + 1;
    }
  } else {
    c->ntrl = ReadBank(d->table, d->table_ram, c->a1b, c->a2a++);
    if (d->indirect) {
      c->das = ReadBank(d->table, d->table_ram, c->a1b, c->a2a++);
      c->das |= ReadBank(d->table, d->table_ram, c->a1b, c->a2a++) << 8;
    }
  }
  c->hdma_terminated = c->ntrl == 0;
  c->hdma_do_transfer = true;
}

// The fixed color as it was before line 0, for SnesHw_HdmaColdata.
static uint8_t coldata_before[3];

ROM_CODE void SnesHw_HdmaInit(void) {
  for (int i = 0; i < 8; i++) {
    SnesDmaChannel *c = &g_snes_hw.dma[i];
    c->hdma_terminated = false;
    c->hdma_do_transfer = false;
  }
  HdmaDecode();
  bool empty = false;
  for (int k = 0; k < hdma_total; k++) {
    hdma[k].c->a2a = hdma[k].c->a1t;
    HdmaNextEntry(&hdma[k]);
    empty |= hdma[k].c->hdma_terminated;
  }
  // Without the ones whose table is empty.
  if (empty)
    HdmaDecode();
  memcpy(coldata_before, g_snes_hw.r.coldata, 3);
}

// Writes a scroll register's two bytes, low then high, as WriteBBus would.
static inline void WriteScroll(uint8_t adr, uint8_t lo, uint8_t hi) {
  SnesHw *h = &g_snes_hw;
  PpuRegs *r = &h->r;
  uint16_t value = hi << 8 | lo;
  if (adr & 1) {
    if (adr == 0x0d)
      r->m7hofs = value & 0x1fff;
    r->hofs[(adr - 0xd) >> 1] = value & 0x3ff;
    h->bghofs_latch = hi;
  } else {
    if (adr == 0x0e)
      r->m7vofs = value & 0x1fff;
    r->vofs[(adr - 0xe) >> 1] = value & 0x3ff;
  }
  if (adr <= 0x0e) {
    h->m7_latch = hi;
    h->regs_dirty = true;  // Mode 7's
  }
  h->bgofs_latch = hi;
  h->scroll_dirty = true;
}

void SnesHw_HdmaRunLine(void) {
  if (hdma_dirty)
    HdmaDecode();
  bool terminated = false;
  for (int k = 0; k < hdma_count; k++) {
    HdmaChannel *d = &hdma[k];
    SnesDmaChannel *c = d->c;
    // (Only BG1's scrolling, here: see HdmaScrolls.)
    if (c->hdma_do_transfer && d->scroll) {
      for (int j = 0; j < d->bytes; j += 2) {
        uint8_t lo, hi;
        if (d->indirect) {
          lo = ReadBank(d->data, d->data_ram, c->dasb, c->das++);
          hi = ReadBank(d->data, d->data_ram, c->dasb, c->das++);
        } else {
          lo = ReadBank(d->table, d->table_ram, c->a1b, c->a2a++);
          hi = ReadBank(d->table, d->table_ram, c->a1b, c->a2a++);
        }
        WriteScroll(d->regs[j], lo, hi);
      }
    } else if (c->hdma_do_transfer) {
      for (int j = 0; j < d->bytes; j++) {
        uint8_t v = d->indirect ? ReadBank(d->data, d->data_ram, c->dasb, c->das++)
                                : ReadBank(d->table, d->table_ram, c->a1b, c->a2a++);
        WriteBBus(d->regs[j], v);
      }
    }
    c->ntrl--;
    c->hdma_do_transfer = c->ntrl & 0x80;
    if (!(c->ntrl & 0x7f)) {
      HdmaNextEntry(d);
      terminated |= c->hdma_terminated;
    }
  }
  if (terminated)
    HdmaDecode();
}

void Vector_IRQ(void);

// The channels that write only scroll registers, for all the frame, on
// their own, after the others: Norfair's heat and Maridia's water write
// them on every line, from tables of a line an entry. A channel at a time,
// its state in registers, as HdmaWindows: from a line it writes a
// register on, line_ofs has that value for it, until a line where something
// else (the IRQ, after HDMA on its line) wrote the register (|written|).
// An HDMA table in ROM, indirect, as it runs from its start over a frame:
// on which lines it transfers, and the data's address then. (Norfair's and
// Maridia's are of a line an entry: going through them a byte at a time,
// from the cartridge, was most of HDMA's work.)
typedef struct BakedTable {
  uint32_t key;   // bank << 16 | address | bytes per line << 24, or 0
  uint32_t used;  // the frame (g_vblank_count) it was last asked for
  uint16_t end;   // the line its ending entry (of 0) starts on, or kNoEnd
  // The entry the last line is in (or 0, not known), and the line it
  // starts on: where to go on from when the table slides up a few lines.
  uint16_t last_a;
  int16_t last_v;
  uint16_t das[kSnesLines];  // or kNoTransfer
} BakedTable;
enum { kNoTransfer = 0xffff, kNoEnd = 0xffff };

extern volatile uint32_t g_vblank_count;

// The lines |from| to |to| of a table in the bank's ROM |table|, from its
// entry at |a|, which starts on line |v| (by |from|), into |das| (from its
// start), as HdmaInit, SnesHw_HdmaRunLine and HdmaNextEntry run it; the
// line its ending entry starts on, if by |to| + 1, into |end|; and the
// entry |to| is in and the line it starts on, into |last_a| and |last_v|
// (or 0, if none is). False if it goes out of the bank's ROM.
ROM_CODE static bool BakeLines(const uint8_t *table, uint32_t a, int v, int bytes, int from,
                               int to, uint16_t *das, uint16_t *end, uint16_t *last_a,
                               int16_t *last_v) {
  for (int w = from; w <= to; w++)
    das[w - from] = kNoTransfer;
  *end = kNoEnd;
  *last_a = 0;
  // |v|: the line the entry at |a| starts on. Of its |n| lines, the first
  // transfers, and with a count over 0x80 (as ntrl counts down, its top bit
  // still set) the others.
  while (v <= to + 1) {
    if (a < 0x8000 || a > 0xfffd)
      return false;  // (out of the bank's ROM)
    const uint8_t *p = table + a;
    uint8_t ntrl = p[0];
    if (!ntrl) {
      *end = v;
      break;
    }
    int n = ntrl & 0x7f ? ntrl & 0x7f : 128;
    bool repeat = ntrl > 0x80;
    uint16_t first = p[1] | p[2] << 8;
    if (v <= to && to < v + n)
      *last_a = a, *last_v = v;
    a += 3;
    int j = from > v ? from - v : 0;
    for (; j < n && v + j <= to && (!j || repeat); j++) {
      uint16_t x = first + bytes * j;
      if (x == kNoTransfer)
        return false;  // (not seen in the game)
      das[v + j - from] = x;
    }
    v += n;
  }
  return true;
}

// The lines the table from the entry at |a| takes to the one at |to|, after
// it (with entries of 3 bytes), if fewer than a frame's; or -1.
ROM_CODE static int LinesTo(const uint8_t *table, uint32_t a, uint32_t to) {
  // (A multiple of 3 times 3's inverse, mod 2^32, is at most 0xffffffff / 3:
  // no division, which the ARM7TDMI hasn't.)
  uint32_t bytes = to - a;
  if (bytes >= 3 * kSnesLines || bytes * 0xaaaaaaabu > 0x55555555u)
    return -1;
  int v = 0;
  for (; a < to; a += 3) {
    if (a < 0x8000 || a > 0xfffd || !table[a])
      return -1;
    v += table[a] & 0x7f ? table[a] & 0x7f : 128;
    if (v >= kSnesLines)
      return -1;
  }
  return v;
}

// memmove's, of halfwords: it goes a byte at a time when they're 2 bytes
// out of step, as a slide of an odd number of lines leaves them.
__attribute__((no_builtin("memmove"))) ROM_CODE static void MoveLines(uint16_t *to,
                                                                    const uint16_t *from, int n) {
  if (to < from) {
    for (int i = 0; i < n; i++)
      to[i] = from[i];
  } else {
    for (int i = n; i-- > 0;)
      to[i] = from[i];
  }
}

// Of the |n| tables, the one asked for least lately, but for |keep|.
ROM_CODE static BakedTable *LeastUsed(BakedTable *baked, int n, const BakedTable *keep,
                                      uint32_t now) {
  BakedTable *b = NULL;
  for (int i = 0; i < n; i++) {
    if (&baked[i] != keep && (!b || now - baked[i].used > now - b->used))
      b = &baked[i];
  }
  return b;
}

// The channel's table, baked, if it's in ROM and indirect; or NULL.
ROM_CODE static const BakedTable *Baked(const HdmaChannel *d) {
  enum { kBaked = 6 };  // (Norfair has as many going)
  static BakedTable baked[kBaked];
  const SnesDmaChannel *c = d->c;
  if (!d->indirect || d->table_ram || c->a1t < 0x8000)
    return NULL;
  uint32_t key = (uint32_t)c->a1b << 16 | c->a1t | (uint32_t)d->bytes << 24;
  uint32_t now = g_vblank_count;
  for (int i = 0; i < kBaked; i++) {
    if (baked[i].key == key) {
      baked[i].used = now;
      return &baked[i];
    }
  }
  BakedTable *keep = NULL, *b;
  // Another entry of a table baked: Maridia's water's starts at another of
  // them as the water's surface scrolls. Its lines, slid, and the lines
  // that brings in.
  int slide = 0;
  for (int i = 0; i < kBaked; i++) {
    uint32_t other = baked[i].key;
    if (!other || (other ^ key) >> 16)
      continue;
    int ahead = LinesTo(d->table, other & 0xffff, c->a1t);
    int behind = ahead < 0 ? LinesTo(d->table, c->a1t, other & 0xffff) : -1;
    int lines = ahead >= 0 ? ahead : behind;
    if (lines > 0 && (!keep || lines < (slide < 0 ? -slide : slide)))
      keep = &baked[i], slide = ahead >= 0 ? lines : -lines;
  }
  if (keep) {
    // Into another, if the other table went this frame (for another
    // channel).
    if (keep->used == now) {
      b = LeastUsed(baked, kBaked, keep, now);
      memcpy(b->das, keep->das, sizeof(b->das));
      b->end = keep->end;
      b->last_a = keep->last_a;
      b->last_v = keep->last_v;
    } else {
      b = keep;
    }
    b->key = 0;
    if (slide > 0) {
      MoveLines(b->das, b->das + slide, kSnesLines - slide);
      if (b->end != kNoEnd) {
        b->end -= slide;
        b->last_a = 0;
        for (int v = kSnesLines - slide; v < kSnesLines; v++)
          b->das[v] = kNoTransfer;
      } else {
        // From the entry the last line was in, now |slide| lines up, if
        // known, rather than through the lines before (Maridia's water's
        // are of a line an entry).
        uint32_t a = b->last_a ? b->last_a : c->a1t;
        int v = b->last_a ? b->last_v - slide : 0;
        if (!BakeLines(d->table, a, v, d->bytes, kSnesLines - slide, kSnesLines - 1,
                       b->das + kSnesLines - slide, &b->end, &b->last_a, &b->last_v))
          return NULL;
      }
    } else {
      slide = -slide;
      MoveLines(b->das + slide, b->das, kSnesLines - slide);
      uint16_t end, last_a;
      int16_t last_v;
      if (!BakeLines(d->table, c->a1t, 0, d->bytes, 0, slide - 1, b->das, &end, &last_a, &last_v))
        return NULL;
      b->end = b->end != kNoEnd && b->end + slide <= kSnesLines ? b->end + slide : kNoEnd;
      // The last line's entry, now: it's |slide| lines back, and as the
      // entries are of 3 bytes, the ones before it are at 3 bytes less.
      if (b->last_a) {
        uint32_t a = b->last_a;
        int v = b->last_v;
        while (v > kSnesLines - 1 - slide) {
          a -= 3;
          v -= d->table[a] & 0x7f ? d->table[a] & 0x7f : 128;
        }
        b->last_a = a;
        b->last_v = v + slide;
      }
    }
    b->key = key;
    b->used = now;
    return b;
  }
  // Baked the second time a table's asked for: some start at another place
  // every frame (as a door's fade scrolls), and baking them would only
  // double the work of going through them.
  static uint32_t seen[8];
  static int next_seen;
  bool again = false;
  for (int i = 0; i < 8; i++)
    again |= seen[i] == key;
  if (!again) {
    seen[next_seen] = key;
    next_seen = (next_seen + 1) & 7;
    return NULL;
  }
  b = LeastUsed(baked, kBaked, NULL, now);
  b->key = 0;
  if (!BakeLines(d->table, c->a1t, 0, d->bytes, 0, kSnesLines - 1, b->das, &b->end, &b->last_a,
                 &b->last_v))
    return NULL;
  b->key = key;
  b->used = now;
  return b;
}

// Each line's byte of a register, into every fourth byte of |out|, from
// the data at the baked lines' addresses |das| (plus |j|), in a bank of
// RAM |data|; or the value before (|value|, at first). Returns the last.
// (In IWRAM, for HdmaWindows: a power bomb's explosion's edges.)
__attribute__((noinline)) static uint8_t BakedBytes(uint8_t *out, const uint16_t *das,
                                                    const uint8_t *data, int j, uint8_t value) {
  for (int v = 0; v < kSnesLines; v++) {
    uint16_t a = das[v];
    if (a != kNoTransfer)
      value = data[(uint16_t)(a + j)];
    out[v * 4] = value;
  }
  return value;
}

// The channels that write only the windows' edges, for all the frame's
// lines: as their edges aren't in the states, they don't make every line
// a line HDMA has to stop at (see SnesHw_RunDisplay). A channel at a time,
// its state in registers. Where the game writes the edges itself partway
// down the screen, though, it's lost.
ROM_CODE static void HdmaWindows(uint32_t *line_windows) {
  uint8_t *wh = g_snes_hw.r.wh;
  uint32_t start = wh[0] | wh[1] << 8 | wh[2] << 16 | (uint32_t)wh[3] << 24;
  // (As a rule, there are none, and the edges are as they were.)
  static uint32_t filled IWRAM_BSS;
  static bool uniform IWRAM_BSS;
  if (hdma_scrolls == hdma_windows && uniform && filled == start)
    return;
  for (int v = 0; v < kSnesLines; v++)
    line_windows[v] = start;
  filled = start;
  uniform = hdma_scrolls == hdma_windows;
  // A channel at a time, each of its registers' bytes of every line's word
  // (WH0 in the low byte), in turn.
  uint8_t *out = (uint8_t *)line_windows;
  // (Bytes through the pointers as a rule, as ReadBank has them: it's in
  // IWRAM, and a call from here, from the cartridge, costs a thunk.)
#define WINDOW_READ(p, ram, bank, addr) \
  ((ram) || (addr) >= 0x8000 ? (p)[addr] : ReadBank(p, ram, bank, addr))
  for (int k = hdma_scrolls; k < hdma_windows; k++) {
    HdmaChannel *d = &hdma[k];
    SnesDmaChannel *c = d->c;
    int bytes = d->bytes;
    const BakedTable *b = d->data_ram ? Baked(d) : NULL;
    if (b) {
      // A power bomb's explosion: a table in ROM, of a line an entry, with
      // the edges in RAM. From the baked table, a register at a time.
      for (int j = 0; j < bytes; j++) {
        int reg = d->regs[j] - 0x26;
        wh[reg] = BakedBytes(out + reg, b->das, d->data, j, wh[reg]);
      }
      c->hdma_terminated = b->end <= kSnesLines;
      continue;
    }
    const uint8_t *table = d->table, *data = d->data;
    bool table_ram = d->table_ram, data_ram = d->data_ram, indirect = d->indirect;
    uint8_t a1b = c->a1b, dasb = c->dasb, ntrl = c->ntrl;
    uint16_t a2a = c->a2a, das = c->das;
    bool transfer = c->hdma_do_transfer, terminated = c->hdma_terminated;
    // The first 8 KB of RAM in the lower banks, as a bank of RAM: if the
    // table's there, and the frame's lines of it (3 bytes each at most).
    if (!table_ram && !(a1b & 0x40) && a2a < 0x2000 - 3 * kSnesLines) {
      table = g_ram;
      table_ram = true;
    }
    if (!data_ram && !(dasb & 0x40) && das < 0x2000 - 4 * kSnesLines) {
      data = g_ram;
      data_ram = true;
    }
    for (int v = 0; v < kSnesLines; v++) {
      if (!terminated) {
        if (transfer) {
          for (int j = 0; j < bytes; j++) {
            uint8_t value;
            if (indirect)
              value = WINDOW_READ(data, data_ram, dasb, das), das++;
            else
              value = WINDOW_READ(table, table_ram, a1b, a2a), a2a++;
            wh[d->regs[j] - 0x26] = value;
          }
        }
        ntrl--;
        transfer = ntrl & 0x80;
        if (!(ntrl & 0x7f)) {
          // The next entry, as HdmaNextEntry.
          ntrl = WINDOW_READ(table, table_ram, a1b, a2a), a2a++;
          if (indirect) {
            das = WINDOW_READ(table, table_ram, a1b, a2a), a2a++;
            das |= WINDOW_READ(table, table_ram, a1b, a2a) << 8, a2a++;
          }
          terminated = ntrl == 0;
          transfer = true;
        }
      }
      for (int j = 0; j < bytes; j++)
        out[v * 4 + d->regs[j] - 0x26] = wh[d->regs[j] - 0x26];
    }
    c->ntrl = ntrl;
    c->a2a = a2a;
    c->das = das;
    c->hdma_do_transfer = transfer;
    c->hdma_terminated = terminated;
  }
#undef WINDOW_READ
}

// (|written_at|: the |num_written| lines where |written| isn't 0, in
// order.)
static void HdmaScrolls(uint32_t (*line_ofs)[4], const uint8_t *written,
                        const uint8_t *written_at, int num_written) {
  PpuRegs *r = &g_snes_hw.r;
  for (int k = hdma_count; k < hdma_scrolls; k++) {
    HdmaChannel *d = &hdma[k];
    SnesDmaChannel *c = d->c;
    const uint8_t *table = d->table, *data = d->data;
    bool table_ram = d->table_ram, data_ram = d->data_ram, indirect = d->indirect;
    uint8_t a1b = c->a1b, dasb = c->dasb, ntrl = c->ntrl;
    uint16_t a2a = c->a2a, das = c->das;
    bool transfer = c->hdma_do_transfer, terminated = c->hdma_terminated;
    bool two = d->bytes == 4;
    // Its registers (one, or two): their bits in |written|, their halves of
    // line_ofs, and their values (or -1, before it writes them).
    uint8_t bit[2], half[2];
    for (int i = 0; i < 2; i++) {
      uint8_t adr = d->regs[two ? 2 * i : 0];
      int reg = adr & 1 ? (adr - 0xd) >> 1 : 4 + ((adr - 0xe) >> 1);
      bit[i] = 1 << reg;
      half[i] = (reg & 3) * 2 + (reg >> 2);
    }
    uint8_t bit0 = bit[0], bit1 = bit[1], half0 = half[0], half1 = half[1];
    int value0 = -1, value1 = -1;
    const BakedTable *b = Baked(d);
    if (b) {
      // From the baked table: the data, on the lines it transfers; in runs
      // of lines between those where something else wrote the registers.
      uint8_t mask = bit0 | (two ? bit1 : 0);
      uint16_t *ofs16 = (uint16_t *)line_ofs;
      const uint16_t *das_at = b->das;
      int next = 0;
      for (int v = 0; v < kSnesLines;) {
        while (next < num_written &&
               (written_at[next] < v || !(written[written_at[next]] & mask)))
          next++;
        int end = next < num_written ? written_at[next] : kSnesLines;
        if (data_ram && !two) {
          for (; v < end; v++) {
            uint16_t a = das_at[v];
            if (a != kNoTransfer)
              value0 = (data[a] | data[(uint16_t)(a + 1)] << 8) & 0x3ff;
            if (value0 >= 0)
              ofs16[v * 8 + half0] = value0;
          }
        } else if (data_ram) {
          for (; v < end; v++) {
            uint16_t a = das_at[v];
            if (a != kNoTransfer) {
              value0 = (data[a] | data[(uint16_t)(a + 1)] << 8) & 0x3ff;
              value1 = (data[(uint16_t)(a + 2)] | data[(uint16_t)(a + 3)] << 8) & 0x3ff;
            }
            if (value0 >= 0)
              ofs16[v * 8 + half0] = value0;
            if (value1 >= 0)
              ofs16[v * 8 + half1] = value1;
          }
        }
        // Lines with data elsewhere than RAM, or where something else wrote
        // a register.
        for (; v < kSnesLines && (v < end || v == end); v++) {
          uint16_t a = das_at[v];
          if (a != kNoTransfer) {
            value0 = (ReadBank(data, data_ram, dasb, a) |
                      ReadBank(data, data_ram, dasb, (uint16_t)(a + 1)) << 8) & 0x3ff;
            if (two)
              value1 = (ReadBank(data, data_ram, dasb, (uint16_t)(a + 2)) |
                        ReadBank(data, data_ram, dasb, (uint16_t)(a + 3)) << 8) & 0x3ff;
          }
          uint8_t w = written[v];
          if (w & bit0)
            value0 = -1;
          else if (value0 >= 0)
            ofs16[v * 8 + half0] = value0;
          if (two) {
            if (w & bit1)
              value1 = -1;
            else if (value1 >= 0)
              ofs16[v * 8 + half1] = value1;
          }
        }
      }
      c->hdma_terminated = b->end <= kSnesLines;
    }
    for (int v = 0; v < kSnesLines && !b; v++) {
      if (!terminated) {
        if (transfer) {
          for (int i = 0; i <= two; i++) {
            uint8_t lo, hi;
            if (indirect) {
              if (data_ram || das >= 0x8000) {
                lo = data[das];
                hi = data[(uint16_t)(das + 1)];
              } else {
                lo = ReadBank(data, data_ram, dasb, das);
                hi = ReadBank(data, data_ram, dasb, (uint16_t)(das + 1));
              }
              das += 2;
            } else {
              lo = ReadBank(table, table_ram, a1b, a2a);
              hi = ReadBank(table, table_ram, a1b, (uint16_t)(a2a + 1));
              a2a += 2;
            }
            if (i)
              value1 = (hi << 8 | lo) & 0x3ff;
            else
              value0 = (hi << 8 | lo) & 0x3ff;
          }
        }
        ntrl--;
        transfer = ntrl & 0x80;
        if (!(ntrl & 0x7f)) {
          // The next entry, as HdmaNextEntry.
          if (table_ram || (a2a >= 0x8000 && a2a <= 0xfffd)) {
            const uint8_t *p = table + a2a;
            ntrl = p[0];
            if (indirect)
              das = p[1] | p[2] << 8;
            a2a += indirect ? 3 : 1;
          } else {
            ntrl = ReadBank(table, table_ram, a1b, a2a++);
            if (indirect) {
              das = ReadBank(table, table_ram, a1b, a2a++);
              das |= ReadBank(table, table_ram, a1b, a2a++) << 8;
            }
          }
          terminated = ntrl == 0;
          transfer = true;
        }
      }
      uint16_t *line = (uint16_t *)line_ofs[v];
      uint8_t w = written[v];
      if (w & bit0)
        value0 = -1;
      else if (value0 >= 0)
        line[half0] = value0;
      if (two) {
        if (w & bit1)
          value1 = -1;
        else if (value1 >= 0)
          line[half1] = value1;
      }
      if (terminated && value0 < 0 && (!two || value1 < 0))
        break;
    }
    if (!b) {
      c->ntrl = ntrl;
      c->a2a = a2a;
      c->das = das;
      c->hdma_do_transfer = transfer;
      c->hdma_terminated = terminated;
    }
    // As HDMA left the registers, for the next frame.
    if (value0 >= 0)
      ((half0 & 1) ? r->vofs : r->hofs)[half0 >> 1] = value0;
    if (two && value1 >= 0)
      ((half1 & 1) ? r->vofs : r->hofs)[half1 >> 1] = value1;
  }
}

// A write to COLDATA, of the channels it has bits 5-7 set for.
ROM_CODE static void WriteColdata(uint8_t *coldata, uint8_t val) {
  for (int i = 0; i < 3; i++) {
    if (val & (0x20 << i))
      coldata[i] = val & 0x1f;
  }
}

// The fixed color, from a channel of a byte a line with its table baked
// (|das|): into the states, as SnesHw_HdmaColdata, and |coldata|. False if
// the data's below |lowest| (0, or 0x8000 in ROM), where ReadBank has to
// read it.
ROM_CODE static bool BakedColdata(
    const uint16_t *das, const uint8_t *data, uint32_t lowest, PpuRegs *states,
    const uint8_t *line_state, int color_line, uint8_t *coldata) {
  uint32_t color = coldata[0] | coldata[1] << 8 | coldata[2] << 16;
  int prev = -1;
  for (int v = 0; v < kSnesLines; v++) {
    uint16_t a = das[v];
    if (a != kNoTransfer) {
      if (a < lowest)
        return false;
      uint32_t val = data[a];
      for (int i = 0; i < 3; i++) {
        if (val & (0x20 << i))
          color = (color & ~(0xffu << (8 * i))) | (val & 0x1f) << (8 * i);
      }
    }
    int s = line_state[v];
    if (s != prev || v == color_line) {
      uint8_t *o = states[s].coldata;
      o[0] = color, o[1] = color >> 8, o[2] = color >> 16;
    }
    prev = s;
  }
  coldata[0] = color, coldata[1] = color >> 8, coldata[2] = color >> 16;
  return true;
}

// The channels that write only the fixed color (COLDATA), for all the
// frame's lines, from coldata_before, after the others (see snes_hw.h):
// Ceres's backgrounds' gradients write it on every line, which, were it in
// the states, would make every line a state, and one HDMA has to stop at.
// The GBA can't show a gradient: the palette has one line's color
// (|color_line|'s: see FixedMath) and color math a segment's. So each state
// has the color of its first line, but for |color_line|'s, which has that
// line's. (As in HdmaWindows, where the game writes the color itself
// partway down the screen, it's lost.)
ROM_CODE void SnesHw_HdmaColdata(PpuRegs *states, const uint8_t *line_state, int color_line) {
  if (hdma_total == hdma_windows)
    return;
  uint8_t *coldata = coldata_before;
  // As a rule, a channel of a byte a line, its table baked. Or, a channel
  // at a time, a line at a time.
  const HdmaChannel *only = &hdma[hdma_windows];
  const BakedTable *b =
      hdma_total - hdma_windows == 1 && only->bytes == 1 ? Baked(only) : NULL;
  if (b && BakedColdata(b->das, only->data, only->data_ram ? 0 : 0x8000, states, line_state,
                        color_line, coldata)) {
    only->c->hdma_terminated = b->end <= kSnesLines;
    memcpy(g_snes_hw.r.coldata, coldata, 3);  // as HDMA left it, for the next frame
    return;
  }
  int prev = -1;
  for (int v = 0; v < kSnesLines; v++) {
    for (int k = hdma_windows; k < hdma_total; k++) {
      HdmaChannel *d = &hdma[k];
      SnesDmaChannel *c = d->c;
      if (c->hdma_terminated)
        continue;
      if (c->hdma_do_transfer) {
        for (int j = 0; j < d->bytes; j++)
          WriteColdata(coldata, d->indirect
                                    ? ReadBank(d->data, d->data_ram, c->dasb, c->das++)
                                    : ReadBank(d->table, d->table_ram, c->a1b, c->a2a++));
      }
      c->ntrl--;
      c->hdma_do_transfer = c->ntrl & 0x80;
      if (!(c->ntrl & 0x7f))
        HdmaNextEntry(d);
    }
    int s = line_state[v];
    if (s != prev || v == color_line) {
      PpuRegs *r = &states[s];
      r->coldata[0] = coldata[0], r->coldata[1] = coldata[1], r->coldata[2] = coldata[2];
    }
    prev = s;
  }
  memcpy(g_snes_hw.r.coldata, coldata, 3);  // as HDMA left it, for the next frame
}

// How many lines from now HDMA does nothing but count down: for each
// channel, the lines left in a table entry that doesn't repeat, after its
// first. Up to |max|.
static int HdmaQuietLines(int max) {
  for (int k = 0; k < hdma_count && max; k++) {
    SnesDmaChannel *c = hdma[k].c;
    int quiet = (c->hdma_do_transfer || (c->ntrl & 0x80)) ? 0 : (c->ntrl & 0x7f) - 1;
    if (quiet < max)
      max = quiet;
  }
  return max;
}

static void HdmaSkipLines(int lines) {
  for (int k = 0; k < hdma_count; k++)
    hdma[k].c->ntrl -= lines;
}

// But for the scrolling (bytes 10-25: the top of word 2, words 3-5 and the
// bottom of word 6), which has states of its own, and the windows' edges
// (bytes 47-50: the top of word 11 and the bottom of word 12).
_Static_assert(offsetof(PpuRegs, hofs) == 10 && offsetof(PpuRegs, vofs) == 18 &&
                   offsetof(PpuRegs, wh) == 47 && sizeof(PpuRegs) == 64,
               "PpuRegs's layout");
static bool SameRegs(const PpuRegs *a, const PpuRegs *b) {
  const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
  if (x[0] != y[0] || x[1] != y[1] || ((x[2] ^ y[2]) & 0xffff) || ((x[6] ^ y[6]) & 0xffff0000))
    return false;
  for (unsigned i = 7; i < 11; i++) {
    if (x[i] != y[i])
      return false;
  }
  for (unsigned i = 13; i < sizeof(PpuRegs) / 4; i++) {
    if (x[i] != y[i])
      return false;
  }
  return !((x[11] ^ y[11]) & 0x00ffffff) && !((x[12] ^ y[12]) & 0xff000000);
}

int SnesHw_RunDisplay(PpuRegs *states, uint8_t *line_state, uint32_t *line_windows,
                      uint32_t (*line_ofs)[4]) {
  SnesHw *h = &g_snes_hw;
  SnesHw_HdmaInit();
  HdmaWindows(line_windows);
  // The scroll registers the CPU wrote on each line, for HdmaScrolls.
  static uint8_t written[kSnesLines], written_at[kSnesLines];
  int num_written = 0;
  memset(written, 0, sizeof(written));
  int n = 0;
  h->regs_dirty = h->scroll_dirty = true;
  h->scroll_written = 0;
  // The scrolling, as line_ofs has it.
  uint32_t ofs[4];
  // A line shows the registers as HDMA and the IRQ at the end of the line
  // before it left them (as src/snes/snes.c draws a frame).
  for (int v = 0; v < kSnesLines;) {
    bool irq = (h->nmitimen & 0x30) && h->vtime < kSnesLines;
    // Lines where HDMA only counts down, before the IRQ's, if any.
    int quiet = HdmaQuietLines((irq && h->vtime >= v ? h->vtime : kSnesLines) - v);
    if (quiet) {
      HdmaSkipLines(quiet);
    } else {
      SnesHw_HdmaRunLine();
      if (irq && v == h->vtime)
        Vector_IRQ();
      quiet = 1;
    }
    // HDMA often writes registers with the values they have.
    if (h->regs_dirty) {
      h->regs_dirty = false;
      if (!n || !SameRegs(&states[n - 1], &h->r))
        states[n++] = h->r;
    }
    if (h->scroll_dirty) {
      h->scroll_dirty = false;
      for (int i = 0; i < 4; i++)
        ofs[i] = h->r.hofs[i] | (uint32_t)h->r.vofs[i] << 16;
    }
    if (h->scroll_written) {
      written[v] = h->scroll_written;
      written_at[num_written++] = v;
      h->scroll_written = 0;
    }
    for (int end = v + quiet; v < end; v++) {
      line_state[v] = n - 1;
      line_ofs[v][0] = ofs[0];
      line_ofs[v][1] = ofs[1];
      line_ofs[v][2] = ofs[2];
      line_ofs[v][3] = ofs[3];
    }
  }
  if (hdma_scrolls > hdma_count)
    HdmaScrolls(line_ofs, written, written_at, num_written);
  return n;
}
