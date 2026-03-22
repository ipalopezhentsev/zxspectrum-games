# Spect - ZX Spectrum Z80 Project

## Overview
Cross-compiled C project targeting the ZX Spectrum (Z80) via z88dk, producing .TAP files.

## Build

Compile with z88dk's `zcc` targeting the ZX Spectrum:

```sh
zcc +zx -vn -O2 --opt-code-speed -o out/maze.bin maze.c -lndos -lsp1-zx -create-app
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

### spectrum.h display address functions — parameter order gotcha
- `zx_cxy2aaddr(x, y)` and `zx_cxy2saddr(x, y)` take **col first, row second** despite the header declaring them as `(uchar row, uchar col)`. The asm implementation treats the first param as x (col) and second as y (row). The header parameter names are wrong.
- Similarly `zx_cyx2aaddr(a,b)` is defined as `zx_cxy2aaddr_callee(b,a)` — it swaps to (col,row) internally

### Known issues
- UDG characters (codes 144+) do **not** render through z88dk's `printf` — it bypasses the ROM. Don't use `printf("%c", 144)` etc. for custom graphics.
- `draw()` for long lines may render at unexpected positions. Prefer `plot()` loops for reliable horizontal/vertical lines. Short `draw()` calls (a few pixels) work fine.
- `drawb()` reliably renders box outlines at correct positions.
- Text-based separators (`---`) are more reliable than pixel separator lines.
- `plot()`/`unplot()` can corrupt pixel data and attributes unpredictably — prefer direct screen memory writes for game sprites (see above)

## SP1 software sprite library — CRITICAL

### IY register corruption — solved with IM2
SP1 functions corrupt the Z80 IY register. The default ROM IM1 interrupt handler relies on IY — if it fires after SP1 has changed IY, the ROM handler writes to wrong memory, corrupting SP1 state and making sprites disappear.

**Solution: IM2 null interrupt handler.** At startup, install a custom IM2 ISR that does nothing (`ei; reti`). This completely eliminates the IY corruption problem — no DI/EI wrapping needed around SP1 calls.

```c
/* IM2 setup — vector table at $D000, null ISR at $D1D1 */
intrinsic_di();
memset((void *)0xD000, 0xD1, 257);
*((unsigned char *)0xD1D1) = 0xFB;  /* EI */
*((unsigned char *)0xD1D2) = 0xED;  /* RETI prefix */
*((unsigned char *)0xD1D3) = 0x4D;  /* RETI opcode */
/* ld a, $D0 / ld i, a / im 2  (in asm) */
intrinsic_ei();
```

**Consequences of IM2:**
- SP1 calls are safe with interrupts enabled — no DI/EI ceremony needed
- `getk()` and `fgetc_cons()` no longer work (ROM keyboard handler doesn't run). Use `<input.h>` functions instead: `in_WaitForKey()`, `in_WaitForNoKey()`, `in_Inkey()`, `in_KeyPressed()`, `in_JoyKeyboard()`
- z88dk classic library `printf`/`gotoxy` work fine — they don't use the ROM or IY
- `bit_beep()` disables interrupts internally — always call `intrinsic_ei()` after it

### SP1 sprite configuration for single-column NR sprites
- Use `SP1_DRAW_MASK2NR` (non-rotated) for sprites that don't need pixel-level scrolling
- Set `spr->xthresh = 0` — default xthresh=1 suppresses the only column at hrot=0, making sprites invisible
- Set `spr->ythresh = 1` — ythresh=0 causes the transparent bottom overflow row to paint sprite attribute to the cell below
- Height=2 for 8x8 content (1 content row + 1 overflow row)
- Sprite data format: 48 bytes (top overflow + content + bottom overflow), each row = mask byte + graphic byte

### SP1 memory layout
- Update array: $D200
- SP1 internal vars: $D1ED-$D1FF (UPDATELISTH, PIXELBUFFER, ATTRBUFFER)
- IM2 vector table: $D000-$D100 (257 bytes of $D1, ISR at $D1D1-$D1D3)
- Tile array: $F000
- Heap for sprites: `sbrk((void *)0xF200, 0x0B34)` — $F200-$FD33
- Frame buffers: $FD34-$FEFF (player + 4 enemies, 5×92 = 460 bytes, fixed addresses)
- Stack: $D000 (`#pragma output STACKPTR=0xD000`), grows DOWN into BSS

