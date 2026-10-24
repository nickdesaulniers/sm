# Super Metroid for the Game Boy Advance

<p align="center">
  <img src="docs/images/title.png" width="240" alt="The title screen">
  <img src="docs/images/ceres-ridley.png" width="240" alt="Ridley takes the baby Metroid on Ceres station">
  <img src="docs/images/ceres-explodes.png" width="240" alt="Ceres station explodes as Samus escapes">
  <br>
  <img src="docs/images/landing.png" width="240" alt="Samus leaves her ship in the rain">
  <img src="docs/images/bomb-torizo.png" width="240" alt="Bomb Torizo throws bombs at Samus">
  <img src="docs/images/kraid.png" width="240" alt="Kraid">
  <br>
  <img src="docs/images/crocomire.png" width="240" alt="Crocomire swings at Samus">
  <img src="docs/images/golden-torizo.png" width="240" alt="Samus shoots the Golden Torizo, who throws his orbs and crescents">
  <img src="docs/images/ridley.png" width="240" alt="Ridley">
  <br>
  <img src="docs/images/phantoon.png" width="240" alt="Phantoon">
  <img src="docs/images/botwoon.png" width="240" alt="Botwoon">
  <img src="docs/images/draygon.png" width="240" alt="Draygon swoops and grabs Samus">
  <br>
  <img src="docs/images/golden-statue.png" width="240" alt="The golden statue at the entrance to Tourian">
  <img src="docs/images/mother-brain.png" width="240" alt="Mother Brain">
</p>

