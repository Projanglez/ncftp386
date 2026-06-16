/* =============================================================================
 * tui.cpp - TUI engine for NCFTP386
 * -----------------------------------------------------------------------------
 * Implementation: direct far-pointer access to the text screen memory at
 * 0xB800:0000 (color adapter). Cursor and mode control via BIOS INT 10h.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <dos.h>
#include <i86.h>

#include "tui.h"

/* Segment of the color text memory. Monochrome adapters sit at 0xB000 - we
 * assume color (see CLAUDE.md, ATTR_PANEL = white on blue). */
#define VIDEO_SEG 0xB800u

/* Far pointer to the top-left screen cell. Wrapped in a function so MK_FP
 * doesn't have to be evaluated as a static initializer. */
static unsigned char far *vidmem(void)
{
    return (unsigned char far *)MK_FP(VIDEO_SEG, 0);
}

/* -------------------------------------------------------------------------
 * Init / shutdown
 * ---------------------------------------------------------------------- */
void tui_init(void)
{
    union REGS r;

    /* Video mode 03h = 80x25 color text. Also resets the screen. */
    r.h.ah = 0x00;
    r.h.al = 0x03;
    int86(0x10, &r, &r);

    show_cursor(0);
    clear_screen(ATTR_PANEL);
}

void tui_shutdown(void)
{
    /* Clear the screen to "normal" DOS colors and bring back the cursor. */
    clear_screen(0x07);
    set_cursor(0, 0);
    show_cursor(1);
}

/* -------------------------------------------------------------------------
 * Basic output
 * ---------------------------------------------------------------------- */
void clear_screen(unsigned char attr)
{
    fill_rect(0, 0, SCREEN_ROWS, SCREEN_COLS, ' ', attr);
}

void putchar_at(int row, int col, char ch, unsigned char attr)
{
    unsigned char far *v;
    int offset;

    if (row < 0 || row >= SCREEN_ROWS || col < 0 || col >= SCREEN_COLS)
        return;

    v = vidmem();
    offset = (row * SCREEN_COLS + col) * 2;
    v[offset]     = (unsigned char)ch;
    v[offset + 1] = attr;
}

void putattr_at(int row, int col, unsigned char attr)
{
    unsigned char far *v;
    int offset;

    if (row < 0 || row >= SCREEN_ROWS || col < 0 || col >= SCREEN_COLS)
        return;

    v = vidmem();
    offset = (row * SCREEN_COLS + col) * 2;
    v[offset + 1] = attr;
}

void fill_rect(int row, int col, int rows, int cols, char ch, unsigned char attr)
{
    int r, c, r2, c2;

    /* Clip the rectangle to the visible area. */
    r2 = row + rows;
    c2 = col + cols;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (r2 > SCREEN_ROWS) r2 = SCREEN_ROWS;
    if (c2 > SCREEN_COLS) c2 = SCREEN_COLS;

    for (r = row; r < r2; r++) {
        unsigned char far *v = vidmem() + (r * SCREEN_COLS + col) * 2;
        for (c = col; c < c2; c++) {
            *v++ = (unsigned char)ch;
            *v++ = attr;
        }
    }
}

void hline(int row, int col, int len, char ch, unsigned char attr)
{
    int i;
    for (i = 0; i < len; i++)
        putchar_at(row, col + i, ch, attr);
}

void vline(int row, int col, int len, char ch, unsigned char attr)
{
    int i;
    for (i = 0; i < len; i++)
        putchar_at(row + i, col, ch, attr);
}

/* -------------------------------------------------------------------------
 * Borders & text
 * ---------------------------------------------------------------------- */
void draw_box(int row, int col, int rows, int cols, unsigned char attr, int dbl)
{
    unsigned char tl, tr, bl, br, hz, vt;

    if (rows < 2 || cols < 2)
        return;

    if (dbl) {
        tl = BOX_D_TL; tr = BOX_D_TR; bl = BOX_D_BL; br = BOX_D_BR;
        hz = BOX_D_HLINE; vt = BOX_D_VLINE;
    } else {
        tl = BOX_S_TL; tr = BOX_S_TR; bl = BOX_S_BL; br = BOX_S_BR;
        hz = BOX_S_HLINE; vt = BOX_S_VLINE;
    }

    /* Corners */
    putchar_at(row,            col,            (char)tl, attr);
    putchar_at(row,            col + cols - 1, (char)tr, attr);
    putchar_at(row + rows - 1, col,            (char)bl, attr);
    putchar_at(row + rows - 1, col + cols - 1, (char)br, attr);

    /* Edges */
    hline(row,            col + 1, cols - 2, (char)hz, attr);
    hline(row + rows - 1, col + 1, cols - 2, (char)hz, attr);
    vline(row + 1,        col,            rows - 2, (char)vt, attr);
    vline(row + 1,        col + cols - 1, rows - 2, (char)vt, attr);
}

void draw_hsep(int row, int col, int cols, unsigned char attr, int dbl)
{
    unsigned char lt, rt, hz;

    if (cols < 2)
        return;

    if (dbl) {
        lt = BOX_D_TLEFT; rt = BOX_D_TRIGHT; hz = BOX_D_HLINE;
    } else {
        lt = BOX_S_TLEFT; rt = BOX_S_TRIGHT; hz = BOX_S_HLINE;
    }

    putchar_at(row, col,            (char)lt, attr);
    putchar_at(row, col + cols - 1, (char)rt, attr);
    hline(row, col + 1, cols - 2, (char)hz, attr);
}

void draw_text(int row, int col, const char far *text, unsigned char attr, int maxlen)
{
    int i;

    if (text == 0)
        return;

    for (i = 0; text[i] != '\0'; i++) {
        if (maxlen > 0 && i >= maxlen)
            break;
        if (col + i >= SCREEN_COLS)
            break;
        putchar_at(row, col + i, text[i], attr);
    }
}

/* -------------------------------------------------------------------------
 * Cursor (BIOS INT 10h)
 * ---------------------------------------------------------------------- */
void set_cursor(int row, int col)
{
    union REGS r;

    r.h.ah = 0x02;            /* Set Cursor Position */
    r.h.bh = 0x00;            /* display page 0      */
    r.h.dh = (unsigned char)row;
    r.h.dl = (unsigned char)col;
    int86(0x10, &r, &r);
}

void show_cursor(int visible)
{
    union REGS r;

    r.h.ah = 0x01;            /* Set Cursor Type */
    if (visible) {
        r.h.ch = 0x06;        /* normal cursor scanlines 6..7 */
        r.h.cl = 0x07;
    } else {
        r.h.ch = 0x20;        /* bit 5 set = cursor invisible */
        r.h.cl = 0x00;
    }
    int86(0x10, &r, &r);
}

/* -------------------------------------------------------------------------
 * Save / restore screen
 * ---------------------------------------------------------------------- */
void save_screen(unsigned char far *buf)
{
    unsigned char far *v = vidmem();
    int i, n = SCREEN_CELLS * 2;
    for (i = 0; i < n; i++)
        buf[i] = v[i];
}

void restore_screen(const unsigned char far *buf)
{
    unsigned char far *v = vidmem();
    int i, n = SCREEN_CELLS * 2;
    for (i = 0; i < n; i++)
        v[i] = buf[i];
}