### Memory map constraints — CRITICAL
- **BSS must stay below $D000.** The stack at $D000 grows downward into BSS. The IM2 `memset` at startup writes $D1 to $D000-$D100, trashing any BSS vars in that range. If BSS creeps above $D000, static locals in the overlap zone get corrupted — this may appear to work by luck (if those vars happen to be reassigned before use) but ANY code change shifts BSS addresses and can cause random crashes/resets
- **After any change, check BSS_END in the map file**: build with `-m` flag, then `grep "^__BSS_END_head" out/maze.map` — the address MUST be below $D000. If it's not, move large arrays to upper memory (like frame buffers at $FD34+) to shrink BSS
- **Large arrays go to upper memory, not BSS.** The region $FD34-$FEFF is reserved for frame buffers. The SP1 heap at $F200-$FD33 is mostly unused (~500 bytes used for sprite structs out of 2868). If you need more space, reduce the heap and place arrays in the freed region
- **The $D000-$D1EC gap** between the IM2 table and SP1 vars is dead space (used only by the 3-byte ISR at $D1D1). Don't place anything there — the stack transiently writes into this area during deep call chains

### SP1 sprite data — no `const`
Do NOT use `const` on SP1 sprite graphic arrays. SP1 examples never use const, and it may affect section placement causing SP1 to not find the data.

## Game loop and input — frame-based design

### Frame-synced main loop
- Use `intrinsic_halt()` (from `<intrinsic.h>`) at the top of each game loop iteration to sync to the 50Hz IM2 interrupt
- This gives consistent, predictable timing for all game mechanics
- Interrupts stay enabled throughout — IM2 null handler makes this safe

