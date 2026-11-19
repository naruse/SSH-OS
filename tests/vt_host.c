/* kernel/vt.c on this computer: reads what a program would send from
 * standard input, then prints the glass (what was actually drawn, not what
 * vt.c remembers), the cursor, and what the terminal answered.
 * Used by tests/test_vtc.py. */
#include <stdio.h>
#include <string.h>
#include "../kernel/kernel.h"

static u8 glass[CON_ROWS][CON_COLS], looks[CON_ROWS][CON_COLS];
/* charset_extra and charset_box come from the font file. */

void con_clear(void)
{
    memset(glass, ' ', sizeof glass);
    memset(looks, 0, sizeof looks);
}

void con_draw(int col, int row, const u8 *text, int n, u8 attr)
{
    for (; n > 0 && col < CON_COLS; n--, col++) {
        glass[row][col] = *text++;
        looks[row][col] = attr;
    }
}

void con_fill(int col, int row, int n, u8 ch, u8 attr)
{
    for (; n > 0 && col < CON_COLS; n--, col++) {
        glass[row][col] = ch;
        looks[row][col] = attr;
    }
}

void con_scroll(int top, int bottom, int lines)
{
    int height = bottom - top + 1, n = lines < 0 ? -lines : lines, i;

    if (lines > 0) {
        memmove(glass[top], glass[top + n], (size_t)(height - n) * CON_COLS);
        memmove(looks[top], looks[top + n], (size_t)(height - n) * CON_COLS);
        for (i = bottom - n + 1; i <= bottom; i++) {
            memset(glass[i], ' ', CON_COLS);
            memset(looks[i], 0, CON_COLS);
        }
    } else {
        memmove(glass[top + n], glass[top], (size_t)(height - n) * CON_COLS);
        memmove(looks[top + n], looks[top], (size_t)(height - n) * CON_COLS);
        for (i = top; i < top + n; i++) {
            memset(glass[i], ' ', CON_COLS);
            memset(looks[i], 0, CON_COLS);
        }
    }
}

static int cursor_col = -1, cursor_row = -1;

void con_invert(int col, int row)
{
    if (cursor_col == col && cursor_row == row) {
        cursor_col = cursor_row = -1;
    } else {
        cursor_col = col;
        cursor_row = row;
    }
}

void con_goto(int col, int row) { (void)col; (void)row; }
void con_putc(char c) { (void)c; }
void snd_beep(u32 n) { (void)n; }
void serial_putc(char c) { (void)c; }

int main(void)
{
    static u8 in[1 << 20];
    u8 back[64];
    size_t n = fread(in, 1, sizeof in, stdin);
    int r, c, k;

    vt_reset();
    vt_write(in, (u32)n);
    vt_cursor(1);
    for (r = 0; r < CON_ROWS; r++) {
        for (c = 0; c < CON_COLS; c++)
            printf("%02x%x", glass[r][c], looks[r][c]);
        putchar('\n');
    }
    printf("cursor %d %d\n", cursor_col, cursor_row);
    k = vt_reply(back, sizeof back);
    printf("reply ");
    for (c = 0; c < k; c++)
        printf("%02x", back[c]);
    putchar('\n');
    return 0;
}
