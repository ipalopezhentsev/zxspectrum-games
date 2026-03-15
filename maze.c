#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <features.h>
#include <sound.h>
#include <intrinsic.h>
#include <arch/zx/spectrum.h>
#include <arch/zx/sprites/sp1.h>
#include <malloc.h>

#pragma output STACKPTR=0xD000

#define TEXT_SCR_WIDTH 64


#define COLS 14
#define ROWS 9
#define NUM_HISCORES 5

/* Expanded grid dimensions (each maze cell becomes 2 grid cells + walls) */
#define ECOLS (2*COLS+1)
#define EROWS (2*ROWS+1)

/* Maze grid origin in character cells */
#define MAZE_R0 2
#define MAZE_C0 1

/* Screen char row/col from expanded grid position */
#define SROW(gy) (MAZE_R0 + (gy))
#define SCOL(gx) (MAZE_C0 + (gx))


/* bit 0 = right wall, bit 1 = bottom wall */
unsigned char walls[ROWS][COLS];
/* Precomputed wall map: 1=wall, 0=passable. Indexed as [gy*ECOLS+gx]. */
unsigned char wallmap[EROWS * ECOLS];
unsigned char px, py;    /* player position in expanded grid */
unsigned char enx[4], eny[4];    /* enemy positions in expanded grid */
unsigned char last_edir_arr[4];  /* last direction each enemy moved */
unsigned char enemy_next;        /* round-robin: which enemy moves next */
unsigned char enemy_accum;       /* Bresenham accumulator for spreading */
unsigned int rseed;
unsigned char port_key;  /* keyboard scan result for asm bridge */

/* Coin map: 1=coin present at maze cell (cx,cy). Index = cy*COLS+cx */
unsigned char coinmap[ROWS * COLS];
uint score;
unsigned char coins_left;
unsigned char level;
unsigned char difficulty;    /* 1=Easy, 2=Normal, 3=Hard, 4=Nightmare */
unsigned char num_enemies;   /* 1, 2, 3, or 4 */
unsigned char enemy_frames;  /* frames between enemy moves */
unsigned char chase_pct;     /* % chance enemy uses BFS chase */
unsigned char extra_wall_pct; /* 1-in-N chance to remove a wall */
unsigned char extra_halls_base; /* base number of extra halls */
unsigned char extra_halls_rng;  /* random additional halls */
unsigned char time_limit;       /* seconds per level for this difficulty */

/* High scores table */
uint hiscores[NUM_HISCORES];
unsigned char hilevel[NUM_HISCORES];

/* Timer state */
unsigned int timer_sec;    /* seconds remaining */
unsigned char timer_frac;  /* frame counter within current second (0-49) */

/* Buffer for formatting text */
char txt_buffer[TEXT_SCR_WIDTH + 1];

/* SP1 heap for sprite allocation (classic library).
   Heap lives in the rotation table area (0xF200-0xFFFF) which is
   unused since we use NR (no-rotation) sprites only. */
long heap;

void *u_malloc(uint size) { return malloc(size); }
void u_free(void *addr) { free(addr); }

/* SP1 sprite handles */
struct sp1_ss *spr_player;
struct sp1_ss *spr_enemies[4];
struct sp1_ss *spr_exit_s;

/* SP1 clipping rect for maze area */
struct sp1_Rect maze_clip = {MAZE_R0, MAZE_C0, ECOLS, EROWS};
struct sp1_Rect full_screen = {0, 0, 32, 24};

/* SP1 colour callback globals */
unsigned char sp1_colour;
unsigned char sp1_cmask;

void colourSpr(unsigned int count, struct sp1_cs *c)
{
	c->attr_mask = sp1_cmask;
	c->attr = sp1_colour;
}

/* BFS lookup tables: precomputed row/col from linear index,
   eliminates expensive Z80 division by 14 in the BFS inner loop */
unsigned char bfs_row[ROWS * COLS];
unsigned char bfs_col[ROWS * COLS];
/* Precomputed row * COLS: eliminates Z80 multiply by 14 */
unsigned char row_x_cols[ROWS];
/* Precomputed row * ECOLS: eliminates Z80 multiply by 29 */
unsigned int erow_x_ecols[EROWS];

/* Precomputed adjacency for BFS: 4 neighbors per cell (L,R,U,D).
   255 = wall or boundary. Built once after maze generation. */
unsigned char adj[ROWS * COLS * 4];

/* Transient globals for inline-asm BFS loop */
unsigned char bfs_efi_g;
unsigned char bfs_head_g;
unsigned char bfs_tail_g;
unsigned char bfs_result_g;
unsigned int bfs_adj_ptr_g;

#define ATTR_P_ADDR 23693
#define WALL_ATTR   (BRIGHT | INK_RED | PAPER_YELLOW)
#define CORR_ATTR   (INK_BLACK | PAPER_BLACK)
#define PLAYER_ATTR (BRIGHT | INK_GREEN | PAPER_BLACK)
#define ENEMY_ATTR  (BRIGHT | INK_RED | PAPER_BLACK)
#define ENEMY2_ATTR (BRIGHT | INK_MAGENTA | PAPER_BLACK)
#define ENEMY3_ATTR (BRIGHT | INK_CYAN | PAPER_BLACK)
#define ENEMY4_ATTR (BRIGHT | INK_WHITE | PAPER_BLACK)
#define EXIT_ATTR   (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define COIN_ATTR   (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define TITLE_ATTR  (BRIGHT | INK_YELLOW | PAPER_BLUE)
#define WIN_ATTR    (BRIGHT | INK_GREEN | PAPER_BLACK)
#define HISCORE_ATTR (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define TIMER_ATTR   (BRIGHT | INK_WHITE | PAPER_BLACK)
#define TIMER_WARN_ATTR (BRIGHT | INK_RED | PAPER_BLACK)

/* Exit position in expanded grid (randomized each level) */
unsigned char exit_gx, exit_gy;

/* Pre-computed 8-byte bitmaps for sprites (all within one 8x8 cell) */
//don't remove formatting and binary constants, it's more readable this way
/* Brick pattern: red ink on yellow paper */
const unsigned char brick[8] = {
	0b11110111, 
	0b11110111, 
	0b11110111, 
	0b00000000,
	0b11011111, 
	0b11011111, 
	0b11011111, 
	0b00000000
};

