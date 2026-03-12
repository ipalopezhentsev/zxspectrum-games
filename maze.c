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
unsigned int rseed;

#define ATTR_BASE   22528
#define ATTR_P_ADDR 23693
#define WALL_ATTR   (BRIGHT | INK_RED | PAPER_YELLOW)
#define CORR_ATTR   (INK_BLACK | PAPER_BLACK)
#define PLAYER_ATTR (BRIGHT | INK_GREEN | PAPER_BLACK)
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

void draw_dot(int gx, int gy)
{
	int dx, dy;
	int pcx = ICX(gx);
	int pcy = ICY(gy);
	for (dy = -2; dy <= 2; dy++)
		for (dx = -2; dx <= 2; dx++)
			if (dx * dx + dy * dy <= 5)
				plot(pcx + dx, pcy + dy);
}

void erase_dot(int gx, int gy)
{
	int dx, dy;
	int pcx = ICX(gx);
	int pcy = ICY(gy);
	for (dy = -2; dy <= 2; dy++)
		for (dx = -2; dx <= 2; dx++)
			if (dx * dx + dy * dy <= 5)
				unplot(pcx + dx, pcy + dy);
}

/* Draw X marker for the exit — radius 3 to fit interior cell */
void draw_exit(int gx, int gy)
{
	int i;
	int pcx = ICX(gx);
	int pcy = ICY(gy);
	for (i = -3; i <= 3; i++) {
		plot(pcx + i, pcy + i);
		plot(pcx + i, pcy - i);
	}
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

void snd_win()
{
	bit_beep(15, 150);
	intrinsic_ei();
	bit_beep(15, 120);
	intrinsic_ei();
	bit_beep(30, 80);
	intrinsic_ei();
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

main()
{
	char k;
	int dx, dy;
	int moves;

	while (1) {
		zx_border(INK_BLUE);
		zx_cls_attr(INK_WHITE | PAPER_BLACK);
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
		printf("  MAZE  O/P/Q/A=move\n");
		colour_title();

		generate_maze();
		draw_maze();

		px = 0;
		py = 0;
		moves = 0;

		/* Draw exit and player in their corridor cells */
		*((unsigned char *)ATTR_P_ADDR) = EXIT_ATTR;
		draw_exit(COLS - 1, ROWS - 1);

		*((unsigned char *)ATTR_P_ADDR) = PLAYER_ATTR;
		draw_dot(px, py);

		*((unsigned char *)ATTR_P_ADDR) = CORR_ATTR;

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
					set_attr(IROW(py), ICOL(px),
					         CORR_ATTR);
					px += dx;
					py += dy;
					moves++;
					*((unsigned char *)ATTR_P_ADDR) =
						PLAYER_ATTR;
					draw_dot(px, py);
					*((unsigned char *)ATTR_P_ADDR) =
						CORR_ATTR;

					if (px == COLS - 1 &&
					    py == ROWS - 1) {
						snd_win();
						printf("\n\n\n\n\n\n\n\n\n\n\n");
						printf("\n\n  YOU WIN!");
						printf(" Moves: %d\n", moves);
						printf("  Any key=new maze");
						/* Colour win text area */
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
				} else {
					snd_bump();
				}

				/* Wait for key release */
#ifdef __HAVE_KEYBOARD
				while (getk()) ;
#endif
			}
		}
	}
}
