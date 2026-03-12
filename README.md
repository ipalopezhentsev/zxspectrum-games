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

## Toolchain

| | |
|---|---|
| **Compiler** | z88dk (`zcc`) |
| **Target** | ZX Spectrum 48K (`+zx`) |
| **Language** | C (z88dk dialect, C89 subset) |
| **Output** | `.TAP` tape image |


## TODO

- //TODO: after first win second level always starts with 42
- on long key press enemies stop
- add more entropy to levels
- randomize exit
- introduce complexity level for a maze and increase it
- speed up enemies with level?
- introduce asm inserts in inner loops?
- 