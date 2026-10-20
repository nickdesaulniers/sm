// Minimal C library for the GBA. Implemented in ../stdlib.c.
#pragma once

#include <stddef.h>

// A bump allocator over the end of EWRAM: free() does nothing.
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void free(void *p);
_Noreturn void abort(void);
_Noreturn void exit(int status);
int abs(int x);
