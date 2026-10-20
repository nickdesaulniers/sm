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

SnesHw g_snes_hw IWRAM_BSS;
uint16_t g_cgram[256];
uint8_t g_oam[544];
uint16_t g_vram[kVramWords];
uint32_t g_vram_dirty[kVramChunks / 32];
// The game wrote HDMAEN: HDMA's decoded channels are out of date.
static bool hdma_dirty IWRAM_BSS;

static uint32_t wram_port_addr;

ROM_CODE void SnesHw_Reset(void) {
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
  g_snes_hw.cgram_dirty = g_snes_hw.oam_dirty = true;
}

static inline void MarkVramDirty(uint16_t addr) {
  uint32_t chunk = (addr & 0x7fff) >> kVramChunkShift;
  g_vram_dirty[chunk >> 5] |= 1u << (chunk & 31);
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

// The PPU registers below 0x34 that aren't in PpuRegs, by bit: OBSEL and
// the ports to OAM, VRAM and CGRAM.
static const uint32_t kNotPpuRegs[2] = {
  1u << 0x01 | 1u << 0x02 | 1u << 0x03 | 1u << 0x04 | 1u << 0x15 | 1u << 0x16 | 1u << 0x17 |
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
  case 0x01: h->obsel = val; h->oam_dirty = true; break;
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
    h->oam_addr = (a + 1) & 0x3ff;
    h->oam_dirty = true;
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
    break;
  case 0x0e:
    r->m7vofs = ((val << 8) | h->m7_latch) & 0x1fff;
    h->m7_latch = val;
    // fallthrough
  case 0x10: case 0x12: case 0x14:
    r->vofs[(adr - 0xe) >> 1] = ((val << 8) | h->bgofs_latch) & 0x3ff;
    h->bgofs_latch = val;
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
    MarkVramDirty(a);
    if (!(h->vmain & 0x80))
      h->vram_ptr += h->vram_inc;
    break;
  }
  case 0x19: {
    uint16_t a = VramRemap() & 0x7fff;

    g_vram[a] = (g_vram[a] & 0x00ff) | (val << 8);
    MarkVramDirty(a);
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

static uint8_t ReadBBus(uint8_t adr) {
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
    CopyBytes(&g_cgram[h->cgram_ptr], src, count);
    h->cgram_ptr += count / 2;
    h->cgram_dirty = true;
  } else if (c->bbad == 0x04 && (mode == 0 || mode == 2) && !(h->oam_addr & 1) &&
             h->oam_addr + count <= 0x220) {
    // The low table and then the high table, as the game uploads OAM.
    uint32_t low = h->oam_addr < 0x200 ? 0x200 - h->oam_addr : 0;
    if (low > count)
      low = count;
    CopyBytes(&g_oam[h->oam_addr], src, low);
    CopyBytes(&g_oam[h->oam_addr + low], src + low, count - low);
    h->oam_addr = (h->oam_addr + count) & 0x3ff;
    h->oam_dirty = true;
  } else if (c->bbad == 0x22 && (mode == 0 || mode == 2)) {
    for (uint32_t i = 0; i < count; i++) {
      if (!h->cgram_high)
        h->cgram_latch = src[i];
      else
        g_cgram[h->cgram_ptr++] = ((src[i] & 0x7f) << 8) | h->cgram_latch;
      h->cgram_high = !h->cgram_high;
    }
    h->cgram_dirty = true;
  } else if (c->bbad == 0x04 && (mode == 0 || mode == 2)) {
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
    h->oam_dirty = true;
  } else if (c->bbad == 0x18 && mode == 1 && no_remap && inc_on_high && !(count & 1)) {
    uint16_t p = h->vram_ptr, inc = h->vram_inc;
    if (inc == 1) {
      // In up to two pieces, as VRAM wraps around.
      uint32_t words = count / 2;
      while (words) {
        uint32_t n = 0x8000 - (p & 0x7fff);
        n = n < words ? n : words;
        CopyBytes(&g_vram[p & 0x7fff], src, n * 2);
        for (uint32_t q = p & ~15; q < (uint32_t)p + n; q += 16)
          MarkVramDirty(q);
        src += n * 2;
        p += n;
        words -= n;
      }
    } else {
      for (uint32_t i = 0; i < count; i += 2, p += inc) {
        g_vram[p & 0x7fff] = src[i] | (src[i + 1] << 8);
        MarkVramDirty(p);
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
        MarkVramDirty(p);
    }
    MarkVramDirty(h->vram_ptr);
    MarkVramDirty(p - 1);
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
    if (!FastDma(c, count)) {
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
  // Writes the windows' edges only (WH0-WH3).
  bool window;
  // Pointers to the table's bank and the indirect data's: see BankPtr.
  const uint8_t *table, *data;
  bool table_ram, data_ram;
} HdmaChannel;

// The channels on, those that write only the windows' edges last: they
// run on their own, for all the frame (HdmaWindows). hdma_count are the
// others.
static HdmaChannel hdma[8] IWRAM_BSS;
static int hdma_count IWRAM_BSS, hdma_total IWRAM_BSS;

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

static void HdmaDecode(void) {
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
  // The windows' last.
  HdmaChannel others[8], windows[8];
  int n = 0, w = 0;
  for (int k = 0; k < hdma_total; k++) {
    if (hdma[k].window)
      windows[w++] = hdma[k];
    else
      others[n++] = hdma[k];
  }
  for (int k = 0; k < n; k++)
    hdma[k] = others[k];
  for (int k = 0; k < w; k++)
    hdma[n + k] = windows[k];
  hdma_count = n;
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

ROM_CODE void SnesHw_HdmaInit(void) {
  for (int i = 0; i < 8; i++) {
    SnesDmaChannel *c = &g_snes_hw.dma[i];
    c->hdma_terminated = false;
    c->hdma_do_transfer = false;
  }
  HdmaDecode();
  for (int k = 0; k < hdma_total; k++) {
    hdma[k].c->a2a = hdma[k].c->a1t;
    HdmaNextEntry(&hdma[k]);
  }
  // Without the ones whose table is empty.
  HdmaDecode();
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
  if (adr <= 0x0e)
    h->m7_latch = hi;
  h->bgofs_latch = hi;
  h->regs_dirty = true;
}

void SnesHw_HdmaRunLine(void) {
  if (hdma_dirty)
    HdmaDecode();
  bool terminated = false;
  for (int k = 0; k < hdma_count; k++) {
    HdmaChannel *d = &hdma[k];
    SnesDmaChannel *c = d->c;
    if (c->hdma_do_transfer && d->scroll && d->indirect && d->data_ram) {
      // The game's usual HDMA: scrolling, from tables of pointers to RAM.
      // As WriteScroll, with the registers worked out in HdmaDecode.
      // (Often with the values they have: then the registers aren't dirty.)
      SnesHw *h = &g_snes_hw;
      const uint8_t *p = d->data;
      uint16_t a = c->das;
      uint8_t hi = 0;
      bool changed = false;
      for (int k = 0; k < d->bytes >> 1; k++) {
        uint8_t lo = p[a];
        hi = p[(uint16_t)(a + 1)];
        a += 2;
        uint16_t value = hi << 8 | lo;
        changed |= *d->ofs[k] != (value & 0x3ff);
        *d->ofs[k] = value & 0x3ff;
        if (d->m7ofs[k]) {
          changed |= *d->m7ofs[k] != (value & 0x1fff);
          *d->m7ofs[k] = value & 0x1fff;
          h->m7_latch = hi;
        }
        if (d->regs[2 * k] & 1)
          h->bghofs_latch = hi;
      }
      c->das = a;
      h->bgofs_latch = hi;
      h->regs_dirty |= changed;
    } else if (c->hdma_do_transfer && d->scroll) {
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

// The channels that write only the windows' edges, for all the frame's
// lines: as their edges aren't in the states, they don't make every line
// a line HDMA has to stop at (see SnesHw_RunDisplay). A channel at a time,
// its state in registers. Where the game writes the edges itself partway
// down the screen, though, it's lost.
static void HdmaWindows(uint32_t *line_windows) {
  uint8_t *wh = g_snes_hw.r.wh;
  uint32_t start = wh[0] | wh[1] << 8 | wh[2] << 16 | (uint32_t)wh[3] << 24;
  // (As a rule, there are none, and the edges are as they were.)
  static uint32_t filled IWRAM_BSS;
  static bool uniform IWRAM_BSS;
  if (hdma_count == hdma_total && uniform && filled == start)
    return;
  for (int v = 0; v < kSnesLines; v++)
    line_windows[v] = start;
  filled = start;
  uniform = hdma_count == hdma_total;
  uint8_t *out = (uint8_t *)line_windows;
  for (int k = hdma_count; k < hdma_total; k++) {
    HdmaChannel *d = &hdma[k];
    SnesDmaChannel *c = d->c;
    const uint8_t *table = d->table, *data = d->data;
    bool table_ram = d->table_ram, data_ram = d->data_ram, indirect = d->indirect;
    uint8_t a1b = c->a1b, dasb = c->dasb, ntrl = c->ntrl;
    uint16_t a2a = c->a2a, das = c->das;
    bool transfer = c->hdma_do_transfer, terminated = c->hdma_terminated;
    int bytes = d->bytes;
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
          for (int j = 0; j < bytes; j++)
            wh[d->regs[j] - 0x26] = indirect ? ReadBank(data, data_ram, dasb, das++)
                                             : ReadBank(table, table_ram, a1b, a2a++);
        }
        ntrl--;
        transfer = ntrl & 0x80;
        if (!(ntrl & 0x7f)) {
          // The next entry, as HdmaNextEntry.
          ntrl = ReadBank(table, table_ram, a1b, a2a++);
          if (indirect) {
            das = ReadBank(table, table_ram, a1b, a2a++);
            das |= ReadBank(table, table_ram, a1b, a2a++) << 8;
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

// But for the windows' edges (bytes 47-50: the top of word 11 and the
// bottom of word 12).
_Static_assert(offsetof(PpuRegs, wh) == 47, "PpuRegs.wh");
static bool SameRegs(const PpuRegs *a, const PpuRegs *b) {
  const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
  for (unsigned i = 0; i < 11; i++) {
    if (x[i] != y[i])
      return false;
  }
  for (unsigned i = 13; i < sizeof(PpuRegs) / 4; i++) {
    if (x[i] != y[i])
      return false;
  }
  return !((x[11] ^ y[11]) & 0x00ffffff) && !((x[12] ^ y[12]) & 0xff000000);
}

int SnesHw_RunDisplay(PpuRegs *states, uint8_t *line_state, uint32_t *line_windows) {
  SnesHw *h = &g_snes_hw;
  SnesHw_HdmaInit();
  HdmaWindows(line_windows);
  int n = 0;
  h->regs_dirty = true;
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
    for (int end = v + quiet; v < end; v++)
      line_state[v] = n - 1;
  }
  return n;
}