Super Metroid, running natively on the Game Boy Advance. The game itself is
the [snesrev/sm](https://github.com/snesrev/sm) decompilation, the SNES game's
code rewritten as C. This port compiles that C for the GBA's ARM7TDMI and
replaces the SNES's graphics, sound and input with the GBA's own. Nothing is
emulated: the GBA's 16.8 MHz CPU has no time to spare for an emulator.
Almost every room runs at or near 60 frames per second.

The port includes none of the game's data. You build the cartridge from your
own copy of the SNES ROM.

## Building the cartridge

You need clang and lld (it's built with LLVM 22), CMake, Python 3 and git.
There's no devkitARM or GCC. On Debian or Ubuntu:

```
sudo apt install clang lld llvm cmake python3 git
```

Then:

1. Clone this repository and check out the `gba-port` branch:

   ```
   git clone -b gba-port https://github.com/<you>/sm.git
   cd sm
   ```

2. Put your Super Metroid ROM in that directory and name it `sm.smc`. It must
   be the US release without a copier header (sha1
   `da957f0d63d14cb441d215462904c4fa8519c613`).

3. Build:

   ```
   ./build-gba.sh
   ```

   The first build also fetches LLVM's compiler-rt sources, a few MB, for the
   division routines the ARM7TDMI lacks. It then compiles the game and
   prepares the ROM's data for the GBA: the graphics are converted as the game
   loads them, and the sound samples are decoded ahead of time.

4. The cartridge is `sm.gba`, in the same directory. Run it in
   [mGBA](https://mgba.io) or on a flash cartridge. Saves go to the
   cartridge's SRAM (in an emulator, a `.sav` file next to `sm.gba`).

Instead of building it all, you can download `sm-engine.gba` and
`make_cartridge.py` from a release and add your ROM's data to the engine:

```
python3 make_cartridge.py sm-engine.gba sm.smc sm.gba
```

## Reading more

- [Progress report](docs/HOW-IT-WAS-MADE.md): from a black screen to every
  room of the game, the notable changes and the bugs found on the way.
- [Issues to report upstream](docs/UPSTREAM-ISSUES.md): bugs found in the
  decompilation while porting it, with their fixes.
- [Porting a SNES game to the GBA](docs/PORTING-GUIDE.md): a guide for agents
  (and people) attempting a similar port. It covers the approach, the
  hardware mapping, the tools for checking the result, and the pitfalls.

## Controls

The SNES pad has X and Y buttons and the GBA doesn't, so gameplay uses the
layout of the GBA Metroids, *Zero Mission* and *Fusion*:

| Action                 | SNES (default)        | GBA                                  |
|------------------------|-----------------------|--------------------------------------|
| Jump                   | A                     | A                                    |
| Shoot                  | X                     | B                                    |
| Run                    | hold B                | always on                            |
| Aim diagonally up      | R                     | hold L                               |
| Aim diagonally down    | L                     | hold L + Down                        |
| Pick an item           | Select cycles through them | hold R, Select picks what R readies instead of missiles |
| Missiles (or the item) | pick, then X          | hold R, then B                       |
| X-ray scope            | pick, then hold B     | pick, then hold R                    |
| Power bombs            | pick, then X in morph ball | hold R in morph ball, then B    |
| Put the item away      | Y                     | let go of R                          |
| Pause, map             | Start                 | Start                                |

So, as on the GBA Metroids, missiles are a shoulder button away: hold R and
press B. With R held, Select cycles R's item through missiles, super
missiles, the grapple beam and the X-ray scope, whichever Samus has found,
shown on the HUD; the pick is kept for the next time R is held. Select does
nothing without R. Letting go of R doesn't drop Samus from the grapple beam.
Because Samus always runs, the speed booster charges whenever she runs long
enough.

On the title screen, the menus and the pause screen, each button is the SNES
button of the same name (B goes back).

The options menu's Controller Setting Mode shows these controls, with the
GBA's buttons. They can't be changed there: only END and RESET do anything.

<p align="center">
  <img src="docs/images/controller-settings.png" width="240" alt="Controller Setting Mode, with the GBA's buttons">
</p>

## How it differs from the SNES

- **The screen.** The GBA's screen is 240x160, and the SNES's is 256x224.
  During gameplay the GBA shows a 1:1 crop that follows Samus vertically, with
  the HUD at the top. The title screen, menus and pause screen show the whole
  SNES screen, squashed to fit.
- **Sound.** The SNES's sound driver runs on the GBA's CPU, with the SNES's
  echo, and a mixer in place of the SNES's sound chip. It sounds right but
  has the GBA's 8-bit hiss.
- **Effects.** The SNES's color math and windows are mapped onto the GBA's
  blending and windows: rain, fog, water, heat shimmer, shadows, the X-ray
  scope's cone and power bombs. The GBA can't subtract colors, so darkened
  rooms are approximated, and power bombs' explosions brighten the screen
  rather than turning it yellow.
- **Speed.** Timed in every room, none runs below 59 fps. Ridley's fight
  on Ceres station averages 58 fps but dips to about 50, doors take longer
  than on the SNES, and the X-ray scope and big power bomb explosions slow
  the game down.

## Developer tools

The cartridge can start Samus anywhere, with any loadout, to test a room, a
fight or a weapon without playing up to it. It's the developers' own debug
mode: the game keeps a switch (`enable_debug`) that makes the load-game map
list every load station, the developers' spawn points among them. The
cartridge's buttons turn it on, and add a loadout and the bosses' doors.

**Turning it on.** On the title screen or the file select, hold L and R and
press Start. A sound tells you, and it lasts until the console is reset
(Start + Select + L + R in the menus resets it, too). Nothing in the demos or
in play can turn it on.

**Spawning.** Pick a file and start it, as usual. A saved file goes to the map
of Planet Zebes as ever; with the tools on, a new file skips its intro and goes
there too (to play the intro instead, hold L from Start until the screen fades).

| Screen      | Buttons                                                                 |
|-------------|-------------------------------------------------------------------------|
| Planet Zebes | Up and Left, Down and Right or Select: another area. A or Start: that area |
| Area map    | Select: the next station; after an area's last, its bosses; then the first again. D-pad: scroll the map. A or Start: spawn. B: back to the areas |
|             | L: the next loadout. R: the next beams                                  |

Every station is there, visited or not: the area's save stations and
elevators, then the **debug stations**, numbered from 16, that no file ever
had. Under the map, a line names the one picked (or its room's number, in
hex, for the unnamed), then the loadout and the beams.

<p align="center">
  <img src="docs/images/dev-map.png" width="240" alt="The map with a debug station, the loadout and the beams under it">
  <img src="docs/images/dev-crocomire.png" width="240" alt="Crocomire, spawned into with all items">
  <img src="docs/images/dev-draygon.png" width="240" alt="Draygon's room, spawned into with all items">
</p>

**The debug stations.** The game's own, in the order the map steps through
them (area, number: the room, by its pointer; it has each of the bosses' rooms
next to one, but only the Golden Torizo's inside it):

| Station | Room | Where |
|---------|------|-------|
| Crateria 16 | 91F8 | the landing site, at Samus's ship |
| Crateria 17 | 94FD | |
| Brinstar 16 | 9AD9 | |
| Brinstar 17 | A56B | next to Kraid: a door of Kraid's room leads here |
| Brinstar 18 | 9D19 | |
| Norfair 16 | A7DE | |
| Norfair 17 | A923 | next to Crocomire: a door leads into his room |
| Norfair 18 | B37A | next to Ridley |
| Norfair 19 | AA82 | next to Crocomire, by his room's other door |
| Norfair 20 | B236 | |
| Norfair 21 | B283 | the Golden Torizo's room, but the developers' spot is outside the room: the map spawns at 22's instead |
| Norfair 22 | B283 | the Golden Torizo's room, at its left door |
| Wrecked Ship 16 | CA08 | |
| Wrecked Ship 17 | CC6F | next to Phantoon |
| Maridia 16 | D1DD | |
| Maridia 17 | D78F | next to Draygon |
| Maridia 18 | D617 | next to Botwoon |
| Maridia 19 | D48E | |
| Tourian 16, 17 | DDF3 | next to Mother Brain |

**The bosses.** After an area's last station, the map goes on to its bosses'
doors (the game's own doors, from the side Samus comes in by): Bomb Torizo
(Crateria), Spore Spawn and Kraid (Brinstar), Crocomire and Ridley (Norfair),
Phantoon (Wrecked Ship), Botwoon and Draygon (Maridia) and Mother Brain
(Tourian). The spawn loads the area's last station, and when Samus has her
controls, goes through the door as if she had walked into it. (Crocomire's is
the hatch in the ceiling of his room, as in the game. As there, beams don't
hurt him: missiles in his mouth push him back.)

**Loadouts.** L picks what Samus has; every one but the first makes every boss
alive again, so that a file that has beaten them can fight them:

| Name       | Energy | Reserve | Missiles | Super | Power bombs | Items                                | Beams                |
|------------|-------:|--------:|---------:|------:|------------:|--------------------------------------|----------------------|
| AS SAVED   | the file's | | | | | | |
| ALL ITEMS  | 1499   | 400     | 230      | 50    | 50          | all                                  | charge, ice, wave, plasma (spazer collected) |
| NEW GAME   | 99     | 0       | 0        | 0     | 0           | none                                 | power beam           |
| EARLY      | 299    | 0       | 20       | 0     | 0           | morph ball, bombs                    | charge               |
| MID        | 799    | 100     | 100      | 20    | 10          | varia, spring ball, morph ball, hi-jump, bombs, speed booster, x-ray | charge, ice, wave |
| LATE       | 1199   | 300     | 150      | 30    | 20          | all                                  | charge, ice, wave, spazer |
| DEV TORIZO | 700    | 300     | 100      | 20    | 20          | all but the screw attack             | charge, ice, wave, plasma (spazer collected) |

R picks the beams instead, from the ones the game allows (never spazer and
plasma together, as when it gives you a beam or in its equipment screen:
plasma takes the spazer off), with and without the charge beam: from the power
beam alone to charge, ice, wave and plasma. DEV TORIZO is what the developers'
cheat in the Golden Torizo's room gives (A, B, X and Y together, as it loads).
The cheat has all four beams on, spazer and plasma too, and with both no shots
showed in testing, so this takes the spazer off.

A spawn with a loadout can't save: it would write the loadout over your file,
so saving does nothing until you spawn again with AS SAVED, or reset. Dying
goes back to the map, to spawn again.

**In the game.** Hold L, R and Select, and press: A to turn invincibility on
or off, B to refill energy and ammunition, Left to freeze the enemies or let
them go, Up to die (and so to go back to the map).

These are the game's own debug features that the buttons reach (there's more
in the code: a Draygon that a second controller moves, a scroll position
saved and loaded, a demo recorder's pause). See the comments in
[gba/dev_tools.c](gba/dev_tools.c) and the
[porting guide](docs/PORTING-GUIDE.md).

## The PC version

This branch keeps the decompilation's PC build too, which runs the C code and
the original game side by side and compares them frame by frame. See
[BUILDING.md](BUILDING.md) for its build, and the
[snesrev Discord](https://discord.gg/AJJbJAzNNJ) for the project.
