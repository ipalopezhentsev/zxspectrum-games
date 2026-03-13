# Spect - ZX Spectrum Z80 Project

## Overview
Cross-compiled C project targeting the ZX Spectrum (Z80) via z88dk, producing .TAP files.

## Build

Compile with z88dk's `zcc` targeting the ZX Spectrum:

```sh
zcc +zx -vn -o out/maze.bin maze.c -lndos -create-app
```

This produces a `.tap` file loadable in a ZX Spectrum emulator.

## Toolchain
- **Compiler**: z88dk (`zcc`)
- **Target**: ZX Spectrum (`+zx`)
- **Output**: `.TAP` tape image
- **Language**: C (z88dk dialect — K&R-style `main()` is fine)

## Toolchain paths
- z88dk install: `C:/Users/ilyap/progs/z88dk/`
- Headers: `C:/Users/ilyap/progs/z88dk/include/`

## Conventions
- Use z88dk-compatible C (subset of C89/C99, no modern C features)
- Use z88dk headers (`<features.h>`, `<conio.h>`, etc.) for platform-specific functionality
- Z80 has very limited RAM (~48KB) — keep code and data small

## Graphics (z88dk `<graphics.h>`)

### Coordinate system
- ZX Spectrum screen: 256x192 pixels, 32x24 character cells (each 8x8 pixels)
- `plot(x, y)` uses 0,0 at **top-left**, x increases right, y increases downward
- Character cell (row, col): pixel x = col*8, pixel y = row*8
- To center a dot in a character cell: `x = col*8 + 3`, `y = row*8 + 4`

### Function signatures
- `plot(x, y)` — plot a single pixel (2 args)
- `draw(x1, y1, x2, y2)` — draw line between two absolute points (4 args, NOT 2)
- `drawb(x, y, w, h)` — draw box outline, (x,y) = top-left corner, w/h = dimensions; min 3x3

### Attribute system — CRITICAL
- Each 8x8 character cell has ONE attribute byte: 1 ink colour, 1 paper colour, 1 bright bit
- **ALL pixels in an 8x8 cell share the same ink/paper** — you CANNOT have two different colours in one cell
- This is called "attribute clash" — the #1 constraint when designing ZX Spectrum graphics

### Avoiding attribute clash
- **Design graphics on the 8x8 grid.** Every moving sprite, wall, corridor, etc. must align to 8x8 character cell boundaries
- **Never let differently-coloured objects share an 8x8 cell.** If a player sprite and a wall occupy the same character cell, one will "clash" — taking on the other's colour
- For tile-based games: use the character cell grid directly. Walls = filled 8x8 cells, corridors = empty 8x8 cells. Sprites move between corridor cells
- For brick/textured walls: write pixel data directly to screen memory (address = `16384 + (row>>3)<<11 + (row&7)<<5 + col`; pixel lines within a cell are 256 bytes apart). Set the cell's attribute separately

### ATTR_P system variable (address 23693)
- `plot()` and `unplot()` set the attribute of the character cell they write to, using the value at ATTR_P
- **Always set ATTR_P before calling plot/unplot** to control what colour those calls produce: `*((unsigned char *)23693) = desired_attr;`
- For sprites that move: set ATTR_P to the sprite colour before draw, set ATTR_P back to background colour after
- Attribute memory: `22528 + row*32 + col` for character cell (row, col)

### Colour workflow for games
1. Draw all static graphics (walls, borders) — set their attributes once
2. Set ATTR_P to the background attribute
3. Draw/erase moving objects using plot/unplot — ATTR_P controls their colour
4. To recolour a cell after an object leaves: poke the attribute byte directly, don't redraw everything
5. **Never bulk-recolour the screen on every frame** — only update the 1-2 cells that changed

### Preferred: direct screen memory writes for sprites
- `plot()`/`unplot()` use the ROM PLOT routine which depends on ATTR_P, MASK_P, and P_FLAG system variables — these can get into bad states and cause pixel/attribute corruption
- **For moving sprites, write pre-computed 8-byte bitmaps directly to screen memory** instead of using `plot()`/`unplot()`. This is faster and avoids ROM side effects
- Pattern: define sprite as `unsigned char spr[8] = {...};`, then write each byte to screen address + i*256 for i=0..7, and set the attribute with a direct poke to attribute memory
- When erasing a sprite, zero all 8 pixel lines of the cell and set the attribute to the background colour — don't use `unplot()` per-pixel
- `zx_cls_attr()` only clears attribute memory, **not pixel data**. To fully clear the screen between games, also zero the 6144 bytes at address 16384

