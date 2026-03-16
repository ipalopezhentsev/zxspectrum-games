#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <features.h>
#include <sound.h>
#include <intrinsic.h>
#include <input.h>
#include <arch/zx/spectrum.h>
#include <arch/zx/sprites/sp1.h>
#include <malloc.h>

#pragma output CRT_ORG_CODE = 0x6000
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

/* Pixel positions for smooth scrolling (pixel = grid_pos * 8) */
unsigned char ppx, ppy;         /* player pixel position in maze area */
unsigned char epx[4], epy[4];   /* enemy pixel positions */

/* Animation state: 0=at grid boundary, >0=frames remaining */
unsigned char panim;
unsigned char pdir;              /* player animation direction (0-3) */
unsigned char eanim[4];          /* enemy animation counters */
unsigned char edir_anim[4];     /* enemy animation directions */

#define MOVE_SPEED 2
#define ANIM_FRAMES 4

/* Frame buffers for pre-shifted 2-column sprites.
   Layout per buffer: 46 bytes col0 + 46 bytes col1 = 92 bytes.
   Each column: 8 transparent + 8 content + 7 padding = 23 lines × 2 bytes. */
unsigned char framebuf_player[92];
unsigned char framebuf_enemies[4][92];
unsigned int rseed;
/* User-defined keys for in_JoyKeyboard() — OPQA directions */
struct in_UDK udk;

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

/* Gun pickup state */
unsigned char gun_gx, gun_gy;  /* gun position in expanded grid */
unsigned char has_gun;          /* 1 = player carrying the gun */
unsigned char gun_placed;       /* 1 = gun exists on the map */
unsigned char enemy_stun[4];    /* stun timer in seconds, 0 = active */

#define STUN_SECS 8

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

/* SP1 clipping rect for maze area — expanded for pixel scrolling overflow */
struct sp1_Rect maze_clip = {MAZE_R0 - 1, MAZE_C0, ECOLS + 1, EROWS + 2};
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
#define CORR_ATTR   (INK_WHITE | PAPER_BLACK)
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
#define GUN_ATTR    (BRIGHT | INK_CYAN | PAPER_BLACK)
#define SHOT_ATTR   (BRIGHT | INK_CYAN | PAPER_CYAN)

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

/* SP1 MASK2NR sprite graphics: (mask, graphic) pairs × 8 rows.
   Masks are 0x00 (fully opaque) so sprite overwrites background completely.
   Overflow rows use 0xFF mask (fully transparent) to not affect adjacent cells.
   Height=2: content row + transparent overflow row.
   Frame pointer points past the top overflow row (data[16]). */

/* Transparent overflow row: 16 bytes of (0xFF, 0x00) */
#define SP1_TRANSPARENT_ROW \
	0xFF,0x00, 0xFF,0x00, 0xFF,0x00, 0xFF,0x00, \
	0xFF,0x00, 0xFF,0x00, 0xFF,0x00, 0xFF,0x00


