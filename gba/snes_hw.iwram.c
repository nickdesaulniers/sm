// See snes_hw.h. Register behavior follows src/snes/ppu.c, snes.c and
// dma.c, the emulator the PC build checks the game against.
//
// ARM code in IWRAM, as the game's DMA and the SNES's display of each frame
// run through here. (So no division: see cmake/compiler-rt.cmake.)
#include "snes_hw.h"

#include <string.h>

#include "gba_hw.h"
#include "sm_rtl.h"

SnesHw g_snes_hw IWRAM_BSS;
uint16_t g_cgram[256];
uint8_t g_oam[544];
uint16_t g_vram[kVramWords];
uint32_t g_vram_dirty[kVramChunks / 32];

static uint32_t wram_port_addr;

void SnesHw_Reset(void) {
  memset(&g_snes_hw, 0, sizeof(g_snes_hw));
  g_snes_hw.vram_inc = 1;
  for (int i = 0; i < 8; i++) {
    DmaChannel *c = &g_snes_hw.dma[i];
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

uint8_t SnesHw_ReadA(uint32_t addr) {
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

// DMA from the A bus to the PPU's memories without going through WriteBBus
// a byte at a time: palettes, OAM and VRAM, from a span of RAM or ROM, as
// the game does almost all its DMA. Returns false for anything else.
static bool FastDma(DmaChannel *c, uint32_t count) {
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
    memcpy(&g_cgram[h->cgram_ptr], src, count);
    h->cgram_ptr += count / 2;
    h->cgram_dirty = true;
  } else if (c->bbad == 0x04 && (mode == 0 || mode == 2) && !(h->oam_addr & 1) &&
             h->oam_addr + count <= 0x220) {
    // The low table and then the high table, as the game uploads OAM.
    uint32_t low = h->oam_addr < 0x200 ? 0x200 - h->oam_addr : 0;
    if (low > count)
      low = count;
    memcpy(&g_oam[h->oam_addr], src, low);
    memcpy(&g_oam[h->oam_addr + low], src + low, count - low);
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
      for (uint32_t i = 0; i < count; i += 2, p++) {
        g_vram[p & 0x7fff] = src[i] | (src[i + 1] << 8);
        if (!(p & 15))
          MarkVramDirty(p);
      }
      MarkVramDirty(h->vram_ptr);
      MarkVramDirty(p - 1);
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
    DmaChannel *c = &g_snes_hw.dma[i];
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
  DmaChannel *c = &g_snes_hw.dma[(reg >> 4) & 7];
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

static uint8_t ReadDmaReg(uint16_t reg) {
  DmaChannel *c = &g_snes_hw.dma[(reg >> 4) & 7];
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
  case HDMAEN: h->hdmaen = value; break;
  }
}

uint8 ReadReg(uint16 reg) {
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

uint16 ReadRegWord(uint16 reg) {
  uint16 lo = ReadReg(reg);
  return lo | (ReadReg(reg + 1) << 8);
}

void WriteRegWord(uint16 reg, uint16 value) {
  WriteReg(reg, (uint8)value);
  WriteReg(reg + 1, value >> 8);
}

void SnesHw_HdmaInit(void) {
  for (int i = 0; i < 8; i++) {
    DmaChannel *c = &g_snes_hw.dma[i];
    c->hdma_terminated = false;
    c->hdma_do_transfer = false;
    if (!(g_snes_hw.hdmaen & (1 << i)))
      continue;
    c->a2a = c->a1t;
    c->ntrl = SnesHw_ReadA((c->a1b << 16) | c->a2a++);
    if (c->ntrl == 0) {
      c->hdma_terminated = true;
      continue;
    }
    if (c->dmap & 0x40) {
      c->das = SnesHw_ReadA((c->a1b << 16) | c->a2a++);
      c->das |= SnesHw_ReadA((c->a1b << 16) | c->a2a++) << 8;
    }
    c->hdma_do_transfer = true;
  }
}

void SnesHw_HdmaRunLine(void) {
  for (int i = 0; i < 8; i++) {
    DmaChannel *c = &g_snes_hw.dma[i];
    if (!(g_snes_hw.hdmaen & (1 << i)) || c->hdma_terminated)
      continue;
    uint8_t mode = c->dmap & 7;
    if (c->hdma_do_transfer) {
      for (int j = 0; j < kTransferLength[mode]; j++) {
        uint32_t a = (c->dmap & 0x40) ? (c->dasb << 16) | c->das++ : (c->a1b << 16) | c->a2a++;
        WriteBBus(c->bbad + kBbadOffsets[mode][j], SnesHw_ReadA(a));
      }
    }
    c->ntrl--;
    c->hdma_do_transfer = c->ntrl & 0x80;
    if ((c->ntrl & 0x7f) == 0) {
      c->ntrl = SnesHw_ReadA((c->a1b << 16) | c->a2a++);
      if (c->dmap & 0x40) {
        c->das = SnesHw_ReadA((c->a1b << 16) | c->a2a++);
        c->das |= SnesHw_ReadA((c->a1b << 16) | c->a2a++) << 8;
      }
      if (c->ntrl == 0)
        c->hdma_terminated = true;
      c->hdma_do_transfer = true;
    }
  }
}

void Vector_IRQ(void);

// How many lines from now HDMA does nothing but count down: for each
// channel, the lines left in a table entry that doesn't repeat, after its
// first. Up to |max|.
static int HdmaQuietLines(int max) {
  for (int i = 0; i < 8 && max; i++) {
    DmaChannel *c = &g_snes_hw.dma[i];
    if (!(g_snes_hw.hdmaen & (1 << i)) || c->hdma_terminated)
      continue;
    int quiet = (c->hdma_do_transfer || (c->ntrl & 0x80)) ? 0 : (c->ntrl & 0x7f) - 1;
    if (quiet < max)
      max = quiet;
  }
  return max;
}

static void HdmaSkipLines(int lines) {
  for (int i = 0; i < 8; i++) {
    if ((g_snes_hw.hdmaen & (1 << i)) && !g_snes_hw.dma[i].hdma_terminated)
      g_snes_hw.dma[i].ntrl -= lines;
  }
}

static bool SameRegs(const PpuRegs *a, const PpuRegs *b) {
  const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
  for (unsigned i = 0; i < sizeof(PpuRegs) / 4; i++) {
    if (x[i] != y[i])
      return false;
  }
  return true;
}

int SnesHw_RunDisplay(PpuRegs *states, uint8_t *line_state) {
  SnesHw *h = &g_snes_hw;
  SnesHw_HdmaInit();
  int n = 0;
  h->regs_dirty = true;
  for (int v = 0; v < kSnesLines; v++) {
    // HDMA often writes registers with the values they have.
    if (h->regs_dirty) {
      h->regs_dirty = false;
      if (!n || !SameRegs(&states[n - 1], &h->r))
        states[n++] = h->r;
    }
    line_state[v] = n - 1;
    bool irq = (h->nmitimen & 0x30) && h->vtime < kSnesLines;
    // Lines with nothing for HDMA to do, before the IRQ at the end of its
    // line, have this line's registers.
    int quiet = HdmaQuietLines((irq && h->vtime >= v ? h->vtime : kSnesLines - 1) - v);
    if (quiet) {
      HdmaSkipLines(quiet);
      for (int end = v + quiet; v < end; v++)
        line_state[v + 1] = n - 1;
    }
    SnesHw_HdmaRunLine();
    if (irq && v == h->vtime)
      Vector_IRQ();
  }
  return n;
}