### Sprite overlap rules
- When erasing a sprite from a cell, check if another sprite (exit marker, player, etc.) shares that cell and redraw it after erasing
- Always check for player/enemy collision both after the player moves AND after the enemy moves

### Known issues
- UDG characters (codes 144+) do **not** render through z88dk's `printf` — it bypasses the ROM. Don't use `printf("%c", 144)` etc. for custom graphics.
- `draw()` for long lines may render at unexpected positions. Prefer `plot()` loops for reliable horizontal/vertical lines. Short `draw()` calls (a few pixels) work fine.
- `drawb()` reliably renders box outlines at correct positions.
- Text-based separators (`---`) are more reliable than pixel separator lines.
- `plot()`/`unplot()` can corrupt pixel data and attributes unpredictably — prefer direct screen memory writes for game sprites (see above)

## Game loop and input — frame-based design

### Frame-synced main loop
- Use `intrinsic_halt()` (from `<intrinsic.h>`) at the top of each game loop iteration to sync to the 50Hz interrupt. Each iteration = exactly one frame (20ms)
- This gives consistent, predictable timing for all game mechanics

### Keyboard input
- `getk()` reads from system variable 23560, updated by the ROM interrupt at 50Hz. Good for game input — fast, no debounce overhead
- `getk_inkey()` scans keyboard hardware directly via I/O port $FE — responds instantly to physical key state, with debouncing. Better for "press any key" / RNG seeding (where you need precise timing), worse for game controls (debounce adds latency, rejects simultaneous keys)
- For movement: read `getk()` each frame, use a frame counter for key repeat delay (e.g. `KEY_REPEAT = 4` frames = 80ms between moves while held). Reset delay to 0 on key release for instant response to new presses

### Enemy/timer independence
- **Never reset the enemy tick counter when the player moves.** This causes enemies to freeze while the player holds a key
- Enemy movement and player input must run on independent frame counters — both increment every frame regardless of what the other does

### Sound and blocking
- `bit_beep()` blocks the CPU and disables interrupts. Always call `intrinsic_ei()` after to re-enable interrupts
- Blocking sound during gameplay freezes everything — keep beeps very short (duration 0 or 1)

## sccz80 compiler bugs — CRITICAL

### `unsigned char` locals corrupted across function calls
sccz80 does NOT reliably preserve `unsigned char` (and likely `char`) local variables across function calls. After any function call (including `rng()`, `draw_sprite()`, `set_attr()`, `memset()`, etc.), 8-bit local variables may contain garbage values. This causes:
- Loop indices going out of bounds → array corruption, infinite loops, crashes
- Screen coordinates becoming wrong → graphics drawn at wrong positions
- Variables used after a function call having wrong values

**Rule: ALL local variables that are live across a function call MUST be `int`, not `unsigned char` or `char`.** This applies to:
- Loop counters in loops that contain function calls
- Variables set before a function call and read after it
- Function parameters of functions that call other functions (use `int` params)

`unsigned char` locals are ONLY safe when:
- Used in a function that makes NO function calls (pure computation)
- Set and consumed entirely BETWEEN two function calls (not across one)
- Used as the loop variable in `for (i = 0; i < 8; i++) { *base = x; base += 256; }` style loops with no function calls in the body

### `unsigned char` globals are fine
Global variables as `unsigned char` work correctly — the compiler loads/stores them from fixed memory addresses, not registers. Use `unsigned char` freely for globals to save RAM.

### Shift-as-multiply may silently truncate to 8 bits
`(unsigned int)row << 5` may be evaluated as an 8-bit shift despite the cast, overflowing for values ≥ 8. Use `row * 32` instead — sccz80's multiplication routine handles 16-bit correctly. In general, prefer `*` over `<<` for computed multiplies when the operand could be `unsigned char`.

### Safe optimizations for z88dk/sccz80
- **Global narrowing**: `unsigned char` for global variables (positions, counters, flags) — saves RAM and generates faster load/store
- **`memset()` for bulk clears**: `memset((unsigned char *)16384u, 0, 6144u)` for screen clearing, `memset(array, 0, size)` for array init — faster than manual loops
- **`SCR_ADDR` macro**: precompute screen addresses with `16384u + ((unsigned int)(sr >> 3) << 11) + ((unsigned int)(sr & 7) << 5) + sc` — avoids repeated calculation
- **`unsigned char` for arrays that only store small values**: `stk[]`, `vis[]` etc. can be `unsigned char` instead of `int` to halve memory usage
- **`unsigned char` function params for leaf functions**: functions like `set_attr(unsigned char row, unsigned char col, unsigned char attr)` that don't call other functions can safely use 8-bit params
