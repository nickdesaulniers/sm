// A frame's GBA display registers, line by line, as render.iwram.c builds
// them and irq.iwram.c sets them while the GBA draws the frame.
//
// Registers that change from line to line (the backgrounds' scrolling, or
// in frames with Mode 7, BG2's transformation) are a stream that HBlank DMA
// copies in before each line. The others (which layers show, and their
// settings) only change at a few lines, such as the HUD's last: they're in
// segments, which the VCount interrupt sets in the HBlank before their
// first line.
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
  kGbaLines = 160,
};

// SameSegment (render.iwram.c) compares all after first_line.
typedef struct DisplaySegment {
  uint16_t first_line;
  uint16_t dispcnt;
  uint16_t bgcnt[4];
  // Color math: BLDCNT, BLDALPHA, BLDY.
  uint16_t bld[3];
  // The scroll registers, in frames whose stream is BG2's transformation.
  uint16_t ofs[8];
} DisplaySegment;

typedef struct DisplayFrame {
  // Four words per line, and one more: DMA copies in an entry at the end of
  // the last line. In IWRAM, as DMA reads it every line.
  const uint32_t (*stream)[4];
  // 0x04000010 (BG0HOFS-BG3VOFS) or 0x04000020 (BG2PA-BG2Y).
  uint32_t stream_dest;
  int num_segments;
  const DisplaySegment *segments;
} DisplayFrame;

// The frame on show, or null: irq.iwram.c sets it up at every VBlank.
extern const DisplayFrame *volatile g_display;

// Sets up g_display now, in VBlank, with interrupts disabled.
void Display_Start(void);
