# sm

Our discord server is: https://discord.gg/AJJbJAzNNJ

Early version. It has bugs and the code is messy.

For building instructions, see: https://github.com/snesrev/sm/blob/main/BUILDING.md

Put sm.smc (sha1 hash da957f0d63d14cb441d215462904c4fa8519c613) in the root folder. When running, it will run both versions and compare frame by frame. If it detects a mismatch, it saves a snapshot in saves/ and displays a counter on screen counting down from 300.


## Game Boy Advance

The `gba-port` branch also builds the game for the Game Boy Advance, with
clang and lld (no GCC, no devkitARM), with `sm.smc` built into the cartridge:

```
cmake -B build-gba -DCMAKE_TOOLCHAIN_FILE=cmake/gba-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-gba
```

That makes `build-gba/sm-engine.gba`, the engine, and from it and your ROM,
`build-gba/sm.gba`, the cartridge. Run `sm.gba` in mGBA. The engine has none
of the game's data, so it can be shared: `tools/make_cartridge.py` makes a
cartridge from it and a ROM.

```
python3 tools/make_cartridge.py sm-engine.gba sm.smc sm.gba
```

GitHub builds the engine for each push (`.github/workflows/gba.yml`), and for a
tag named `v*`, publishes a release of it with `make_cartridge.py`.

The game runs natively, not in an emulator: `gba/` replaces the PC's
platform layer, at 60 frames per second in most places. In gameplay, the
SNES's 256x224 picture is shown 1:1, cropped to the GBA's 240x160 around
Samus; elsewhere (the title, menus, the pause screen), all of it, squashed.
The sound plays, with the SNES's echo, if with the GBA's 8-bit hiss. The
SNES's color math and windows are there as far as the GBA can do them:
rain, fog and water over the rooms, tints, the X-ray scope's cone and
power bombs' explosions (brightening, not yellow).

Controls in gameplay work as in Metroid: Zero Mission and Fusion (the GBA
has no X and Y buttons):

| GBA          | Action                                                    |
|--------------|-----------------------------------------------------------|
| A            | Jump                                                      |
| B            | Shoot                                                     |
| R (hold)     | Ready missiles, or what Select picked; power bombs in morph ball |
| L (hold)     | Aim diagonally up (with Down held: down)                  |
| Select       | Pick what R readies: missiles, super missiles, grapple beam, X-ray scope |
| Start        | Pause                                                     |
| -            | Run: always on                                            |

In menus and on the pause screen, the buttons are the SNES's buttons of the
same names (B to go back).
