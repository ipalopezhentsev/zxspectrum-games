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

/* Pointer goto next 8x8 screen row (increment only high byte).
   This avoids a full 16-bit add in tight sprite draw loops. */
#define SC_NEXT_LINE(p) (((unsigned char *)&p)[1]++)

/* bit 0 = right wall, bit 1 = bottom wall */
unsigned char walls[ROWS][COLS];
/* Precomputed wall map: 1=wall, 0=passable. Indexed as [gy*ECOLS+gx]. */
unsigned char wallmap[EROWS * ECOLS];
unsigned char px, py;    /* player position in expanded grid */
unsigned char enx[4], eny[4];    /* enemy positions in expanded grid */
unsigned char last_edir_arr[4];  /* last direction each enemy moved */
unsigned char enemy_next;        /* round-robin: which enemy moves next */
unsigned int enemy_accum;        /* Bresenham accumulator for spreading */
unsigned int rseed;

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
int hiscores[NUM_HISCORES];
unsigned char hilevel[NUM_HISCORES];

/* Timer state */
unsigned int timer_sec;    /* seconds remaining */
unsigned char timer_frac;  /* frame counter within current second (0-49) */

/* Buffer for formatting text */
char txt_buffer[TEXT_SCR_WIDTH + 1];

/* Transient globals for inline-asm sprite routines */
unsigned int ds_scr;
unsigned char *ds_spr_ptr;
unsigned int ds_attr_a;
unsigned char ds_attr_v;
unsigned char ds_row;
unsigned char ds_col;

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

#define ATTR_BASE   22528
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


/* Set attribute for character cell (row, col).
   Inline asm computes addr via 5 shifts (55 T-states)
   instead of library multiply by 32 (~200 T-states). */
void set_attr(unsigned char row, unsigned char col,
              unsigned char attr)
{
	ds_row = row;
	ds_col = col;
	ds_attr_v = attr;

#asm
	; Compute 22528 + row*32 + col
	ld a, (_ds_row)
	ld l, a
	ld h, 0
	add hl, hl
	add hl, hl
	add hl, hl
	add hl, hl
	add hl, hl            ; HL = row * 32
	ld a, (_ds_col)
	add a, l
	ld l, a               ; row*32 low 5 bits are 0, col<32, no carry
	ld de, 22528
	add hl, de            ; HL = attr address
	ld a, (_ds_attr_v)
	ld (hl), a
#endasm
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
	int x, y, i, n;

	/* Pass 1: randomly remove walls → loops (difficulty controls frequency) */
	for (y = 0; y < ROWS; y++)
		for (x = 0; x < COLS; x++) {
			if (x < COLS - 1 && (walls[y][x] & 1) && rand() % extra_wall_pct == 0)
				walls[y][x] &= ~1;
			if (y < ROWS - 1 && (walls[y][x] & 2) && rand() % extra_wall_pct == 0)
				walls[y][x] &= ~2;
		}

	/* Pass 2: create 2x2 halls (difficulty controls count) */
	n = extra_halls_base + rand() % (extra_halls_rng + 1);
	for (i = 0; i < n; i++) {
		x = rand() % (COLS - 1);
		y = rand() % (ROWS - 1);
		/* Remove walls between (x,y), (x+1,y), (x,y+1), (x+1,y+1) */
		walls[y][x]     &= ~3;  /* right + bottom */
		walls[y][x + 1] &= ~2;  /* bottom */
		walls[y + 1][x] &= ~1;  /* right */
	}
}

void draw_brick(unsigned char sr, unsigned char sc);

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
				if (gr == 0 || gr == (ROWS << 1) ||
				    gc == 0 || gc == (COLS << 1)) {
					w = 1;
				} else {
					/* Interior corner post: wall only if
					   any adjacent wall segment exists */
					int cy, cx;
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
				draw_brick(sr, sc);
			else
				set_attr(sr, sc, CORR_ATTR);
		}
	}
}

/* Write an 8-byte bitmap into character cell (sr, sc) and set attr.
   Unrolled inline asm: INC H steps to the next pixel row within
   the same character cell (rows are 256 bytes apart in screen RAM). */
