// The GBA's buttons, as the SNES joypad and the item the game should have
// selected: see input.c.
#pragma once

#include <stdint.h>

// Reads the buttons, once a frame of the game, before it.
uint16_t Input_ReadJoypad(void);