/* SP1 masked sprite graphics: (mask, graphic) pairs × 8 rows.
   mask = ~graphic where opaque, 0xFF where transparent.
   Height=2: content row + transparent overflow row (standard SP1 pattern).
   Frame pointer points past the top overflow row (column data[16]). */

/* Transparent row: 16 bytes of (0xFF, 0x00) */
#define SP1_TRANSPARENT_ROW \
	0xFF,0x00, 0xFF,0x00, 0xFF,0x00, 0xFF,0x00, \
	0xFF,0x00, 0xFF,0x00, 0xFF,0x00, 0xFF,0x00

unsigned char sp1_dot_data[] = {
	SP1_TRANSPARENT_ROW,   /* top overflow (LB col) */
	0xFF,0x00, 0xFF,0x00,
	0xC7,0x38, 0x83,0x7C,
	0x83,0x7C, 0x83,0x7C,
	0xC7,0x38, 0xFF,0x00,
	SP1_TRANSPARENT_ROW    /* bottom overflow (LB col) */
};
unsigned char *sp1_dot_gfx = &sp1_dot_data[16];

unsigned char sp1_enemy_data[] = {
	SP1_TRANSPARENT_ROW,
	0xFF,0x00, 0xFF,0x00,
	0xEF,0x10, 0xD7,0x28,
	0xAB,0x54, 0xD7,0x28,
	0xEF,0x10, 0xFF,0x00,
	SP1_TRANSPARENT_ROW
};
unsigned char *sp1_enemy_gfx = &sp1_enemy_data[16];

unsigned char sp1_exit_data[] = {
	SP1_TRANSPARENT_ROW,
	0xFF,0x00, 0x7D,0x82,
	0xBB,0x44, 0xD7,0x28,
	0xEF,0x10, 0xD7,0x28,
	0xBB,0x44, 0x7D,0x82,
	SP1_TRANSPARENT_ROW
};
unsigned char *sp1_exit_gfx = &sp1_exit_data[16];
const unsigned char spr_coin[8] = {
	0b00000000,
	0b00111100,
	0b01111110,
	0b01111110,
	0b00111100,
	0b00111100,
	0b00011000,
	0b00000000
};


/* Set attribute for character cell (row, col).
   Used for text rows outside SP1-managed area. */
void set_attr(unsigned char row, unsigned char col,
              unsigned char attr)
{
	*zx_cxy2aaddr(col, row) = attr;
}

/* Helper: set SP1 sprite colour */
void sp1_set_spr_colour(struct sp1_ss *s, unsigned char attr)
{
	sp1_cmask = 0x00;
	sp1_colour = attr;
	sp1_IterateSprChar(s, colourSpr);
}

/* Hide all SP1 sprites (move off-screen) */
void hide_sprites()
{
	static unsigned char i;
	sp1_MoveSprAbs(spr_player, &maze_clip, 0, 25, 0, 0, 0);
	for (i = 0; i != 4; ++i)
		sp1_MoveSprAbs(spr_enemies[i], &maze_clip, 0, 25, 0, 0, 0);
	sp1_MoveSprAbs(spr_exit_s, &maze_clip, 0, 25, 0, 0, 0);
}

/* Visited flags and explicit stack for DFS and BFS.
   Max index = ROWS*COLS-1 (maze cell grid). */
unsigned char vis[ROWS * COLS];
unsigned char stk[ROWS * COLS];
unsigned char sp;

void generate_maze()
{
	static unsigned char x, y, cx, cy, nx, ny;
	static unsigned char dirs[4], nd, i, j, t;

	for (y = 0; y != ROWS; ++y)
		for (x = 0; x != COLS; ++x) {
			walls[y][x] = 3;
			vis[y * COLS + x] = 0;
		}

	/* Recursive backtracker using explicit stack */
	cx = 0; cy = 0;
	vis[0] = 1;
	sp = 0;
	stk[sp++] = 0;

	while (sp > 0) {
		/* Collect unvisited neighbors */
		nd = 0;
		if (cx > 0 && !vis[cy * COLS + cx - 1])
			dirs[nd++] = 0;
		if (cx < COLS-1 && !vis[cy * COLS + cx + 1])
			dirs[nd++] = 1;
		if (cy > 0 && !vis[(cy-1) * COLS + cx])
			dirs[nd++] = 2;
		if (cy < ROWS-1 && !vis[(cy+1) * COLS + cx])
			dirs[nd++] = 3;

		if (nd == 0) {
			/* Backtrack */
			sp--;
			if (sp > 0) {
				t = stk[sp - 1];
				cx = t % COLS;
				cy = t / COLS;
			}
			continue;
		}

		/* Shuffle directions */
		for (i = nd - 1; i > 0; --i) {
			j = rand() % (i + 1);
			t = dirs[i];
			dirs[i] = dirs[j];
			dirs[j] = t;
		}

		/* Pick first (random) direction */
		nx = cx; ny = cy;
		switch (dirs[0]) {
		case 0: nx--; walls[cy][nx] &= ~1; break;
		case 1: walls[cy][cx] &= ~1; nx++; break;
		case 2: ny--; walls[ny][cx] &= ~2; break;
		case 3: walls[cy][cx] &= ~2; ny++; break;
		}

		vis[ny * COLS + nx] = 1;
		cx = nx; cy = ny;
		stk[sp++] = cy * COLS + cx;
	}
}

/* After generating the perfect maze, punch extra holes to create
   loops and small halls so the player can dodge the enemy. */
void add_extra_passages()
{
	static unsigned char x, y, i, n;

	/* Pass 1: randomly remove walls → loops (difficulty controls frequency) */
	for (y = 0; y != ROWS; ++y)
		for (x = 0; x != COLS; ++x) {
			if (x < COLS - 1 && (walls[y][x] & 1) && rand() % extra_wall_pct == 0)
				walls[y][x] &= ~1;
			if (y < ROWS - 1 && (walls[y][x] & 2) && rand() % extra_wall_pct == 0)
				walls[y][x] &= ~2;
		}

	/* Pass 2: create 2x2 halls (difficulty controls count) */
	n = extra_halls_base + rand() % (extra_halls_rng + 1);
	for (i = 0; i != n; ++i) {
		x = rand() % (COLS - 1);
		y = rand() % (ROWS - 1);
		/* Remove walls between (x,y), (x+1,y), (x,y+1), (x+1,y+1) */
		walls[y][x]     &= ~3;  /* right + bottom */
		walls[y][x + 1] &= ~2;  /* bottom */
		walls[y + 1][x] &= ~1;  /* right */
	}
}

