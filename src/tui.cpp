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

/* Text video segment: 0xB800 on color adapters (CGA/EGA/VGA), 0xB000 on a
 * monochrome adapter (MDA/Hercules). Chosen at tui_init() time. */
#define VIDEO_SEG_COLOR 0xB800u
#define VIDEO_SEG_MONO  0xB000u

static unsigned int  g_vidseg = VIDEO_SEG_COLOR;
static int           g_mono   = 0;   /* 1 = monochrome adapter (mode 7)        */

/* Far pointer to the top-left screen cell. Wrapped in a function so MK_FP
 * doesn't have to be evaluated as a static initializer. */
static unsigned char far *vidmem(void)
{
    return (unsigned char far *)MK_FP(g_vidseg, 0);
}

/* On a monochrome adapter the only legal attributes are normal (0x07),
 * intense (0x0F), reverse (0x70) and the blink bit (0x80). The app's color
 * attributes are remapped to those at write time so the rest of the code can
 * keep using the color ATTR_* values unchanged.
 *
 * The app's canvas is white-on-blue (background nibble 1); on mono that is the
 * *normal* background, so blue (and plain black) backgrounds map to normal
 * text. Genuine highlight backgrounds (cyan cursor bar, gray status/menu/
 * dialog, red error) map to reverse video. A non-white foreground on the
 * canvas (yellow marks, green column headers, bright dialog highlight) becomes
 * intense so it still stands out. The blink bit is preserved. */
static unsigned char map_attr(unsigned char a)
{
    unsigned char bl, fg, bg;

    if (!g_mono)
        return a;

    bl = (unsigned char)(a & 0x80);
    fg = (unsigned char)(a & 0x0F);
    bg = (unsigned char)((a >> 4) & 0x07);

    if (bg == 0 || bg == 1) {                 /* black / blue canvas -> normal bg */
        if (fg != 0x0F && fg >= 0x08)
            return (unsigned char)(0x0F | bl);  /* bright color -> intense        */
        return (unsigned char)(0x07 | bl);      /* white/dark text -> normal      */
    }
    return (unsigned char)(0x70 | bl);          /* highlight bg -> reverse video  */
}

/* -------------------------------------------------------------------------
 * Init / shutdown
 * ---------------------------------------------------------------------- */
/* mono_pref: -1 = auto-detect, 0 = force color, 1 = force monochrome. */
void tui_init(int mono_pref)
{
    union REGS r;

    if (mono_pref == 0) {
        g_mono = 0;
    } else if (mono_pref == 1) {
        g_mono = 1;
    } else {
        /* Auto-detect: BIOS INT 10h AH=0Fh returns the current video mode in
         * AL. Mode 7 = monochrome adapter (MDA/Hercules). */
        r.h.ah = 0x0F;
        int86(0x10, &r, &r);
        g_mono = (r.h.al == 0x07) ? 1 : 0;
    }

    g_vidseg = g_mono ? VIDEO_SEG_MONO : VIDEO_SEG_COLOR;

    /* Set 80x25 text mode (07h mono, 03h color). Also resets the screen. */
    r.h.ah = 0x00;
    r.h.al = (unsigned char)(g_mono ? 0x07 : 0x03);
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
    v[offset + 1] = map_attr(attr);
}

void putattr_at(int row, int col, unsigned char attr)
{
    unsigned char far *v;
    int offset;

    if (row < 0 || row >= SCREEN_ROWS || col < 0 || col >= SCREEN_COLS)
        return;

    v = vidmem();
    offset = (row * SCREEN_COLS + col) * 2;
    v[offset + 1] = map_attr(attr);
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

    attr = map_attr(attr);
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
