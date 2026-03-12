#include <stdio.h>
#include <stdlib.h>
#include <graphics.h>
#include <conio.h>
#include <features.h>
#include <sound.h>
#include <intrinsic.h>
#include <arch/zx/spectrum.h>

#define COLS 14
#define ROWS 9

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
int px, py;    /* player position as flat index into expanded grid */
int ex, ey;    /* enemy position as flat index into expanded grid */
unsigned int rseed;

#define ATTR_BASE   22528
#define ATTR_P_ADDR 23693
#define WALL_ATTR   (BRIGHT | INK_RED | PAPER_YELLOW)
#define CORR_ATTR   (INK_BLACK | PAPER_BLACK)
#define PLAYER_ATTR (BRIGHT | INK_GREEN | PAPER_BLACK)
#define ENEMY_ATTR  (BRIGHT | INK_RED | PAPER_BLACK)
#define EXIT_ATTR   (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define TITLE_ATTR  (BRIGHT | INK_YELLOW | PAPER_BLUE)
#define WIN_ATTR    (BRIGHT | INK_GREEN | PAPER_BLACK)

/* Exit position in expanded grid */
#define EXIT_GX (2*COLS-1)
#define EXIT_GY (2*ROWS-1)

/* Brick pattern: red ink on yellow paper */
unsigned char brick[8] = {
	0xF7, 0xF7, 0xF7, 0x00,
	0xDF, 0xDF, 0xDF, 0x00
};

/* Set attribute for character cell (row, col) */
void set_attr(unsigned char row, unsigned char col,
              unsigned char attr)
{
	unsigned char *p;
	p = (unsigned char *)(ATTR_BASE + row * 32 + col);
	*p = attr;
}

/* Draw a brick-textured 8x8 block at screen char (sr, sc) */
void draw_brick(unsigned char sr, unsigned char sc)
{
	unsigned char *base;
	unsigned char i;
	unsigned int addr;
	addr = 16384u + ((unsigned int)(sr >> 3) << 11)
	     + ((unsigned int)(sr & 7) << 5) + sc;
	base = (unsigned char *)addr;
	for (i = 0; i < 8; i++) {
		*base = brick[i];
		base += 256;
	}
	set_attr(sr, sc, WALL_ATTR);
}

/* Clear all pixel data (6144 bytes at 16384) */
void clear_pixels()
{
	unsigned char *p;
	unsigned int i;
	p = (unsigned char *)16384u;
	for (i = 0; i < 6144u; i++)
		*p++ = 0;
}

/* Colour the title row */
void colour_title()
{
	int c;
	for (c = 0; c < 32; c++)
		set_attr(0, c, TITLE_ATTR);
}

unsigned int rng()
{
	rseed = rseed * 181 + 359;
	return rseed >> 2;
}

/* Visited flags and explicit stack for DFS and BFS.
   Sized for expanded grid (largest use case). */
unsigned char vis[EROWS * ECOLS];
int stk[EROWS * ECOLS];
int sp;

