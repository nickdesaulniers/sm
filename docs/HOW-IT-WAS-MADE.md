# How Super Metroid came to the Game Boy Advance

Super Metroid was never meant to fit in your pocket. It was written in 1994
for a console with 128 KB of RAM and a graphics chip built for layered,
scrolling worlds. Its music runs on a separate sound computer with its own
CPU and 64 KB of memory. The Game Boy Advance has 32 KB of fast RAM, a
smaller screen, and a sound system that amounts to two 8-bit DACs and a
timer.

It runs anyway, at 60 frames per second. This is how.

## The bet: no emulator

The usual way to play a SNES game on a GBA is to emulate the SNES. That
works for simple games, and Super Metroid is not one. The SNES's 65816 CPU,
its graphics chip, its sound CPU and its sound chip all run at once, and
emulating each would take several GBA frames to produce one SNES frame.

The [snesrev/sm](https://github.com/snesrev/sm) project made another way
possible. Its authors decompiled the whole game into C, function by function.
Their PC build runs the C next to the original code in an emulator and
compares the two every frame. That makes the C a faithful copy of the game
that can be compiled for any CPU, including the GBA's ARM7TDMI.

So the bet was this: compile the game's logic for the GBA and leave it alone.
Write new code only where the game meets the hardware. That means the
picture, the sound, the controls, and the memory the game expects.

The toolchain was part of the bet too. It uses clang and lld, with no GCC,
devkitARM or vendor SDK. The port has its own startup code, linker script
and small C library, and compiles LLVM's compiler-rt from source for the
divisions the ARM7TDMI can't do. The build from scratch is
`./build-gba.sh`.

## First light

Before the game could boot at all, it had to get past alignment.

The decompiled C is very faithful to a CPU with no alignment rules. The
65816 reads a 16-bit value from any address. The ARM7TDMI reads a 32-bit
value from an odd address by quietly rotating it, so it returns garbage
without crashing. The game casts byte pointers to wider types everywhere.
The fix was to define the game's `uint16` and `uint32` types as 1-byte
aligned on the GBA. The compiler then emits safe byte loads wherever the
game might misbehave, and the C stays the same as the PC build's.

Then the game reached its title screen, the file select, and Ceres station.
The first glimpse of Samus on a GBA screen looked rough: no sound, no
transparency, and a frame rate in the twenties. But it was the real game,
making its own decisions, on a handheld.

## Speaking SNES to a GBA

The game talks to the SNES's graphics chip the way all SNES games do. It
writes to its registers, uploads tiles over DMA, and reprograms the chip in
the middle of the screen with HDMA, a mechanism that changes registers line
by line as the screen is drawn. The GBA's graphics hardware is a cousin of
the SNES's, with similar tiles, maps and sprites, but the details differ.

The renderer keeps a model of the SNES's video memory and registers. Once
per frame it translates that model into the GBA's terms:

- SNES Mode 1 has two 16-color layers and one 4-color layer, each tile
  with a priority bit. On the GBA it becomes four tiled backgrounds. One
  SNES layer is split in two by tile priority, so that sprites can sit
  between its halves as they do on the SNES.
- Mode 7, the rotating, scaling layer of Ceres station's tilting rooms and
  of the intro, becomes the GBA's affine background. The HUD stays on a tiled
  layer above it, in the same frame.
- The SNES's 4-color tiles are widened to the GBA's 16-color format. Maps
  are converted 16 entries at a time, only where the game changed them.
- HDMA becomes a stream of GBA registers that the GBA's own HBlank DMA
  copies in before each line. The registers that change only a few times
  a frame, such as at the HUD's bottom edge, are set by a line-counter
  interrupt.

The GBA shows 240x160 pixels, and the SNES shows 256x224. During gameplay
the picture is a 1:1 crop that follows Samus, with the HUD pinned at the top.
On the title, menus and pause screen, which have text that must stay
readable, it shows the whole screen, squashed. It drops two of every eight
lines, always the same two rows of each tile, so letters keep their tops and
bottoms.

## The race to 60

The game ran at 20-something frames per second, and the first guess was
that the game's own logic was too slow. A profiler proved that guess wrong.

That profiler was a small headless runner, `smrun`, built on mGBA's
library. It executes the cartridge one instruction at a time and charges
every cycle to a function. It showed that the game's logic was never the
problem. The time went to the layer between the game and the hardware: DMA
copied a byte at a time through emulated ports, every HDMA write was
recorded even when it changed nothing, and the line registers were rebuilt
from scratch.

So the fixes went into that layer:

- DMA to video memory copies in bulk.
- HDMA records registers only when their value changes, and skips lines
  where it only counts down.
- Hot code runs from the GBA's 32 KB of fast internal RAM, as 32-bit ARM
  code instead of the 16-bit Thumb code used elsewhere.
- Maps convert only the 16-entry chunks that changed.

Fast RAM became the scarcest resource in the project. Every feature since
has had to earn its bytes. Code that runs once a frame moves out to the
cartridge ROM, and the linker refuses to build when the fast RAM would
overflow into the stack. After all that, the landing site on Zebes runs a
full frame of game, picture and sound in under 280,000 cycles, the GBA's
whole budget for a frame.

## Teaching the GBA to sing

Super Metroid's music is famous, and it comes from a second computer inside
the SNES: the SPC700, a sound CPU with 64 KB of RAM and an 8-voice sampler
chip. The decompilation had translated the game's sound driver to C as well,
so it could run on the GBA's main CPU like everything else.

The 64 KB was the problem, since the GBA has no 64 KB to spare. But
everything the driver reads from that RAM was uploaded from the ROM to begin
with, so on the GBA it reads the ROM directly, through a map of where each
upload came from.

The samples are compressed in the SNES's BRR format. The cartridge build
decodes all 175 of them to 8-bit PCM ahead of time, so the GBA never has to.
In place of the SNES's sampler chip, a mixer written in ARM assembly mixes
eight voices, with the SNES's envelopes, noise and echo, into the GBA's two
sound channels. It runs from a timer interrupt after each screen refresh, so
a slow game frame doesn't stall the music.

Comparing the result was its own project. A PC tool plays the same requests
through the original driver and an emulated sound chip, and a script compares
the two recordings' loudness and frequencies.

## Light, fog and the X-ray scope

The SNES can add or subtract a whole second picture, the "sub screen", to or
from the main one, pixel by pixel. Super Metroid uses this constantly, for
rain on Crateria, fog, water, the glow of lava, and darkness. The GBA can
only blend a layer with the one directly beneath it.

Because addition doesn't care about order, the sub screen's layers can be
drawn on top and blended down for the same sum. Subtraction is harder, since
the GBA can't subtract at all. In rooms that darken with subtraction, the
GBA darkens instead. Each frame it samples a few dozen pixels on screen and
works out how much light the SNES would have taken away there.

The SNES's two windows, which shape the X-ray scope's cone and power bombs'
blasts, map onto the GBA's two windows. Their edges move every line in the
same DMA stream as the scrolling.

## The hills that wouldn't let Samus climb

With the picture and sound working, Samus landed on Zebes, walked right, and
stopped at the foot of the first hill. She couldn't climb a slope.

Playing and guessing wasn't going to find it, so the port gained a flight
recorder. A trace build logs the joypad and the game's state every frame. A
host tool, `hostrun`, replays that joypad on the PC build, which is itself
checked frame by frame against the original game. The two logs went their
separate ways at the exact frame Samus loaded the room.

The cause was a single `memcpy`. As the game loads a room, it copies the
slope data (BTS) out of the level data to a spot just past it, and in big
rooms the source and destination overlap. That is undefined behavior in C.
The PC's C library happens to copy such buffers safely. The GBA's fast
`memcpy`, copying front to back, overwrote the data before reading it. The
fix went into the game source, `memmove` for the two overlapping copies,
and `memcpy` went back to being fast. Today the GBA and the PC agree on
every one of thousands of traced frames.

## Ridley's door

Near the end of the Ceres escape, the player has to find the door out of
Ridley's room. On the GBA it was nearly invisible: Ridley, the floor, and the
door's blue cap floating in darkness.

That room draws its walls, blocks and door frame on the SNES's second
background layer, flagged to appear in front, behind a first layer that is
one solid dark tile across the screen. The renderer had assumed what is true
of nearly every room, that the first layer is the one worth splitting by
priority. Now it counts each layer's priority tiles as it converts them,
and in a frame where the second layer shows more, it splits that layer
instead.

## Every room, one by one

Bugs like the door raised a question: how many other rooms are broken
somewhere nobody has walked yet?

To answer it, the port gained a tour. A test build reads the door lists out
of the ROM, finds all 254 rooms on Zebes by walking from door to door, and
sends Samus through a door into each in turn. She can't die on the way, and
she's pinned where she enters. Otherwise she falls back down through the
door she came up through and bounces between two rooms forever. In each
room, the tour holds a frame still while `smrun` takes a screenshot and
dumps the GBA's memory.

Then a second tool, `snesref`, takes that memory dump, finds the SNES's
video state inside it, and draws the frame with the PC build's SNES graphics
emulator. That shows what a real SNES would have displayed from the same
state, pixel for pixel. A script crops it as the GBA does and ranks all 254
rooms by how much differs.

The first ranking was humbling. Only half the rooms matched. A third of the
game uses a setup where the room sits entirely on the sub screen and fog is
the only thing on the main screen. On the GBA, the fog layer won a priority
tie and covered the room, so some rooms came out pitch black.

The tour ran again after every fix:

- Fog rooms: the room is drawn normally with the fog blended on top, and
  Samus is visible again.
- Rooms where fog falls on the background but not the foreground: the fog
  layer slides between them.
- Kraid's lair, the only room with a 64x64 tile map, had shown its top half.
  It now shows the half on screen.
- Maridia's and Tourian's darkness gets the sampled subtraction.

Then came the subtlest bug of all. In Norfair's heat shimmer, every other
line of the background was wrong. The SNES shows background row
`scroll + 1` on its first visible line, and the GBA shows row `scroll`. With
a still background that is an invisible one-pixel shift, and the comparison
script had been quietly compensating for it. With a background whose scroll
changes every line, each line showed its neighbor's.

After those fixes, 234 of the 254 rooms matched the SNES to within 5%. The
rest were Maridia and Tourian, where the room grows dark by subtraction,
and three stubborn lines just under the HUD.

The three lines turned out to be the hardware. When `DISPCNT` switches a
background on partway down the screen, the GBA waits three scanlines before
drawing it. The fix keeps the gameplay layers switched on from the top of the
frame and blacks them out behind the HUD instead.

The subtraction took more cunning. The GBA can only add, but subtraction
can be split in two. The palette subtracts one colour from the whole room,
and the shadow layer, drawn on top, adds back the difference between that
colour and its own. Every frame, the renderer samples forty points on
screen. It works out which of three tricks comes closest there, and which
colour to subtract, channel by channel. The first sampling grid stepped down
the screen in multiples of eight lines, so every sample landed on the same
row of its tile, and the samples never saw the thin lines of Maridia's
shadow pattern. Staggering the samples fixed that. The HUD shares the
shadow layer's palettes, so the add-back colours are switched in at the
HUD's bottom edge and switched back at VBlank.

Now 252 of the 254 rooms match the SNES to within 5%, most of them pixel for
pixel.

## What's left

The port isn't finished. The same tour, run slowly enough to time each
room once it settles, showed what the screenshots couldn't:

- Norfair's heat shimmer and Maridia's water rewrite a background's
  scrolling on every line. Emulating the SNES's HDMA line by line for them
  still costs too much, and most of those rooms run at 25-40 fps.
- The X-ray scope, big power bomb explosions and a few rooms on Ceres
  station slow the game down too.
- The GBA's 8-bit sound has a hiss the SNES never had.

But all of Zebes is there, and most of it at full speed, in your hands.
