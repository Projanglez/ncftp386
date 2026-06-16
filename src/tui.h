/* =============================================================================
 * tui.h - TUI engine for NCFTP386
 * -----------------------------------------------------------------------------
 * Direct access to the CGA/EGA/VGA text screen memory (0xB800:0000), box
 * drawing, text output, cursor control. No curses, no BIOS TTY - everything
 * goes through a far pointer into video memory for maximum speed on real
 * 386 hardware.
 *
 * Coordinates: row = row (0..24), col = column (0..79). 0/0 = top left.
 * Attribute byte: bits 0-3 foreground, 4-6 background, 7 blink.
 * ===========================================================================*/
#ifndef TUI_H
#define TUI_H

/* ---- Screen geometry (80x25 color text mode) ---- */
#define SCREEN_COLS  80
#define SCREEN_ROWS  25
#define SCREEN_CELLS (SCREEN_ROWS * SCREEN_COLS)  /* 2000 character/attribute pairs */

/* ---- Color attributes (DOS attribute bytes), see CLAUDE.md ---- */
#define ATTR_PANEL      0x1F  /* white on blue    (panel content)      */
#define ATTR_SELECTED   0x30  /* black on cyan    (cursor row)         */
#define ATTR_MARKED     0x1E  /* yellow on blue   (marked entry)       */
#define ATTR_MARKED_SEL 0x3E  /* yellow on cyan   (marked + cursor)    */
#define ATTR_HEADER     0x30  /* black on cyan    (path header)        */
#define ATTR_COLHDR     0x1A  /* light green on blue (column headers)  */
#define ATTR_BORDER     0x1F  /* white on blue    (border)             */
#define ATTR_MENUBAR    0x70  /* black on light gray                   */
#define ATTR_STATUSBAR  0x70  /* black on light gray                   */
#define ATTR_FNKEY_NUM  0x30  /* black on cyan (F1, F2, ...)           */
#define ATTR_FNKEY_LBL  0x70  /* black on light gray (label)           */
#define ATTR_DIALOG_BG  0x70  /* dialog background                     */
#define ATTR_DIALOG_HL  0x0F  /* dialog highlight (white on black)     */
#define ATTR_ERROR      0x4F  /* white on red (error dialogs)          */

/* ---- CP437 box-drawing characters ---- */
/* Double border */
#define BOX_D_HLINE  0xCD  /* = */
#define BOX_D_VLINE  0xBA  /* | */
#define BOX_D_TL     0xC9  /* top-left corner     */
#define BOX_D_TR     0xBB  /* top-right corner    */
#define BOX_D_BL     0xC8  /* bottom-left corner  */
#define BOX_D_BR     0xBC  /* bottom-right corner */
#define BOX_D_TLEFT  0xCC  /* T pointing right (|=)  on the left edge   */
#define BOX_D_TRIGHT 0xB9  /* T pointing left        on the right edge  */
#define BOX_D_TTOP   0xCB  /* T pointing down        on the top edge    */
#define BOX_D_TBOT   0xCA  /* T pointing up          on the bottom edge */
/* Single border */
#define BOX_S_HLINE  0xC4  /* - */
#define BOX_S_VLINE  0xB3  /* | */
#define BOX_S_TL     0xDA
#define BOX_S_TR     0xBF
#define BOX_S_BL     0xC0
#define BOX_S_BR     0xD9
#define BOX_S_TLEFT  0xC3  /* T pointing right */
#define BOX_S_TRIGHT 0xB4  /* T pointing left  */

/* ---- Init / shutdown ---- */
void tui_init(void);       /* force 80x25 text mode, hide the cursor          */
void tui_shutdown(void);   /* show the cursor again, clear screen normally    */

/* ---- Basic output routines (direct video memory access) ---- */
void clear_screen(unsigned char attr);
void putchar_at(int row, int col, char ch, unsigned char attr);
void putattr_at(int row, int col, unsigned char attr);  /* change only the attribute */
void fill_rect(int row, int col, int rows, int cols, char ch, unsigned char attr);
void hline(int row, int col, int len, char ch, unsigned char attr);
void vline(int row, int col, int len, char ch, unsigned char attr);

/* ---- Borders & text ---- */
/* draw_box: draws only the border (interior untouched). dbl!=0 -> double line */
void draw_box(int row, int col, int rows, int cols, unsigned char attr, int dbl);
/* draw_hsep: horizontal divider with T-pieces, fits a box of width 'cols'
 * whose left edge is at 'col' (e.g. the panel header separator) */
void draw_hsep(int row, int col, int cols, unsigned char attr, int dbl);
/* draw_text: writes text starting at (row,col). Stops at NUL, at column 80,
 * and - if maxlen>0 - after maxlen characters. No padding (use fill_rect for that). */
void draw_text(int row, int col, const char far *text, unsigned char attr, int maxlen);

/* ---- Cursor (BIOS INT 10h) ---- */
void set_cursor(int row, int col);
void show_cursor(int visible);   /* 0 = hide, !=0 = show */

/* ---- Save/restore screen (for modal dialogs) ---- */
/* buf must be at least SCREEN_CELLS*2 bytes. */
void save_screen(unsigned char far *buf);
void restore_screen(const unsigned char far *buf);

#endif /* TUI_H */
