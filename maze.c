#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <graphics.h>
#include <conio.h>
#include <features.h>
#include <sound.h>
#include <intrinsic.h>
#include <arch/zx/spectrum.h>

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

/* Screen address from char cell (sr, sc) — avoids repeating the formula */
#define SCR_ADDR(sr, sc) \
	(16384u + ((unsigned int)((sr) >> 3) << 11) \
	 + ((unsigned int)((sr) & 7) << 5) + (sc))

/* bit 0 = right wall, bit 1 = bottom wall */
unsigned char walls[ROWS][COLS];
/* Precomputed wall map: 1=wall, 0=passable. Indexed as [gy*ECOLS+gx]. */
unsigned char wallmap[EROWS * ECOLS];
unsigned char px, py;    /* player position in expanded grid */
unsigned char enx[2], eny[2];    /* enemy positions in expanded grid */
unsigned char last_edir_arr[2];  /* last direction each enemy moved */
unsigned int rseed;

/* Coin map: 1=coin present at maze cell (cx,cy). Index = cy*COLS+cx */
unsigned char coinmap[ROWS * COLS];
int score;
unsigned char coins_left;
unsigned char level;

/* High scores table */
int hiscores[NUM_HISCORES];
unsigned char hilevel[NUM_HISCORES];

/* Buffer for formatting text */
char txt_buffer[TEXT_SCR_WIDTH + 1];

#define ATTR_BASE   22528
#define ATTR_P_ADDR 23693
#define WALL_ATTR   (BRIGHT | INK_RED | PAPER_YELLOW)
#define CORR_ATTR   (INK_BLACK | PAPER_BLACK)
#define PLAYER_ATTR (BRIGHT | INK_GREEN | PAPER_BLACK)
#define ENEMY_ATTR  (BRIGHT | INK_RED | PAPER_BLACK)
#define ENEMY2_ATTR (BRIGHT | INK_MAGENTA | PAPER_BLACK)
#define EXIT_ATTR   (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define COIN_ATTR   (BRIGHT | INK_YELLOW | PAPER_BLACK)
#define TITLE_ATTR  (BRIGHT | INK_YELLOW | PAPER_BLUE)
#define WIN_ATTR    (BRIGHT | INK_GREEN | PAPER_BLACK)
#define HISCORE_ATTR (BRIGHT | INK_YELLOW | PAPER_BLACK)

/* Exit position in expanded grid */
#define EXIT_GX (2*COLS-1)
#define EXIT_GY (2*ROWS-1)

/* Pre-computed 8-byte bitmaps for sprites (all within one 8x8 cell) */
//don't remove formatting and binary constants, it's more readable this way
/* Brick pattern: red ink on yellow paper */
unsigned char brick[8] = {
	0b11110111, 
	0b11110111, 
	0b11110111, 
	0b00000000,
	0b11011111, 
	0b11011111, 
	0b11011111, 
	0b00000000
};

unsigned char spr_dot[8]  = {
	0b00000000,
	0b00000000,
	0b00111000,
	0b01111100,
	0b01111100,
	0b01111100,
	0b00111000,
	0b00000000
};
unsigned char spr_enemy[8]= {
	0b00000000,
	0b00000000,
	0b00010000,
	0b00101000,
	0b01010100,
	0b00101000,
	0b00010000,
	0b00000000
};
unsigned char spr_exit[8] = {
	0b00000000,
	0b10000010,
	0b01000100,
	0b00101000,
	0b00010000,
	0b00101000,
	0b01000100,
	0b10000010
};
unsigned char spr_coin[8] = {
	0b00000000,
	0b00111100,
	0b01111110,
	0b01111110,
	0b00111100,
	0b00111100,
	0b00011000,
	0b00000000
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
	base = (unsigned char *)SCR_ADDR(sr, sc);
	for (i = 0; i < 8; i++) {
		*base = brick[i];
		base += 256;
	}
	set_attr(sr, sc, WALL_ATTR);
}

/* Clear all pixel data (6144 bytes at 16384) */
void clear_pixels()
{
	memset((unsigned char *)16384u, 0, 6144u);
}

unsigned int rng()
{
	rseed = rseed * 181 + 359;
	return rseed >> 2;
}

/* Visited flags and explicit stack for DFS and BFS.
   Sized for expanded grid (largest use case). */
unsigned char vis[EROWS * ECOLS];
unsigned char stk[EROWS * ECOLS];
unsigned char sp;

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

