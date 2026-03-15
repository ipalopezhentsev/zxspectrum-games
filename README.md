# Spect

ZX Spectrum programs written in C, cross-compiled with [z88dk](https://z88dk.org/) to `.TAP` tape images.

## Programs

### Maze Game (`maze.c`)

A playable maze game with brick-textured walls, colour attributes, and sound effects. A new random maze is generated each time from keypress timing. Navigate from the top-left corner to the bottom-right exit.

**Controls:** O/P = left/right, Q/A = up/down

### Calendar (`calendar.c`)

Displays a monthly calendar for any given year and month, with decorative pixel-art borders (diamond patterns, corner crosses, double-line frame).

## Building

Requires [z88dk](https://z88dk.org/) installed.

```sh
# Maze game
zcc +zx -vn -o out/maze.bin maze.c -lndos -create-app

# Calendar
zcc +zx -vn -o out/calendar.bin calendar.c -lndos -create-app
```

This produces `.tap` files in the `out/` directory, loadable in any ZX Spectrum emulator (e.g. Fuse, ZXSpin).

## Setting up VS Code

I recommend using it for comfortable development cycle.

Preparation:
- install VS Code
- Open project folder
- It will propose to install C/C++ extension bundle, do it to get Intellisense working
- Ctrl-Shift-P -> `C/C++ Configurations` -> Add configuration `ZX Spectrum`.
- Scroll to `Include path`. Include there full path to the include folder for your z88dk installation
- Then go to a C file and click Win32 in the top bottom corner and select `ZX Spectrum` - now you can go to stdlib of z88dk from your source file

Build/run cycle:
- Ctrl-Shift-P -> `Tasks: Run Task`
- Select `Build and Run` - will execute zcc and then start Spectaculator
- If you have another emulator, just edit the task command in `.vscode/tasks.json`

## Toolchain

| | |
|---|---|
| **Compiler** | z88dk (`zcc`) |
| **Target** | ZX Spectrum 48K (`+zx`) |
| **Language** | C (z88dk dialect, C89 subset) |
| **Output** | `.TAP` tape image |


## TODO

- speed up enemies with level?
+ introduce asm inserts in inner loops?
- title page
- joysticks?
- show zoomed location of player on start
+ remove D button and its stats
- web emul
- don't open exit until min coins collected?
- introduce delay for studying level - can start with zoom on player then places enemies one by one with stumping sound
- ay effects/music
+ prefer unsigned to signed and char to int
+ follow all advice from https://github.com/z88dk/z88dk/wiki/WritingOptimalCode
- pixel scrolling (with masks?! - if we add background to maze)
- some background in corridors?
- player figure -> man with several movement phases
+ use more standard stuff from arch/zx/spectrum.h: zx_cls_attr,  // DISPLAY PIXEL ADDRESS MANIPULATORS
+// DISPLAY ATTRIBUTE ADDRESS MANIPULATORS
+arch/zx/sp1.h - SP1 software sprite engine for zx https://github.com/z88dk/z88dk/wiki/LIBRARY-SP1-Software-Sprites-(sp1.h)
input.h - direct interface with user-input devices (keyboard, joystick, mouse)
sound/aywyz.h - WYZ tracker for AY819x sound chips
sound/bit.h - audio generation functions using a 1-bit device
+stdlib.h - general utilities (sorting, number↔ascii, !!!!random numbers, …)