void draw_sprite(unsigned char sr, unsigned char sc,
                 unsigned char *spr, unsigned char attr)
{
	ds_scr = SCR_ADDR(sr, sc);
	ds_spr_ptr = spr;
	ds_row = sr;
	ds_col = sc;
	ds_attr_v = attr;

#asm
	ld hl, (_ds_spr_ptr)
	ex de, hl           ; DE = sprite data
	ld hl, (_ds_scr)    ; HL = screen address

	ld a, (de)
	ld (hl), a
	inc h
	inc de
	ld a, (de)
	ld (hl), a
	inc h
	inc de
	ld a, (de)
	ld (hl), a
	inc h
	inc de
	ld a, (de)
	ld (hl), a
	inc h
	inc de
	ld a, (de)
	ld (hl), a
	inc h
	inc de
	ld a, (de)
	ld (hl), a
	inc h
	inc de
	ld a, (de)
	ld (hl), a
	inc h
	inc de
	ld a, (de)
	ld (hl), a

	; Compute attribute address: 22528 + sr * 32 + sc
	ld a, (_ds_row)
	ld l, a
	ld h, 0
	add hl, hl
	add hl, hl
	add hl, hl
	add hl, hl
	add hl, hl          ; HL = sr * 32
	ld a, (_ds_col)
	add a, l
	ld l, a             ; HL = sr * 32 + sc
	ld bc, 22528
	add hl, bc          ; HL = attribute address
	ld a, (_ds_attr_v)
	ld (hl), a
#endasm
}

/* Clear a character cell (just attribute, CORR_ATTR=0).
   Inline asm avoids set_attr function call overhead entirely. */