void generate_maze()
{
	int x, y, cx, cy, nx, ny;
	int dirs[4], nd, i, j, t;

	for (y = 0; y < ROWS; y++)
		for (x = 0; x < COLS; x++) {
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
		for (i = nd - 1; i > 0; i--) {
			j = rng() % (i + 1);
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

void draw_maze()
{
	int gr, gc, sr, sc, w;
	unsigned char *wm;

	wm = wallmap;
	for (gr = 0; gr < EROWS; gr++) {
		for (gc = 0; gc < ECOLS; gc++) {
			sr = MAZE_R0 + gr;
			sc = MAZE_C0 + gc;
			w = 0;

			if (!(gr & 1) && !(gc & 1)) {
				w = 1;
			}
			else if (!(gr & 1) && (gc & 1)) {
				if (gr == 0 || gr == 2 * ROWS)
					w = 1;
				else if (walls[gr/2 - 1][gc/2] & 2)
					w = 1;
			}
			else if ((gr & 1) && !(gc & 1)) {
				if (gc == 0 || gc == 2 * COLS)
					w = 1;
				else if (walls[gr/2][gc/2 - 1] & 1)
					w = 1;
			}

			*wm++ = w;
			if (w)
				draw_brick(sr, sc);
			else
				set_attr(sr, sc, CORR_ATTR);
		}
	}
}

/* Pre-computed 8-byte bitmaps for sprites (all within one 8x8 cell) */
unsigned char spr_dot[8]  = {0x00,0x00,0x38,0x7C,0x7C,0x7C,0x38,0x00};
unsigned char spr_enemy[8]= {0x00,0x00,0x10,0x28,0x54,0x28,0x10,0x00};
unsigned char spr_exit[8] = {0x00,0x82,0x44,0x28,0x10,0x28,0x44,0x82};

/* Write an 8-byte bitmap into character cell (sr, sc) and set attr. */
void draw_sprite(unsigned char sr, unsigned char sc,
                 unsigned char *spr, unsigned char attr)
{
	unsigned char *base;
	unsigned char i;
	unsigned int addr;
	addr = 16384u + ((unsigned int)(sr >> 3) << 11)
	     + ((unsigned int)(sr & 7) << 5) + sc;
	base = (unsigned char *)addr;
	for (i = 0; i < 8; i++) {
		*base = spr[i];
		base += 256;
	}
	set_attr(sr, sc, attr);
}

/* Clear a character cell (pixels + attribute) */
void clear_cell(unsigned char sr, unsigned char sc)
{
	unsigned char *base;
	unsigned char i;
	unsigned int addr;
	addr = 16384u + ((unsigned int)(sr >> 3) << 11)
	     + ((unsigned int)(sr & 7) << 5) + sc;
	base = (unsigned char *)addr;
	for (i = 0; i < 8; i++) {
		*base = 0;
		base += 256;
	}
	set_attr(sr, sc, CORR_ATTR);
}

void draw_dot(int gx, int gy)
{
	draw_sprite(SROW(gy), SCOL(gx), spr_dot, PLAYER_ATTR);
}

void erase_dot(int gx, int gy)
{
	clear_cell(SROW(gy), SCOL(gx));
}

void draw_exit(int gx, int gy)
{
	draw_sprite(SROW(gy), SCOL(gx), spr_exit, EXIT_ATTR);
}

void snd_step()
{
	bit_beep(1, 200);
	intrinsic_ei();
}

void snd_bump()
{
	bit_beep(3, 800);
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

void snd_win()
{
	bit_beep(15, 150);
	intrinsic_ei();
	bit_beep(15, 120);
	intrinsic_ei();
	bit_beep(30, 80);
	intrinsic_ei();
}

void draw_enemy(int gx, int gy)
{
	draw_sprite(SROW(gy), SCOL(gx), spr_enemy, ENEMY_ATTR);
}

void erase_enemy(int gx, int gy)
{
	clear_cell(SROW(gy), SCOL(gx));
}

/* BFS from enemy to player on expanded grid.
   Uses flat indices — no multiply/divide in inner loop.
   Returns: 0=left,1=right,2=up,3=down, -1=no path */
int enemy_bfs()
{
	int head, tail;
	int ci, ni, d;
	int efi;  /* enemy flat index */

	for (ci = 0; ci < EROWS * ECOLS; ci++)
		vis[ci] = 0;

	efi = ey * ECOLS + ex;

	/* BFS from player back to enemy, so we get the first step */
	head = 0;
	tail = 0;
	ci = py * ECOLS + px;
	stk[tail++] = ci;
	vis[ci] = 5; /* mark as origin */

	while (head < tail) {
		ci = stk[head++];

		if (ci == efi) {
			d = vis[efi] - 1;
			if (d == 0) return 1;
			if (d == 1) return 0;
			if (d == 2) return 3;
			if (d == 3) return 2;
			return -1;
		}

		/* Left: ci - 1 */
		ni = ci - 1;
		if (!vis[ni] && !wallmap[ni]) {
			vis[ni] = 1;
			stk[tail++] = ni;
		}
		/* Right: ci + 1 */
		ni = ci + 1;
		if (!vis[ni] && !wallmap[ni]) {
			vis[ni] = 2;
			stk[tail++] = ni;
		}
		/* Up: ci - ECOLS */
		ni = ci - ECOLS;
		if (ni >= 0 && !vis[ni] && !wallmap[ni]) {
			vis[ni] = 3;
			stk[tail++] = ni;
		}
		/* Down: ci + ECOLS */
		ni = ci + ECOLS;
		if (ni < EROWS * ECOLS && !vis[ni] && !wallmap[ni]) {
			vis[ni] = 4;
			stk[tail++] = ni;
		}
	}
	return -1;
}

/* Pick a random valid direction from (ex,ey) */
int enemy_random_dir()
{
	int dirs[4], nd;
	int fi;
	fi = ey * ECOLS + ex;
	nd = 0;
	if (!wallmap[fi - 1])     dirs[nd++] = 0;
	if (!wallmap[fi + 1])     dirs[nd++] = 1;
	if (!wallmap[fi - ECOLS]) dirs[nd++] = 2;
	if (!wallmap[fi + ECOLS]) dirs[nd++] = 3;
	if (nd == 0) return -1;
	return dirs[rng() % nd];
}

/* Move enemy one step — 50% chance to chase, 50% random wander */
void move_enemy()
{
	int dir;
	int old_ex, old_ey;

	if (rng() % 2 == 0)
		dir = enemy_bfs();
	else
		dir = enemy_random_dir();
	if (dir < 0) return;

	old_ex = ex;
	old_ey = ey;
	erase_enemy(ex, ey);

	if (dir == 0) ex--;
	else if (dir == 1) ex++;
	else if (dir == 2) ey--;
	else if (dir == 3) ey++;

	/* Redraw exit/player if enemy was on their cell */
	if (old_ex == EXIT_GX && old_ey == EXIT_GY)
		draw_exit(EXIT_GX, EXIT_GY);
	if (old_ex == px && old_ey == py)
		draw_dot(px, py);

	draw_enemy(ex, ey);
}

int can_move(int dx, int dy)
{
	return !wallmap[(py + dy) * ECOLS + (px + dx)];
}

/* Enemy move interval — number of main loop ticks between moves */
#define ENEMY_TICK 600

main()
{
	char k;
	int dx, dy;
	int moves;
	int caught;
	unsigned int tick;

	while (1) {
		zx_border(INK_BLUE);
		zx_cls_attr(INK_WHITE | PAPER_BLACK);
		clear_pixels();
		printf("     -= MAZE GAME =-\n");
		printf(" Press any key to start...");
		colour_title();

		/* Seed RNG from keypress timing */
		rseed = 0;
#ifdef __HAVE_KEYBOARD
		while (!getk())
			rseed++;
		while (getk()) ;
#endif
		if (rseed == 0) rseed = 42;

		zx_cls_attr(INK_WHITE | PAPER_BLACK);
		clear_pixels();
		printf("  MAZE  O/P/Q/A=move\n");
		colour_title();

		generate_maze();
		draw_maze();

		/* Positions in expanded grid coords */
		px = 1;
		py = 1;
		ex = 2 * COLS - 1;
		ey = 1;
		moves = 0;
		caught = 0;
		tick = 0;

		/* Draw exit, enemy, and player */
		draw_exit(EXIT_GX, EXIT_GY);
		draw_enemy(ex, ey);
		draw_dot(px, py);

		while (1) {
#ifdef __HAVE_KEYBOARD
			k = getk();
#else
			k = 0;
#endif
			dx = 0;
			dy = 0;
			if (k == 'o' || k == 'O') dx = -1;
			if (k == 'p' || k == 'P') dx = 1;
			if (k == 'q' || k == 'Q') dy = -1;
			if (k == 'a' || k == 'A') dy = 1;

			if (dx || dy) {
				if (can_move(dx, dy)) {
					erase_dot(px, py);
					px += dx;
					py += dy;
					moves++;
					draw_dot(px, py);

					/* Player walked onto enemy? */
					if (px == ex && py == ey)
						caught = 1;

					if (px == EXIT_GX &&
					    py == EXIT_GY) {
						snd_win();
						printf("\n\n\n\n\n\n\n\n\n\n\n");
						printf("\n\n  YOU WIN!");
						printf(" Moves: %d\n", moves);
						printf("  Any key=new maze");
						{
							int wr;
							for (wr = 13; wr <= 16; wr++) {
								int wc;
								for (wc = 0; wc < 32; wc++)
									set_attr(wr, wc, WIN_ATTR);
							}
						}
#ifdef __HAVE_KEYBOARD
						fgetc_cons();
#endif
						break;
					}

					snd_step();
					tick = 0; /* reset tick so enemy doesn't
					             move right after player */
				} else {
					snd_bump();
				}

				/* Wait for key release (debounced) */
#ifdef __HAVE_KEYBOARD
				{
					unsigned char rel = 0;
					while (rel < 30) {
						if (getk()) rel = 0;
						else rel++;
					}
				}
#endif
			}

			/* Enemy moves on its own timer */
			tick++;
			if (tick >= ENEMY_TICK) {
				tick = 0;
				move_enemy();

				if (ex == px && ey == py)
					caught = 1;
			}

			if (caught) {
				snd_caught();
				printf("\n\n\n\n\n\n\n\n\n\n\n");
				printf("\n\n  CAUGHT!");
				printf(" Moves: %d\n", moves);
				printf("  Any key=new maze");
				{
					int wr;
					for (wr = 13; wr <= 16; wr++) {
						int wc;
						for (wc = 0; wc < 32; wc++)
							set_attr(wr, wc,
							  BRIGHT | INK_RED |
							  PAPER_BLACK);
					}
				}
#ifdef __HAVE_KEYBOARD
				fgetc_cons();
#endif
				break;
			}
		}
	}
}
