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

/* Maze grid origin in character cells */
#define MAZE_R0 2
#define MAZE_C0 1

/* bit 0 = right wall, bit 1 = bottom wall */
unsigned char walls[ROWS][COLS];
int px, py;
int ex, ey;  /* enemy position */
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

/* Screen char row/col of corridor cell for maze cell (gx,gy) */
#define IROW(gy) (MAZE_R0 + 1 + (gy) * 2)
#define ICOL(gx) (MAZE_C0 + 1 + (gx) * 2)

/* Pixel center of corridor cell (plot uses y=0 at top) */
#define ICX(gx) (ICOL(gx) * 8 + 3)
#define ICY(gy) (IROW(gy) * 8 + 4)

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

/* Visited flags and explicit stack for DFS
   (no recursion — Z80 stack is tiny) */
unsigned char visited[ROWS][COLS];
unsigned char stack[COLS * ROWS];
int sp;

void generate_maze()
{
	int x, y, cx, cy, nx, ny;
	int dirs[4], nd, i, j, t;

	for (y = 0; y < ROWS; y++)
		for (x = 0; x < COLS; x++) {
			walls[y][x] = 3;
			visited[y][x] = 0;
		}

	/* Recursive backtracker using explicit stack */
	cx = 0; cy = 0;
	visited[0][0] = 1;
	sp = 0;
	stack[sp++] = 0;

	while (sp > 0) {
		/* Collect unvisited neighbors */
		nd = 0;
		if (cx > 0 && !visited[cy][cx-1])
			dirs[nd++] = 0;
		if (cx < COLS-1 && !visited[cy][cx+1])
			dirs[nd++] = 1;
		if (cy > 0 && !visited[cy-1][cx])
			dirs[nd++] = 2;
		if (cy < ROWS-1 && !visited[cy+1][cx])
			dirs[nd++] = 3;

		if (nd == 0) {
			/* Backtrack */
			sp--;
			if (sp > 0) {
				t = stack[sp - 1];
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

		visited[ny][nx] = 1;
		cx = nx; cy = ny;
		stack[sp++] = cy * COLS + cx;
	}
}

void draw_maze()
{
	int gr, gc, sr, sc;
	int is_wall;

	for (gr = 0; gr < 2 * ROWS + 1; gr++) {
		for (gc = 0; gc < 2 * COLS + 1; gc++) {
			sr = MAZE_R0 + gr;
			sc = MAZE_C0 + gc;
			is_wall = 0;

			if (!(gr & 1) && !(gc & 1)) {
				/* Post — always brick */
				is_wall = 1;
			}
			else if (!(gr & 1) && (gc & 1)) {
				/* Horizontal wall segment */
				if (gr == 0 || gr == 2 * ROWS)
					is_wall = 1;
				else if (walls[gr/2 - 1][gc/2] & 2)
					is_wall = 1;
			}
			else if ((gr & 1) && !(gc & 1)) {
				/* Vertical wall segment */
				if (gc == 0 || gc == 2 * COLS)
					is_wall = 1;
				else if (walls[gr/2][gc/2 - 1] & 1)
					is_wall = 1;
			}

			if (is_wall)
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

/* Write an 8-byte bitmap into character cell (sr, sc) and set attr.
   Clears the cell first so no stale pixels remain. */
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
	draw_sprite(IROW(gy), ICOL(gx), spr_dot, PLAYER_ATTR);
}

void erase_dot(int gx, int gy)
{
	clear_cell(IROW(gy), ICOL(gx));
}

void draw_exit(int gx, int gy)
{
	draw_sprite(IROW(gy), ICOL(gx), spr_exit, EXIT_ATTR);
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
	draw_sprite(IROW(gy), ICOL(gx), spr_enemy, ENEMY_ATTR);
}

void erase_enemy(int gx, int gy)
{
	clear_cell(IROW(gy), ICOL(gx));
}

/* BFS from enemy to player; returns direction for enemy's next step.
   Reuses visited[] and stack[] arrays (not needed after maze gen).
   Returns: 0=left,1=right,2=up,3=down, -1=no path */
int enemy_bfs()
{
	int head, tail;
	int cx, cy, nx, ny, d, idx;
	/* parent stores the direction taken TO reach each cell */
	/* We encode (dir+1) so 0 means unvisited */
	/* Use visited[] as parent-direction storage */

	for (cy = 0; cy < ROWS; cy++)
		for (cx = 0; cx < COLS; cx++)
			visited[cy][cx] = 0;

	/* BFS from player back to enemy, so we get the first step */
	head = 0;
	tail = 0;
	stack[tail++] = py * COLS + px;
	visited[py][px] = 5; /* mark as origin */

	while (head < tail) {
		idx = stack[head++];
		cx = idx % COLS;
		cy = idx / COLS;

		if (cx == ex && cy == ey) {
			/* Trace back: visited[ey][ex] has the dir used to
			   reach enemy FROM player-side. Reverse it. */
			d = visited[ey][ex] - 1;
			/* d is the direction from neighbor to enemy;
			   enemy should move opposite */
			if (d == 0) return 1;  /* came from left, go right */
			if (d == 1) return 0;  /* came from right, go left */
			if (d == 2) return 3;  /* came from up, go down */
			if (d == 3) return 2;  /* came from down, go up */
			return -1;
		}

		/* Try 4 directions: left(0) right(1) up(2) down(3) */
		/* Left */
		nx = cx - 1; ny = cy;
		if (nx >= 0 && !visited[ny][nx] && !(walls[cy][nx] & 1)) {
			visited[ny][nx] = 1; /* dir 0 + 1 */
			stack[tail++] = ny * COLS + nx;
		}
		/* Right */
		nx = cx + 1; ny = cy;
		if (nx < COLS && !visited[ny][nx] && !(walls[cy][cx] & 1)) {
			visited[ny][nx] = 2; /* dir 1 + 1 */
			stack[tail++] = ny * COLS + nx;
		}
		/* Up */
		nx = cx; ny = cy - 1;
		if (ny >= 0 && !visited[ny][nx] && !(walls[ny][cx] & 2)) {
			visited[ny][nx] = 3; /* dir 2 + 1 */
			stack[tail++] = ny * COLS + nx;
		}
		/* Down */
		nx = cx; ny = cy + 1;
		if (ny < ROWS && !visited[ny][nx] && !(walls[cy][cx] & 2)) {
			visited[ny][nx] = 4; /* dir 3 + 1 */
			stack[tail++] = ny * COLS + nx;
		}
	}
	return -1;
}

/* Pick a random valid direction from (ex,ey) */
int enemy_random_dir()
{
	int dirs[4], nd, i, j, t;
	nd = 0;
	if (ex > 0 && !(walls[ey][ex-1] & 1))        dirs[nd++] = 0;
	if (ex < COLS-1 && !(walls[ey][ex] & 1))      dirs[nd++] = 1;
	if (ey > 0 && !(walls[ey-1][ex] & 2))         dirs[nd++] = 2;
	if (ey < ROWS-1 && !(walls[ey][ex] & 2))      dirs[nd++] = 3;
	if (nd == 0) return -1;
	return dirs[rng() % nd];
}

/* Move enemy one step — 50% chance to chase, 50% random wander */
void move_enemy()
{
	int dir;
	if (rng() % 2 == 0)
		dir = enemy_bfs();
	else
		dir = enemy_random_dir();
	if (dir < 0) return;

	{
		int old_ex = ex, old_ey = ey;

		erase_enemy(ex, ey);

		if (dir == 0) ex--;
		else if (dir == 1) ex++;
		else if (dir == 2) ey--;
		else if (dir == 3) ey++;

		/* Redraw exit/player if enemy was on their cell */
		if (old_ex == COLS - 1 && old_ey == ROWS - 1)
			draw_exit(COLS - 1, ROWS - 1);
		if (old_ex == px && old_ey == py)
			draw_dot(px, py);

		draw_enemy(ex, ey);
	}
}

int can_move(int dx, int dy)
{
	int nx = px + dx;
	int ny = py + dy;
	if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS)
		return 0;
	if (dx == 1)  return !(walls[py][px] & 1);
	if (dx == -1) return !(walls[py][nx] & 1);
	if (dy == 1)  return !(walls[py][px] & 2);
	if (dy == -1) return !(walls[ny][px] & 2);
	return 0;
}

/* Enemy move interval — number of main loop ticks between moves */
#define ENEMY_TICK 800

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

		px = 0;
		py = 0;
		ex = COLS - 1;
		ey = 0;
		moves = 0;
		caught = 0;
		tick = 0;

		/* Draw exit, enemy, and player */
		draw_exit(COLS - 1, ROWS - 1);
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

					if (px == COLS - 1 &&
					    py == ROWS - 1) {
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