unsigned char center_x(uchar text_len) __z88dk_fastcall;

void draw_maze()
{
	static unsigned char gr, gc, sr, sc, w;
	static unsigned char *wm;

	wm = wallmap;
	for (gr = 0; gr != EROWS; ++gr) {
		for (gc = 0; gc != ECOLS; ++gc) {
			sr = MAZE_R0 + gr;
			sc = MAZE_C0 + gc;
			w = 0;

			if (!(gr & 1) && !(gc & 1)) {
				/* Border posts are always walls */
				if (gr == 0 || gr == (ROWS << 1) ||
				    gc == 0 || gc == (COLS << 1)) {
					w = 1;
				} else {
					/* Interior corner post: wall only if
					   any adjacent wall segment exists */
					unsigned char cy, cx;
					cy = gr >> 1;
					cx = gc >> 1;
					if ((walls[cy-1][cx-1] & 1) ||
					    (walls[cy][cx-1] & 1) ||
					    (walls[cy-1][cx-1] & 2) ||
					    (walls[cy-1][cx] & 2))
						w = 1;
				}
			}
			else if (!(gr & 1) && (gc & 1)) {
				if (gr == 0 || gr == (ROWS << 1))
					w = 1;
				else if (walls[(gr >> 1) - 1][gc >> 1] & 2)
					w = 1;
			}
			else if ((gr & 1) && !(gc & 1)) {
				if (gc == 0 || gc == (COLS << 1))
					w = 1;
				else if (walls[gr >> 1][(gc >> 1) - 1] & 1)
					w = 1;
			}

			*wm++ = w;
			if (w)
				sp1_PrintAtInv(sr, sc, WALL_ATTR, 'B');
			else
				sp1_PrintAtInv(sr, sc, CORR_ATTR, ' ');
		}
	}
}

void draw_dot(unsigned char gx, unsigned char gy)
{
	sp1_MoveSprAbs(spr_player, &maze_clip, sp1_dot_gfx, SROW(gy), SCOL(gx), 0, 0);
}

void draw_exit(unsigned char gx, unsigned char gy)
{
	sp1_MoveSprAbs(spr_exit_s, &maze_clip, sp1_exit_gfx, SROW(gy), SCOL(gx), 0, 0);
}

void snd_step()
{
	bit_beep(0, 400);
	intrinsic_ei();
}

void snd_bump()
{
	bit_beep(0, 800);
	intrinsic_ei();
}

void snd_caught()
{
	bit_beep(20, 600);
	intrinsic_ei();
	bit_beep(20, 700);
	intrinsic_ei();
	bit_beep(40, 900);
	intrinsic_ei();
}

void snd_coin()
{
	bit_beep(0, 200);
	bit_beep(0, 100);
	intrinsic_ei();
}

void snd_win()
{
	bit_beep(15, 150);
	intrinsic_ei();
	bit_beep(15, 120);
	intrinsic_ei();
	bit_beep(30, 80);
	intrinsic_ei();
}

/* Wait for all keys released, then wait for any key press.
   Uses direct port scan — works without ROM interrupts. */
void wait_any_key()
{
#asm
	; Wait for all keys released (port $00FE reads all rows)
.wak_loop1
	xor a
	in a, ($FE)
	and $1F
	cp $1F
	jr nz, wak_loop1
	; Wait for any key pressed
.wak_loop2
	xor a
	in a, ($FE)
	and $1F
	cp $1F
	jr z, wak_loop2
#endasm
}

/* Wait until all keys are released */
void wait_key_release()
{
#asm
.wkr_loop
	xor a
	in a, ($FE)
	and $1F
	cp $1F
	jr nz, wkr_loop
#endasm
}

void draw_enemy_n(unsigned char n, unsigned char gx, unsigned char gy)
{
	sp1_MoveSprAbs(spr_enemies[n], &maze_clip, sp1_enemy_gfx, SROW(gy), SCOL(gx), 0, 0);
}

void draw_coin(unsigned char cx, unsigned char cy)
{
	unsigned char gx, gy;
	gx = (cx << 1) + 1;
	gy = (cy << 1) + 1;
	sp1_PrintAtInv(SROW(gy), SCOL(gx), COIN_ATTR, 'C');
}

/* Place coins on ~40% of maze cells, avoiding start/exit/enemies */
void place_coins()
{
	static unsigned char target, attempts, idx, cx, cy, gx, gy;
	coins_left = 0;
	memset(coinmap, 0, ROWS * COLS);

	target = (ROWS * COLS) * 2 / 5;
	attempts = target * 4;
	while (coins_left < target && attempts > 0) {
		attempts--;
		idx = rand() % (ROWS * COLS);
		if (coinmap[idx]) continue;
		cy = idx / COLS;
		cx = idx - cy * COLS;
		gx = cx * 2 + 1;
		gy = cy * 2 + 1;
		/* Skip player, exit, enemies */
		if (gx == px && gy == py) continue;
		if (gx == exit_gx && gy == exit_gy) continue;
		{
			unsigned char skip, ei;
			skip = 0;
			for (ei = 0; ei != num_enemies; ++ei)
				if (gx == enx[ei] && gy == eny[ei]) skip = 1;
			if (skip) continue;
		}
		/* No adjacent coins — min 2-cell gap */
		if (cx > 0 && coinmap[idx - 1]) continue;
		if (cx < COLS - 1 && coinmap[idx + 1]) continue;
		if (cy > 0 && coinmap[idx - COLS]) continue;
		if (cy < ROWS - 1 && coinmap[idx + COLS]) continue;
		coinmap[idx] = 1;
		coins_left++;
		draw_coin(cx, cy);
	}
}

/* Check and collect coin at expanded grid position (gx,gy) */
unsigned char try_collect_coin(unsigned char gx, unsigned char gy)
{
	unsigned char cx, cy;
	if (!(gx & 1) || !(gy & 1)) return 0;  /* not a cell center */
	cx = gx >> 1;
	cy = gy >> 1;
	unsigned char idx=row_x_cols[cy] + cx;
	if (coinmap[idx]) {
		coinmap[idx] = 0;
		coins_left--;
		score += 10;
		/* Remove coin tile — SP1 will restore corridor background */
		sp1_PrintAtInv(SROW(gy), SCOL(gx), CORR_ATTR, ' ');
		return 1;
	}
	return 0;
}

