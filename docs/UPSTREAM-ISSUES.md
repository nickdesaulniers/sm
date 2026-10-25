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
`FileSelectMenu_1_LoadFileSelectMenuBG2` (commit f6c58ee). The C still
matches the original frame by frame.

## 5. Tables indexed by a block's BTS, past their end

**Where:** `src/sm_94.c`: `BlockInsideReact_SpikeAir`,
`BlockColl_Horiz_SpikeBlock` and `BlockColl_Vert_SpikeBlock`,
`BlockReactGrapple_GrappleBlock` and `BlockReactGrapple_SpikeBlock`, and
`BlockInsideReact_SpecialAir`.

**What:** each indexes a table of handlers (or PLMs) by the block's BTS
byte, which can be anything: the C's tables have 16 entries (the grapple
block's 4; the special air table's, at `$94:9966`, 80), and the ROM's are
as short (the spike air table at `$94:98CC` is 8 entries: the next
function starts 16 bytes on; the special air table is followed by
data), so a larger BTS reads past the table, where the SNES would take
the next code's bytes for addresses. In C it's undefined; on the GBA, with
Samus held inside a block in a Maridia room by a test that walks every room,
it called a null pointer and reset (which pointer depended on the build).

**Fix:** a BTS past the table does nothing (commit be704b4; and for
`BlockInsideReact_SpecialAir`, whose BTS 80 to 127 read the data after its
table, the commit that adds the developer tools). That one showed up
when adding a few statics moved the others: with Samus outside the room (the
tour's door transitions run her handler with her at x = -185 for a moment;
the developers' debug station Norfair 21 starts her out of bounds), her
block index is far past the room's, and `BTS[cur_block_index]` reads past
the end of `g_ram`, into whatever follows. The tour reset in Maridia's
room D2D9 and, once the new statics went last, its pictures didn't move.
Keeping Samus's block inside the room's would be the root fix; issue 7 is
the one made.

## 6. The debug soft reset runs a state that isn't

**Where:** `src/sm_80.c`, `ReadJoypadInputs`, and `src/sm_82.c`,
`RunOneFrameOfGameInner`.

**What:** with `enable_debug` set (the developers' switch, which the load-game
map's station list and more depend on), holding Start, Select, L and R calls
`SoftReset` from `ReadJoypadInputs`. That sets `game_state` to 0xffff, as the
other callers do, but they're inside a state's function: this one is at the
top of the frame, which then goes on to `kGameStateFuncs[game_state]`, 65535
entries past a table of 45. On the PC build that's undefined; on the GBA it
jumped into the weeds. (The SNES's reset vector runs at once.)

**Fix:** `RunOneFrameOfGameInner` returns after `ReadJoypadInputs` if
`game_state` is 0xffff, and the frame ends as the other soft resets do
(commit: the one that adds the developer tools).

## 7. A block index of -1 reads past `level_data` and `BTS`

**Where:** `src/sm_94.c` `CalculateBlockAt`, and every read of
`level_data[cur_block_index]` and `BTS[cur_block_index]` (94 of them, in
`src/sm_94.c`, `src/sm_a0.c`, `src/sm_86.c` and `src/sm_a3.c`: Samus's,
the enemies' and the enemy projectiles' block collisions, slopes).

**What:** `CalculateBlockAt` sets `cur_block_index` to -1 for a point
outside the room (Samus's, for a moment in a door transition, or spawned
out of bounds), and the callers go on to read the block there. The SNES
reads with a 16-bit X (`LDA $7F0002,X`, `LDA $7F6402,X`): `level_data`'s
read wraps to `$80:0000`, the mirror of `$7E:0000`, and `BTS`'s is
`$80:6401`, the open bus. In C, `level_data[0xffff]` and `BTS[0xffff]` are
128 KB and 64 KB past the start of the arrays, past the end of `g_ram`:
undefined. On the GBA they read whatever follows `g_ram` (the copy of
VRAM), so the block's type and BTS were garbage: a BTS of 0x80 or more in
`BlockInsideReact_SpecialAir` spawns a PLM from a table by it, which is how
the tour reset in room D2D9 (issue 5) and could leave a door transition on
a black screen.

**Fix:** `LevelDataAt` and `BtsAt` (`src/variables.h`) read as the SNES
does, `$7E:0000` and the open bus's `$7F` (the instruction's bank byte, the
last thing on the bus, as emulators have it), and the reads go through
them; in the room, they are the arrays. Found by smrun's MEMCHECK
(`tools/memcheck_ranges.py`), which logs every load and store the game's
code makes outside the game's RAM: with this, none in the room tour or the
239 scenarios.

