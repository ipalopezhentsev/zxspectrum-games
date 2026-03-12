#include <stdio.h>
#include <string.h>
#include <features.h>
#include <graphics.h>

int days_in_month(int month, int year)
{
	int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
	if (month == 2) {
		if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
			return 29;
	}
	return days[month - 1];
}

/* Zeller's formula: day of week for any date (0=Sun,1=Mon,...,6=Sat) */
int day_of_week(int day, int month, int year)
{
	int q, m, k, j, h;
	q = day;
	m = month;
	if (m < 3) {
		m += 12;
		year--;
	}
	k = year % 100;
	j = year / 100;
	h = (q + (13*(m+1))/5 + k + k/4 + j/4 + 5*j) % 7;
	/* Convert from Zeller (0=Sat) to 0=Sun */
	return ((h + 6) % 7);
}

/* Draw a horizontal line using plot() only */
void hline(int x1, int x2, int y)
{
	int x;
	for (x = x1; x <= x2; x++) {
		plot(x, y);
	}
}

/* Draw a dotted horizontal line */
void hline_dotted(int x1, int x2, int y)
{
	int x;
	for (x = x1; x <= x2; x += 3) {
		plot(x, y);
	}
}

/* Draw a small diamond at pixel position cx, cy */
void draw_diamond(int cx, int cy)
{
	draw(cx, cy + 3, cx + 3, cy);
	draw(cx + 3, cy, cx, cy - 3);
	draw(cx, cy - 3, cx - 3, cy);
	draw(cx - 3, cy, cx, cy + 3);
}

main()
{
	int year, month;
	int dim, start, i;
	char *names[] = {
		"January","February","March","April",
		"May","June","July","August",
		"September","October","November","December"
	};
	char buf[8];

	while (1) {
		printf("%c", 12);
		printf("Year (0 to quit): ");
		fgets(buf, sizeof(buf), stdin);
		year = atoi(buf);
		if (year == 0) break;

		printf("Month (1-12): ");
		fgets(buf, sizeof(buf), stdin);
		month = atoi(buf);
		if (month < 1 || month > 12) {
			printf("Invalid month!\n");
			fgetc_cons();
		}

		printf("%c", 12);

		/* Leave row 0 for decoration, row 1 for border top */
		printf("\n\n");
		printf("    %s %d\n", names[month-1], year);
		printf("  ----------------------\n");
		printf("  Mo Tu We Th Fr Sa Su\n");
		printf("  ----------------------\n");

		/* Convert to Monday-start (0=Mon,...,6=Sun) */
		start = (day_of_week(1, month, year) + 6) % 7;
		dim = days_in_month(month, year);

		printf("  ");
		for (i = 0; i < start; i++)
			printf("   ");

		for (i = 1; i <= dim; i++) {
			printf("%2d ", i);
			if ((start + i) % 7 == 0) {
				printf("\n  ");
			}
		}
		printf("\n");

		/* Pixel graphics overlay.
		   ZX Spectrum: 256x192 pixels.
		   0,0 = bottom-left, 255,191 = top-right.
		   Text row N: top pixel = 191 - N*8 */

		/* Double border box around rows 1-12 */
		drawb(2, 183, 252, 96);
		drawb(5, 180, 246, 90);

		/* Row of diamonds at top (row 0 area, y~188) */
		for (i = 0; i < 8; i++)
			draw_diamond(20 + i * 28, 188);

		/* Row of diamonds at bottom (below box, y~82) */
		for (i = 0; i < 8; i++)
			draw_diamond(20 + i * 28, 82);

		/* Corner crosses */
		draw(10, 182, 16, 182);
		draw(13, 185, 13, 179);
		draw(240, 182, 246, 182);
		draw(243, 185, 243, 179);
		draw(10, 90, 16, 90);
		draw(13, 93, 13, 87);
		draw(240, 90, 246, 90);
		draw(243, 93, 243, 87);

		printf("\n  Press any key...");
		fgetc_cons();
	}
}