/* Display score on the title row */
void show_score()
{
	static unsigned char len;
	len = sprintf(txt_buffer, "SCORE: %06d", score);
	gotoxy(center_x(len), 22); printf(txt_buffer);
}

/* Display remaining time on row 0 */
void show_timer()
{
	static unsigned char len, c;
	static unsigned char m, s;
	static unsigned char attr;
	/* divmod 60 via subtraction — faster than library divide on Z80 */
	s = timer_sec;
	m = 0;
	while (s >= 60) { s -= 60; m++; }
	len = sprintf(txt_buffer, "TIME %d:%02d", m, s);
	gotoxy(center_x(len), 0); printf(txt_buffer);
	attr = (timer_sec <= 10) ? TIMER_WARN_ATTR : TIMER_ATTR;
	if (timer_sec <= 10 || timer_sec == time_limit) {
		for (c = 12; c != 20; ++c)
			set_attr(0, c, attr);
	}
}

/* Update high scores table, return rank (0-based) or -1 */
char update_hiscores()
{
	static unsigned char i, j;
	for (i = 0; i != NUM_HISCORES; ++i) {
		if (score > hiscores[i]) {
			/* Shift lower scores down */
			for (j = NUM_HISCORES - 1; j > i; --j) {
				hiscores[j] = hiscores[(unsigned char)(j-1)];
				hilevel[j] = hilevel[(unsigned char)(j-1)];
			}
			hiscores[i] = score;
			hilevel[i] = level;
			return i;
		}
	}
	return -1;
}

/* Display high scores screen */
void show_hiscores(char rank) __z88dk_fastcall
{
	static unsigned char i, r, c;
	static unsigned char attr;

	hide_sprites();
	/* Clear screen via SP1 */
	sp1_ClearRectInv(&full_screen, PAPER_BLACK | INK_WHITE, ' ',
		SP1_RFLAG_TILE | SP1_RFLAG_COLOUR);
	sp1_UpdateNow();


	static unsigned char len;
	len = sprintf(txt_buffer, "-= HIGH SCORES =-");
	gotoxy(center_x(len), 1);
	printf(txt_buffer);

	for (i = 0; i != NUM_HISCORES; ++i) {
		r = 3 + (i << 1);
		if (hiscores[i] == 0) {
			gotoxy(6, r);
			printf("%d.  ----", i + 1);
		} else {
			gotoxy(6, r);
			printf("%d.  %06u  Level %d", i + 1,
			       hiscores[i], hilevel[i]);
		}
		attr = (i == rank) ? (BRIGHT | INK_GREEN | PAPER_BLACK)
		                    : HISCORE_ATTR;
		for (c = 4; c != 28; ++c)
			set_attr(r, c, attr);
	}

	gotoxy(9, 16);
	printf("Press any key...");
	for (c = 4; c != 28; ++c)
		set_attr(16, c, TITLE_ATTR);

	wait_any_key();
}

/* Build adjacency table from walls[][] for fast BFS.
   Must be called after generate_maze() + add_extra_passages(). */
void build_adj()
{
	static unsigned char i, r, c;
	static uint idx;
	for (i = 0; i != ROWS * COLS; ++i) {
		r = bfs_row[i];
		c = bfs_col[i];
		idx = i * 4;
		adj[idx]     = (c > 0 && !(walls[r][c - 1] & 1)) ? i - 1 : 255;
		adj[idx + 1] = (c < COLS - 1 && !(walls[r][c] & 1)) ? i + 1 : 255;
		adj[idx + 2] = (r > 0 && !(walls[r - 1][c] & 2)) ? i - COLS : 255;
		adj[idx + 3] = (r < ROWS - 1 && !(walls[r][c] & 2)) ? i + COLS : 255;
	}
}

/* BFS on maze cell grid with inline-asm inner loop.
   Precomputed adj[] eliminates all walls/boundary checks.
   Returns: 0=left,1=right,2=up,3=down, -1=no path */
