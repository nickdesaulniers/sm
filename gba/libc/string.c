// The C library functions the game and the compiler call. Clang emits
// calls to memcpy/memset/memmove on its own (for struct copies, zeroing, and
// recognized loops), so these must exist.
//
// Built -fno-builtin, and the functions carry no_builtin too, so that the
// optimizer doesn't turn these loops back into calls to themselves; and
// noinline, for one copy of each in IWRAM, not one per __aeabi_ alias.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NO_BUILTIN __attribute__((no_builtin, noinline))

// In IWRAM, as ARM code (see CMakeLists.txt): the game copies a lot.
#pragma clang section text = ".iwram.text.libc"

// For word-at-a-time access to memory of any type.
typedef uint32_t __attribute__((may_alias)) Word;

// A word at a time when the two are equally aligned: the ARM7TDMI takes
// about as long per word as per byte, more so from 16-bit-wide memory.
NO_BUILTIN void *memcpy(void *__restrict dst, const void *__restrict src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if ((((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
    for (; n && ((uintptr_t)d & 3); --n)
      *d++ = *s++;
    Word *dw = (Word *)d;
    const Word *sw = (const Word *)s;
    for (; n >= 16; n -= 16) {
      dw[0] = sw[0];
      dw[1] = sw[1];
      dw[2] = sw[2];
      dw[3] = sw[3];
      dw += 4;
      sw += 4;
    }
    for (; n >= 4; n -= 4)
      *dw++ = *sw++;
    d = (unsigned char *)dw;
    s = (const unsigned char *)sw;
  }
  while (n--)
    *d++ = *s++;
  return dst;
}

NO_BUILTIN void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if (d < s) {
    while (n--)
      *d++ = *s++;
  } else {
    d += n;
    s += n;
    while (n--)
      *--d = *--s;
  }
  return dst;
}

NO_BUILTIN void *memset(void *dst, int value, size_t n) {
  unsigned char *d = dst;
  unsigned char byte = (unsigned char)value;
  for (; n && ((uintptr_t)d & 3); --n)
    *d++ = byte;
  Word *dw = (Word *)d;
  uint32_t word = byte * 0x01010101u;
  for (; n >= 16; n -= 16) {
    dw[0] = word;
    dw[1] = word;
    dw[2] = word;
    dw[3] = word;
    dw += 4;
  }
  for (; n >= 4; n -= 4)
    *dw++ = word;
  d = (unsigned char *)dw;
  while (n--)
    *d++ = byte;
  return dst;
}

// The rest of the C library is seldom used: in ROM.
#pragma clang section text = ""

NO_BUILTIN int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (; n; --n, ++x, ++y) {
    if (*x != *y)
      return *x - *y;
  }
  return 0;
}

NO_BUILTIN void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = s;
  for (; n; --n, ++p) {
    if (*p == (unsigned char)c)
      return (void *)p;
  }
  return NULL;
}

NO_BUILTIN size_t strlen(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  return (size_t)(p - s);
}

NO_BUILTIN char *strcpy(char *__restrict dst, const char *__restrict src) {
  char *d = dst;
  while ((*d++ = *src++) != 0) {}
  return dst;
}

NO_BUILTIN int strcmp(const char *a, const char *b) {
  for (; *a && *a == *b; ++a, ++b) {}
  return (unsigned char)*a - (unsigned char)*b;
}

// ARM run-time ABI memory helpers, which clang emits instead of memcpy/
// memset on EABI targets. The 4/8 variants promise aligned pointers, which
// these simple versions don't need. Note __aeabi_memset's argument order
// differs from memset's.
// https://github.com/ARM-software/abi-aa/blob/main/rtabi32/rtabi32.rst
#pragma clang section text = ".iwram.text.libc"
void __aeabi_memcpy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy4(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy8(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memmove(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
void __aeabi_memmove4(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
void __aeabi_memmove8(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
void __aeabi_memset(void *dst, size_t n, int value) { memset(dst, value, n); }
void __aeabi_memset4(void *dst, size_t n, int value) { memset(dst, value, n); }
void __aeabi_memset8(void *dst, size_t n, int value) { memset(dst, value, n); }
void __aeabi_memclr(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr4(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr8(void *dst, size_t n) { memset(dst, 0, n); }