void clear_cell(unsigned char sr, unsigned char sc)
{
	ds_row = sr;
	ds_col = sc;

#asm
	ld a, (_ds_row)
	ld l, a
	ld h, 0
	add hl, hl
	add hl, hl
	add hl, hl
	add hl, hl
	add hl, hl
	ld a, (_ds_col)
	add a, l
	ld l, a
	ld de, 22528
	add hl, de
	ld (hl), 0            ; CORR_ATTR = INK_BLACK|PAPER_BLACK = 0
#endasm
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

/* Draw a brick-textured 8x8 block at screen char (sr, sc).
   Hardcoded pixel values avoid sprite data reads entirely. */
void draw_brick(unsigned char sr, unsigned char sc)
{
	ds_scr = SCR_ADDR(sr, sc);
	ds_row = sr;
	ds_col = sc;
	ds_attr_v = WALL_ATTR;

#asm
	ld hl, (_ds_scr)

	ld a, 247            ; 0b11110111
	ld (hl), a
	inc h
	ld (hl), a
	inc h
	ld (hl), a
	inc h
	xor a                ; 0x00
	ld (hl), a
	inc h
	ld a, 223            ; 0b11011111
	ld (hl), a
	inc h
	ld (hl), a
	inc h
	ld (hl), a
	inc h
	xor a
	ld (hl), a

	; Compute attribute address: 22528 + sr * 32 + sc
	ld a, (_ds_row)
	ld l, a
	ld h, 0
	add hl, hl
	add hl, hl
	add hl, hl
	add hl, hl
	add hl, hl          ; HL = sr * 32
	ld a, (_ds_col)
	add a, l
	ld l, a             ; HL = sr * 32 + sc
	ld bc, 22528
	add hl, bc          ; HL = attribute address
	ld a, (_ds_attr_v)
	ld (hl), a
#endasm
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
	int target, attempts, idx, cx, cy, gx, gy;
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
			int skip, ei;
			skip = 0;
			for (ei = 0; ei < num_enemies; ei++)
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
	int idx=row_x_cols[cy] + cx;
	if (coinmap[idx]) {
		coinmap[idx] = 0;
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

/* Display remaining time on row 0 */
void show_timer()
{
	int len, c;
	unsigned int m, s;
	unsigned char attr;
	/* divmod 60 via subtraction — faster than library divide on Z80 */
	s = timer_sec;
	m = 0;
	while (s >= 60) { s -= 60; m++; }
	len = sprintf(txt_buffer, "TIME %d:%02d", m, s);
	gotoxy(center_x(len), 0); printf(txt_buffer);
	attr = (timer_sec <= 10) ? TIMER_WARN_ATTR : TIMER_ATTR;
	if (timer_sec <= 10 || timer_sec == time_limit) {
		for (c = 12; c < 20; c++)
			set_attr(0, c, attr);
	}
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

	zx_cls_attr(PAPER_BLACK | INK_WHITE);
	int len = sprintf(txt_buffer, "-= HIGH SCORES =-");
	gotoxy(center_x(len), 1); 
	printf(txt_buffer);

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

/* Build adjacency table from walls[][] for fast BFS.
   Must be called after generate_maze() + add_extra_passages(). */
void build_adj()
{
	int i, r, c, idx;
	for (i = 0; i < ROWS * COLS; i++) {
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
int enemy_bfs(int exx, int eyy)
{
	int ecx, ecy, pcx, pcy;
	int ci;
	unsigned char d;

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
		uint i;
		for (i = 0; i < bfs_tail_g; i++)
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
int enemy_random_dir(int exx, int eyy)
{
	int dirs[4];
	int fi = erow_x_ecols[eyy] + exx;
	int nd = 0;
	if (!wallmap[fi - 1])     dirs[nd++] = 0;
	if (!wallmap[fi + 1])     dirs[nd++] = 1;
	if (!wallmap[fi - ECOLS]) dirs[nd++] = 2;
	if (!wallmap[fi + ECOLS]) dirs[nd++] = 3;
	if (nd == 0) return -1;
	return dirs[rand() % nd];
}

/* Try Manhattan direction toward player from (exx,eyy) on expanded grid.
   Returns direction if passable, -1 if both axes blocked. */
int enemy_manhattan(int exx, int eyy)
{
	int adx, ady, fi, dir1, dir2;
	int ddx, ddy;
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
   At even position (corridor): continue in same direction. */
void move_enemy(uchar n)
{
	char dir;
	uchar old_ex, old_ey;
	int other;
	unsigned char attr;

	old_ex = enx[n];
	old_ey = eny[n];
	attr = (n == 0) ? ENEMY_ATTR : (n == 1) ? ENEMY2_ATTR : (n == 2) ? ENEMY3_ATTR : ENEMY4_ATTR;

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
	if (old_ex == exit_gx && old_ey == exit_gy)
		draw_exit(exit_gx, exit_gy);
	if (old_ex == px && old_ey == py)
		draw_dot(px, py);
	{
		uchar oi;
		unsigned char oa;
		for (oi = 0; oi < num_enemies; oi++) {
			if (oi == n) continue;
			if (old_ex == enx[oi] && old_ey == eny[oi]) {
				oa = (oi == 0) ? ENEMY_ATTR : (oi == 1) ? ENEMY2_ATTR : (oi == 2) ? ENEMY3_ATTR : ENEMY4_ATTR;
				draw_sprite(SROW(eny[oi]), SCOL(enx[oi]), spr_enemy, oa);
			}
		}
	}
	/* Redraw coin if enemy left a coin cell (coin may still exist
	   if enemy arrived from a corridor step, not a cell center) */
	if ((old_ex & 1) && (old_ey & 1)) {
		uchar ccx, ccy;
		ccx = old_ex >> 1;
		ccy = old_ey >> 1;
		if (coinmap[ccy * COLS + ccx])
			draw_coin(ccx, ccy);
	}

	/* Enemy eats coin if it lands on one */
	if ((enx[n] & 1) && (eny[n] & 1)) {
		uchar ccx, ccy;
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
					SC_NEXT_LINE(base);
				}
				set_attr(r, c, border_attr);
			} else {
				for (i = 0; i < 8; i++) {
					*base = 0;
					SC_NEXT_LINE(base);
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
	int len = sprintf(txt_buffer, "** CAUGHT! **");
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
	int len = sprintf(txt_buffer, "** TIME UP! **");
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
	char k;
	int dx, dy;
	int caught;
	unsigned int tick;
	int rank;
	unsigned int key_delay;
	int game_over;

	/* Initialize high scores */
	for (rank = 0; rank < NUM_HISCORES; rank++) {
		hiscores[rank] = 0;
		hilevel[rank] = 0;
	}
	level = 0;
	score = 0;

	/* Init BFS lookup tables (avoids Z80 division by 14 in inner loop) */
	for (rank = 0; rank < ROWS * COLS; rank++) {
		bfs_row[rank] = rank / COLS;
		bfs_col[rank] = rank % COLS;
	}
	/* Init row*COLS lookup table (avoids Z80 multiply by 14) */
	for (rank = 0; rank < ROWS; rank++) {
		row_x_cols[rank] = rank * COLS;
	}
	/* Init row*ECOLS lookup table (avoids Z80 multiply by 29) */
	for (rank = 0; rank < EROWS; rank++) {
		erow_x_ecols[rank] = rank * ECOLS;
	}

	rseed = 0;

	/* Difficulty selection (first pick also seeds RNG) */
	while (1) {
		zx_cls_attr(PAPER_BLACK | INK_WHITE);
		{
			int len, c;
			len = sprintf(txt_buffer, "-= MAZE RUNNER =-");
			gotoxy(center_x(len), 3); printf(txt_buffer);
			for (c = 6; c < 26; c++)
				set_attr(3, c, TITLE_ATTR);

			len = sprintf(txt_buffer, "Select difficulty:");
			gotoxy(center_x(len), 7); printf(txt_buffer);

			len = sprintf(txt_buffer, "1 - Easy");
			gotoxy(center_x(len), 10); printf(txt_buffer);
			for (c = 12; c < 20; c++)
				set_attr(10, c, BRIGHT | INK_GREEN | PAPER_BLACK);

			len = sprintf(txt_buffer, "2 - Normal");
			gotoxy(center_x(len), 12); printf(txt_buffer);
			for (c = 11; c < 21; c++)
				set_attr(12, c, BRIGHT | INK_YELLOW | PAPER_BLACK);

			len = sprintf(txt_buffer, "3 - Hard");
			gotoxy(center_x(len), 14); printf(txt_buffer);
			for (c = 12; c < 20; c++)
				set_attr(14, c, BRIGHT | INK_RED | PAPER_BLACK);

			len = sprintf(txt_buffer, "4 - Nightmare");
			gotoxy(center_x(len), 16); printf(txt_buffer);
			for (c = 10; c < 22; c++)
				set_attr(16, c, BRIGHT | INK_MAGENTA | PAPER_BLACK);
		}

		/* Wait for 1/2/3; spin increments rseed for RNG seeding */
		k = 0;
		while (k != '1' && k != '2' && k != '3' && k != '4') {
			rseed++;
			k = getk_inkey();
		}
		while (getk()) ;  /* wait for key release */
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
		zx_cls_attr(INK_WHITE | PAPER_BLACK);
		{
			int len;
			char *dname;
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
			int ei;
			for (ei = 0; ei < num_enemies; ei++)
				last_edir_arr[ei] = 0;
		}

		/* Place coins and draw everything */
		place_coins();
		draw_exit(exit_gx, exit_gy);
		{
			int ei;
			unsigned char ea;
			for (ei = 0; ei < num_enemies; ei++) {
				ea = (ei == 0) ? ENEMY_ATTR : (ei == 1) ? ENEMY2_ATTR : (ei == 2) ? ENEMY3_ATTR : ENEMY4_ATTR;
				draw_sprite(SROW(eny[ei]), SCOL(enx[ei]), spr_enemy, ea);
			}
		}
		draw_dot(px, py);
		show_score();
		show_timer();

		while (1) {
			intrinsic_halt();  /* sync to 50Hz frame */

			/* --- Player input --- */
			k = getk();
			dx = 0;
			dy = 0;

			if (k) {
				if (key_delay > 0) {
					key_delay--;
				} else {
					if (k == 'o' || k == 'O') dx = -1;
					if (k == 'p' || k == 'P') dx = 1;
					if (k == 'q' || k == 'Q') dy = -1;
					if (k == 'a' || k == 'A') dy = 1;
				}
			} else {
				key_delay = 0;
			}

			if (dx || dy) {
				key_delay = KEY_REPEAT;
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
					{
						int ei;
						for (ei = 0; ei < num_enemies; ei++)
							if (px == enx[ei] && py == eny[ei])
								caught = 1;
					}

					if (px == exit_gx &&
					    py == exit_gy) {
						/* Bonus: 50 pts + 1 per second remaining */
						score += 50 + timer_sec;
						win_cut_scene();
						fgetc_cons();
						break;
					}
					snd_step();
				} else {
					snd_bump();
				}
			}

			/* --- Enemy moves spread across frames --- */
			/* Bresenham: accumulate num_enemies per frame, fire one
			   move each time accum >= enemy_frames.  Over enemy_frames
			   frames exactly num_enemies moves happen (one per enemy),
			   but at most 1-2 per frame instead of all N at once. */
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
						fgetc_cons();
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
				fgetc_cons();
				rank = update_hiscores();
				show_hiscores(rank);
				/* Reset for new game */
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