unsigned char sp1_exit_data[] = {
	SP1_TRANSPARENT_ROW,
	0x00,0x00, 0x00,0x82,
	0x00,0x44, 0x00,0x28,
	0x00,0x10, 0x00,0x28,
	0x00,0x44, 0x00,0x82,
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

/* Gun pickup: arrow/blaster shape */
const unsigned char spr_gun[8] = {
	0b00000000,
	0b00011110,
	0b00111111,
	0b01111100,
	0b01111100,
	0b00111111,
	0b00011110,
	0b00000000
};

/* Floor tile: sparse dot pattern for corridor background */
const unsigned char floor_tile[8] = {
	0b00000000,
	0b00100010,
	0b00000000,
	0b00000000,
	0b00000000,
	0b10001000,
	0b00000000,
	0b00000000
};

/* Raw 8-byte graphics for frame generation (pixel scrolling) */
const unsigned char gfx_player[8] = {0x00, 0x00, 0x38, 0x7C, 0x7C, 0x7C, 0x38, 0x00};
const unsigned char gfx_enemy[8]  = {0x00, 0x10, 0x28, 0x54, 0x28, 0x10, 0x00, 0x00};


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

/* Generate pre-shifted frame buffer for a 2-column MASK2NR sprite.
   buf: 92-byte output buffer (46 col0 + 46 col1).
   gfx: 8-byte source graphic.
   h: horizontal shift amount (0-7).
   cell_aligned: 1 = content first (for vrot=0), 0 = transparent first (for vrot>0).
   Mask = ~graphic for transparent sprite edges. */
void gen_frame(unsigned char *buf, const unsigned char *gfx,
               unsigned char h, unsigned char cell_aligned)
{
	static unsigned char i, g, g0, g1, shl;
	static unsigned char *p;

	p = buf;
	shl = 8 - h;

	/* Column 0 */
	if (!cell_aligned)
		for (i = 0; i != 8; ++i) { *p++ = 0xFF; *p++ = 0x00; }
	for (i = 0; i != 8; ++i) {
		g = gfx[i];
		g0 = g >> h;
		*p++ = ~g0;
		*p++ = g0;
	}
	for (i = 0; i != (cell_aligned ? 15 : 7); ++i) { *p++ = 0xFF; *p++ = 0x00; }

	/* Column 1 */
	if (!cell_aligned)
		for (i = 0; i != 8; ++i) { *p++ = 0xFF; *p++ = 0x00; }
	for (i = 0; i != 8; ++i) {
		g = gfx[i];
		g1 = (g << shl) & 0xFF;
		*p++ = ~g1;
		*p++ = g1;
	}
	for (i = 0; i != (cell_aligned ? 15 : 7); ++i) { *p++ = 0xFF; *p++ = 0x00; }
}

/* Render a pixel-scrolled sprite at pixel position (pixel_x, pixel_y)
   relative to the maze origin. Uses vrot for vertical sub-cell offset
   and pre-shifted frames for horizontal. */
void render_spr_pix(struct sp1_ss *s, unsigned char *buf,
                    const unsigned char *gfx,
                    unsigned char pixel_x, unsigned char pixel_y)
{
	static unsigned char h, d, v;
	static int R;

	h = pixel_x & 7;
	d = pixel_y & 7;

	if (d == 0) {
		gen_frame(buf, gfx, h, 1);  /* content first */
		R = pixel_y >> 3;
		v = 0;
		sp1_MoveSprAbs(s, &maze_clip, buf,
			MAZE_R0 + R, MAZE_C0 + (pixel_x >> 3), v, h);
	} else {
		gen_frame(buf, gfx, h, 0);  /* transparent first */
		R = pixel_y >> 3;
		v = d;
		/* frame pointer at buf+16 (content start) so SP1's
		   backward read (frame - 2*vrot) lands in the
		   transparent padding at buf[0..15] */
		sp1_MoveSprAbs(s, &maze_clip, buf + 16,
			MAZE_R0 + R, MAZE_C0 + (pixel_x >> 3), v, h);
	}
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
unsigned char maze_attr_at(unsigned char r, unsigned char c);

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
				sp1_PrintAtInv(sr, sc, CORR_ATTR, 'F');
		}
	}
}

void draw_dot(unsigned char gx, unsigned char gy)
{
	ppx = gx * 8;
	ppy = gy * 8;
	render_spr_pix(spr_player, framebuf_player, gfx_player, ppx, ppy);
}

void draw_exit(unsigned char gx, unsigned char gy)
{
	sp1_MoveSprAbs(spr_exit_s, &maze_clip, sp1_exit_gfx, SROW(gy), SCOL(gx), 0, 0);
}

void snd_step()
{
	bit_beep(1, 400);
	intrinsic_ei();
}

