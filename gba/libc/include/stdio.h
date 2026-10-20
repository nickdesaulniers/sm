// Minimal C library for the GBA: output goes to mGBA's debug log, a line
// at a time. There are no files. Implemented in ../stdio.c.
#pragma once

#include <stdarg.h>
#include <stddef.h>

typedef struct FILE FILE;
extern FILE *const stdout;
extern FILE *const stderr;
#define EOF (-1)

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vprintf(const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