char enemy_bfs(unsigned char exx, unsigned char eyy)
{
	static unsigned char ecx, ecy, pcx, pcy;
	static unsigned char ci;
	static unsigned char d;

	ecx = exx >> 1;  ecy = eyy >> 1;
	pcx = px >> 1;   pcy = py >> 1;

	bfs_efi_g = row_x_cols[ecy] + ecx;

	bfs_head_g = 0;
	bfs_tail_g = 1;
	ci = row_x_cols[pcy] + pcx;
	stk[0] = ci;
	vis[ci] = 5;
	bfs_result_g = 255;

#asm
	push ix

	;=== BFS main loop (inline asm for speed) ===
.bfs_il_loop
	; --- Check termination ---
	ld a, (_bfs_head_g)
	ld b, a
	ld a, (_bfs_tail_g)
	cp b
	jp z, bfs_il_end          ; head == tail, queue empty
	ld a, b
	cp 40
	jp nc, bfs_il_end         ; head >= 40, depth limit

	; --- Dequeue: ci = stk[head]; head++ ---
	ld l, b
	ld h, 0
	ld de, _stk
	add hl, de
	ld c, (hl)                ; C = ci
	inc b
	ld a, b
	ld (_bfs_head_g), a

	; --- Check if found target ---
	ld a, (_bfs_efi_g)
	cp c
	jp z, bfs_il_found

	; --- Compute adj base: HL = &adj[ci*4] ---
	ld l, c
	ld h, 0
	add hl, hl
	add hl, hl                ; HL = ci * 4
	ld de, _adj
	add hl, de                ; HL = &adj[ci*4]

	; === Dir 0 (left), vis = 1 ===
	ld a, (hl)
	inc hl
	ld (_bfs_adj_ptr_g), hl   ; save ptr for next dir
	cp 255
	jr z, bfs_il_s0
	ld e, a
	ld d, 0
	ld hl, _vis
	add hl, de
	ld a, (hl)
	or a
	jr nz, bfs_il_s0
	ld (hl), 1
	ld c, e                   ; C = ni (ci no longer needed)
	ld a, (_bfs_tail_g)
	ld l, a
	ld h, 0
	ld de, _stk
	add hl, de
	ld (hl), c
	inc a
	ld (_bfs_tail_g), a
.bfs_il_s0

	; === Dir 1 (right), vis = 2 ===
	ld hl, (_bfs_adj_ptr_g)
	ld a, (hl)
	inc hl
	ld (_bfs_adj_ptr_g), hl
	cp 255
	jr z, bfs_il_s1
	ld e, a
	ld d, 0
	ld hl, _vis
	add hl, de
	ld a, (hl)
	or a
	jr nz, bfs_il_s1
	ld (hl), 2
	ld c, e
	ld a, (_bfs_tail_g)
	ld l, a
	ld h, 0
	ld de, _stk
	add hl, de
	ld (hl), c
	inc a
	ld (_bfs_tail_g), a
.bfs_il_s1

	; === Dir 2 (up), vis = 3 ===
	ld hl, (_bfs_adj_ptr_g)
	ld a, (hl)
	inc hl
	ld (_bfs_adj_ptr_g), hl
	cp 255
	jr z, bfs_il_s2
	ld e, a
	ld d, 0
	ld hl, _vis
	add hl, de
	ld a, (hl)
	or a
	jr nz, bfs_il_s2
	ld (hl), 3
	ld c, e
	ld a, (_bfs_tail_g)
	ld l, a
	ld h, 0
	ld de, _stk
	add hl, de
	ld (hl), c
	inc a
	ld (_bfs_tail_g), a
.bfs_il_s2

	; === Dir 3 (down), vis = 4 ===
	ld hl, (_bfs_adj_ptr_g)
	ld a, (hl)
	; (no need to save adj ptr — last direction)
	cp 255
	jr z, bfs_il_s3
	ld e, a
	ld d, 0
	ld hl, _vis
	add hl, de
	ld a, (hl)
	or a
	jr nz, bfs_il_s3
	ld (hl), 4
	ld c, e
	ld a, (_bfs_tail_g)
	ld l, a
	ld h, 0
	ld de, _stk
	add hl, de
	ld (hl), c
	inc a
	ld (_bfs_tail_g), a
.bfs_il_s3

	jp bfs_il_loop

.bfs_il_found
	; C = ci = efi — read vis[efi] as result
	ld l, c
	ld h, 0
	ld de, _vis
	add hl, de
	ld a, (hl)
	ld (_bfs_result_g), a

.bfs_il_end
	pop ix
#endasm

	/* Cleanup: zero visited entries */
	{
		unsigned char i;
		for (i = 0; i != bfs_tail_g; ++i)
			vis[stk[i]] = 0;
	}

	d = bfs_result_g;
	if (d == 255) return -1;
	d--;
	if (d == 0) return 1;  /* was left, enemy goes right */
	if (d == 1) return 0;  /* was right, enemy goes left */
	if (d == 2) return 3;  /* was up, enemy goes down */
	if (d == 3) return 2;  /* was down, enemy goes up */
	return -1;
}

/* Pick a random valid direction from (exx,eyy) on expanded grid */
char enemy_random_dir(unsigned char exx, unsigned char eyy)
{
	static unsigned char dirs[4];
	static uint fi;
	static unsigned char nd;
	fi = erow_x_ecols[eyy] + exx;
	nd = 0;
	if (!wallmap[fi - 1])     dirs[nd++] = 0;
	if (!wallmap[fi + 1])     dirs[nd++] = 1;
	if (!wallmap[fi - ECOLS]) dirs[nd++] = 2;
	if (!wallmap[fi + ECOLS]) dirs[nd++] = 3;
	if (nd == 0) return -1;
	return dirs[rand() % nd];
}

/* Try Manhattan direction toward player from (exx,eyy) on expanded grid.
   Returns direction if passable, -1 if both axes blocked. */
char enemy_manhattan(unsigned char exx, unsigned char eyy)
{
	static unsigned char adx, ady;
	static uint fi;
	static char dir1, dir2;
	static char ddx, ddy;
	ddx = px - exx;
	ddy = py - eyy;
	adx = ddx < 0 ? -ddx : ddx;
	ady = ddy < 0 ? -ddy : ddy;
	fi = erow_x_ecols[eyy] + exx;

	/* Pick primary axis (larger distance), secondary as fallback */
	if (adx >= ady) {
		dir1 = (ddx > 0) ? 1 : 0;
		dir2 = (ady == 0) ? -1 : ((ddy > 0) ? 3 : 2);
	} else {
		dir1 = (ddy > 0) ? 3 : 2;
		dir2 = (adx == 0) ? -1 : ((ddx > 0) ? 1 : 0);
	}

	/* Try primary direction */
	if (dir1 == 0 && !wallmap[fi - 1]) return 0;
	if (dir1 == 1 && !wallmap[fi + 1]) return 1;
	if (dir1 == 2 && !wallmap[fi - ECOLS]) return 2;
	if (dir1 == 3 && !wallmap[fi + ECOLS]) return 3;

	/* Try secondary direction */
	if (dir2 >= 0) {
		if (dir2 == 0 && !wallmap[fi - 1]) return 0;
		if (dir2 == 1 && !wallmap[fi + 1]) return 1;
		if (dir2 == 2 && !wallmap[fi - ECOLS]) return 2;
		if (dir2 == 3 && !wallmap[fi + ECOLS]) return 3;
	}

	return -1;  /* both axes blocked — need BFS */
}

/* Move enemy n one step.
   At odd position (maze cell): use cached BFS, recalc, or random.
   At even position (corridor): continue in same direction.
   SP1 handles background restoration and sprite compositing. */
void move_enemy(uchar n) __z88dk_fastcall
{
	static char dir;
	static uchar old_ex, old_ey;
	static uchar sn;   /* copy n to static — sccz80 corrupts uchar params across calls */

	sn = n;
	old_ex = enx[sn];
	old_ey = eny[sn];

	if ((old_ex & 1) && (old_ey & 1)) {
		/* At maze cell — chase_pct% chase, rest random */
		if ((uint)(rand() % 100) < chase_pct) {
			dir = enemy_manhattan(old_ex, old_ey);
			if (dir < 0)
				dir = enemy_bfs(old_ex, old_ey);
		} else
			dir = enemy_random_dir(old_ex, old_ey);
	} else {
		/* In corridor between cells — keep going */
		dir = last_edir_arr[sn];
	}
	if (dir < 0) return;
	last_edir_arr[sn] = dir;

	if (dir == 0) enx[sn]--;
	else if (dir == 1) enx[sn]++;
	else if (dir == 2) eny[sn]--;
	else if (dir == 3) eny[sn]++;

	/* Enemy eats coin if it lands on one */
	if ((enx[sn] & 1) && (eny[sn] & 1)) {
		uchar ccx, ccy;
		ccx = enx[sn] >> 1;
		ccy = eny[sn] >> 1;
		if (coinmap[ccy * COLS + ccx]) {
			coinmap[ccy * COLS + ccx] = 0;
			coins_left--;
			/* Remove coin tile */
			sp1_PrintAtInv(SROW(eny[sn]), SCOL(enx[sn]), CORR_ATTR, ' ');
		}
	}

	/* Move SP1 sprite to new position */
	draw_enemy_n(sn, enx[sn], eny[sn]);
}

