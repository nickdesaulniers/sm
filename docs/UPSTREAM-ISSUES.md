# Issues to report to snesrev/sm

Problems in the decompilation itself (`src/`), found while porting it to
the GBA, that affect the upstream PC build too or would affect any other
port. Each has the fix this branch made, to offer upstream. None has been
reported yet.

## 1. Overlapping `memcpy` when a room loads

**Where:** `src/sm_82.c`, `LoadLevelDataAndOtherThings` and
`LoadLevelScrollAndCre`.

**What:** each copies the room's custom background and BTS (block types)
out of its decompressed level data to just after it:

```c
memcpy(custom_background, (uint8 *)level_data + size + (size >> 1), size);
memcpy(BTS, (uint8 *)level_data + size, size >> 1);
```

In large rooms the source and destination overlap. That's undefined
behavior for `memcpy`. glibc happens to copy such buffers correctly, so the
PC build works. A plain front-to-back `memcpy` (as on the GBA) corrupts the
data: on the landing site, Samus couldn't climb the hills, because their
slopes' BTS came out wrong.

**Fix:** `memmove` for these four calls (commit 1078097 on this branch).
A Zebes playthrough on the PC build with an overlap-checking `memcpy`
caught the overlaps (in `LoadLevelDataAndOtherThings`; the other function
copies the same way) and found no other overlapping copies.

## 2. Functions used in only one file aren't `static`

**Where:** throughout `src/`, with prototypes in `src/funcs.h`.

**What:** 2111 functions are only called (or named, as by `FUNC16`) in
the file that defines them, but are declared in `funcs.h` and defined
without `static`. At `-O3` many are inlined at every call site, yet the
compiler must still emit a standalone copy of each, as it has external
linkage: about 240 KB of code nothing calls. Being non-static also hides
from the compiler that it sees every caller.

**Fix:** make them `static`, and move their prototypes from `funcs.h` into
their files (commit b74d1c0). The PC build (gcc, `-Werror`) still builds,
and the C still matches the original frame by frame.

## 3. A leftover debugging `printf`

**Where:** `src/sm_90.c`, `Samus_HandleAnimDelay`.

**What:** `printf("v1=%x\n", v1);` runs whenever Samus's animation reaches
a delay marker, which happens every few frames during play, and prints to
stdout.

**Fix:** remove it (commit 5691898).
