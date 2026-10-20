// Converting SNES tiles and maps to the GBA's formats: see convert.iwram.c.
#pragma once

#include <stdint.h>

void Convert_Init(void);
void Convert_Tiles4bpp(volatile uint32_t *dst, const uint16_t *src, int count);
void Convert_Tiles2bpp(volatile uint32_t *dst, const uint16_t *src, int count);
int Convert_Map(volatile uint16_t *dst, const uint16_t *src, int count, uint32_t pal_add);
int Convert_MapSplit(volatile uint16_t *hi, volatile uint16_t *lo, const uint16_t *src,
                     int count, uint32_t blank);
