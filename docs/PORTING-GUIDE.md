# Porting a SNES game to the GBA: a guide for agents

This guide is for an agent (or a person) setting out to port a SNES game to
the Game Boy Advance as this repository ported Super Metroid. It records the
approach that worked, how the hardware maps across, the tools that made the
work checkable, and the pitfalls that cost the most time. File paths refer
to this repository. Read the code alongside this guide: the comments in
`gba/` explain the details.

## 1. Start from C, not from an emulator

Emulating a SNES on a GBA is far too slow for a demanding game. The GBA has
about 280,000 CPU cycles per frame, and the SNES's CPU, graphics chip and
sound CPU all need emulating at once. This port works because a
**decompilation** exists ([snesrev/sm](https://github.com/snesrev/sm)): the
game's code as C, checked frame by frame against the original. Look for one
first (snesrev has others, such as Zelda 3). Without one, budget for writing
it, which is a far larger project than the port.

Keep the game's logic untouched, compile it for the ARM7TDMI, and replace
only the platform layer: video, sound, input, saves and timing. When the
game's own code has a real bug that the PC's C library hides, fix the game
source (see §7), and keep the fix to the smallest change.

A thin compatibility layer is fine to start with. Here `gba/snes_hw.c` keeps
the game's `WriteReg`/`ReadReg` calls working by modelling VRAM, CGRAM, OAM,
DMA and HDMA. Make it fast later; don't rewrite the game to avoid it.

## 2. The toolchain

- **clang and lld only**, `--target=armv4t-none-eabi -mcpu=arm7tdmi`. See
  `cmake/gba-toolchain.cmake`. No GCC or devkitARM is needed.
- **No sysroot.** You write the startup code (`gba/crt0.S`), the linker
  script (`gba/gba.ld`), the cartridge header (`gba/header.c`) and a
  minimal C library (`gba/libc/`).
- **compiler-rt from source** (`cmake/compiler-rt.cmake`), for division
  and 64-bit operations. It is built as Thumb, because ARMv4T's `pop {pc}`
  doesn't switch between ARM and Thumb. **ARM-mode code must never call
  these helpers**: keep division out of `*.iwram.c`, and check with
  `llvm-nm` when in doubt.
- **Thumb by default, ARM for hot code.** Everything is `-mthumb` except
  `*.iwram.c`, which is `-marm` and linked into IWRAM. ARM runs well from
  the 32-bit, zero-wait-state IWRAM, while Thumb suits the 16-bit cartridge
  bus. `ROM_CODE` (`gba/gba_hw.h`) moves cold functions in an IWRAM file
  back to ROM as Thumb.
- **Never ship the ROM's data.** The build makes `sm-engine.gba` with no
  game data, and `tools/make_cartridge.py` appends the player's own ROM
  (checked by sha1) along with preprocessed assets. CI builds and releases
  the engine only.

## 3. Memory: plan IWRAM like a budget

| Region | Size   | Speed            | Use here |
|--------|--------|------------------|----------|
| IWRAM  | 32 KB  | 32-bit, 0 wait   | Hot ARM code, the SNES's registers, DMA state, a 2 KB stack |
| EWRAM  | 256 KB | 16-bit, 2 wait   | The game's 128 KB of RAM, SNES VRAM (64 KB), CGRAM, OAM |
| VRAM   | 96 KB  | 16-bit           | GBA tiles and maps, converted |
| ROM    | 32 MB  | 16-bit, 3/1 wait | Code (Thumb), the game's data, preprocessed assets |

IWRAM always runs out first. The linker script fails the build when IWRAM
code and data run into the stack (`IWRAM overflows into the stack`).
Measure with `llvm-nm sm.elf | grep __iwram_bss_end`, and move once-a-frame
code to `ROM_CODE` to make room.

Where the SNES has memory the GBA can't spare, find out why the game needs
it. The SNES's 64 KB of sound RAM only ever holds data uploaded from the
ROM, so the sound driver reads the ROM directly (`gba/audio.c`).

## 4. Mapping the hardware

### Alignment

The game casts byte pointers to `uint16*` and `uint32*` at any address. The
ARM7TDMI rotates misaligned loads, so they return garbage without
crashing. `src/types.h` makes the game's integer types 1-byte aligned on
the GBA, so the compiler emits byte loads. Your own code that reads ROM
data must do the same, or read bytes explicitly, as `gba/tour.c` does:
SNES data structures sit at odd addresses.

### Backgrounds

- SNES Mode 1 maps to GBA Mode 0. One 4bpp SNES layer is split into two
  GBA layers by the tile priority bit, so that sprites can sit between its
  halves. The other 4bpp layer takes a third GBA layer, and the 2bpp layer
  is widened to 4bpp on the fourth (`gba/render.iwram.c`'s header comment
  has the full mapping).
- Pick which layer to split per frame: some rooms put their scenery in the
  second layer's priority tiles. Count priority tiles while converting maps.
- **Equal priorities:** on the GBA, the lower-numbered background wins a
  tie. Any time you raise a layer to priority 0, check what else is there.
- SNES Mode 7 maps to GBA Mode 1's affine BG2, with a tiled layer kept for
  the HUD in the same frame.
- Convert tiles and maps incrementally, from a dirty bitmap of SNES VRAM
  in 16-entry chunks. Full reconversion is far too slow.
- 64x64 maps don't fit the screenblocks you'll have left. Convert the half
  on screen.
- **The SNES's first visible line shows background row VOFS + 1.** Add 1
  to every vertical scroll, or per-line scrolling effects come out a line
  off.

### HDMA and per-line effects

Run HDMA once per frame into a table of register states per line, recording
only real changes. Then:

- Registers that change every line, such as scrolling, Mode 7 parameters
  and window edges, go in a stream that GBA HBlank DMA copies in. Write
  line 0's entry directly, then point DMA at entry 1. HBlank DMA after line
  *n* affects line *n* + 1.
- Registers that change at a few lines, such as the HUD's bottom edge, go
  in "segments" set by the VCount interrupt.

### Color math and windows

The SNES adds or subtracts a whole sub screen. The GBA blends one layer with
the layer directly beneath it (BLDCNT, BLDALPHA, BLDY). The mappings that
work:

- **Adding the sub screen:** draw its layers on top and blend them with the
  layer below. The sum is the same, because addition is commutative.
- **Main screen = the effect layer only, room on the sub screen:** draw the
  room normally and the effect layer on top, blended.
- **Effect added to some main layers only:** move the effect layer between
  them in priority, shifting the others and the sprites.
- **Subtracting:** the GBA can't. Three approximations, picked each frame by
  their error at a few dozen sampled pixels (main and sub screen, per
  channel, clamped at 0 as the SNES does):
  - darken under the sub layer: EVA = 0, EVB < 16;
  - subtract the sub screen's average colour in the palette;
  - for an effect layer covering the screen, subtract a colour S in the
    palette, and draw the layer on top, with each colour c replaced by
    S − c, blended additively. This is exact wherever the main colour is at
    least S.

  Stagger the sample points so they don't all fall on the same row of
  their tiles. If the effect layer shares palettes with the HUD, switch its
  colours in mid-frame.
- **The fixed color:** fold it into the palette.
- **Windows:** SNES window 1 → WIN0, window 2 → WIN1, with their edges
  in the HBlank stream.

### Sprites

Map SNES OAM to GBA OAM every frame that OAM changes. SNES sprite priorities
map to GBA priorities relative to the background layout. Keep a copy of
each sprite tile row so that rows that wrap around still show correctly.

### Screen size

The GBA's screen is 240x160 and the SNES's is 256x224. Crop 1:1 during
gameplay, following the player, with the HUD pinned. On menus and text
screens, squash by dropping the same rows of each tile so text stays
readable.

### Sound

- If the game's sound driver is decompiled, run it on the main CPU.
- Replace the DSP with a mixer: ARM assembly in IWRAM, at a modest rate
  (here 10512 Hz), with stereo through DMA sound A and B.
- Decode BRR samples to PCM at cartridge-build time.
- Run the mixer from a timer interrupt after VBlank, not from the game
  loop, so a slow game frame doesn't cascade into slow sound frames.

### Input

The GBA has no X or Y. In gameplay, design the controls the way a GBA game
in the same genre would (here *Metroid: Zero Mission*). Translate them into
the SNES joypad bits the game reads (`gba/input.c`). Keep the SNES buttons'
own names in menus.

## 5. Verification: make every claim checkable

The work went fastest whenever a bug could be reproduced and compared
against ground truth headlessly. Build these tools early.

- **A headless runner** (`tools/mgba/smrun.c`, on libmgba): it scripts
  buttons (`KEYS`), takes screenshots at frames or on log lines (`SHOTS`,
  `SHOT_ON`), dumps memory (`DUMP`), records audio (`AUDIO`), and profiles
  cycles by function and instruction (`PROFILE`, `PCS`).
- **A logic oracle** (`tools/hostrun.c`): the cartridge built with
  `-DSM_TRACE=ON` logs the joypad and a hash of game state each frame.
  `hostrun` replays that joypad on the PC build, which is itself verified
  against the original game, and prints the same log to diff. The first
  divergent frame points at the bug.
- **A picture oracle** (`tools/snesref.c`): it takes a memory dump, finds
  the SNES's VRAM, CGRAM, OAM and per-line registers in it by symbol, and
  draws the frame with the PC build's PPU. That is what a SNES would have
  shown from the very same state.
- **A coverage sweep** (`tools/room-tour.sh`): `-DSM_ROOM_TOUR=ON` walks
  the ROM's door graph and sends the player into every room. Then
  `tools/room_compare.py` ranks the rooms by the pixel difference from
  `snesref`. Re-run it after every renderer change and check that nothing
  regressed. One sweep found half a dozen renderer bugs that playing never had.
- **A sound oracle** (`tools/spcref.c`, `tools/compare_audio.py`): replay
  the game's sound requests through the original driver and an emulated
  DSP, then compare the loudness envelopes and spectra.
- **Shortcuts:** `-DSM_SKIP_INTRO`, `-DSM_START_ZEBES` and `-DSM_TEST_ITEMS`
  get to the interesting state in seconds.

When the comparison tool and the port disagree, suspect both. The
comparison script once silently compensated for the renderer's off-by-one
line.

## 6. Performance

- **Profile before optimizing.** The game's own logic was never the
  bottleneck. The compatibility layer was: byte-at-a-time DMA, recording
  redundant HDMA writes, and rebuilding line registers.
- Budget: about 280,000 cycles per frame. Log each 60 frames' game, render
  and audio cycles, and the VBlanks taken, as `gba/main.c` does.
- Put hot loops in IWRAM as ARM, and use the GBA's DMA for bulk copies from
  ROM.
- Precompute everything that can be computed at cartridge-build time.
- Cache derived state keyed on its inputs (window regions, palette by
  brightness), and convert only what changed.

## 7. Pitfalls that cost the most

1. **Undefined behavior that the PC hides.** The game copied overlapping
   buffers with `memcpy`. glibc copies them safely, but a fast forward
   `memcpy` corrupts them. The symptom, Samus unable to climb slopes, was
   far from the cause. Find such cases with an overlap-checking `memcpy`
   on the PC build, and fix the game source with `memmove`.
2. **Misaligned reads** of ROM structures (see §4).
3. **The VOFS + 1 rule** (see §4).
4. **Priority ties** between GBA backgrounds (see §4).
5. **Division in ARM code** links compiler-rt's Thumb helpers into ARM
   callers. The result is wrong returns, not link errors.
6. **IWRAM creep.** Every feature costs bytes. Check `__iwram_bss_end`
   after each change.
7. **Detecting the HUD by heuristics.** Rules like "BG3 alone means HUD"
   break in rooms that use BG3 alone for fog. Compare several registers.
8. **Test scripts that hang.** A test player who walks into a door can
   bounce between two rooms forever. Pin the player and cancel doors you
   didn't trigger. Give them enough health that one hit in a single frame
   can't kill them.
9. **Backgrounds enabled mid-frame.** A background switched on in DISPCNT
   partway down the screen appears three scanlines late. Keep the layers on
   from line 0 and hide them in the lines above (darken to black with BLDY).
10. **Testing a stale build.** Check the cartridge's timestamp before
    trusting a regression or an improvement. One "regression" here came
    from a build step that never ran.
11. **Measuring speed during transitions.** Per-room timings that include
    the room load are noise. Settle in each room first (`SM_TOUR_SETTLE`).

## 8. Working practice

- Commit each working step on a branch, with a message explaining the
  cause and the check. Don't push unless the human asks.
- Never commit the ROM, cartridges built from it, or extracted assets.
  Screenshots for documentation are the exception.
- Report results with numbers: frames per VBlank, cycles, rooms matching,
  frames traced identical. Say plainly what is approximated and what is
  still slow.
