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
int px, py;    /* player position as flat index into expanded grid */
int ex, ey;    /* enemy position as flat index into expanded grid */
unsigned int rseed;

/* Coin map: 1=coin present at maze cell (cx,cy). Index = cy*COLS+cx */
unsigned char coinmap[ROWS * COLS];
int score;
int coins_left;
int level;

/* High scores table */
int hiscores[NUM_HISCORES];
int hilevel[NUM_HISCORES];

#define ATTR_BASE   22528
#define ATTR_P_ADDR 23693
#define WALL_ATTR   (BRIGHT | INK_RED | PAPER_YELLOW)
#define CORR_ATTR   (INK_BLACK | PAPER_BLACK)
#define PLAYER_ATTR (BRIGHT | INK_GREEN | PAPER_BLACK)
#define ENEMY_ATTR  (BRIGHT | INK_RED | PAPER_BLACK)
#define EXIT_ATTR   (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define COIN_ATTR   (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define TITLE_ATTR  (BRIGHT | INK_YELLOW | PAPER_BLUE)
#define WIN_ATTR    (BRIGHT | INK_GREEN | PAPER_BLACK)
#define HISCORE_ATTR (BRIGHT | INK_YELLOW | PAPER_BLACK)

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

/* After generating the perfect maze, punch extra holes to create
   loops and small halls so the player can dodge the enemy. */