/* Pick a random maze cell center in expanded grid, avoiding
   positions already used. ox1,oy1,ox2,oy2 = positions to avoid
   (255 means ignore). */
void random_start(unsigned char *gx, unsigned char *gy,
                  unsigned char ox1, unsigned char oy1,
                  unsigned char ox2, unsigned char oy2)
{
	unsigned char cx, cy;
	do {
		cx = rand() % COLS;
		cy = rand() % ROWS;
		*gx = (cx << 1) + 1;
		*gy = (cy << 1) + 1;
	} while ((*gx == ox1 && *gy == oy1) ||
	         (*gx == ox2 && *gy == oy2));
}

unsigned char can_move(char dx, char dy)
{
	return !wallmap[erow_x_ecols[py + dy] + px + dx];
}

/* Draw a popup window background: rows 10-14, cols 5-26.
   Uses SP1 tiles: brick for border, space for inner. */
void draw_popup_bg(unsigned char border_attr, unsigned char inner_attr)
{
	static unsigned char r, c;
	hide_sprites();
	for (r = 10; r <= 14; ++r) {
		for (c = 5; c <= 26; ++c) {
			if (r == 10 || r == 14 || c == 5 || c == 26)
				sp1_PrintAtInv(r, c, border_attr, 'B');
			else
				sp1_PrintAtInv(r, c, inner_attr, ' ');
		}
	}
	sp1_UpdateNow();

}

/* Re-apply inner_attr to text rows 11-13, cols 6-25 (undoes printf attr side-effects). */
void popup_fix_attrs(unsigned char inner_attr) __z88dk_fastcall
{
	static unsigned char c;
	for (c = 6; c <= 25; ++c) {
		set_attr(11, c, inner_attr);
		set_attr(12, c, inner_attr);
		set_attr(13, c, inner_attr);
	}
}

unsigned char center_x(uchar text_len) __z88dk_fastcall
{
	return (TEXT_SCR_WIDTH - text_len) >> 1;
}

void win_cut_scene()
{
	zx_border(INK_GREEN);
	snd_win();
	zx_border(INK_BLACK);
	draw_popup_bg(
		BRIGHT | INK_YELLOW | PAPER_GREEN,
		BRIGHT | INK_WHITE  | PAPER_GREEN);
	*((unsigned char *)ATTR_P_ADDR) =
		BRIGHT | INK_WHITE | PAPER_GREEN;
	
	static unsigned char len;
	len = sprintf(txt_buffer, "** ESCAPED! **");
	gotoxy(center_x(len), 11); printf(txt_buffer);
	len = sprintf(txt_buffer, "Time bonus: +%d", timer_sec);
	gotoxy(center_x(len), 12); printf(txt_buffer);
	len = sprintf(txt_buffer, "Any key - next level");
	gotoxy(center_x(len), 13); printf(txt_buffer);
	popup_fix_attrs(BRIGHT | INK_BLACK | PAPER_GREEN);
}

void game_over_cut_scene()
{
	zx_border(INK_RED);
	snd_caught();
	zx_border(INK_BLACK);
	draw_popup_bg(
		BRIGHT | INK_YELLOW | PAPER_RED,
		BRIGHT | INK_WHITE  | PAPER_RED);
	*((unsigned char *)ATTR_P_ADDR) =
		BRIGHT | INK_WHITE | PAPER_RED;
	static unsigned char len;
	len = sprintf(txt_buffer, "** CAUGHT! **");
	gotoxy(center_x(len), 11); printf(txt_buffer);
	len = sprintf(txt_buffer, "Score: %d", score);
	gotoxy(center_x(len), 12); printf(txt_buffer);
	len = sprintf(txt_buffer, "Press any key...");
	gotoxy(center_x(len), 13); printf(txt_buffer);
	popup_fix_attrs(BRIGHT | INK_WHITE | PAPER_RED);
}

void time_up_cut_scene()
{
	zx_border(INK_RED);
	snd_caught();
	zx_border(INK_BLACK);
	draw_popup_bg(
		BRIGHT | INK_YELLOW | PAPER_RED,
		BRIGHT | INK_WHITE  | PAPER_RED);
	*((unsigned char *)ATTR_P_ADDR) =
		BRIGHT | INK_WHITE | PAPER_RED;
	static unsigned char len;
	len = sprintf(txt_buffer, "** TIME UP! **");
	gotoxy(center_x(len), 11); printf(txt_buffer);
	len = sprintf(txt_buffer, "Score: %d", score);
	gotoxy(center_x(len), 12); printf(txt_buffer);
	len = sprintf(txt_buffer, "Press any key...");
	gotoxy(center_x(len), 13); printf(txt_buffer);
	popup_fix_attrs(BRIGHT | INK_WHITE | PAPER_RED);
}

/* Frame-based timing (1 frame = 20ms at 50Hz) */
#define KEY_REPEAT    4   /* held key repeats every 4 frames = 80ms */