### Keyboard input — `<input.h>` library (IM2-safe)
- **Use `<input.h>` functions** — they do direct port reads internally, work with IM2
- `in_JoyKeyboard(&udk)` — returns `F000RLDU` bitmask for user-defined keys. Set up `struct in_UDK` with scancodes from `in_LookupKey()` at init. Use `in_LEFT`, `in_RIGHT`, `in_UP`, `in_DOWN`, `in_FIRE` masks to decode
- `in_Inkey()` — returns ASCII of single key press or 0 (instantaneous, no debounce)
- `in_KeyPressed(scancode)` — check specific key via 16-bit scancode from `in_LookupKey()`
- `in_WaitForKey()` / `in_WaitForNoKey()` — blocking waits for key press/release
- `in_LookupKey(char)` — convert ASCII to 16-bit scancode (call once at init, cache result)
- `getk()` and `fgetc_cons()` do NOT work with IM2 (ROM keyboard handler doesn't run)
- For movement: use frame counter for key repeat delay (e.g. `KEY_REPEAT = 4` frames = 80ms)
- ZX Spectrum keyboard ports (for reference, active low — bit=0 means pressed):
  - O/P row: port `$DFFE` — bit0=P, bit1=O
  - Q row: port `$FBFE` — bit0=Q
  - A row: port `$FDFE` — bit0=A
  - 1-5 row: port `$F7FE` — bit0=1, bit1=2, bit2=3, bit3=4, bit4=5
  - All rows: port `$00FE` — AND of all rows, for "any key pressed" check

### Enemy/timer independence
- **Never reset the enemy tick counter when the player moves.** This causes enemies to freeze while the player holds a key
- Enemy movement and player input must run on independent frame counters — both increment every frame regardless of what the other does

### Sound and blocking
- `bit_beep()` blocks the CPU and disables interrupts internally. Always call `intrinsic_ei()` after to re-enable the IM2 interrupt
- Blocking sound during gameplay freezes everything — keep beeps very short (duration 0 or 1)

## sccz80 compiler bugs — CRITICAL

### `unsigned char` locals corrupted across function calls
sccz80 does NOT reliably preserve `unsigned char` (and likely `char`) local variables across function calls. After any function call (including `rng()`, `draw_sprite()`, `set_attr()`, `memset()`, etc.), 8-bit local variables may contain garbage values. This causes:
- Loop indices going out of bounds → array corruption, infinite loops, crashes
- Screen coordinates becoming wrong → graphics drawn at wrong positions
- Variables used after a function call having wrong values

**Rule: ALL non-static local variables that are live across a function call MUST be `int`, not `unsigned char` or `char`.** This applies to:
- Loop counters in loops that contain function calls
- Variables set before a function call and read after it
- Function parameters that are read after a function call in the body

`unsigned char` non-static locals/params are safe when:
- The variable is consumed entirely BEFORE the first function call (e.g. params copied to static locals at the top of a function)
- Used in a loop whose body makes NO function calls
- Used in a function that makes NO function calls (pure computation / leaf function)

**`static` locals bypass this bug entirely** — they use fixed memory addresses like globals, so `static unsigned char` is always safe, even across function calls. Prefer `static` locals in non-reentrant code (all game functions).

### `unsigned char` globals are fine
Global variables as `unsigned char` work correctly — the compiler loads/stores them from fixed memory addresses, not registers. Use `unsigned char` freely for globals to save RAM.

### Shift-as-multiply may silently truncate to 8 bits
`(unsigned int)row << 5` may be evaluated as an 8-bit shift despite the cast, overflowing for values ≥ 8. Use `row * 32` instead — sccz80's multiplication routine handles 16-bit correctly. In general, prefer `*` over `<<` for computed multiplies when the operand could be `unsigned char`.

### Safe optimizations for z88dk/sccz80
- **Global narrowing**: `unsigned char` for global variables (positions, counters, flags) — saves RAM and generates faster load/store
- **`memset()` for bulk clears**: `memset((unsigned char *)16384u, 0, 6144u)` for screen clearing, `memset(array, 0, size)` for array init — faster than manual loops
- **`SCR_ADDR` macro**: precompute screen addresses with `16384u + ((unsigned int)(sr >> 3) << 11) + ((unsigned int)(sr & 7) << 5) + sc` — avoids repeated calculation
- **`unsigned char` for arrays that only store small values**: `stk[]`, `vis[]` etc. can be `unsigned char` instead of `int` to halve memory usage
- **`unsigned char` function params**: safe for leaf functions AND for non-leaf functions where params are consumed before the first function call (e.g. copied to static locals at the top). Only use `int` params when the param is read after a function call in the body

### C-level optimization rules (from z88dk WritingOptimalCode wiki)

#### Build flags
- Use `zcc +zx -vn -O2 --opt-code-speed -o out/maze.bin maze.c -lndos -lsp1-zx -create-app` for maximum runtime speed
- `-O2 --opt-code-speed` enables inlined 16-bit get/set, faster unsigned char multiply, and peephole optimizations
- Selective: `--opt-code-speed=inlineints,ucharmult` for specific speedups without full code size increase
- **Remove debug flags** (`--c-code-in-asm`, `-Cc--gcline`) before final builds — they degrade peephole optimization
- `#pragma define CLIB_EXIT_STACK_SIZE=0` — game never exits, so atexit() stack is wasted BSS. Set to 0

#### Data types
- **Prefer unsigned types** — unsigned comparisons are much faster on Z80 than signed
- **Prefer `unsigned char`** as the default small type — both compilers generate superior code with it
- **Right-size types**: Z80 handles 8-bit and 16-bit efficiently; 32-bit operations are very expensive. Demote to smaller types as soon as possible after arithmetic

#### Variable storage
- **`static` local variables** are a large speed/size win — Z80 has no efficient stack-relative addressing, so locals accessed via SP offsets are slow. Static locals use direct memory addressing instead. Trade-off: functions become non-reentrant (fine for game code)
- **`static` locals also fix the sccz80 `unsigned char` corruption bug** — like globals, they use fixed memory addresses, so `static unsigned char` locals are safe across function calls
- **Declaration order (sccz80)**: for non-static locals, declare most frequently used variables last — compiler optimizes access to variables near top of stack

#### Function design
- **`__z88dk_fastcall`** for single-parameter functions — parameter passed in HL register instead of stack, saving ~30 T-states per call. Syntax: `void func(int x) __z88dk_fastcall`
- **`__z88dk_callee`** for multi-parameter functions where the callee cleans the stack
- **Minimize parameter count** — stack-based parameter passing is expensive on Z80. For unavoidable cases, copy params to static locals before use

#### Loop optimization
- **Use `!=` (equality) over `<` (magnitude) in loop conditions** — Z80 equality check is faster than magnitude comparison. `for (i = 0; i != 10; ++i)` is faster than `for (i = 0; i < 10; ++i)`
- **Pre-increment `++i` over post-increment `i++`** — post-increment generates extra code to save the pre-increment value that the compiler often cannot eliminate
- **Compute loop-invariant expressions once** outside the loop — don't repeat calculations yielding the same value

#### Data sections
- **`const` on read-only data** (sprite bitmaps, lookup tables) — moves data to RODATA/CODE section, may reduce RAM usage and enable compression in the TAP file
- **`char *str = "..."` for string literals** (pointer, not array) — stores string once in ROM; `char str[] = "..."` copies to RAM
- **`const` arrays need matching `const` pointer params** — e.g. `const unsigned char *spr` when passing `const unsigned char spr_data[]`

#### Arithmetic
- **Constant operand ordering (sccz80)**: `a = 3 + 2 - a` generates better code than `a = 3 - a + 2` — group constants together first
- **Use constant shift values** — variable shifts are much slower than constant ones
- **Prefer `*` over `<<` for computed multiplies** (also avoids the sccz80 8-bit truncation bug)

## Inline assembly optimization — patterns and findings

### When to use inline asm
- **Hot inner loops** doing screen writes or address computation — C loop overhead and library multiply calls dominate
- **Leaf functions called hundreds of times** (e.g. `set_attr`, `draw_sprite`, `clear_cell`) — function call overhead (param push + CALL + RET ≈ 110 T-states) can exceed the actual work
- **Division/modulo by non-power-of-2** — Z80 has no hardware divide; library routines cost ~200–500 T-states. Use lookup tables instead

### Inline asm syntax in z88dk/sccz80
- Use `#asm` / `#endasm` blocks inside C function bodies
- C globals are referenced with underscore prefix: C `ds_scr` → asm `_ds_scr`
- IDE will show errors on `#asm` blocks (it doesn't understand z88dk asm) — these are false positives; only `zcc` compilation matters
- **Globals bridge pattern**: pass values between C and asm via `unsigned char` globals to avoid fragile stack-offset access. Declare transient globals (e.g. `ds_row`, `ds_col`, `ds_attr_v`) for this purpose

### Fastest Z80 patterns for ZX Spectrum screen writes

#### 8x8 sprite draw (1 byte wide): `INC H` unrolled loop
- Within a character cell, pixel rows are 256 bytes apart — incrementing H register moves to the next row
- Unrolled: `ld a,(de) / ld (hl),a / inc h / inc de` × 8 = ~192 T-states (vs ~700+ for C loop)
- For sprites with known pixel data (e.g. brick pattern), hardcode values as immediates: `ld a, 247 / ld (hl), a / inc h` — eliminates all sprite memory reads

#### Multiply by 32 (attribute address): 5× `ADD HL,HL`
- `ld l,a / ld h,0 / add hl,hl` ×5 = 55 T-states for `row * 32`
- Since low 5 bits of result are always 0, can add col with `add a,l / ld l,a` (no carry possible when col < 32)
- Much faster than sccz80's library multiply routine (~200 T-states)

#### Why PUSH trick doesn't apply to 8x8 sprites
- PUSH writes 2 contiguous bytes and decrements SP — useful for contiguous memory fills (screen clears, wide sprite blits)
- 8x8 character cell pixel rows are 256 bytes apart (non-contiguous) — PUSH can't stride between them
- PUSH also writes 2 bytes, which would corrupt the adjacent column for 1-byte-wide sprites
- The `INC H` approach is the canonical fast technique for this memory layout

### Algorithmic optimizations for gameplay performance
- **Replace division with lookup tables**: for BFS on a grid, precompute `row[i] = i / COLS` and `col[i] = i % COLS` at startup. Table lookup = ~19 T-states vs division = ~200–500 T-states. With 30–60 BFS iterations × 3 enemies, saves ~30,000–90,000 T-states per enemy tick
- **Incremental array cleanup instead of bulk memset**: if an algorithm (like BFS) tracks which array entries it modified, clean only those entries at the end instead of memset-ing the whole array on every call. Move the initial memset to a one-time setup point (e.g. after maze generation)