void snd_bump()
{
	bit_beep(1, 800);
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
	bit_beep(1, 200);
	bit_beep(1, 100);
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

void snd_gun_pickup()
{
	bit_beep(1, 150);
	bit_beep(1, 100);
	bit_beep(1, 50);
	intrinsic_ei();
}

void snd_shot()
{
	bit_beep(1, 30);
	bit_beep(1, 60);
	bit_beep(1, 90);
	intrinsic_ei();
}

void snd_shot_hit()
{
	bit_beep(10, 100);
	intrinsic_ei();
	bit_beep(10, 60);
	intrinsic_ei();
}

/* Wait for all keys released, then wait for any key press. */
void wait_any_key()
{
	in_WaitForNoKey();
	in_WaitForKey();
}

/* Wait until all keys are released */
void wait_key_release()
{
	in_WaitForNoKey();
}

void draw_enemy_n(unsigned char n, unsigned char gx, unsigned char gy)
{
	static unsigned char sn;
	sn = n;
	epx[sn] = gx * 8;
	epy[sn] = gy * 8;
	render_spr_pix(spr_enemies[sn], framebuf_enemies[sn], gfx_enemy,
	               epx[sn], epy[sn]);
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

/* Place a gun pickup at a random corridor cell */
void place_gun()
{
	static unsigned char cx, cy, gx, gy, attempts, ei, skip;
	gun_placed = 0;
	has_gun = 0;
	attempts = 50;
	while (attempts > 0) {
		attempts--;
		cx = rand() % COLS;
		cy = rand() % ROWS;
		gx = (cx << 1) + 1;
		gy = (cy << 1) + 1;
		if (gx == px && gy == py) continue;
		if (gx == exit_gx && gy == exit_gy) continue;
		skip = 0;
		for (ei = 0; ei != num_enemies; ++ei)
			if (gx == enx[ei] && gy == eny[ei]) skip = 1;
		if (skip) continue;
		/* Avoid placing on a coin */
		if (coinmap[cy * COLS + cx]) continue;
		gun_gx = gx;
		gun_gy = gy;
		gun_placed = 1;
		sp1_PrintAtInv(SROW(gy), SCOL(gx), GUN_ATTR, 'G');
		return;
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
		sp1_PrintAtInv(SROW(gy), SCOL(gx), CORR_ATTR, 'F');
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

/* Show/hide GUN indicator on HUD */
void show_gun_hud()
{
	static unsigned char c;
	if (has_gun) {
		gotoxy(0, 0);
		printf("GUN");
		for (c = 0; c != 2; ++c)
			set_attr(0, c, GUN_ATTR);
	} else {
		gotoxy(0, 0);
		printf("   ");
		for (c = 0; c != 2; ++c)
			set_attr(0, c, INK_WHITE | PAPER_BLACK);
	}
}

/* Fire a shot in direction pdir. Traces through corridor cells,
   flashes them, kills first enemy hit. Consumes the gun. */
void fire_shot()
{
	static int gx, gy;
	static char ddx, ddy;
	static unsigned char hit_enemy, path_len, i;

	has_gun = 0;
	hit_enemy = 255;
	path_len = 0;

	if (pdir == 0) { ddx = -1; ddy = 0; }
	else if (pdir == 1) { ddx = 1; ddy = 0; }
	else if (pdir == 2) { ddx = 0; ddy = -1; }
	else { ddx = 0; ddy = 1; }

	/* Trace path, flash attrs, find enemy */
	gx = px; gy = py;
	while (path_len < 29) {
		gx += ddx; gy += ddy;
		if (gx < 0 || gx >= ECOLS || gy < 0 || gy >= EROWS) break;
		if (wallmap[erow_x_ecols[gy] + gx]) break;
		path_len++;
		set_attr(SROW(gy), SCOL(gx), SHOT_ATTR);
		/* Check for live enemy */
		{
			unsigned char ei;
			for (ei = 0; ei != num_enemies; ++ei) {
				if (!enemy_stun[ei] &&
				    enx[ei] == (unsigned char)gx &&
				    eny[ei] == (unsigned char)gy) {
					hit_enemy = ei;
				}
			}
		}
		if (hit_enemy != 255) break;
	}

	if (path_len == 0) {
		/* Facing a wall — shot wasted with a dud sound */
		bit_beep(1, 800); intrinsic_ei();
		show_gun_hud();
		return;
	}

	/* Zap sound */
	snd_shot();

	/* Pause to show the shot line */
	for (i = 0; i != 6; ++i) intrinsic_halt();

	/* Restore attrs along the same path */
	gx = px; gy = py;
	for (i = 0; i != path_len; ++i) {
		gx += ddx; gy += ddy;
		set_attr(SROW(gy), SCOL(gx),
		         maze_attr_at(SROW(gy), SCOL(gx)));
	}

	/* Stun enemy if hit */
	if (hit_enemy != 255) {
		enemy_stun[hit_enemy] = STUN_SECS;
		snd_shot_hit();
	}

	show_gun_hud();
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

/* Decide direction for enemy n. Does NOT move the enemy.
   Returns 0=left,1=right,2=up,3=down, -1=no valid direction. */
char decide_enemy_dir(uchar n) __z88dk_fastcall
{
	static char dir;
	static uchar old_ex, old_ey;
	static uchar sn;

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
	return dir;
}

/* Start enemy n's pixel-scrolling animation in given direction.
   Sets animation counter and first pixel advance. */
void start_enemy_move(unsigned char n, char dir)
{
	static unsigned char sn;
	sn = n;
	last_edir_arr[sn] = dir;
	edir_anim[sn] = dir;
	eanim[sn] = ANIM_FRAMES - 1;

	/* First pixel advance */
	if (dir == 0) epx[sn] -= MOVE_SPEED;
	else if (dir == 1) epx[sn] += MOVE_SPEED;
	else if (dir == 2) epy[sn] -= MOVE_SPEED;
	else epy[sn] += MOVE_SPEED;
}

/* Advance enemy n's animation by one frame.
   When animation completes, updates grid position and checks coins. */
void advance_enemy_anim(unsigned char n)
{
	static unsigned char sn, d;
	sn = n;
	d = edir_anim[sn];

	/* Advance pixel position */
	if (d == 0) epx[sn] -= MOVE_SPEED;
	else if (d == 1) epx[sn] += MOVE_SPEED;
	else if (d == 2) epy[sn] -= MOVE_SPEED;
	else epy[sn] += MOVE_SPEED;

	eanim[sn]--;
	if (eanim[sn] == 0) {
		/* Animation complete — update grid position */
		if (d == 0) enx[sn]--;
		else if (d == 1) enx[sn]++;
		else if (d == 2) eny[sn]--;
		else eny[sn]++;

		/* Snap pixel position */
		epx[sn] = enx[sn] * 8;
		epy[sn] = eny[sn] * 8;

		/* Enemy eats coin if it lands on one */
		if ((enx[sn] & 1) && (eny[sn] & 1)) {
			unsigned char ccx, ccy;
			ccx = enx[sn] >> 1;
			ccy = eny[sn] >> 1;
			if (coinmap[ccy * COLS + ccx]) {
				coinmap[ccy * COLS + ccx] = 0;
				coins_left--;
				sp1_PrintAtInv(SROW(eny[sn]), SCOL(enx[sn]), CORR_ATTR, 'F');
			}
		}
	}
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

/* Return the correct maze attribute for screen cell (r, c).
   Used by restore_attr_ring to repair cells after the zoom effect. */
unsigned char maze_attr_at(unsigned char r, unsigned char c)
{
	static unsigned char gy, gx;
	if (r < MAZE_R0 || r >= MAZE_R0 + EROWS) return INK_WHITE | PAPER_BLACK;
	if (c < MAZE_C0 || c >= MAZE_C0 + ECOLS)  return INK_WHITE | PAPER_BLACK;
	gy = r - MAZE_R0;
	gx = c - MAZE_C0;
	if (wallmap[(unsigned int)gy * ECOLS + gx]) return WALL_ATTR;
	/* Entity positions — checked before coins so sprites stay coloured */
	if (gx == exit_gx && gy == exit_gy) return EXIT_ATTR;
	if (gx == px && gy == py) return PLAYER_ATTR;
	{
		static unsigned char ei;
		for (ei = 0; ei != num_enemies; ++ei)
			if (gx == enx[ei] && gy == eny[ei])
				return (ei == 0) ? ENEMY_ATTR  : (ei == 1) ? ENEMY2_ATTR :
				       (ei == 2) ? ENEMY3_ATTR : ENEMY4_ATTR;
	}
	/* Gun pickup */
	if (gun_placed && gx == gun_gx && gy == gun_gy) return GUN_ATTR;
	/* Coin cells sit at odd expanded-grid positions (maze cell centres) */
	if ((gx & 1) && (gy & 1) && coinmap[(unsigned int)(gy >> 1) * COLS + (gx >> 1)])
		return COIN_ATTR;
	return CORR_ATTR;
}

/* Paint the outline (border only) of a rectangle centred on (sr,sc)
   with the given attribute.  Radius is half-size in character cells. */
void draw_attr_ring(unsigned char sr, unsigned char sc,
                    unsigned char radius, unsigned char attr)
{
	static int r, c, r1, c1, r2, c2;
	r1 = (int)sr - radius;  r2 = (int)sr + radius;
	c1 = (int)sc - radius;  c2 = (int)sc + radius;
	if (r1 < 0)  r1 = 0;   if (r2 > 23) r2 = 23;
	if (c1 < 0)  c1 = 0;   if (c2 > 31) c2 = 31;
	for (c = c1; c <= c2; ++c) {
		set_attr(r1, c, attr);
		if (r2 != r1) set_attr(r2, c, attr);
	}
	for (r = r1 + 1; r < r2; ++r) {
		set_attr(r, c1, attr);
		if (c2 != c1) set_attr(r, c2, attr);
	}
}

/* Restore the outline painted by draw_attr_ring back to maze colours. */
void restore_attr_ring(unsigned char sr, unsigned char sc, unsigned char radius)
{
	static int r, c, r1, c1, r2, c2;
	r1 = (int)sr - radius;  r2 = (int)sr + radius;
	c1 = (int)sc - radius;  c2 = (int)sc + radius;
	if (r1 < 0)  r1 = 0;   if (r2 > 23) r2 = 23;
	if (c1 < 0)  c1 = 0;   if (c2 > 31) c2 = 31;
	for (c = c1; c <= c2; ++c) {
		set_attr(r1, c, maze_attr_at(r1, c));
		if (r2 != r1) set_attr(r2, c, maze_attr_at(r2, c));
	}
	for (r = r1 + 1; r < r2; ++r) {
		set_attr(r, c1, maze_attr_at(r, c1));
		if (c2 != c1) set_attr(r, c2, maze_attr_at(r, c2));
	}
}

/* Level introduction:
   1. Enemies revealed one at a time with a shrinking-rectangle zoom effect
      and a fat bass thump for each.
   2. Exit revealed with a chime.
   3. Enemies revealed one at a time with a fat bass thump.
   4. Player revealed with an ascending fanfare.
   5. READY-STEADY-GO before control is handed to the player. */
void level_intro()
{
	static unsigned char ei, sr, sc, radius, ea;
	static int f;
	static unsigned char len, c;

	/* Hide all moving sprites — revealed one by one below */
	sp1_MoveSprAbs(spr_player, &maze_clip, 0, 25, 0, 0, 0);
	sp1_MoveSprAbs(spr_exit_s, &maze_clip, 0, 25, 0, 0, 0);
	for (ei = 0; ei != 4; ++ei)
		sp1_MoveSprAbs(spr_enemies[ei], &maze_clip, 0, 25, 0, 0, 0);
	sp1_UpdateNow();

	/* ── Present the exit ── */
	sr = MAZE_R0 + exit_gy;
	sc = MAZE_C0 + exit_gx;

	/* Chime: three rising notes */
	bit_beep(6, 400); intrinsic_ei();
	bit_beep(6, 280); intrinsic_ei();
	bit_beep(10, 180); intrinsic_ei();

	for (radius = 7; radius != 0; --radius) {
		draw_attr_ring(sr, sc, radius, EXIT_ATTR);
		for (f = 0; f != 2; ++f) intrinsic_halt();
		restore_attr_ring(sr, sc, radius);
	}

	set_attr(sr, sc, EXIT_ATTR);
	draw_exit(exit_gx, exit_gy);
	sp1_UpdateNow();

	for (f = 0; f != 20; ++f) intrinsic_halt();

	/* ── Present each enemy ── */
	for (ei = 0; ei != num_enemies; ++ei) {
		sr = MAZE_R0 + eny[ei];
		sc = MAZE_C0 + enx[ei];
		ea = (ei == 0) ? ENEMY_ATTR  : (ei == 1) ? ENEMY2_ATTR :
		     (ei == 2) ? ENEMY3_ATTR : ENEMY4_ATTR;

		/* Fat bass thump */
		bit_beep(30, 1500); intrinsic_ei();
		bit_beep(12, 900);  intrinsic_ei();

		/* Zoom-in: large ring shrinks down to the entity cell */
		for (radius = 7; radius != 0; --radius) {
			draw_attr_ring(sr, sc, radius, ea);
			for (f = 0; f != 2; ++f) intrinsic_halt();
			restore_attr_ring(sr, sc, radius);
		}

		/* Flash the entity cell then reveal the sprite */
		set_attr(sr, sc, ea);
		draw_enemy_n(ei, enx[ei], eny[ei]);
		sp1_UpdateNow();

		for (f = 0; f != 20; ++f) intrinsic_halt();
	}

	/* ── Present the player ── */
	sr = MAZE_R0 + py;
	sc = MAZE_C0 + px;

	/* Ascending three-note fanfare */
	bit_beep(8, 500); intrinsic_ei();
	bit_beep(8, 350); intrinsic_ei();
	bit_beep(12, 200); intrinsic_ei();

	for (radius = 7; radius != 0; --radius) {
		draw_attr_ring(sr, sc, radius, PLAYER_ATTR);
		for (f = 0; f != 2; ++f) intrinsic_halt();
		restore_attr_ring(sr, sc, radius);
	}

	set_attr(sr, sc, PLAYER_ATTR);
	draw_dot(px, py);
	sp1_UpdateNow();

	for (f = 0; f != 25; ++f) intrinsic_halt();

	/* ── Present the gun ── */
	if (gun_placed) {
		sr = MAZE_R0 + gun_gy;
		sc = MAZE_C0 + gun_gx;

		/* Pickup chime: two quick high notes */
		bit_beep(4, 200); intrinsic_ei();
		bit_beep(6, 120); intrinsic_ei();

		for (radius = 7; radius != 0; --radius) {
			draw_attr_ring(sr, sc, radius, GUN_ATTR);
			for (f = 0; f != 2; ++f) intrinsic_halt();
			restore_attr_ring(sr, sc, radius);
		}

		set_attr(sr, sc, GUN_ATTR);
		sp1_UpdateNow();

		for (f = 0; f != 20; ++f) intrinsic_halt();
	}

	/* ── Countdown READY-STEADY-GO ── */
	len = sprintf(txt_buffer, "  READY  ");
	gotoxy(center_x(len), 21); printf(txt_buffer);
	zx_border(INK_RED);
	bit_beep(4, 400); intrinsic_ei();
	for (f = 0; f != 30; ++f) intrinsic_halt();

	len = sprintf(txt_buffer, " STEADY  ");
	gotoxy(center_x(len), 21); printf(txt_buffer);
	zx_border(INK_YELLOW);
	bit_beep(4, 300); intrinsic_ei();
	for (f = 0; f != 30; ++f) intrinsic_halt();

	len = sprintf(txt_buffer, "** GO! **");
	gotoxy(center_x(len), 21); printf(txt_buffer);
	zx_border(INK_GREEN);
	bit_beep(6, 150); intrinsic_ei();
	bit_beep(6, 100); intrinsic_ei();
	for (f = 0; f != 15; ++f) intrinsic_halt();

	/* Clear countdown row and restore its attributes */
	gotoxy(0, 21);
	printf("                                                                ");
	for (c = 0; c != 32; ++c)
		set_attr(21, c, INK_WHITE | PAPER_BLACK);
	zx_border(INK_BLACK);
}

/* Frame-based timing (1 frame = 20ms at 50Hz) */
#define KEY_REPEAT    4   /* held key repeats every 4 frames = 80ms */

main()
{
	static char k;
	static char dx, dy;
	static unsigned char caught;
	static char rank;
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
		CORR_ATTR, 'F');
	sp1_TileEntry('B', brick);
	sp1_TileEntry('C', spr_coin);
	sp1_TileEntry('F', floor_tile);
	sp1_TileEntry('G', spr_gun);

	/* Create SP1 sprites for pixel scrolling.
	   Moving sprites: 2-column MASK2NR, height=2, with pre-shifted frames.
	   Col 0 = shifted graphic, col 1 = horizontal overflow (offset 46).
	   xthresh=1: col 1 only drawn when hrot>=1 (no overflow at hrot=0).
	   ythresh=0: always draw row 1 (content lives there at vrot=0). */
	spr_player = sp1_CreateSpr(SP1_DRAW_MASK2NR, SP1_TYPE_2BYTE, 2, 0, 0);
	sp1_AddColSpr(spr_player, SP1_DRAW_MASK2NR, 0, 46, 0);
	spr_player->xthresh = 1;
	spr_player->ythresh = 1;
	sp1_set_spr_colour(spr_player, PLAYER_ATTR);

	/* Exit sprite: single-column, stays cell-aligned (no pixel scrolling) */
	spr_exit_s = sp1_CreateSpr(SP1_DRAW_MASK2NR, SP1_TYPE_2BYTE, 2, 0, 1);
	spr_exit_s->xthresh = 0;
	spr_exit_s->ythresh = 1;
	sp1_set_spr_colour(spr_exit_s, EXIT_ATTR);

	{
		static unsigned char ei;
		static unsigned char ea;
		for (ei = 0; ei != 4; ++ei) {
			spr_enemies[ei] = sp1_CreateSpr(SP1_DRAW_MASK2NR, SP1_TYPE_2BYTE, 2, 0, 2);
			sp1_AddColSpr(spr_enemies[ei], SP1_DRAW_MASK2NR, 0, 46, 2);
			spr_enemies[ei]->xthresh = 1;
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

	/* Configure keyboard joystick: O=left, P=right, Q=up, A=down */
	udk.left  = in_LookupKey('o');
	udk.right = in_LookupKey('p');
	udk.up    = in_LookupKey('q');
	udk.down  = in_LookupKey('a');
	udk.fire  = in_LookupKey(' ');

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
			k = in_Inkey();
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
		enemy_next = 0;
		enemy_accum = 0;
		timer_sec = time_limit;
		timer_frac = 50;  /* 50 frames = 1 second at 50Hz */
		panim = 0;
		pdir = 1;  /* default facing right */
		{
			unsigned char ei;
			for (ei = 0; ei != num_enemies; ++ei) {
				last_edir_arr[ei] = 0;
				eanim[ei] = 0;
				enemy_stun[ei] = 0;
			}
		}

		/* Place coins, gun, and draw everything */
		place_coins();
		place_gun();
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
		show_gun_hud();
		level_intro();

		while (1) {
			intrinsic_halt();  /* sync to 50Hz frame (IM2 null ISR — safe) */

			/* --- Player pixel-scrolling movement --- */
			if (panim > 0) {
				/* Continue animation */
				if (pdir == 0) ppx -= MOVE_SPEED;
				else if (pdir == 1) ppx += MOVE_SPEED;
				else if (pdir == 2) ppy -= MOVE_SPEED;
				else ppy += MOVE_SPEED;

				panim--;

				/* Corner tolerance: when 3px from destination,
				   allow perpendicular turn by snapping early */
				if (panim == 2) {
					k = in_JoyKeyboard(&udk);
					if (k) {
						dx = 0; dy = 0;
						if (k & in_LEFT)  dx = -1;
						if (k & in_RIGHT) dx = 1;
						if (k & in_UP)    dy = -1;
						if (k & in_DOWN)  dy = 1;
						if (dx && dy) dy = 0;
						/* Check perpendicular input only */
						if ((pdir <= 1 && dy && !dx) ||
						    (pdir >= 2 && dx && !dy)) {
							/* Compute destination grid pos */
							static char dest_px, dest_py;
							if (pdir == 0) dest_px = px - 1;
							else if (pdir == 1) dest_px = px + 1;
							else dest_px = px;
							if (pdir == 2) dest_py = py - 1;
							else if (pdir == 3) dest_py = py + 1;
							else dest_py = py;
							/* Check if turn is valid from dest */
							if (!wallmap[erow_x_ecols[dest_py + dy] + dest_px + dx]) {
								/* Snap to destination, start turn */
								px = dest_px;
								py = dest_py;
								ppx = px * 8;
								ppy = py * 8;
								/* Start perpendicular move */
								if (dx == -1) pdir = 0;
								else if (dx == 1) pdir = 1;
								else if (dy == -1) pdir = 2;
								else pdir = 3;
								panim = ANIM_FRAMES - 1;
								if (pdir == 0) ppx -= MOVE_SPEED;
								else if (pdir == 1) ppx += MOVE_SPEED;
								else if (pdir == 2) ppy -= MOVE_SPEED;
								else ppy += MOVE_SPEED;
								snd_step();
								goto snap_done;
							}
						}
					}
				}

				if (panim == 0) {
					/* Animation complete — update grid position */
					if (pdir == 0) px--;
					else if (pdir == 1) px++;
					else if (pdir == 2) py--;
					else py++;
					ppx = px * 8;
					ppy = py * 8;

				snap_done:
					/* Collect coin? */
					if (try_collect_coin(px, py)) {
						zx_border(INK_YELLOW);
						snd_coin();
						zx_border(INK_BLACK);
						show_score();
					}

					/* Pick up gun? */
					if (gun_placed && px == gun_gx && py == gun_gy) {
						gun_placed = 0;
						has_gun = 1;
						sp1_PrintAtInv(SROW(gun_gy), SCOL(gun_gx),
						               CORR_ATTR, 'F');
						zx_border(INK_CYAN);
						snd_gun_pickup();
						zx_border(INK_BLACK);
						show_gun_hud();
					}

					/* Player walked onto enemy? */
					{
						unsigned char ei;
						unsigned char dest_x, dest_y;
						for (ei = 0; ei != num_enemies; ++ei) {
							if (enemy_stun[ei]) continue;
							if (eanim[ei] == 0) {
								/* Enemy stationary -- direct check */
								if (px == enx[ei] && py == eny[ei])
									caught = 1;
							} else {
								/* Enemy mid-animation: enx/eny is the departure
								   cell (not yet updated). Check destination. */
								dest_x = enx[ei];
								dest_y = eny[ei];
								if (edir_anim[ei] == 0) dest_x--;
								else if (edir_anim[ei] == 1) dest_x++;
								else if (edir_anim[ei] == 2) dest_y--;
								else dest_y++;
								if (px == dest_x && py == dest_y)
									caught = 1;
							}
						}
					}

					if (px == exit_gx && py == exit_gy) {
						score += 50 + timer_sec;
						render_spr_pix(spr_player, framebuf_player,
						               gfx_player, ppx, ppy);
						sp1_UpdateNow();
						win_cut_scene();
						wait_any_key();
						break;
					}
				}
			}

			if (panim == 0) {
				/* Accept new input */
				dx = 0; dy = 0;
				k = in_JoyKeyboard(&udk);
				/* Fire gun: space bar, takes priority over movement */
				if (has_gun && (k & in_FIRE)) {
					fire_shot();
				} else {
				if (k) {
					if (k & in_LEFT)  dx = -1;
					if (k & in_RIGHT) dx = 1;
					if (k & in_UP)    dy = -1;
					if (k & in_DOWN)  dy = 1;
				}
				/* Only one axis at a time — prevent diagonal wall skip */
				if (dx && dy) dy = 0;
				if ((dx || dy) && can_move(dx, dy)) {
					if (dx == -1) pdir = 0;
					else if (dx == 1) pdir = 1;
					else if (dy == -1) pdir = 2;
					else pdir = 3;
					panim = ANIM_FRAMES - 1;
					/* First pixel advance */
					if (pdir == 0) ppx -= MOVE_SPEED;
					else if (pdir == 1) ppx += MOVE_SPEED;
					else if (pdir == 2) ppy -= MOVE_SPEED;
					else ppy += MOVE_SPEED;
					snd_step();
				} else if (dx || dy) {
					snd_bump();
				}
				} /* end else (not firing) */
			}

			/* Render player at current pixel position */
			render_spr_pix(spr_player, framebuf_player,
			               gfx_player, ppx, ppy);

			/* --- Enemy moves: decide + animate --- */
			{
				static unsigned char ei;
				static char edir;

				/* Trigger new enemy moves via Bresenham accumulator */
				enemy_accum += num_enemies;
				while (enemy_accum >= enemy_frames) {
					enemy_accum -= enemy_frames;
					if (eanim[enemy_next] == 0) {
						edir = decide_enemy_dir(enemy_next);
						if (edir >= 0 && !enemy_stun[enemy_next])
							start_enemy_move(enemy_next, edir);
					}
					enemy_next++;
					if (enemy_next >= num_enemies)
						enemy_next = 0;
				}

				/* Advance all enemy animations and render */
				for (ei = 0; ei != num_enemies; ++ei) {
					if (!enemy_stun[ei] && eanim[ei] > 0) {
						advance_enemy_anim(ei);
						/* Check collision when animation completes */
						if (eanim[ei] == 0 &&
						    enx[ei] == px && eny[ei] == py)
							caught = 1;
					}
					render_spr_pix(spr_enemies[ei],
					               framebuf_enemies[ei],
					               gfx_enemy, epx[ei], epy[ei]);
				}
			}

			/* --- Render all SP1 changes this frame --- */
			sp1_UpdateNow();

			/* --- Timer countdown --- */
			timer_frac--;
			if (timer_frac == 0) {
				timer_frac = 50;
				/* Decrement stun timers once per second */
				{
					unsigned char ei;
					for (ei = 0; ei != num_enemies; ++ei)
						if (enemy_stun[ei] > 0) enemy_stun[ei]--;
				}
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
