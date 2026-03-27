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

#pragma output CRT_ORG_CODE = 0x5E00
#pragma output STACKPTR=0xD000
#pragma define CLIB_EXIT_STACK_SIZE=0

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
unsigned char pwalk;             /* walk phase: 0=stand, 1=walk */
unsigned char eanim[4];          /* enemy animation counters */
unsigned char edir_anim[4];     /* enemy animation directions */
unsigned char ewalk[4];         /* enemy walk phase: 0/1 */

#define MOVE_SPEED 2
#define ANIM_FRAMES 4

/* Frame buffers for pre-shifted 2-column sprites.
   Layout per buffer: 46 bytes col0 + 46 bytes col1 = 92 bytes.
   Each column: 8 transparent + 8 content + 7 padding = 23 lines × 2 bytes.
   Placed at fixed addresses in upper memory (above SP1 heap usage at ~$F400)
   to keep BSS below $D000. */
#define FRAMEBUF_BASE 0xFD34u
#define framebuf_player ((unsigned char *)FRAMEBUF_BASE)
unsigned char *framebuf_enemies[4] = {
	(unsigned char *)(FRAMEBUF_BASE + 92),
	(unsigned char *)(FRAMEBUF_BASE + 184),
	(unsigned char *)(FRAMEBUF_BASE + 276),
	(unsigned char *)(FRAMEBUF_BASE + 368)
};
unsigned int rseed;
/* User-defined keys for in_JoyKeyboard() — OPQA directions */
struct in_UDK udk;
unsigned char joy_type;    /* 0=keyboard, 1=kempston, 2=sinclair */
unsigned char menu_cursor; /* 0-3=difficulty */
unsigned char diff_cursor; /* 0-3: last selected difficulty item */

/* Gem map: 1=gem present at maze cell (cx,cy). Index = cy*COLS+cx */
unsigned char gemmap[ROWS * COLS];
uint score;
unsigned char gems_left;
unsigned char gems_collected;
unsigned char gems_needed;   /* min gems to open exit */
unsigned char total_gems;
unsigned char exit_open;      /* 1 = exit unlocked */
unsigned char hud_dirty;      /* 1 = redraw gems/score HUD after sp1_UpdateNow */
unsigned char level;
unsigned char difficulty;    /* 1=Easy, 2=Normal, 3=Hard, 4=Nightmare */
unsigned char demo_mode;
unsigned char num_enemies;   /* 1, 2, 3, or 4 */
unsigned char enemy_frames;  /* frames between enemy moves */
unsigned char chase_pct;     /* % chance enemy uses BFS chase */
unsigned char extra_wall_pct; /* 1-in-N chance to remove a wall */
unsigned char extra_halls_base; /* base number of extra halls */
unsigned char extra_halls_rng;  /* random additional halls */
unsigned char time_limit;       /* seconds per level for this difficulty */
unsigned char base_enemies;     /* starting num_enemies for chosen difficulty */
unsigned char base_frames;      /* starting enemy_frames for chosen difficulty */
unsigned char base_chase;       /* starting chase_pct for chosen difficulty */
unsigned char base_time;        /* starting time_limit for chosen difficulty */

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

/* Forward declarations */
void show_gems_hud();
void open_exit_gate();

/* SP1 sprite handles */
struct sp1_ss *spr_player;
struct sp1_ss *spr_enemies[4];

/* SP1 clipping rect for maze area — expanded for pixel scrolling overflow */
struct sp1_Rect maze_clip = {MAZE_R0 - 1, MAZE_C0, ECOLS + 1, EROWS + 2};
struct sp1_Rect full_screen = {0, 0, 32, 24};

/* SP1 colour callback globals */
unsigned char sp1_colour;
unsigned char sp1_cmask;
unsigned char sp1_colour_ht;  /* chars per column (= sprite height) */

void colourSpr(unsigned int count, struct sp1_cs *c)
{
	if (count < sp1_colour_ht) {
		/* Main column: set sprite colour */
		c->attr_mask = sp1_cmask;
		c->attr = sp1_colour;
	} else {
		/* Overflow column: attribute-transparent —
		   prevents colour bleed into adjacent cells */
		c->attr_mask = 0xFF;
		c->attr = 0x00;
	}
}

/* BFS lookup tables: precomputed row/col from linear index,
   eliminates expensive Z80 division by 14 in the BFS inner loop */
unsigned char bfs_row[ROWS * COLS];
unsigned char bfs_col[ROWS * COLS];
/* Precomputed row * COLS: eliminates Z80 multiply by 14 */
unsigned char row_x_cols[ROWS];
/* Precomputed row * ECOLS: eliminates Z80 multiply by 29 */
unsigned int erow_x_ecols[EROWS];

/* Precomputed adjacency for old BFS: 4 neighbors per cell (L,R,U,D).
   504 bytes at $F600-$F7F7. Still used by demo AI. */
#define adj ((unsigned char *)0xF600u)

/* Full expanded-grid BFS arrays in upper memory.
   vis: 551 bytes at $F800-$FA26.
   stk: 551 uint entries at $FA28-$FE75 (overlaps frame bufs $FD34+,
        safe because BFS completes before rendering). */
#define fbfs_vis ((unsigned char *)0xF800u)
#define fbfs_stk ((unsigned int *)0xFA28u)

unsigned char nav_valid;  /* 0=need BFS, 1-4=cached dir+1 */

/* Transient globals for inline-asm BFS loop */
unsigned char bfs_efi_g;
unsigned char bfs_head_g;
unsigned char bfs_tail_g;
unsigned char bfs_result_g;
unsigned int bfs_adj_ptr_g;
unsigned char bfs_mode_g;  /* 0=fixed direction (enemy), 1=propagate (demo) */
unsigned char demo_target;   /* sticky gem target cell (255=none) */

#define ATTR_P_ADDR 23693
#define WALL_ATTR   (BRIGHT | INK_RED | PAPER_YELLOW)
#define CORR_ATTR   (INK_WHITE | PAPER_BLACK)
#define PLAYER_ATTR (BRIGHT | INK_GREEN | PAPER_BLACK)
#define ENEMY_ATTR  (BRIGHT | INK_RED | PAPER_BLACK)
#define ENEMY2_ATTR (BRIGHT | INK_MAGENTA | PAPER_BLACK)
#define ENEMY3_ATTR (BRIGHT | INK_CYAN | PAPER_BLACK)
#define ENEMY4_ATTR (BRIGHT | INK_WHITE | PAPER_BLACK)
#define EXIT_ATTR   (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define EXIT_LOCKED_ATTR (INK_RED | PAPER_BLACK)
#define GEM_ATTR   (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define TITLE_ATTR  (BRIGHT | INK_YELLOW | PAPER_BLUE)
#define WIN_ATTR    (BRIGHT | INK_GREEN | PAPER_BLACK)
#define HISCORE_ATTR (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define TIMER_ATTR   (BRIGHT | INK_WHITE | PAPER_BLACK)
#define TIMER_WARN_ATTR (BRIGHT | INK_RED | PAPER_BLACK)
#define GUN_ATTR    (BRIGHT | INK_CYAN | PAPER_BLACK)
#define SHOT_ATTR   (BRIGHT | INK_CYAN | PAPER_CYAN)

/* Direction delta lookup tables: 0=left, 1=right, 2=up, 3=down */
const char dir_dx[] = {-1, 1, 0, 0};
const char dir_dy[] = {0, 0, -1, 1};
/* Pixel deltas (dir_d * MOVE_SPEED) to avoid multiply */
const char dir_dpx[] = {-MOVE_SPEED, MOVE_SPEED, 0, 0};
const char dir_dpy[] = {0, 0, -MOVE_SPEED, MOVE_SPEED};
/* Enemy attribute lookup by index */
const unsigned char enemy_attrs[] = {ENEMY_ATTR, ENEMY2_ATTR, ENEMY3_ATTR, ENEMY4_ATTR};

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

