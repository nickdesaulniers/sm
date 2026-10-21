#pragma once
#include <stdbool.h>

// For testing: see tour.c. Before each frame of the game; and after, whether
// to hold the frame on screen for a screenshot.
bool Tour_BeforeFrame(void);
bool Tour_AfterFrame(void);
