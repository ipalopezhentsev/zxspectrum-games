# Spect

ZX Spectrum programs written in C, cross-compiled with [z88dk](https://z88dk.org/) to `.TAP` tape images.

## Programs

### Maze Game (`maze.c`)

A playable maze game with brick-textured walls, colour attributes, and sound effects. A new random maze is generated each time from keypress timing. Navigate from the top-left corner to the bottom-right exit.

**Controls:** O/P = left/right, Q/A = up/down

You can either download out/maze.tap into an emulator or just play online:
https://ipalopezhentsev.github.io/zxspectrum-games/

## Building

Requires [z88dk](https://z88dk.org/) installed.

```sh
# Maze game
zcc +zx -vn -o out/maze.bin maze.c -lndos -create-app
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

# bugs:
- wobbly movement in demo

# features:
- don't place enemies in positions that don't allow me to pass them (not corner me)
- ay effects/music
+ use more standard stuff from arch/zx/spectrum.h: zx_cls_attr,  // DISPLAY PIXEL ADDRESS MANIPULATORS
+// DISPLAY ATTRIBUTE ADDRESS MANIPULATORS
sound/aywyz.h - WYZ tracker for AY819x sound chips
sound/bit.h - audio generation functions using a 1-bit device
+stdlib.h - general utilities (sorting, number↔ascii, !!!!random numbers, …)