const unsigned char spr_gem[8] = {
	0b00000000,
	0b00111100,
	0b01101110,
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
/* Player man sprites: 6 variants (front/right/left × stand/walk) */
const unsigned char gfx_man_front1[8] = {0x18,0x18,0x3C,0x18,0x18,0x18,0x24,0x00}; /* front stand */
const unsigned char gfx_man_front2[8] = {0x18,0x18,0x3C,0x18,0x18,0x24,0x24,0x00}; /* front walk */
const unsigned char gfx_man_right1[8] = {0x18,0x18,0x1E,0x30,0x18,0x18,0x24,0x00}; /* right stand */
const unsigned char gfx_man_right2[8] = {0x18,0x18,0x1E,0x30,0x18,0x24,0x42,0x00}; /* right walk */
const unsigned char gfx_man_left1[8]  = {0x18,0x18,0x78,0x0C,0x18,0x18,0x24,0x00}; /* left stand */
const unsigned char gfx_man_left2[8]  = {0x18,0x18,0x78,0x0C,0x18,0x24,0x42,0x00}; /* left walk */

const unsigned char msk_man_front1[8] = {0x3C,0x7E,0x7E,0x7E,0x3C,0x7E,0x7E,0x7E};
const unsigned char msk_man_front2[8] = {0x3C,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E};
const unsigned char msk_man_right1[8] = {0x3C,0x3F,0x7F,0x7F,0x7C,0x7E,0x7E,0x7E};
const unsigned char msk_man_right2[8] = {0x3C,0x3F,0x7F,0x7F,0x7E,0xFF,0xFF,0xE7};
const unsigned char msk_man_left1[8]  = {0x3C,0xFC,0xFE,0xFE,0x3E,0x7E,0x7E,0x7E};
const unsigned char msk_man_left2[8]  = {0x3C,0xFC,0xFE,0xFE,0x7E,0xFF,0xFF,0xE7};

/* Current player sprite pointers — set by update_player_spr() */
const unsigned char *cur_pgfx;
const unsigned char *cur_pmsk;

void update_player_spr()
{
	if (pdir == 0) {
		if (pwalk) { cur_pgfx = gfx_man_left2;  cur_pmsk = msk_man_left2; }
		else       { cur_pgfx = gfx_man_left1;  cur_pmsk = msk_man_left1; }
	} else if (pdir == 1) {
		if (pwalk) { cur_pgfx = gfx_man_right2; cur_pmsk = msk_man_right2; }
		else       { cur_pgfx = gfx_man_right1; cur_pmsk = msk_man_right1; }
	} else {
		if (pwalk) { cur_pgfx = gfx_man_front2; cur_pmsk = msk_man_front2; }
		else       { cur_pgfx = gfx_man_front1; cur_pmsk = msk_man_front1; }
	}
}

void update_enemy_spr(unsigned char ei) __z88dk_fastcall
{
	unsigned char d, w;
	d = last_edir_arr[ei];
	w = ewalk[ei];
	if (d == 0) {
		if (w) { cur_pgfx = gfx_man_left2;  cur_pmsk = msk_man_left2; }
		else   { cur_pgfx = gfx_man_left1;  cur_pmsk = msk_man_left1; }
	} else if (d == 1) {
		if (w) { cur_pgfx = gfx_man_right2; cur_pmsk = msk_man_right2; }
		else   { cur_pgfx = gfx_man_right1; cur_pmsk = msk_man_right1; }
	} else {
		if (w) { cur_pgfx = gfx_man_front2; cur_pmsk = msk_man_front2; }
		else   { cur_pgfx = gfx_man_front1; cur_pmsk = msk_man_front1; }
	}
}

const unsigned char gfx_exit_tile[8] = {0x00, 0x00, 0x44, 0x28, 0x10, 0x28, 0x44, 0x00};

/* Expanded masks (kept for tile entry) */
const unsigned char msk_enemy[8]  = {0x38, 0x7C, 0xFE, 0xFE, 0xFE, 0x7C, 0x38, 0x00};


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
	sp1_colour_ht = 4;  /* all cells in 2-col height-2 sprite */
	sp1_IterateSprChar(s, colourSpr);
}

/* Generate pre-shifted frame buffer for a 2-column MASK2NR sprite.
   buf: 92-byte output buffer (46 col0 + 46 col1).
   gfx: 8-byte source graphic.
   h: horizontal shift amount (0-7).
   cell_aligned: 1 = content first (for vrot=0), 0 = transparent first (for vrot>0).
   Mask = ~graphic for transparent sprite edges. */
void gen_frame(unsigned char *buf, const unsigned char *gfx,
               const unsigned char *msk,
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
		*p++ = ~(msk[i] >> h);
		*p++ = g0;
	}
	for (i = 0; i != (cell_aligned ? 15 : 7); ++i) { *p++ = 0xFF; *p++ = 0x00; }

	/* Column 1 */
	if (!cell_aligned)
		for (i = 0; i != 8; ++i) { *p++ = 0xFF; *p++ = 0x00; }
	for (i = 0; i != 8; ++i) {
		g = gfx[i];
		g1 = (g << shl) & 0xFF;
		*p++ = ~((msk[i] << shl) & 0xFF);
		*p++ = g1;
	}
	for (i = 0; i != (cell_aligned ? 15 : 7); ++i) { *p++ = 0xFF; *p++ = 0x00; }
}

/* Render a pixel-scrolled sprite at pixel position (pixel_x, pixel_y)
   relative to the maze origin. Uses vrot for vertical sub-cell offset
   and pre-shifted frames for horizontal. */