/* Write an 8-byte bitmap into character cell (sr, sc) and set attr. */
void draw_sprite(unsigned char sr, unsigned char sc,
                 unsigned char *spr, unsigned char attr)
{
	unsigned char *base;
	unsigned char i;
	base = (unsigned char *)SCR_ADDR(sr, sc);
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
	base = (unsigned char *)SCR_ADDR(sr, sc);
	for (i = 0; i < 8; i++) {
		*base = 0;
		base += 256;
	}
	set_attr(sr, sc, CORR_ATTR);
}

void draw_dot(unsigned char gx, unsigned char gy)
{
	draw_sprite(SROW(gy), SCOL(gx), spr_dot, PLAYER_ATTR);
}

void erase_dot(unsigned char gx, unsigned char gy)
{
	clear_cell(SROW(gy), SCOL(gx));
}

void draw_exit(unsigned char gx, unsigned char gy)
{
	draw_sprite(SROW(gy), SCOL(gx), spr_exit, EXIT_ATTR);
}

void snd_step()
{
	bit_beep(0, 400);
	//otherwise keyboard will stop responding - seems that bit_beep disables interrupts
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
	//intrinsic_ei();
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

void draw_enemy(unsigned char gx, unsigned char gy)
{
	draw_sprite(SROW(gy), SCOL(gx), spr_enemy, ENEMY_ATTR);
}

void erase_enemy(unsigned char gx, unsigned char gy)
{
	clear_cell(SROW(gy), SCOL(gx));
}

void draw_coin(unsigned char cx, unsigned char cy)
{
	unsigned char gx, gy;
	gx = (cx << 1) + 1;
	gy = (cy << 1) + 1;
	draw_sprite(SROW(gy), SCOL(gx), spr_coin, COIN_ATTR);
}

/* Place coins on ~40% of maze cells, avoiding start/exit/enemies */
void place_coins()
{
	int cx, cy, gx, gy;
	coins_left = 0;
	memset(coinmap, 0, ROWS * COLS);

	for (cy = 0; cy < ROWS; cy++)
		for (cx = 0; cx < COLS; cx++) {
			gx = cx * 2 + 1;
			gy = cy * 2 + 1;
			/* Skip player, exit, enemies */
			if (gx == px && gy == py) continue;
			if (gx == EXIT_GX && gy == EXIT_GY) continue;
			if (gx == enx[0] && gy == eny[0]) continue;
			if (gx == enx[1] && gy == eny[1]) continue;
			if (rng() % 5 < 2) {
				coinmap[cy * COLS + cx] = 1;
				coins_left++;
				draw_coin(cx, cy);
			}
		}
}

/* Check and collect coin at expanded grid position (gx,gy) */
unsigned char try_collect_coin(unsigned char gx, unsigned char gy)
{
	unsigned char cx, cy;
	if (!(gx & 1) || !(gy & 1)) return 0;  /* not a cell center */
	cx = gx >> 1;
	cy = gy >> 1;
	if (coinmap[(unsigned int)cy * COLS + cx]) {
		coinmap[(unsigned int)cy * COLS + cx] = 0;
		coins_left--;
		score += 10;
		return 1;
	}
	return 0;
}

/* Display score on the title row */
void show_score()
{
	int len = sprintf(txt_buffer, "SCORE: %06d", score);
	gotoxy(center_x(len), 22); printf(txt_buffer);
}

/* Update high scores table, return rank (0-based) or -1 */
int update_hiscores()
{
	int i, j;
	for (i = 0; i < NUM_HISCORES; i++) {
		if (score > hiscores[i]) {
			/* Shift lower scores down */
			for (j = NUM_HISCORES - 1; j > i; j--) {
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
void show_hiscores(char rank)
{
	int i, r, c;
	unsigned char attr;

	clear_pixels();
	zx_cls_attr(PAPER_BLACK | INK_WHITE);
	int len = sprintf(txt_buffer, "-= HIGH SCORES =-");
	gotoxy(center_x(len), 1); printf(txt_buffer);

	for (i = 0; i < NUM_HISCORES; i++) {
		r = 3 + (i << 1);
		if (hiscores[i] == 0) {
			gotoxy(6, r);
			printf("%d.  ----", i + 1);
		} else {
			gotoxy(6, r);
			printf("%d.  %06d  Level %d", i + 1,
			       hiscores[i], hilevel[i]);
		}
		attr = (i == rank) ? (BRIGHT | INK_GREEN | PAPER_BLACK)
		                    : HISCORE_ATTR;
		for (c = 4; c < 28; c++)
			set_attr(r, c, attr);
	}

	gotoxy(9, 16);
	printf("Press any key...");
	for (c = 4; c < 28; c++)
		set_attr(16, c, TITLE_ATTR);

	fgetc_cons();
}

/* BFS on maze cell grid (14x9=126 cells instead of 29x19=551).
   Returns: 0=left,1=right,2=up,3=down, -1=no path */
int enemy_bfs(int exx, int eyy)
{
	int head, tail;
	int ci, ni;
	int ecx, ecy, pcx, pcy;
	int efi;
	int cr, cc;
	unsigned char d;

	/* Convert expanded grid to maze cell coords (only valid at odd pos) */
	ecx = exx >> 1;  ecy = eyy >> 1;
	pcx = px >> 1;  pcy = py >> 1;

	efi = ecy * COLS + ecx;

	/* Clear vis (may be dirty from generate_maze or previous BFS) */
	memset(vis, 0, ROWS * COLS);

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

/* Pick a random valid direction from (exx,eyy) on expanded grid */
int enemy_random_dir(int exx, int eyy)
{
	int dirs[4], nd;
	int fi;
	fi = eyy * ECOLS + exx;
	nd = 0;
	if (!wallmap[fi - 1])     dirs[nd++] = 0;
	if (!wallmap[fi + 1])     dirs[nd++] = 1;
	if (!wallmap[fi - ECOLS]) dirs[nd++] = 2;
	if (!wallmap[fi + ECOLS]) dirs[nd++] = 3;
	if (nd == 0) return -1;
	return dirs[rng() % nd];
}

/* Move enemy n (0 or 1) one step.
   At odd position (maze cell): use cached BFS, recalc, or random.
   At even position (corridor): continue in same direction. */
void move_enemy(int n)
{
	int dir;
	int old_ex, old_ey;
	int other;
	unsigned char attr;

	old_ex = enx[n];
	old_ey = eny[n];
	attr = (n == 0) ? ENEMY_ATTR : ENEMY2_ATTR;

	if ((old_ex & 1) && (old_ey & 1)) {
		/* At maze cell — 25% chase, 75% random */
		if (rng() % 4 == 0)
			dir = enemy_bfs(old_ex, old_ey);
		else
			dir = enemy_random_dir(old_ex, old_ey);
	} else {
		/* In corridor between cells — keep going */
		dir = last_edir_arr[n];
	}
	if (dir < 0) return;
	last_edir_arr[n] = dir;

	erase_enemy(old_ex, old_ey);

	if (dir == 0) enx[n]--;
	else if (dir == 1) enx[n]++;
	else if (dir == 2) eny[n]--;
	else if (dir == 3) eny[n]++;

	/* Redraw exit/player/coin/other enemy if this enemy left their cell */
	if (old_ex == EXIT_GX && old_ey == EXIT_GY)
		draw_exit(EXIT_GX, EXIT_GY);
	if (old_ex == px && old_ey == py)
		draw_dot(px, py);
	other = 1 - n;
	if (old_ex == enx[other] && old_ey == eny[other])
		draw_sprite(SROW(eny[other]), SCOL(enx[other]), spr_enemy,
		            (other == 0) ? ENEMY_ATTR : ENEMY2_ATTR);
	/* Redraw coin if enemy left a coin cell (coin may still exist
	   if enemy arrived from a corridor step, not a cell center) */
	if ((old_ex & 1) && (old_ey & 1)) {
		int ccx, ccy;
		ccx = old_ex >> 1;
		ccy = old_ey >> 1;
		if (coinmap[ccy * COLS + ccx])
			draw_coin(ccx, ccy);
	}

	/* Enemy eats coin if it lands on one */
	if ((enx[n] & 1) && (eny[n] & 1)) {
		int ccx, ccy;
		ccx = enx[n] >> 1;
		ccy = eny[n] >> 1;
		if (coinmap[ccy * COLS + ccx]) {
			coinmap[ccy * COLS + ccx] = 0;
			coins_left--;
		}
	}

	draw_sprite(SROW(eny[n]), SCOL(enx[n]), spr_enemy, attr);
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
		cx = rng() % COLS;
		cy = rng() % ROWS;
		*gx = (cx << 1) + 1;
		*gy = (cy << 1) + 1;
	} while ((*gx == ox1 && *gy == oy1) ||
	         (*gx == ox2 && *gy == oy2) ||
	         (*gx == EXIT_GX && *gy == EXIT_GY));
}

unsigned char can_move(char dx, char dy)
{
	return !wallmap[(unsigned int)(py + dy) * ECOLS + px + dx];
}

/* Draw a popup window background: rows 10-14, cols 5-26.
   Border cells get brick pixels + border_attr; inner cells are cleared + inner_attr.
   Params are int to avoid sccz80 unsigned char corruption across set_attr calls. */
void draw_popup_bg(int border_attr, int inner_attr)
{
	int r, c, i;
	unsigned char *base;
	for (r = 10; r <= 14; r++) {
		for (c = 5; c <= 26; c++) {
			base = (unsigned char *)SCR_ADDR(r, c);
			if (r == 10 || r == 14 || c == 5 || c == 26) {
				for (i = 0; i < 8; i++) {
					*base = brick[i];
					base += 256;
				}
				set_attr(r, c, border_attr);
			} else {
				for (i = 0; i < 8; i++) {
					*base = 0;
					base += 256;
				}
				set_attr(r, c, inner_attr);
			}
		}
	}
}

/* Re-apply inner_attr to text rows 11-13, cols 6-25 (undoes printf attr side-effects).
   Param is int to avoid sccz80 unsigned char corruption across set_attr calls. */
void popup_fix_attrs(int inner_attr)
{
	int c;
	for (c = 6; c <= 25; c++) {
		set_attr(11, c, inner_attr);
		set_attr(12, c, inner_attr);
		set_attr(13, c, inner_attr);
	}
}

int center_x(int text_len)
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
	
	int len = sprintf(txt_buffer, "** ESCAPED! **");
	gotoxy(center_x(len), 11); printf(txt_buffer);
	len = sprintf(txt_buffer, "Score: %d", score);
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
	int len = sprintf(txt_buffer, "** CAUGHT! **");
	gotoxy(center_x(len), 11); printf(txt_buffer);
	len = sprintf(txt_buffer, "Score: %d", score);
	gotoxy(center_x(len), 12); printf(txt_buffer);
	len = sprintf(txt_buffer, "Press any key...");
	gotoxy(center_x(len), 13); printf(txt_buffer);
	popup_fix_attrs(BRIGHT | INK_WHITE | PAPER_RED);
}

/* Enemy move interval — number of main loop ticks between moves */
#define ENEMY_TICK 300

main()
{
	char k;
	int dx, dy;
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
		zx_border(INK_BLACK);

		/* Seed RNG from keypress timing */
		rseed = 0;
		while (!getk())
			rseed++;
		while (getk()) ;
		if (rseed == 0) rseed = 42;

		level++;
		zx_cls_attr(INK_WHITE | PAPER_BLACK);
		clear_pixels();
		int len = sprintf(txt_buffer, "MAZE Level %d  O/P/Q/A", level);
		gotoxy(center_x(len), 23); printf(txt_buffer);

		generate_maze();
		add_extra_passages();
		draw_maze();

		/* Random starting positions */
		random_start(&px, &py, 255, 255, 255, 255);
		random_start(&enx[0], &eny[0], px, py, 255, 255);
		random_start(&enx[1], &eny[1], px, py, enx[0], eny[0]);
		caught = 0;
		tick = 0;
		last_edir_arr[0] = 0;
		last_edir_arr[1] = 0;

		/* Place coins and draw everything */
		place_coins();
		draw_exit(EXIT_GX, EXIT_GY);
		draw_sprite(SROW(eny[0]), SCOL(enx[0]), spr_enemy, ENEMY_ATTR);
		draw_sprite(SROW(eny[1]), SCOL(enx[1]), spr_enemy, ENEMY2_ATTR);
		draw_dot(px, py);
		show_score();

		while (1) {
			k = getk();
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
					draw_dot(px, py);

					/* Collect coin? */
					if (try_collect_coin(px, py)) {
						zx_border(INK_YELLOW);
						snd_coin();
						zx_border(INK_BLACK);
						show_score();
					}

					/* Player walked onto enemy? */
					if ((px == enx[0] && py == eny[0]) ||
					    (px == enx[1] && py == eny[1]))
						caught = 1;

					if (px == EXIT_GX &&
					    py == EXIT_GY) {
						/* Bonus: 50 pts for escaping */
						score += 50;
						win_cut_scene();
						fgetc_cons();
						break;
					}

					snd_step();
					tick = 0; /* reset tick so enemy doesn't
					             move right after player */
				} else {
					snd_bump();
				}

				/* Wait for key release */
				while (getk()) ;
			}

			/* Enemy moves on its own timer */
			tick++;
			if (tick >= ENEMY_TICK) {
				tick = 0;
				move_enemy(0);
				move_enemy(1);

				if ((enx[0] == px && eny[0] == py) ||
				    (enx[1] == px && eny[1] == py))
					caught = 1;
			}

			if (caught) {
				game_over_cut_scene();
				fgetc_cons();
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
