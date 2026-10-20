// The SNES hardware the game drives through WriteReg/ReadReg: the PPU's
// memories and registers, the CPU's I/O registers, and DMA/HDMA. Nothing
// here draws: render.c translates this state into the GBA's.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// PPU registers that can change partway down the screen (by HDMA or the
// game's IRQ handlers), so are captured line by line. Field names follow
// the registers'.
typedef struct PpuRegs {
  uint8_t inidisp;
  uint8_t bgmode;
  uint8_t mosaic;
  uint8_t bgsc[4];
  uint8_t bgnba[2];
  uint16_t hofs[4], vofs[4];
  uint8_t m7sel;
  int16_t m7[6];  // M7A, M7B, M7C, M7D, M7X, M7Y
  uint16_t m7hofs, m7vofs;
  uint8_t wsel[3];  // W12SEL, W34SEL, WOBJSEL
  uint8_t wh[4];
  uint8_t wbglog, wobjlog;
  uint8_t tm, ts, tmw, tsw;
  uint8_t cgwsel, cgadsub;
  uint8_t coldata[3];  // red, green, blue
  uint8_t setini;
} PpuRegs;

typedef struct SnesDmaChannel {
  uint8_t dmap, bbad;
  uint16_t a1t;
  uint8_t a1b;
  uint16_t das;  // also the indirect HDMA address
  uint8_t dasb;  // the indirect HDMA bank
  uint16_t a2a;  // the HDMA table address
  uint8_t ntrl;  // the HDMA line counter
  uint8_t unused;
  bool hdma_do_transfer;
  bool hdma_terminated;
} SnesDmaChannel;

enum {
  kVramWords = 0x8000,
  // VRAM changes are tracked in chunks of 16 words: a 4bpp tile.
  kVramChunkShift = 4,
  kVramChunks = kVramWords >> kVramChunkShift,
};

typedef struct SnesHw {
  PpuRegs r;
  uint8_t obsel;
  // Write-twice latches.
  uint8_t bgofs_latch, bghofs_latch, m7_latch;

  uint16_t vram_ptr, vram_inc, vram_read_buf;
  uint8_t vmain;
  uint8_t cgram_ptr, cgram_latch;
  bool cgram_high;
  uint16_t oam_addr, oam_addr_reload;
  uint8_t oam_latch;
  bool oam_high_byte;

  // CPU I/O registers.
  uint8_t nmitimen, hdmaen;
  uint16_t htime, vtime;
  uint8_t wrmpya;
  uint16_t wrdiv;
  uint16_t rddiv, rdmpy;
  uint16_t joy1;

  SnesDmaChannel dma[8];

  // What changed since render.c last looked (and g_vram_dirty).
  bool cgram_dirty, oam_dirty, regs_dirty;
} SnesHw;

// In IWRAM: the game's register writes and HDMA go through it all the time.
extern SnesHw g_snes_hw;

// The PPU's memories, in EWRAM. OAM is 544 bytes: 128 four-byte entries,
// then 32 bytes of high bits.
extern uint16_t g_cgram[256];
extern uint8_t g_oam[544];
extern uint16_t g_vram[kVramWords];
extern uint32_t g_vram_dirty[kVramChunks / 32];

void SnesHw_Reset(void);

// HDMA, as the PPU runs down the screen: Init before line 0, then Run at
// the end of each line, to set up the next.
void SnesHw_HdmaInit(void);
void SnesHw_HdmaRunLine(void);

enum { kSnesLines = 224 };

// Runs the SNES's display of a frame: HDMA, and the game's IRQ handlers,
// which change registers partway down the screen. Returns the PPU's
// registers for each line, as states[line_state[line]] with a state for
// each change: the number of states.
int SnesHw_RunDisplay(PpuRegs *states, uint8_t *line_state);

// Reads a byte from the SNES's A bus: work RAM or ROM.
uint8_t SnesHw_ReadA(uint32_t addr);