void add_extra_passages()
{
	int x, y, i, n;

	/* Pass 1: randomly remove ~20% of remaining walls → loops */
	for (y = 0; y < ROWS; y++)
		for (x = 0; x < COLS; x++) {
			if (x < COLS - 1 && (walls[y][x] & 1) && rng() % 5 == 0)
				walls[y][x] &= ~1;
			if (y < ROWS - 1 && (walls[y][x] & 2) && rng() % 5 == 0)
				walls[y][x] &= ~2;
		}

	/* Pass 2: create a few 2x2 halls by removing shared walls
	   between a 2x2 block of maze cells */
	n = 3 + rng() % 3;  /* 3-5 halls */
	for (i = 0; i < n; i++) {
		x = rng() % (COLS - 1);
		y = rng() % (ROWS - 1);
		/* Remove walls between (x,y), (x+1,y), (x,y+1), (x+1,y+1) */
		walls[y][x]     &= ~3;  /* right + bottom */
		walls[y][x + 1] &= ~2;  /* bottom */
		walls[y + 1][x] &= ~1;  /* right */
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
				/* Border posts are always walls */
				if (gr == 0 || gr == 2*ROWS ||
				    gc == 0 || gc == 2*COLS) {
					w = 1;
				} else {
					/* Interior corner post: wall only if
					   any adjacent wall segment exists */
					int cy, cx;
					cy = gr / 2;
					cx = gc / 2;
					if ((walls[cy-1][cx-1] & 1) ||
					    (walls[cy][cx-1] & 1) ||
					    (walls[cy-1][cx-1] & 2) ||
					    (walls[cy-1][cx] & 2))
						w = 1;
				}
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
unsigned char spr_coin[8] = {0x00,0x3C,0x7E,0x7E,0x3C,0x3C,0x18,0x00};

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
	bit_beep(2, 200);
	intrinsic_ei();
	bit_beep(2, 100);
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

void draw_coin(int cx, int cy)
{
	int gx, gy;
	gx = cx * 2 + 1;
	gy = cy * 2 + 1;
	draw_sprite(SROW(gy), SCOL(gx), spr_coin, COIN_ATTR);
}

/* Place coins on ~40% of maze cells, avoiding start/exit/enemy */
void place_coins()
{
	int cx, cy, i;
	coins_left = 0;
	for (i = 0; i < ROWS * COLS; i++)
		coinmap[i] = 0;

	for (cy = 0; cy < ROWS; cy++)
		for (cx = 0; cx < COLS; cx++) {
			/* Skip player start (0,0), exit cell, enemy start */
			if (cx == 0 && cy == 0) continue;
			if (cx == COLS-1 && cy == ROWS-1) continue;
			if (cx == COLS-1 && cy == 0) continue;
			if (rng() % 5 < 2) {
				coinmap[cy * COLS + cx] = 1;
				coins_left++;
				draw_coin(cx, cy);
			}
		}
}

/* Check and collect coin at expanded grid position (gx,gy) */
int try_collect_coin(int gx, int gy)
{
	int cx, cy;
	if (!(gx & 1) || !(gy & 1)) return 0;  /* not a cell center */
	cx = gx >> 1;
	cy = gy >> 1;
	if (coinmap[cy * COLS + cx]) {
		coinmap[cy * COLS + cx] = 0;
		coins_left--;
		score += 10;
		return 1;
	}
	return 0;
}

/* Display score on the title row */
void show_score()
{
	/* Position cursor at row 0, col 18 area for score display */
	unsigned char *attr;
	int c;
	/* Use gotoxy to position text — col 0 is leftmost */
	gotoxy(18, 0);
	printf("S:%04d", score);
	/* Ensure title attr covers score area */
	for (c = 18; c < 24; c++)
		set_attr(0, c, TITLE_ATTR);
}

/* Update high scores table, return rank (0-based) or -1 */
int update_hiscores()
{
	int i, j;
	for (i = 0; i < NUM_HISCORES; i++) {
		if (score > hiscores[i]) {
			/* Shift lower scores down */
			for (j = NUM_HISCORES - 1; j > i; j--) {
				hiscores[j] = hiscores[j-1];
				hilevel[j] = hilevel[j-1];
			}
			hiscores[i] = score;
			hilevel[i] = level;
			return i;
		}
	}
	return -1;
}

/* Display high scores screen */
void show_hiscores(int rank)
{
	int i, r;
	unsigned char attr;

	clear_pixels();
	zx_cls_attr(PAPER_BLACK | INK_BLACK);
	colour_title();
	printf("  -= HIGH SCORES =-\n\n");

	for (i = 0; i < NUM_HISCORES; i++) {
		r = 3 + i * 2;
		if (hiscores[i] == 0) {
			gotoxy(6, r);
			printf("%d.  ----", i + 1);
		} else {
			gotoxy(6, r);
			printf("%d.  %04d  Lv %d", i + 1,
			       hiscores[i], hilevel[i]);
		}
		attr = (i == rank) ? (BRIGHT | INK_GREEN | PAPER_BLACK)
		                    : HISCORE_ATTR;
		{
			int c;
			for (c = 4; c < 28; c++)
				set_attr(r, c, attr);
		}
	}

	gotoxy(4, 16);
	printf("  Press any key...");
	{
		int c;
		for (c = 4; c < 28; c++)
			set_attr(16, c, TITLE_ATTR);
	}

#ifdef __HAVE_KEYBOARD
	fgetc_cons();
#endif
}

/* BFS on maze cell grid (14x9=126 cells instead of 29x19=551).
   Returns: 0=left,1=right,2=up,3=down, -1=no path */
int enemy_bfs()
{
	int head, tail;
	int ci, ni;
	int ecx, ecy, pcx, pcy;
	int efi;
	int cr, cc;
	unsigned char d;

	/* Convert expanded grid to maze cell coords (only valid at odd pos) */
	ecx = ex >> 1;  ecy = ey >> 1;
	pcx = px >> 1;  pcy = py >> 1;

	efi = ecy * COLS + ecx;

	/* Clear vis (may be dirty from generate_maze or previous BFS) */
	for (head = 0; head < ROWS * COLS; head++)
		vis[head] = 0;

	head = 0;
	tail = 0;
	ci = pcy * COLS + pcx;
	stk[tail++] = ci;
	vis[ci] = 5;

	while (head < tail) {
		ci = stk[head++];

		if (ci == efi) {
			d = vis[efi] - 1;
			for (ci = 0; ci < tail; ci++)
				vis[stk[ci]] = 0;
			if (d == 0) return 1;  /* was left, enemy goes right */
			if (d == 1) return 0;  /* was right, enemy goes left */
			if (d == 2) return 3;  /* was up, enemy goes down */
			if (d == 3) return 2;  /* was down, enemy goes up */
			return -1;
		}

		cr = ci / COLS;
		cc = ci - cr * COLS;

		/* Left */
		if (cc > 0) {
			ni = ci - 1;
			if (!vis[ni] && !(walls[cr][cc - 1] & 1)) {
				vis[ni] = 1;
				stk[tail++] = ni;
			}
		}
		/* Right */
		if (cc < COLS - 1) {
			ni = ci + 1;
			if (!vis[ni] && !(walls[cr][cc] & 1)) {
				vis[ni] = 2;
				stk[tail++] = ni;
			}
		}
		/* Up */
		if (cr > 0) {
			ni = ci - COLS;
			if (!vis[ni] && !(walls[cr - 1][cc] & 2)) {
				vis[ni] = 3;
				stk[tail++] = ni;
			}
		}
		/* Down */
		if (cr < ROWS - 1) {
			ni = ci + COLS;
			if (!vis[ni] && !(walls[cr][cc] & 2)) {
				vis[ni] = 4;
				stk[tail++] = ni;
			}
		}
	}
	for (ci = 0; ci < tail; ci++)
		vis[stk[ci]] = 0;
	return -1;
}

/* Last direction the enemy moved (for corridor continuation) */
int last_edir;
/* Cached BFS direction and remaining uses */
int cached_dir;
int cached_steps;

/* Pick a random valid direction from (ex,ey) on expanded grid */
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

/* Move enemy one step.
   At odd position (maze cell): use cached BFS, recalc, or random.
   At even position (corridor): continue in same direction. */
void move_enemy()
{
	int dir;
	int old_ex, old_ey;
	int fi;

	if ((ex & 1) && (ey & 1)) {
		/* At maze cell — 25% chase, 75% random */
		if (rng() % 4 == 0)
			dir = enemy_bfs();
		else
			dir = enemy_random_dir();
	} else {
		/* In corridor between cells — keep going */
		dir = last_edir;
	}
	if (dir < 0) return;
	last_edir = dir;

	old_ex = ex;
	old_ey = ey;
	erase_enemy(ex, ey);

	if (dir == 0) ex--;
	else if (dir == 1) ex++;
	else if (dir == 2) ey--;
	else if (dir == 3) ey++;

	/* Redraw exit/player/coin if enemy was on their cell */
	if (old_ex == EXIT_GX && old_ey == EXIT_GY)
		draw_exit(EXIT_GX, EXIT_GY);
	if (old_ex == px && old_ey == py)
		draw_dot(px, py);
	/* Redraw coin if enemy left a coin cell */
	if ((old_ex & 1) && (old_ey & 1)) {
		int ccx, ccy;
		ccx = old_ex >> 1;
		ccy = old_ey >> 1;
		if (coinmap[ccy * COLS + ccx])
			draw_coin(ccx, ccy);
	}

	draw_enemy(ex, ey);
}

int can_move(int dx, int dy)
{
	return !wallmap[(py + dy) * ECOLS + (px + dx)];
}

/* Enemy move interval — number of main loop ticks between moves */
#define ENEMY_TICK 300

main()
{
	char k;
	int dx, dy;
	int moves;
	int caught;
	unsigned int tick;
	int rank;

	/* Initialize high scores */
	for (rank = 0; rank < NUM_HISCORES; rank++) {
		hiscores[rank] = 0;
		hilevel[rank] = 0;
	}
	level = 0;
	score = 0;

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

		level++;
		zx_cls_attr(INK_WHITE | PAPER_BLACK);
		clear_pixels();
		printf("  MAZE Lv%d  O/P/Q/A\n", level);
		colour_title();

		generate_maze();
		add_extra_passages();
		draw_maze();

		/* Positions in expanded grid coords */
		px = 1;
		py = 1;
		ex = 2 * COLS - 1;
		ey = 1;
		moves = 0;
		caught = 0;
		tick = 0;
		last_edir = 0;
		cached_steps = 0;

		/* Place coins and draw everything */
		place_coins();
		draw_exit(EXIT_GX, EXIT_GY);
		draw_enemy(ex, ey);
		draw_dot(px, py);
		show_score();

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

					/* Collect coin? */
					if (try_collect_coin(px, py)) {
						snd_coin();
						show_score();
					}

					/* Player walked onto enemy? */
					if (px == ex && py == ey)
						caught = 1;

					if (px == EXIT_GX &&
					    py == EXIT_GY) {
						/* Bonus: 50 pts for escaping */
						score += 50;
						snd_win();
						printf("\n\n\n\n\n\n\n\n\n\n\n");
						printf("\n\n  ESCAPED! +50");
						printf(" Score:%d\n", score);
						printf("  Any key=next maze");
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

				/* Wait for key release */
#ifdef __HAVE_KEYBOARD
				while (getk()) ;
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
				printf(" Score:%d\n", score);
				printf("  Any key...");
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
				/* Game over: show high scores */
				rank = update_hiscores();
				show_hiscores(rank);
				/* Reset for new game */
				score = 0;
				level = 0;
				break;
			}
		}
	}
}
