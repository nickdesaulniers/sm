// The interrupt handler, which the BIOS calls in ARM state.
#include "gba_hw.h"

volatile uint32_t g_vblank_count IWRAM_BSS;
// The GBA's registers 0x04000000-0x0400002F for each line of the picture
// on show (see render.c), or null.
const uint32_t *volatile g_line_regs IWRAM_BSS;

void IrqHandler(void) {
  uint16_t flags = REG_IF & REG_IE;
  if (flags & kIrqVBlank) {
    g_vblank_count++;
    // Line 0's registers now, and HBlank DMA sets up each of the others in
    // the HBlank before it. Every frame, as a frame of the game can take
    // more than one of the GBA's.
    const uint32_t *regs = g_line_regs;
    if (regs) {
      REG_DMACNT(0) = 0;
      volatile uint32_t *io = (volatile uint32_t *)0x04000000;
      for (int i = 0; i < 12; i++)
        io[i] = regs[i];
      REG_DMASAD(0) = (uintptr_t)(regs + 12);
      REG_DMADAD(0) = 0x04000000;
      REG_DMACNT(0) = 12 | kDma32 | kDmaDestReload | kDmaRepeat | kDmaAtHBlank | kDmaEnable;
    }
  }
  REG_IFBIOS |= flags;
  REG_IF = flags;
}
