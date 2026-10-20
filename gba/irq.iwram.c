// The interrupt handler, which the BIOS calls in ARM state.
#include "audio.h"
#include "display.h"
#include "gba_hw.h"

volatile uint32_t g_vblank_count IWRAM_BSS;
const DisplayFrame *volatile g_display IWRAM_BSS;
static int next_segment IWRAM_BSS;

static void SetSegment(const DisplayFrame *d, const DisplaySegment *s) {
  REG_DISPCNT = s->dispcnt;
  for (int i = 0; i < 4; i++)
    REG_BGCNT(i) = s->bgcnt[i];
  if (d->stream_dest != 0x04000010) {
    volatile uint16_t *ofs = &REG_BGHOFS(0);
    for (int i = 0; i < 8; i++)
      ofs[i] = s->ofs[i];
  }
}

// The VCount interrupt at the line before the next segment's, if any.
static void ScheduleSegment(const DisplayFrame *d) {
  uint16_t stat = REG_DISPSTAT & (kDispstatVBlankIrq | kDispstatHBlankIrq);
  if (next_segment < d->num_segments)
    stat |= kDispstatVCountIrq | (d->segments[next_segment].first_line - 1) << 8;
  REG_DISPSTAT = stat;
}

void Display_Start(void) {
  const DisplayFrame *d = g_display;
  if (!d)
    return;
  REG_DMACNT(0) = 0;
  SetSegment(d, &d->segments[0]);
  volatile uint32_t *dest = (volatile uint32_t *)d->stream_dest;
  for (int i = 0; i < 4; i++)
    dest[i] = d->stream[0][i];
  REG_DMASAD(0) = (uintptr_t)d->stream[1];
  REG_DMADAD(0) = d->stream_dest;
  REG_DMACNT(0) = 4 | kDma32 | kDmaDestReload | kDmaRepeat | kDmaAtHBlank | kDmaEnable;
  next_segment = 1;
  ScheduleSegment(d);
}

static void VBlank(void) {
  g_vblank_count++;
  Audio_VBlank();
  // Every frame, as a frame of the game can take more than one of the
  // GBA's.
  Display_Start();
}

static void VCount(void) {
  const DisplayFrame *d = g_display;
  if (!d)
    return;
  // This segment, and the next if it starts a line later. (One that
  // starts later still gets an interrupt of its own; one that's late
  // starts as soon as can be.)
  while (next_segment < d->num_segments) {
    unsigned line = d->segments[next_segment].first_line - 1, now = REG_VCOUNT;
    if (line > now + 1)
      break;
    if (line >= now) {
      while (REG_VCOUNT < line) {}
      while (!(REG_DISPSTAT & 2)) {}  // HBlank
    }
    SetSegment(d, &d->segments[next_segment++]);
  }
  ScheduleSegment(d);
}

void IrqHandler(void) {
  uint16_t flags = REG_IF & REG_IE;
  if (flags & kIrqVBlank)
    VBlank();
  if (flags & kIrqVCount)
    VCount();
  REG_IFBIOS |= flags;
  REG_IF = flags;
}
