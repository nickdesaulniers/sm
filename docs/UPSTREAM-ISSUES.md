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

## 4. The menu tilemap is declared a quarter of its size

**Where:** `src/ida_types.h`, `Ram3000_Menu`; used in `src/sm_81.c`.

**What:** `menu_tilemap` ($7E:3600) is declared `uint8 menu_tilemap[512]`,
but every use treats it as 2048 bytes: the menus DMA 2048 bytes from it,
`ClearMenuTilemap`'s loop fills 1024 words of it, and
`FileSelectMenu_1_LoadFileSelectMenuBG2` fills it through another member
of the union, `ram3000.pause_menu_map_tilemap[i + 768]` for `i` up to 1023,
past that array's 1024 entries. It's all within `g_ram`, so nothing goes
wrong, but UBSan reports the index out of bounds.

**Fix:** declare it `uint8 menu_tilemap[2048]`, and fill it as itself in
`FileSelectMenu_1_LoadFileSelectMenuBG2` (this branch, the commit that added this entry). The
C still matches the original frame by frame.

## 5. Tables indexed by a block's BTS, past their end

**Where:** `src/sm_94.c`: `BlockInsideReact_SpikeAir`,
`BlockColl_Horiz_SpikeBlock` and `BlockColl_Vert_SpikeBlock`, and
`BlockReactGrapple_GrappleBlock` and `BlockReactGrapple_SpikeBlock`.

**What:** each indexes a table of handlers (or PLMs) by the block's BTS
byte, which can be anything: the C's tables have 16 entries (the grapple
block's 4), and the ROM's are as short (the spike air table at
`$94:98CC` is 8 entries: the next function starts 16 bytes on), so a
larger BTS reads past the table, where the SNES would take the next
code's bytes for addresses. In C it's undefined; on the GBA, with Samus
held inside a block in a Maridia room by a test that walks every room,
it called a null pointer and reset (which pointer depended on the build).

**Fix:** a BTS past the table does nothing (commit on this branch, with
this entry).