void render_spr_pix(struct sp1_ss *s, unsigned char *buf,
                    const unsigned char *gfx, const unsigned char *msk,
                    unsigned char pixel_x, unsigned char pixel_y)
{
	static unsigned char h, d, v;
	static int R;

	h = pixel_x & 7;
	d = pixel_y & 7;

	if (d == 0) {
		gen_frame(buf, gfx, msk, h, 1);  /* content first */
		R = pixel_y >> 3;
		v = 0;
		sp1_MoveSprAbs(s, &maze_clip, buf,
			MAZE_R0 + R, MAZE_C0 + (pixel_x >> 3), v, h);
	} else {
		gen_frame(buf, gfx, msk, h, 0);  /* transparent first */
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
	update_player_spr();
	render_spr_pix(spr_player, framebuf_player, cur_pgfx, cur_pmsk, ppx, ppy);
}

void draw_exit(unsigned char gx, unsigned char gy)
{
	sp1_PrintAtInv(SROW(gy), SCOL(gx), exit_open ? EXIT_ATTR : EXIT_LOCKED_ATTR, 'X');
}

void snd_step()
{
	if (demo_mode) return;
	bit_beep(1, 400);
	intrinsic_ei();
}

void snd_bump()
{
	if (demo_mode) return;
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

void snd_gem()
{
	if (demo_mode) return;
	bit_beep(1, 200);
	bit_beep(1, 100);
	intrinsic_ei();
}

void snd_exit_open()
{
	if (demo_mode) return;
	bit_beep(5, 300);
	bit_beep(5, 200);
	bit_beep(10, 100);
	intrinsic_ei();
}

void snd_gems_lost()
{
	bit_beep(15, 800);
	intrinsic_ei();
	bit_beep(20, 1000);
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
	if (demo_mode) return;
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

/* Wait for all keys/buttons released, then wait for any key or joystick fire. */
void wait_any_key()
{
	/* Wait for everything released */
	while (in_Inkey())
		;
	if (joy_type == 1) {
		while ((unsigned char)in_JoyKempston() & in_FIRE)
			;
	} else if (joy_type == 2) {
		while ((unsigned char)in_JoySinclair1() & in_FIRE)
			;
	}
	/* Wait for any key or joystick fire */
	if (joy_type == 1) {
		while (!in_Inkey() &&
		       !((unsigned char)in_JoyKempston() & in_FIRE))
			;
	} else if (joy_type == 2) {
		while (!in_Inkey() &&
		       !((unsigned char)in_JoySinclair1() & in_FIRE))
			;
	} else {
		while (!in_Inkey())
			;
	}
}

/* Wait until all keys and joystick buttons are released */
void wait_key_release()
{
	while (in_Inkey())
		;
	if (joy_type == 1) {
		while ((unsigned char)in_JoyKempston() & in_FIRE)
			;
	} else if (joy_type == 2) {
		while ((unsigned char)in_JoySinclair1() & in_FIRE)
			;
	}
}

void draw_enemy_n(unsigned char n, unsigned char gx, unsigned char gy)
{
	static unsigned char sn;
	sn = n;
	epx[sn] = gx * 8;
	epy[sn] = gy * 8;
	update_enemy_spr(sn);
	render_spr_pix(spr_enemies[sn], framebuf_enemies[sn], cur_pgfx, cur_pmsk,
	               epx[sn], epy[sn]);
}

void draw_gem(unsigned char cx, unsigned char cy)
{
	unsigned char gx, gy;
	gx = (cx << 1) + 1;
	gy = (cy << 1) + 1;
	sp1_PrintAtInv(SROW(gy), SCOL(gx), GEM_ATTR, 'C');
}

/* Check if any enemy occupies expanded grid position (gx, gy) */
unsigned char is_enemy_at(unsigned char gx, unsigned char gy)
{
	static unsigned char ei;
	for (ei = 0; ei != num_enemies; ++ei)
		if (gx == enx[ei] && gy == eny[ei]) return 1;
	return 0;
}

/* Place exactly target gems on maze cells.
   Pass 1: spaced placement (no adjacent gems, avoid entities).
   Pass 2: fill remaining in any free corridor cell. */
void place_gems()
{
	static unsigned char target, idx, cx, cy, gx, gy;
	static int attempts;
	gems_left = 0;
	memset(gemmap, 0, ROWS * COLS);

#ifdef DEBUG_ONE_GEM
	/* Debug: single gem at bottom-right corner */
	idx = ROWS * COLS - 1;
	gemmap[idx] = 1;
	gems_left = 1;
	draw_gem(COLS - 1, ROWS - 1);
#else
	target = (ROWS * COLS) * 2 / 5;

	/* Pass 1: place with spacing constraint */
	attempts = 1000;
	while (gems_left < target && attempts > 0) {
		attempts--;
		idx = rand() % (ROWS * COLS);
		if (gemmap[idx]) continue;
		cy = idx / COLS;
		cx = idx - cy * COLS;
		gx = cx * 2 + 1;
		gy = cy * 2 + 1;
		if (gx == px && gy == py) continue;
		if (gx == exit_gx && gy == exit_gy) continue;
		if (is_enemy_at(gx, gy)) continue;
		if (cx > 0 && gemmap[idx - 1]) continue;
		if (cx < COLS - 1 && gemmap[idx + 1]) continue;
		if (cy > 0 && gemmap[idx - COLS]) continue;
		if (cy < ROWS - 1 && gemmap[idx + COLS]) continue;
		gemmap[idx] = 1;
		gems_left++;
		draw_gem(cx, cy);
	}

	/* Pass 2: relax spacing — fill any free corridor cell */
	attempts = 1000;
	while (gems_left < target && attempts > 0) {
		attempts--;
		idx = rand() % (ROWS * COLS);
		if (gemmap[idx]) continue;
		cy = idx / COLS;
		cx = idx - cy * COLS;
		gx = cx * 2 + 1;
		gy = cy * 2 + 1;
		if (gx == px && gy == py) continue;
		if (gx == exit_gx && gy == exit_gy) continue;
		if (is_enemy_at(gx, gy)) continue;
		gemmap[idx] = 1;
		gems_left++;
		draw_gem(cx, cy);
	}
#endif
}

/* Place a gun pickup at a random corridor cell */
void place_gun()
{
	static unsigned char cx, cy, gx, gy, attempts;
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
		if (is_enemy_at(gx, gy)) continue;
		/* Avoid placing on a gem */
		if (gemmap[cy * COLS + cx]) continue;
		gun_gx = gx;
		gun_gy = gy;
		gun_placed = 1;
		sp1_PrintAtInv(SROW(gy), SCOL(gx), GUN_ATTR, 'G');
		return;
	}
}

/* Check and collect gem at expanded grid position (gx,gy) */
unsigned char try_collect_gem(unsigned char gx, unsigned char gy)
{
	unsigned char cx, cy;
	if (!(gx & 1) || !(gy & 1)) return 0;  /* not a cell center */
	cx = gx >> 1;
	cy = gy >> 1;
	unsigned char idx=row_x_cols[cy] + cx;
	if (gemmap[idx]) {
		gemmap[idx] = 0;
		gems_left--;
		gems_collected++;
		score += 10;
		/* Remove gem tile — SP1 will restore corridor background */
		sp1_PrintAtInv(SROW(gy), SCOL(gx), CORR_ATTR, 'F');
		hud_dirty = 1;
		/* Check if we just hit the threshold to open the exit */
		if (!exit_open && gems_collected >= gems_needed)
			open_exit_gate();
		if (demo_mode) nav_valid = 0;  /* recalculate path */
		return 1;
	}
	return 0;
}

/* Set printf ink/paper from an attribute byte */
void set_print_attr(unsigned char a) __z88dk_fastcall
{
	zx_setink(a & 7);
	zx_setpaper((a >> 3) & 7);
	/* bright bit: poke ATTR_P directly */
	if (a & BRIGHT)
		*((unsigned char *)ATTR_P_ADDR) |= BRIGHT;
	else
		*((unsigned char *)ATTR_P_ADDR) &= ~BRIGHT;
}

/* Force all cells in a row to a given attribute */
void set_row_attr(unsigned char row, unsigned char attr)
{
	static unsigned char c;
	for (c = 0; c != 32; ++c)
		set_attr(row, c, attr);
}

/* Column positions for HUD elements (64-col text mode) */
#define SCORE_LABEL_X  25  /* "SCORE: " starts here (row 22) */
#define SCORE_NUM_X    32  /* 6-digit number starts here */
#define TIMER_LABEL_X  27  /* "TIME " starts here (row 0) */
#define TIMER_NUM_X    32  /* 3-digit counter starts here */
#define GEMS_LABEL_X  38  /* "GEMS TO GO" starts here (row 0) */
#define GEMS_NUM_X    50  /* 2-digit number starts here */

/* Print all static HUD labels — call once at level start */
void draw_hud_labels()
{
	set_print_attr(INK_WHITE | PAPER_BLACK);
	gotoxy(SCORE_LABEL_X, 22); printf("SCORE:");

	set_print_attr(TIMER_ATTR);
	gotoxy(TIMER_LABEL_X, 0); printf("TIME");

	set_print_attr(INK_WHITE | PAPER_BLACK);
	gotoxy(GEMS_LABEL_X, 0); printf("GEMS TO GO");
}

/* Update score number only (row 22) */
void show_score()
{
	set_print_attr(INK_WHITE | PAPER_BLACK);
	sprintf(txt_buffer, "%06d", score);
	gotoxy(SCORE_NUM_X, 22); printf(txt_buffer);
}

/* Update time digits only (row 0) — also sets attr for warning */
void show_timer()
{
	static unsigned char attr;
	attr = (timer_sec <= 10) ? TIMER_WARN_ATTR : TIMER_ATTR;
	set_print_attr(attr);
	sprintf(txt_buffer, "%3d", timer_sec);
	gotoxy(TIMER_NUM_X, 0); printf(txt_buffer);
	/* Recolour "TIME" label cells when warning kicks in */
	if (timer_sec <= 10) {
		set_attr(0, 13, TIMER_WARN_ATTR);
		set_attr(0, 14, TIMER_WARN_ATTR);
		set_attr(0, 15, TIMER_WARN_ATTR);
	}
}

/* Show/hide GUN indicator on HUD */
void show_gun_hud()
{
	if (has_gun) {
		set_print_attr(GUN_ATTR);
		gotoxy(0, 0);
		printf("GUN");
	} else {
		set_print_attr(INK_WHITE | PAPER_BLACK);
		gotoxy(0, 0);
		printf("   ");
	}
	set_print_attr(INK_WHITE | PAPER_BLACK);
}

/* Update gems remaining digits only (row 0) — also restores attr */
void show_gems_hud()
{
	static unsigned char attr, remain;
	remain = (gems_collected >= gems_needed) ? 0 :
	         gems_needed - gems_collected;
	attr = exit_open ? (BRIGHT | INK_GREEN | PAPER_BLACK) :
	                   (INK_RED | PAPER_BLACK);
	set_print_attr(attr);
	sprintf(txt_buffer, "%02d", remain);
	gotoxy(GEMS_NUM_X, 0); printf(txt_buffer);
}

/* Fix all row-0 attrs: clear to black, then reapply coloured sections */
void fix_row0_attrs()
{
	set_row_attr(0, INK_WHITE | PAPER_BLACK);
	show_gun_hud();
	show_timer();
	show_gems_hud();
}

/* Unlock the exit: change sprite colour, play sound, update HUD */
void open_exit_gate()
{
	exit_open = 1;
	draw_exit(exit_gx, exit_gy);
	sp1_UpdateNow();
	zx_border(INK_YELLOW);
	snd_exit_open();
	zx_border(INK_BLACK);
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

	ddx = dir_dx[pdir];
	ddy = dir_dy[pdir];

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
	static unsigned char i, r;

	hide_sprites();
	/* Clear screen via SP1 */
	sp1_ClearRectInv(&full_screen, PAPER_BLACK | INK_WHITE, ' ',
		SP1_RFLAG_TILE | SP1_RFLAG_COLOUR);
	sp1_UpdateNow();


	static unsigned char len;

	zx_setink(INK_YELLOW);
	zx_setpaper(PAPER_BLUE);
	*((unsigned char *)ATTR_P_ADDR) |= BRIGHT;
	len = sprintf(txt_buffer, "-= HIGH SCORES =-");
	gotoxy(center_x(len), 1);
	printf(txt_buffer);

	for (i = 0; i != NUM_HISCORES; ++i) {
		r = 3 + (i << 1);
		if (i == rank) {
			zx_setink(INK_GREEN);
			zx_setpaper(PAPER_BLACK);
			*((unsigned char *)ATTR_P_ADDR) |= BRIGHT;
		} else {
			zx_setink(INK_YELLOW);
			zx_setpaper(PAPER_BLACK);
			*((unsigned char *)ATTR_P_ADDR) |= BRIGHT;
		}
		if (hiscores[i] == 0) {
			gotoxy(6, r);
			printf("%d.  ----", i + 1);
		} else {
			gotoxy(6, r);
			printf("%d.  %06u  Level %d", i + 1,
			       hiscores[i], hilevel[i]);
		}
	}

	zx_setink(INK_YELLOW);
	zx_setpaper(PAPER_BLUE);
	*((unsigned char *)ATTR_P_ADDR) |= BRIGHT;
	gotoxy(9, 16);
	printf("Press any button.");

	wait_any_key();
}

/* Build adjacency table from walls[][] for fast BFS.
   Must be called after generate_maze() + add_extra_passages(). */
void build_adj()
{
	static unsigned char i, r, c;
	static uint idx;
	idx = 0;
	for (i = 0; i != ROWS * COLS; ++i) {
		r = bfs_row[i];
		c = bfs_col[i];
		adj[idx]     = (c > 0 && !(walls[r][c - 1] & 1)) ? i - 1 : 255;
		adj[idx + 1] = (c < COLS - 1 && !(walls[r][c] & 1)) ? i + 1 : 255;
		adj[idx + 2] = (r > 0 && !(walls[r - 1][c] & 2)) ? i - COLS : 255;
		adj[idx + 3] = (r < ROWS - 1 && !(walls[r][c] & 2)) ? i + COLS : 255;
		idx += 4;
	}
}

/* Shared BFS core. Caller sets up vis[], stk[], bfs globals before calling.
   bfs_mode_g=0: fixed direction vis (enemy), depth limit 40
   bfs_mode_g=1: propagate vis[ci] (demo), no depth limit
   bfs_efi_g=255: search gemmap[ci]!=0, else: search ci==bfs_efi_g */
void bfs_run_common()
{
	static unsigned char i;
#asm
	push ix

.bfs_il_loop
	; --- Check termination ---
	ld a, (_bfs_head_g)
	ld b, a
	ld a, (_bfs_tail_g)
	cp b
	jp z, bfs_il_end          ; head == tail, queue empty
	; --- Depth limit (mode 0 only) ---
	ld a, (_bfs_mode_g)
	or a
	jr nz, bfs_il_nodepl
	ld a, b
	cp 255
	jp nc, bfs_il_end
.bfs_il_nodepl

	; --- Dequeue: ci = stk[head]; head++ ---
	ld l, b
	ld h, 0
	ld de, _stk
	add hl, de
	ld c, (hl)                ; C = ci
	inc b
	ld a, b
	ld (_bfs_head_g), a

	; --- Read vis[ci] for propagation mode ---
	ld l, c
	ld h, 0
	ld de, _vis
	add hl, de
	ld a, (hl)
	ld (_bfs_adj_ptr_g), a    ; low byte = vis[ci]

	; --- Check target ---
	ld a, (_bfs_efi_g)
	cp 255
	jr z, bfs_il_gemchk
	; Single target mode: ci == efi?
	cp c
	jp z, bfs_il_found
	jr bfs_il_expand
.bfs_il_gemchk
	; Gem search mode: gemmap[ci] != 0?
	ld l, c
	ld h, 0
	ld de, _gemmap
	add hl, de
	ld a, (hl)
	or a
	jp nz, bfs_il_found

.bfs_il_expand
	; --- Compute adj base: HL = &adj[ci*4] ---
	ld l, c
	ld h, 0
	add hl, hl
	add hl, hl
	ld de, $F600
	add hl, de
	push hl
	pop ix                    ; IX = &adj[ci*4]

	; === Dir 0 (left) ===
	ld a, (ix+0)
	cp 255
	jr z, bfs_il_s0
	ld c, a
	ld l, c
	ld h, 0
	ld de, _vis
	add hl, de
	ld a, (hl)
	or a
	jr nz, bfs_il_s0
	; Set vis[ni]: mode 1=propagate, else fixed
	ld a, (_bfs_mode_g)
	cp 1
	jr z, bfs_il_p0
	ld (hl), 1
	jr bfs_il_e0
.bfs_il_p0
	ld a, (_bfs_adj_ptr_g)
	ld (hl), a
.bfs_il_e0
	ld a, (_bfs_tail_g)
	ld l, a
	ld h, 0
	ld de, _stk
	add hl, de
	ld (hl), c
	inc a
	ld (_bfs_tail_g), a
.bfs_il_s0

	; === Dir 1 (right) ===
	ld a, (ix+1)
	cp 255
	jr z, bfs_il_s1
	ld c, a
	ld l, c
	ld h, 0
	ld de, _vis
	add hl, de
	ld a, (hl)
	or a
	jr nz, bfs_il_s1
	ld a, (_bfs_mode_g)
	cp 1
	jr z, bfs_il_p1
	ld (hl), 2
	jr bfs_il_e1
.bfs_il_p1
	ld a, (_bfs_adj_ptr_g)
	ld (hl), a
.bfs_il_e1
	ld a, (_bfs_tail_g)
	ld l, a
	ld h, 0
	ld de, _stk
	add hl, de
	ld (hl), c
	inc a
	ld (_bfs_tail_g), a
.bfs_il_s1

	; === Dir 2 (up) ===
	ld a, (ix+2)
	cp 255
	jr z, bfs_il_s2
	ld c, a
	ld l, c
	ld h, 0
	ld de, _vis
	add hl, de
	ld a, (hl)
	or a
	jr nz, bfs_il_s2
	ld a, (_bfs_mode_g)
	cp 1
	jr z, bfs_il_p2
	ld (hl), 3
	jr bfs_il_e2
.bfs_il_p2
	ld a, (_bfs_adj_ptr_g)
	ld (hl), a
.bfs_il_e2
	ld a, (_bfs_tail_g)
	ld l, a
	ld h, 0
	ld de, _stk
	add hl, de
	ld (hl), c
	inc a
	ld (_bfs_tail_g), a
.bfs_il_s2

	; === Dir 3 (down) ===
	ld a, (ix+3)
	cp 255
	jr z, bfs_il_s3
	ld c, a
	ld l, c
	ld h, 0
	ld de, _vis
	add hl, de
	ld a, (hl)
	or a
	jr nz, bfs_il_s3
	ld a, (_bfs_mode_g)
	cp 1
	jr z, bfs_il_p3
	ld (hl), 4
	jr bfs_il_e3
.bfs_il_p3
	ld a, (_bfs_adj_ptr_g)
	ld (hl), a
.bfs_il_e3
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
	ld a, (_bfs_mode_g)
	cp 1
	jr nz, bfs_il_nosave
	ld a, c
	ld (_demo_target), a
.bfs_il_nosave
	ld a, (_bfs_adj_ptr_g)
	ld (_bfs_result_g), a

.bfs_il_end
	pop ix
#endasm
}

/* Zero vis[] entries that were set during the last BFS run */
void bfs_cleanup()
{
	static unsigned char i;
	for (i = 0; i != bfs_tail_g; ++i)
		vis[stk[i]] = 0;
}

/* Full expanded-grid BFS for enemy pathfinding.
   Uses wallmap[] directly — no precomputed adjacency table.
   BFS from player to enemy; vis[] stores direction of last step.
   Returns: 0=left,1=right,2=up,3=down, -1=no path */
char enemy_bfs(unsigned char exx, unsigned char eyy)
{
	static uint ei, pi, ci, ni;
	static uint head, tail;
	static unsigned char d;

	ei = erow_x_ecols[eyy] + exx;
	pi = erow_x_ecols[py] + px;

	head = 0;
	tail = 1;
	fbfs_stk[0] = pi;
	fbfs_vis[pi] = 5;

	while (head != tail) {
		ci = fbfs_stk[head];
		++head;

		if (ci == ei) {
			d = fbfs_vis[ci];
			/* cleanup visited cells */
			for (head = 0; head != tail; ++head)
				fbfs_vis[fbfs_stk[head]] = 0;
			d--;
			if (d == 0) return 1;  /* left  → enemy goes right */
			if (d == 1) return 0;  /* right → enemy goes left  */
			if (d == 2) return 3;  /* up    → enemy goes down  */
			if (d == 3) return 2;  /* down  → enemy goes up    */
			return -1;
		}

		/* Expand left */
		ni = ci - 1;
		if (!wallmap[ni] && !fbfs_vis[ni]) {
			fbfs_vis[ni] = 1;
			fbfs_stk[tail++] = ni;
		}
		/* Expand right */
		ni = ci + 1;
		if (!wallmap[ni] && !fbfs_vis[ni]) {
			fbfs_vis[ni] = 2;
			fbfs_stk[tail++] = ni;
		}
		/* Expand up */
		ni = ci - ECOLS;
		if (!wallmap[ni] && !fbfs_vis[ni]) {
			fbfs_vis[ni] = 3;
			fbfs_stk[tail++] = ni;
		}
		/* Expand down */
		ni = ci + ECOLS;
		if (!wallmap[ni] && !fbfs_vis[ni]) {
			fbfs_vis[ni] = 4;
			fbfs_stk[tail++] = ni;
		}
	}

	/* Not found — cleanup */
	for (head = 0; head != tail; ++head)
		fbfs_vis[fbfs_stk[head]] = 0;
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
		/* At maze cell — always BFS chase (testing) */
		dir = enemy_bfs(old_ex, old_ey);
		if (dir < 0)
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
	ewalk[sn] ^= 1;
	eanim[sn] = ANIM_FRAMES - 1;

	/* First pixel advance */
	epx[sn] += dir_dpx[dir];
	epy[sn] += dir_dpy[dir];
}

/* Advance enemy n's animation by one frame.
   When animation completes, updates grid position and checks gems. */
void advance_enemy_anim(unsigned char n)
{
	static unsigned char sn, d;
	sn = n;
	d = edir_anim[sn];

	/* Advance pixel position */
	epx[sn] += dir_dpx[d];
	epy[sn] += dir_dpy[d];

	eanim[sn]--;
	if (eanim[sn] == 0) {
		/* Animation complete — update grid position */
		enx[sn] += dir_dx[d];
		eny[sn] += dir_dy[d];

		/* Snap pixel position */
		epx[sn] = enx[sn] * 8;
		epy[sn] = eny[sn] * 8;

		/* Enemy eats gem if it lands on one */
		if ((enx[sn] & 1) && (eny[sn] & 1)) {
			unsigned char ccx, ccy;
			ccx = enx[sn] >> 1;
			ccy = eny[sn] >> 1;
			if (gemmap[ccy * COLS + ccx]) {
				gemmap[ccy * COLS + ccx] = 0;
				gems_left--;
				sp1_PrintAtInv(SROW(eny[sn]), SCOL(enx[sn]), CORR_ATTR, 'F');
				hud_dirty = 1;
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
	set_print_attr(BRIGHT | INK_WHITE | PAPER_GREEN);
	
	static unsigned char len;
	len = sprintf(txt_buffer, "** ESCAPED! **");
	gotoxy(center_x(len), 11); printf(txt_buffer);
	len = sprintf(txt_buffer, "Time bonus: +%d", timer_sec);
	gotoxy(center_x(len), 12); printf(txt_buffer);
	len = sprintf(txt_buffer, "Fire - next level");
	gotoxy(center_x(len), 13); printf(txt_buffer);
	popup_fix_attrs(BRIGHT | INK_BLACK | PAPER_GREEN);
}

/* Generic loss cut-scene: snd_type 0=caught, 1=gems_lost */
void loss_cut_scene(char *title, unsigned char snd_type)
{
	static unsigned char len;
	zx_border(INK_RED);
	if (snd_type) snd_gems_lost();
	else snd_caught();
	zx_border(INK_BLACK);
	draw_popup_bg(
		BRIGHT | INK_YELLOW | PAPER_RED,
		BRIGHT | INK_WHITE  | PAPER_RED);
	set_print_attr(BRIGHT | INK_WHITE | PAPER_RED);
	len = strlen(title);
	gotoxy(center_x(len), 11); printf(title);
	len = sprintf(txt_buffer, "Score: %d", score);
	gotoxy(center_x(len), 12); printf(txt_buffer);
	gotoxy(center_x(16), 13); printf("Press any button.");
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
	/* Entity positions — checked before gems so sprites stay coloured */
	if (gx == exit_gx && gy == exit_gy) return exit_open ? EXIT_ATTR : EXIT_LOCKED_ATTR;
	if (gx == px && gy == py) return PLAYER_ATTR;
	{
		static unsigned char ei;
		for (ei = 0; ei != num_enemies; ++ei)
			if (gx == enx[ei] && gy == eny[ei])
				return enemy_attrs[ei];
	}
	/* Gun pickup */
	if (gun_placed && gx == gun_gx && gy == gun_gy) return GUN_ATTR;
	/* Gem cells sit at odd expanded-grid positions (maze cell centres) */
	if ((gx & 1) && (gy & 1) && gemmap[(unsigned int)(gy >> 1) * COLS + (gx >> 1)])
		return GEM_ATTR;
	return CORR_ATTR;
}

/* Paint or restore the outline of a rectangle centred on (sr,sc).
   restore=0: paint with 'attr'.  restore=1: restore from maze_attr_at(). */
void draw_attr_ring(unsigned char sr, unsigned char sc,
                    unsigned char radius, unsigned char attr,
                    unsigned char restore)
{
	static int r, c, r1, c1, r2, c2;
	static unsigned char a;
	r1 = (int)sr - radius;  r2 = (int)sr + radius;
	c1 = (int)sc - radius;  c2 = (int)sc + radius;
	if (r1 < 0)  r1 = 0;   if (r2 > 23) r2 = 23;
	if (c1 < 0)  c1 = 0;   if (c2 > 31) c2 = 31;
	for (c = c1; c <= c2; ++c) {
		a = restore ? maze_attr_at(r1, c) : attr;
		set_attr(r1, c, a);
		if (r2 != r1) {
			a = restore ? maze_attr_at(r2, c) : attr;
			set_attr(r2, c, a);
		}
	}
	for (r = r1 + 1; r < r2; ++r) {
		a = restore ? maze_attr_at(r, c1) : attr;
		set_attr(r, c1, a);
		if (c2 != c1) {
			a = restore ? maze_attr_at(r, c2) : attr;
			set_attr(r, c2, a);
		}
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
		draw_attr_ring(sr, sc, radius, EXIT_LOCKED_ATTR, 0);
		for (f = 0; f != 2; ++f) intrinsic_halt();
		draw_attr_ring(sr, sc, radius, 0, 1);
	}

	set_attr(sr, sc, EXIT_LOCKED_ATTR);
	draw_exit(exit_gx, exit_gy);
	sp1_UpdateNow();

	for (f = 0; f != 20; ++f) intrinsic_halt();

	/* ── Present each enemy ── */
	for (ei = 0; ei != num_enemies; ++ei) {
		sr = MAZE_R0 + eny[ei];
		sc = MAZE_C0 + enx[ei];
		ea = enemy_attrs[ei];

		/* Fat bass thump */
		bit_beep(30, 1500); intrinsic_ei();
		bit_beep(12, 900);  intrinsic_ei();

		/* Zoom-in: large ring shrinks down to the entity cell */
		for (radius = 7; radius != 0; --radius) {
			draw_attr_ring(sr, sc, radius, ea, 0);
			for (f = 0; f != 2; ++f) intrinsic_halt();
			draw_attr_ring(sr, sc, radius, 0, 1);
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
		draw_attr_ring(sr, sc, radius, PLAYER_ATTR, 0);
		for (f = 0; f != 2; ++f) intrinsic_halt();
		draw_attr_ring(sr, sc, radius, 0, 1);
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
			draw_attr_ring(sr, sc, radius, GUN_ATTR, 0);
			for (f = 0; f != 2; ++f) intrinsic_halt();
			draw_attr_ring(sr, sc, radius, 0, 1);
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

unsigned char read_joy()
{
    if (joy_type == 1) return (unsigned char)in_JoyKempston();
    if (joy_type == 2) return (unsigned char)in_JoySinclair1();
    return (unsigned char)in_JoyKeyboard(&udk);
}


/* Draw one menu row: bright cyan with ">" when selected, dim white otherwise.
   Uses %-22s padding so switching to a shorter item clears old text. */
void draw_item(unsigned char row, char *text, unsigned char sel)
{
	static unsigned char r, s;
	static char *tp;
	r = row; tp = text; s = sel;
	if (s) {
		zx_setink(INK_CYAN);
		*((unsigned char *)ATTR_P_ADDR) |= BRIGHT;
	} else {
		zx_setink(INK_WHITE);
		*((unsigned char *)ATTR_P_ADDR) &= ~BRIGHT;
	}
	zx_setpaper(PAPER_BLACK);
	sprintf(txt_buffer, s ? "> %-22s" : "  %-22s", tp);
	gotoxy(27, r);
	printf(txt_buffer);
}

void draw_menu()
{
	static unsigned char len;

	/* Title */
	zx_setink(INK_YELLOW);
	zx_setpaper(PAPER_BLUE);
	*((unsigned char *)ATTR_P_ADDR) |= BRIGHT;
	len = sprintf(txt_buffer, " -= MAZE RUNNER =- ");
	gotoxy(center_x(len), 2); printf(txt_buffer);

	/* Difficulty section */
	zx_setink(INK_WHITE);
	zx_setpaper(PAPER_BLACK);
	*((unsigned char *)ATTR_P_ADDR) &= ~BRIGHT;
	gotoxy(27, 4); printf("Difficulty:");

	draw_item(5,  "Easy",      menu_cursor == 0);
	draw_item(6,  "Normal",    menu_cursor == 1);
	draw_item(7,  "Hard",      menu_cursor == 2);
	draw_item(8,  "Nightmare", menu_cursor == 3);

	/* Footer */
	zx_setink(INK_CYAN);
	zx_setpaper(PAPER_BLACK);
	*((unsigned char *)ATTR_P_ADDR) &= ~BRIGHT;
	len = sprintf(txt_buffer, "Q/A: select  FIRE: start");
	gotoxy(center_x(len), 10); printf(txt_buffer);

	/* Main Cast — render SP1 tile icons first, then text on top */
	sp1_PrintAtInv(14, 7, PLAYER_ATTR, 'P');
	sp1_PrintAtInv(15, 7, ENEMY_ATTR, 'E');
	sp1_PrintAtInv(16, 7, GEM_ATTR, 'C');
	sp1_PrintAtInv(17, 7, EXIT_LOCKED_ATTR, 'X');
	sp1_PrintAtInv(18, 7, GUN_ATTR, 'G');
	sp1_PrintAtInv(19, 7, WALL_ATTR, 'B');
	sp1_UpdateNow();

	zx_setpaper(PAPER_BLACK);
	*((unsigned char *)ATTR_P_ADDR) |= BRIGHT;
	zx_setink(INK_YELLOW);
	len = sprintf(txt_buffer, "--- The Cast ---");
	gotoxy(center_x(len), 12); printf(txt_buffer);

	zx_setink(INK_GREEN);
	gotoxy(18, 14); printf("YOU    Escape the maze alive!");

	zx_setink(INK_RED);
	gotoxy(18, 15); printf("GHOST  Hunts you & steals gems");

	zx_setink(INK_YELLOW);
	gotoxy(18, 16); printf("GEM    Grab enough to open exit");

	*((unsigned char *)ATTR_P_ADDR) &= ~BRIGHT;
	zx_setink(INK_RED);
	gotoxy(18, 17); printf("EXIT   Locked until quota met");

	*((unsigned char *)ATTR_P_ADDR) |= BRIGHT;
	zx_setink(INK_CYAN);
	gotoxy(18, 18); printf("GUN    Stun ghosts for 8 sec");

	zx_setink(INK_RED);
	gotoxy(18, 19); printf("WALL   Don't get cornered!");

	zx_setink(INK_WHITE);
	zx_setpaper(PAPER_BLACK);
	*((unsigned char *)ATTR_P_ADDR) &= ~BRIGHT;
	len = sprintf(txt_buffer, "(c) 2026 Ilya Palopezhentsev");
	gotoxy(center_x(len), 22); printf(txt_buffer);
}

/* Nav map: precomputed direction toward target for each cell */

/* Demo AI — BFS to nearest gem or exit, cache one direction.
   Reuses vis[]/stk[]/bfs_run_common — cleaned up before return. */
char demo_ai_dir()
{
	static unsigned char pi, nv, tc;

	if (!(px & 1) || !(py & 1)) return pdir;

	if (nav_valid) return nav_valid - 1;  /* cached: 1-4 = dir 0-3 */

	/* Find target cell */
	if (exit_open) {
		tc = row_x_cols[exit_gy >> 1] + (exit_gx >> 1);
	} else {
		for (tc = 0; tc != ROWS * COLS; ++tc)
			if (gemmap[tc]) break;
		if (tc >= ROWS * COLS) return -1;  /* no gems left */
	}

	pi = row_x_cols[py >> 1] + (px >> 1);
	if (tc == pi) return -1;  /* at target */

	/* BFS from target using shared asm BFS */
	vis[tc] = 5;
	stk[0] = tc;
	bfs_head_g = 0;
	bfs_tail_g = 1;
	bfs_efi_g = 254;   /* no search target */
	bfs_mode_g = 2;     /* fixed vis + no depth limit */
	bfs_result_g = 255;
	bfs_run_common();

	nv = vis[pi];
	bfs_cleanup();

	if (nv == 0 || nv == 5) return -1;
	nv = (nv - 1) ^ 1;  /* reverse direction toward target */

	/* Momentum: prefer current direction if passable and not reversing */
	if (adj[(int)pi * 4 + pdir] != 255 && nv != (pdir ^ 1)) {
		nv = pdir;
	}

	nav_valid = nv + 1;  /* cache: 1-4 */
	return nv;
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
	sbrk((void *)0xF200, 0x0400);  /* $F200-$F5FF: sprite heap (before adj[] at $F600) */

	sp1_Initialize(SP1_IFLAG_OVERWRITE_TILES | SP1_IFLAG_OVERWRITE_DFILE,
		CORR_ATTR, 'F');
	sp1_TileEntry('B', brick);
	sp1_TileEntry('C', spr_gem);
	sp1_TileEntry('F', floor_tile);
	sp1_TileEntry('G', spr_gun);
	sp1_TileEntry('P', gfx_man_right1);
	sp1_TileEntry('E', gfx_man_left1);
	sp1_TileEntry('X', gfx_exit_tile);

	/* Set print attributes to white ink on black paper globally */
	set_print_attr(INK_WHITE | PAPER_BLACK);

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

	{
		static unsigned char ei;
		static unsigned char ea;
		for (ei = 0; ei != 4; ++ei) {
			spr_enemies[ei] = sp1_CreateSpr(SP1_DRAW_MASK2NR, SP1_TYPE_2BYTE, 2, 0, 2);
			sp1_AddColSpr(spr_enemies[ei], SP1_DRAW_MASK2NR, 0, 46, 2);
			spr_enemies[ei]->xthresh = 1;
			spr_enemies[ei]->ythresh = 1;
			ea = enemy_attrs[ei];
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
		demo_mode = 0;
		/* Clear screen directly — avoids SP1/console driver conflicts */
		memset((unsigned char *)16384u, 0, 6144u);
		memset((unsigned char *)22528u, INK_WHITE | PAPER_BLACK, 768u);
		zx_border(INK_BLACK);
		gotoxy(0, 0);

		/* Init selections: default Normal */
		diff_cursor = 1;  /* Normal */
		menu_cursor = diff_cursor;
		draw_menu();

		/* Q/A to select difficulty; fire on any device starts + sets joy_type.
		   Sample initial joystick state first to avoid false triggers on
		   floating bus (Kempston) or held buttons at startup. */
		{
			static unsigned char joy_k, joy_s, prev_k, prev_s, prev_key;
			static unsigned int demo_timer;
			prev_k = (unsigned char)in_JoyKempston() & in_FIRE;
			prev_s = (unsigned char)in_JoySinclair1() & in_FIRE;
			prev_key = 0;
			demo_timer = 0;
			while (1) {
				intrinsic_halt();
				rseed++;
				k = in_Inkey();
				if (k) demo_timer = 0; else demo_timer++;
				joy_k = (unsigned char)in_JoyKempston() & in_FIRE;
				joy_s = (unsigned char)in_JoySinclair1() & in_FIRE;
				if (k == 'q' && !prev_key) {
					if (diff_cursor > 0) diff_cursor--;
					menu_cursor = diff_cursor;
					draw_item(5, "Easy",      menu_cursor == 0);
					draw_item(6, "Normal",    menu_cursor == 1);
					draw_item(7, "Hard",      menu_cursor == 2);
					draw_item(8, "Nightmare", menu_cursor == 3);
				} else if (k == 'a' && !prev_key) {
					if (diff_cursor < 3) diff_cursor++;
					menu_cursor = diff_cursor;
					draw_item(5, "Easy",      menu_cursor == 0);
					draw_item(6, "Normal",    menu_cursor == 1);
					draw_item(7, "Hard",      menu_cursor == 2);
					draw_item(8, "Nightmare", menu_cursor == 3);
				} else if ((k == '\r' || k == ' ') && !prev_key) {
					joy_type = 0;
					break;
				} else if (joy_k && !prev_k) {
					joy_type = 1;
					break;
				} else if (joy_s && !prev_s) {
					joy_type = 2;
					break;
				} else if (demo_timer >= 500) {
					demo_mode = 25;  /* grace frames before accepting keys */
					demo_target = 255;
					nav_valid = 0;
					joy_type = 0;
					break;
				}
				prev_k = joy_k;
				prev_s = joy_s;
				prev_key = k;
			}
		}
		if (demo_mode)
			difficulty = 1;
		else
			difficulty = diff_cursor + 1;
		wait_key_release();
		if (rseed == 0) rseed = 42;
		srand(rseed);

		if (difficulty == 1) {
			base_enemies = 1;
			base_frames = 8;
			base_chase = 10;
			extra_wall_pct = 3;   /* 1-in-3 = ~33% walls removed */
			extra_halls_base = 5;
			extra_halls_rng = 3;  /* 5-8 halls */
			base_time = 90;
		} else if (difficulty == 2) {
			base_enemies = 2;
			base_frames = 6;
			base_chase = 25;
			extra_wall_pct = 5;   /* 1-in-5 = ~20% walls removed */
			extra_halls_base = 3;
			extra_halls_rng = 2;  /* 3-5 halls */
			base_time = 60;
		} else if (difficulty == 3) {
			base_enemies = 3;
			base_frames = 4;
			base_chase = 50;
			extra_wall_pct = 8;   /* 1-in-8 = ~12% walls removed */
			extra_halls_base = 1;
			extra_halls_rng = 1;  /* 1-2 halls */
			base_time = 45;
		} else {
			base_enemies = 4;
			base_frames = 3;
			base_chase = 70;
			extra_wall_pct = 10;  /* 1-in-10 = ~10% walls removed */
			extra_halls_base = 1;
			extra_halls_rng = 0;  /* 1 hall */
			base_time = 30;
		}

	game_over = 0;
	while (1) {
		zx_border(INK_BLACK);

		level++;

		/* Level progression: enemies get tougher each level */
		{
			static int prog;  /* levels of progression past base */
			static int v;     /* temp for clamped calculations */
			prog = level - 1; /* level 1 = base settings */

			/* Add an enemy every 10 levels, cap at 4 */
			v = base_enemies + prog / 10;
			num_enemies = (v > 4) ? 4 : v;

			/* Speed up enemies: -1 frame every 2 levels, min 2 */
			v = base_frames - prog / 2;
			enemy_frames = (v < 2) ? 2 : v;

			/* Increase chase AI: +35% per level, cap at 100 */
			v = base_chase + prog * 35;
			chase_pct = (v > 100) ? 100 : v;

			/* Reduce time: -3 sec per level, min 15 */
			v = base_time - prog * 3;
			time_limit = (v < 15) ? 15 : v;
		}

		/* Clear screen via SP1 for new level */
		hide_sprites();
		sp1_ClearRectInv(&full_screen, INK_WHITE | PAPER_BLACK, ' ',
			SP1_RFLAG_TILE | SP1_RFLAG_COLOUR);
		sp1_UpdateNow();

		{
			static unsigned char len;
			static char *dname;
			set_print_attr(INK_WHITE | PAPER_BLACK);
			dname = (difficulty == 1) ? "Easy" :
			        (difficulty == 2) ? "Normal" :
			        (difficulty == 3) ? "Hard" : "Nightmare";
			if (demo_mode)
				len = sprintf(txt_buffer, "--- DEMO ---");
			else {
				len = sprintf(txt_buffer, "Lv%d [%s]", level, dname);
			}
			gotoxy(center_x(len), 23); printf(txt_buffer);
		}

		generate_maze();
		add_extra_passages();
		build_adj();

		draw_maze();
		memset(vis, 0, ROWS * COLS);  /* Clear for old BFS after maze gen */
		memset(fbfs_vis, 0, 551u);    /* Clear full-grid BFS vis */

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
		pwalk = 0;
		{
			unsigned char ei;
			for (ei = 0; ei != num_enemies; ++ei) {
				last_edir_arr[ei] = 0;
				eanim[ei] = 0;
				ewalk[ei] = 0;
				enemy_stun[ei] = 0;
			}
		}

		/* Place gems, gun, and draw everything */
		place_gems();
		gems_collected = 0;
		total_gems = gems_left;
		gems_needed = (total_gems + 1) / 2;  /* need half the gems */
		exit_open = 0;
		if (demo_mode) { gun_placed = 0; has_gun = 0; } else place_gun();
		draw_exit(exit_gx, exit_gy);
		{
			static unsigned char ei;
			for (ei = 0; ei != num_enemies; ++ei)
				draw_enemy_n(ei, enx[ei], eny[ei]);
		}
		draw_dot(px, py);
		sp1_UpdateNow();

		draw_hud_labels();
		show_score();
		set_row_attr(23, INK_WHITE | PAPER_BLACK);
		fix_row0_attrs();
		if (demo_mode) {
			set_print_attr(BRIGHT | INK_YELLOW | PAPER_BLACK);
			gotoxy(0, 0); printf("DEMO");
		} else
			level_intro();

		while (1) {
			intrinsic_halt();  /* sync to 50Hz frame (IM2 null ISR — safe) */

			/* Demo mode: any key exits back to menu (grace period first) */
			if (demo_mode) {
				if (demo_mode > 1) demo_mode--;
				else if (in_Inkey()) { game_over = 1; break; }
			}

			/* --- Player pixel-scrolling movement --- */
			if (panim > 0) {
				/* Continue animation */
				ppx += dir_dpx[pdir];
				ppy += dir_dpy[pdir];

				panim--;

				/* Corner tolerance: when 3px from destination,
				   allow perpendicular turn by snapping early */
				if (panim == 2 && !demo_mode) {
					k = read_joy();
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
							dest_px = px + dir_dx[pdir];
							dest_py = py + dir_dy[pdir];
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
								ppx += dir_dpx[pdir];
								ppy += dir_dpy[pdir];
								snd_step();
								goto snap_done;
							}
						}
					}
				}

				if (panim == 0) {
					/* Animation complete — update grid position */
					px += dir_dx[pdir];
					py += dir_dy[pdir];
					ppx = px * 8;
					ppy = py * 8;
					if (demo_mode) nav_valid = 0;  /* recalc at new cell */

				snap_done:
					/* Collect gem? */
					if (try_collect_gem(px, py)) {
						zx_border(INK_YELLOW);
						snd_gem();
						zx_border(INK_BLACK);
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

					if (exit_open && px == exit_gx && py == exit_gy) {
						if (demo_mode) { game_over = 1; break; }
						score += 50 + timer_sec;
						update_player_spr();
						render_spr_pix(spr_player, framebuf_player, cur_pgfx, cur_pmsk, ppx, ppy);
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
				if (demo_mode) {
					k = demo_ai_dir();
					if (k < 0) k = enemy_random_dir(px, py);
					if (k >= 0) { dx = dir_dx[k]; dy = dir_dy[k]; }
				} else {
				k = read_joy();
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
				} /* end else (not firing) */
				} /* end !demo_mode */
				/* Only one axis at a time — prevent diagonal wall skip */
				if (dx && dy) dy = 0;
				if ((dx || dy) && can_move(dx, dy)) {
					if (dx == -1) pdir = 0;
					else if (dx == 1) pdir = 1;
					else if (dy == -1) pdir = 2;
					else pdir = 3;
					pwalk ^= 1;  /* toggle walk phase */
					panim = ANIM_FRAMES - 1;
					/* First pixel advance */
					ppx += dir_dpx[pdir];
					ppy += dir_dpy[pdir];
					snd_step();
				} else if (dx || dy) {
					snd_bump();
				}
			}

			/* Render player at current pixel position */
			update_player_spr();
			render_spr_pix(spr_player, framebuf_player, cur_pgfx, cur_pmsk, ppx, ppy);
			sp1_set_spr_colour(spr_player, PLAYER_ATTR);

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
					update_enemy_spr(ei);
					render_spr_pix(spr_enemies[ei],
					               framebuf_enemies[ei],
					               cur_pgfx, cur_pmsk, epx[ei], epy[ei]);
					sp1_set_spr_colour(spr_enemies[ei], enemy_attrs[ei]);
				}

				/* Per-frame pixel-proximity collision check —
				   catches swap-throughs and mid-animation overlaps
				   that the grid-based checks miss */
				if (!caught) {
					for (ei = 0; ei != num_enemies; ++ei) {
						if (enemy_stun[ei]) continue;
						if ((unsigned char)(ppx - epx[ei] + 7) < 15 &&
						    (unsigned char)(ppy - epy[ei] + 7) < 15) {
							caught = 1;
							break;
						}
					}
				}

				/* Gems impossible? Game over if can't reach threshold */
				if (!exit_open &&
				    gems_collected + gems_left < gems_needed) {
					if (demo_mode) { game_over = 1; break; }
					sp1_UpdateNow();
					loss_cut_scene("** ENEMIES ATE TOO MANY GEMS! **", 1);
					wait_any_key();
					rank = update_hiscores();
					show_hiscores(rank);
					score = 0;
					level = 0;
					game_over = 1;
					break;
				}
			}

			/* Update gems HUD in SP1 buffer before render */
			/* --- Render all SP1 changes this frame --- */
			sp1_UpdateNow();

			/* Update HUD after render (row 0/22 outside maze area) */
			if (hud_dirty) {
				hud_dirty = 0;
				show_score();
				show_gems_hud();
			}

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
						if (demo_mode) { game_over = 1; break; }
						loss_cut_scene("** TIME UP! **", 0);
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
				if (demo_mode) { game_over = 1; break; }
				loss_cut_scene("** CAUGHT! **", 0);
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
