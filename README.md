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

Run `build-gba/sm.gba` in mGBA. The game runs natively, not in an emulator;
`gba/` replaces the PC's platform layer. The SNES's 256x224 picture is shown
1:1, cropped to the GBA's 240x160 around Samus. No sound yet, and it runs
slower than full speed.

Controls in gameplay (the GBA has no X and Y buttons):

| GBA    | Action                                          | SNES       |
|--------|-------------------------------------------------|------------|
| A      | Jump                                            | A          |
| B      | Shoot                                           | X          |
| R      | Aim diagonally up (with Down held: down)        | R (L)      |
| L      | Switch item                                     | Select     |
| Select | Cancel item                                     | Y          |
| Start  | Pause                                           | Start      |
| -      | Run: always on                                  | B (held)   |

In menus and on the pause screen, the buttons are the SNES's buttons of the
same names (B to go back).