main()
{
	static char k;
	static char dx, dy;
	static unsigned char caught;
	static unsigned char tick;
	static char rank;
	static unsigned char key_delay;
	static unsigned char game_over;

	/* Initialize high scores */
	for (rank = 0; rank != NUM_HISCORES; ++rank) {
		hiscores[rank] = 0;
		hilevel[rank] = 0;
	}
	level = 0;
	score = 0;

	/* Init BFS lookup tables (avoids Z80 division by 14 in inner loop) */
	for (rank = 0; rank != ROWS * COLS; ++rank) {
		bfs_row[rank] = rank / COLS;
		bfs_col[rank] = rank % COLS;
	}
	/* Init row*COLS lookup table (avoids Z80 multiply by 14) */
	for (rank = 0; rank != ROWS; ++rank) {
		row_x_cols[rank] = rank * COLS;
	}
	/* Init row*ECOLS lookup table (avoids Z80 multiply by 29) */
	for (rank = 0; rank != EROWS; ++rank) {
		erow_x_ecols[rank] = rank * ECOLS;
	}

	/* Install IM2 null interrupt handler.
	   This replaces the ROM IM1 handler (which uses IY and corrupts SP1).
	   Null ISR just does EI;RETI — no IY access, no memory corruption.
	   Vector table: 257 bytes at $D000 filled with $D1.
	   ISR at $D1D1 (in free gap between $D100 and SP1 at $D1ED). */
	intrinsic_di();
	memset((void *)0xD000, 0xD1, 257);
	*((unsigned char *)0xD1D1) = 0xFB;  /* EI opcode */
	*((unsigned char *)0xD1D2) = 0xED;  /* RETI prefix */
	*((unsigned char *)0xD1D3) = 0x4D;  /* RETI opcode */
#asm
	ld a, $D0
	ld i, a
	im 2
#endasm
	intrinsic_ei();

	/* Initialize SP1 sprite engine */
	heap = 0L;
	sbrk((void *)0xF200, 0x0DFE);  /* rotation table area, unused with NR sprites */

	sp1_Initialize(SP1_IFLAG_OVERWRITE_TILES | SP1_IFLAG_OVERWRITE_DFILE,
		INK_BLACK | PAPER_BLACK, ' ');
	sp1_TileEntry('B', brick);
	sp1_TileEntry('C', spr_coin);

	/* Create SP1 sprites: single-column NR (non-rotated), height=2.
	   xthresh=0 prevents the only column from being suppressed at hrot=0. */
	spr_player = sp1_CreateSpr(SP1_DRAW_MASK2NR, SP1_TYPE_2BYTE, 2, 0, 0);
	spr_player->xthresh = 0;
	spr_player->ythresh = 1;
	sp1_set_spr_colour(spr_player, PLAYER_ATTR);

	spr_exit_s = sp1_CreateSpr(SP1_DRAW_MASK2NR, SP1_TYPE_2BYTE, 2, 0, 1);
	spr_exit_s->xthresh = 0;
	spr_exit_s->ythresh = 1;
	sp1_set_spr_colour(spr_exit_s, EXIT_ATTR);

	{
		static unsigned char ei;
		static unsigned char ea;
		for (ei = 0; ei != 4; ++ei) {
			spr_enemies[ei] = sp1_CreateSpr(SP1_DRAW_MASK2NR, SP1_TYPE_2BYTE, 2, 0, 2);
			spr_enemies[ei]->xthresh = 0;
			spr_enemies[ei]->ythresh = 1;
			ea = (ei == 0) ? ENEMY_ATTR : (ei == 1) ? ENEMY2_ATTR :
			     (ei == 2) ? ENEMY3_ATTR : ENEMY4_ATTR;
			sp1_set_spr_colour(spr_enemies[ei], ea);
		}
	}

	/* Move all sprites off-screen initially */
	hide_sprites();

	sp1_Invalidate(&full_screen);
	sp1_UpdateNow();

	rseed = 0;

	/* Difficulty selection (first pick also seeds RNG) */
	while (1) {
		/* Clear screen via SP1 */
		hide_sprites();
		sp1_ClearRectInv(&full_screen, PAPER_BLACK | INK_WHITE, ' ',
			SP1_RFLAG_TILE | SP1_RFLAG_COLOUR);
		sp1_UpdateNow();

		{
			static unsigned char len, c;
			len = sprintf(txt_buffer, "-= MAZE RUNNER =-");
			gotoxy(center_x(len), 3); printf(txt_buffer);
			for (c = 6; c != 26; ++c)
				set_attr(3, c, TITLE_ATTR);

			len = sprintf(txt_buffer, "Select difficulty:");
			gotoxy(center_x(len), 7); printf(txt_buffer);

			len = sprintf(txt_buffer, "1 - Easy");
			gotoxy(center_x(len), 10); printf(txt_buffer);
			for (c = 12; c != 20; ++c)
				set_attr(10, c, BRIGHT | INK_GREEN | PAPER_BLACK);

			len = sprintf(txt_buffer, "2 - Normal");
			gotoxy(center_x(len), 12); printf(txt_buffer);
			for (c = 11; c != 21; ++c)
				set_attr(12, c, BRIGHT | INK_YELLOW | PAPER_BLACK);

			len = sprintf(txt_buffer, "3 - Hard");
			gotoxy(center_x(len), 14); printf(txt_buffer);
			for (c = 12; c != 20; ++c)
				set_attr(14, c, BRIGHT | INK_RED | PAPER_BLACK);

			len = sprintf(txt_buffer, "4 - Nightmare");
			gotoxy(center_x(len), 16); printf(txt_buffer);
			for (c = 10; c != 22; ++c)
				set_attr(16, c, BRIGHT | INK_MAGENTA | PAPER_BLACK);
		}

		/* Wait for 1/2/3; spin increments rseed for RNG seeding */
		k = 0;
		while (k != '1' && k != '2' && k != '3' && k != '4') {
			rseed++;
			k = getk_inkey();
		}
		wait_key_release();
		if (rseed == 0) rseed = 42;
		srand(rseed);
		difficulty = k - '0';

		if (difficulty == 1) {
			num_enemies = 1;
			enemy_frames = 8;
			chase_pct = 10;
			extra_wall_pct = 3;   /* 1-in-3 = ~33% walls removed */
			extra_halls_base = 5;
			extra_halls_rng = 3;  /* 5-8 halls */
			time_limit = 90;
		} else if (difficulty == 2) {
			num_enemies = 2;
			enemy_frames = 6;
			chase_pct = 25;
			extra_wall_pct = 5;   /* 1-in-5 = ~20% walls removed */
			extra_halls_base = 3;
			extra_halls_rng = 2;  /* 3-5 halls */
			time_limit = 60;
		} else if (difficulty == 3) {
			num_enemies = 3;
			enemy_frames = 4;
			chase_pct = 50;
			extra_wall_pct = 8;   /* 1-in-8 = ~12% walls removed */
			extra_halls_base = 1;
			extra_halls_rng = 1;  /* 1-2 halls */
			time_limit = 45;
		} else {
			num_enemies = 4;
			enemy_frames = 3;
			chase_pct = 70;
			extra_wall_pct = 10;  /* 1-in-10 = ~10% walls removed */
			extra_halls_base = 1;
			extra_halls_rng = 0;  /* 1 hall */
			time_limit = 30;
		}

	game_over = 0;
	while (1) {
		zx_border(INK_BLACK);

		level++;
		/* Clear screen via SP1 for new level */
		hide_sprites();
		sp1_ClearRectInv(&full_screen, INK_WHITE | PAPER_BLACK, ' ',
			SP1_RFLAG_TILE | SP1_RFLAG_COLOUR);
		sp1_UpdateNow();

		{
			static unsigned char len;
			static char *dname;
			dname = (difficulty == 1) ? "Easy" :
			        (difficulty == 2) ? "Normal" :
			        (difficulty == 3) ? "Hard" : "Nightmare";
			len = sprintf(txt_buffer, "Lv%d [%s]  O/P/Q/A", level, dname);
			gotoxy(center_x(len), 23); printf(txt_buffer);
		}

		generate_maze();
		add_extra_passages();
		build_adj();

		draw_maze();
		memset(vis, 0, ROWS * COLS);  /* Clear for BFS after maze gen */

		/* Random starting positions: exit first so others avoid it */
		random_start(&exit_gx, &exit_gy, 255, 255, 255, 255);
		random_start(&px, &py, exit_gx, exit_gy, 255, 255);
		random_start(&enx[0], &eny[0], px, py, exit_gx, exit_gy);
		if (num_enemies > 1)
			random_start(&enx[1], &eny[1], px, py, enx[0], eny[0]);
		if (num_enemies > 2)
			random_start(&enx[2], &eny[2], px, py, enx[1], eny[1]);
		if (num_enemies > 3)
			random_start(&enx[3], &eny[3], px, py, enx[2], eny[2]);
		caught = 0;
		tick = 0;
		key_delay = 0;
		enemy_next = 0;
		enemy_accum = 0;
		timer_sec = time_limit;
		timer_frac = 50;  /* 50 frames = 1 second at 50Hz */
		{
			unsigned char ei;
			for (ei = 0; ei != num_enemies; ++ei)
				last_edir_arr[ei] = 0;
		}

		/* Place coins and draw everything */
		place_coins();
		draw_exit(exit_gx, exit_gy);
		{
			static unsigned char ei;
			for (ei = 0; ei != num_enemies; ++ei)
				draw_enemy_n(ei, enx[ei], eny[ei]);
		}
		draw_dot(px, py);
		sp1_UpdateNow();

		show_score();
		show_timer();

		while (1) {
			intrinsic_halt();  /* sync to 50Hz frame (IM2 null ISR — safe) */

			/* --- Player input (direct port read) ---
			   ZX Spectrum keyboard: IN port 0xFE, high byte selects half-row.
			   Bit=0 means key pressed.
			   O/P row: port 0xDFFE  — bit0=P, bit1=O
			   Q row:   port 0xFBFE  — bit0=Q
			   A row:   port 0xFDFE  — bit0=A */
			dx = 0;
			dy = 0;
			port_key = 0;
#asm
			ld bc, $DFFE    ; O/P row
			in a, (c)
			bit 1, a
			jr nz, _not_o
			ld a, 'o'
			ld (_port_key), a
_not_o:
			ld bc, $DFFE
			in a, (c)
			bit 0, a
			jr nz, _not_p
			ld a, 'p'
			ld (_port_key), a
_not_p:
			ld bc, $FBFE    ; Q row
			in a, (c)
			bit 0, a
			jr nz, _not_q
			ld a, 'q'
			ld (_port_key), a
_not_q:
			ld bc, $FDFE    ; A row
			in a, (c)
			bit 0, a
			jr nz, _not_a
			ld a, 'a'
			ld (_port_key), a
_not_a:
#endasm
			k = port_key;
			if (k) {
				if (key_delay > 0) {
					key_delay--;
				} else {
					if (k == 'o') dx = -1;
					if (k == 'p') dx = 1;
					if (k == 'q') dy = -1;
					if (k == 'a') dy = 1;
				}
			} else {
				key_delay = 0;
			}

			if (dx || dy) {
				key_delay = KEY_REPEAT;
				if (can_move(dx, dy)) {
					px += dx;
					py += dy;
					draw_dot(px, py);

					/* Collect coin? */
					if (try_collect_coin(px, py)) {
						zx_border(INK_YELLOW);
						snd_coin();
						zx_border(INK_BLACK);
				
						show_score();
					}

					/* Player walked onto enemy? */
					{
						unsigned char ei;
						for (ei = 0; ei != num_enemies; ++ei)
							if (px == enx[ei] && py == eny[ei])
								caught = 1;
					}

					if (px == exit_gx &&
					    py == exit_gy) {
						score += 50 + timer_sec;
						win_cut_scene();
						wait_any_key();
						break;
					}
					snd_step();
				} else {
					snd_bump();
				}
			}

			/* --- Enemy moves spread across frames --- */
			enemy_accum += num_enemies;
			while (enemy_accum >= enemy_frames) {
				enemy_accum -= enemy_frames;
				move_enemy(enemy_next);
				if (enx[enemy_next] == px && eny[enemy_next] == py)
					caught = 1;
				enemy_next++;
				if (enemy_next >= num_enemies)
					enemy_next = 0;
			}

			/* --- Render all SP1 changes this frame --- */
			sp1_UpdateNow();

			/* --- Timer countdown --- */
			timer_frac--;
			if (timer_frac == 0) {
				timer_frac = 50;
				if (timer_sec > 0) {
					timer_sec--;
			
					show_timer();
					if (timer_sec == 10)
						zx_border(INK_RED);
					if (timer_sec == 0) {
						time_up_cut_scene();
						wait_any_key();
						rank = update_hiscores();
						show_hiscores(rank);
						score = 0;
						level = 0;
						game_over = 1;
						break;
					}
				}
			}

			if (caught) {
				game_over_cut_scene();
				wait_any_key();
				rank = update_hiscores();
				show_hiscores(rank);
				score = 0;
				level = 0;
				game_over = 1;
				break;
			}
		}
		if (game_over) break;
	}
	} /* end difficulty selection loop */
}
