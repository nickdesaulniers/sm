// See include/stdlib.h.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// gba.ld: from the end of EWRAM's static data to the end of EWRAM.
extern char __heap_start[], __heap_end[];
static char *heap_next = __heap_start;

void *malloc(size_t size) {
  size = (size + 3) & ~(size_t)3;
  if (size > (size_t)(__heap_end - heap_next)) {
    printf("malloc: out of memory (%u bytes)\n", (unsigned)size);
    abort();
  }
  void *p = heap_next;
  heap_next += size;
  return p;
}

void *calloc(size_t count, size_t size) {
  void *p = malloc(count * size);
  memset(p, 0, count * size);
  return p;
}

void free(void *p) {
  (void)p;
}

_Noreturn void abort(void) {
  for (;;)
    __asm__ volatile("");
}

_Noreturn void exit(int status) {
  printf("exit(%d)\n", status);
  abort();
}

int abs(int x) {
  return x < 0 ? -x : x;
}

void __assert_fail(const char *expr, const char *file, int line) {
  printf("%s:%d: assertion failed: %s\n", file, line, expr);
}
