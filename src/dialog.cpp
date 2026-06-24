/* =============================================================================
 * dialog.cpp - Modal dialogs
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#include "dialog.h"
#include "tui.h"
#include "keymap.h"
#include "i18n.h"
#include "umlaut.h"   /* always the last include */

/* Buffer for the saved screen behind the dialog (4000 bytes). */
static unsigned char dlg_screen[SCREEN_CELLS * 2];

/* Separate buffer for the progress dialog: it may be open at the same time
 * as a modal dialog (e.g. the overwrite prompt during a running copy
 * operation), so it needs its own separate save buffer. */
static unsigned char prog_screen[SCREEN_CELLS * 2];

#define DLG_SHADOW 0x08   /* attribute of the drop-shadow cells (dark) */

/* -------------------------------------------------------------------------
 * Helper functions
 * ---------------------------------------------------------------------- */

/* Split a message into lines at '\n'. Returns: line count (>=1). */
static int split_lines(const char *msg, char lines[][72])
{
    int nl = 0, ci = 0;
    const char *p = msg ? msg : "";

    lines[0][0] = '\0';
    while (*p && nl < DLG_MAX_LINES) {
        if (*p == '\n') {
            lines[nl][ci] = '\0';
            nl++;
            ci = 0;
            if (nl < DLG_MAX_LINES) lines[nl][0] = '\0';
        } else {
            if (ci < 71) lines[nl][ci++] = *p;
        }
        p++;
    }
    if (nl < DLG_MAX_LINES) {
        lines[nl][ci] = '\0';
        nl++;
    }
    return nl;
}

/* Drop shadow to the right and below the dialog (attribute-only). */
static void draw_shadow(int top, int left, int rows, int cols)
{
    int r, c;
    for (r = top + 1; r <= top + rows; r++)
        putattr_at(r, left + cols, DLG_SHADOW);
    for (c = left + 1; c <= left + cols; c++)
        putattr_at(top + rows, c, DLG_SHADOW);
}

/* Frame + fill + centered title on the top border line. */
static void draw_dialog_frame(int top, int left, int rows, int cols,
                              const char *title, unsigned char bg)
{
    fill_rect(top, left, rows, cols, ' ', bg);
    draw_box(top, left, rows, cols, bg, 1);
    draw_shadow(top, left, rows, cols);

    if (title && *title) {
        int tl = (int)strlen(title);
        int tcol;
        if (tl > cols - 4) tl = cols - 4;
        tcol = left + (cols - tl) / 2;
        putchar_at(top, tcol - 1, ' ', bg);
        draw_text(top, tcol, title, bg, tl);
        putchar_at(top, tcol + tl, ' ', bg);
    }
}

/* Draw a "[ label ]" button; focused -> highlighted. */
static void draw_button(int row, int col, const char *label, int focused)
{
    char tmp[40];
    unsigned char a = focused ? ATTR_DIALOG_HL : ATTR_DIALOG_BG;
    sprintf(tmp, "[ %s ]", label);
    draw_text(row, col, tmp, a, (int)strlen(tmp));
}

static int button_width(const char *label)
{
    return (int)strlen(label) + 4;   /* "[ " + label + " ]" */
}

/* Derive the width/position of a centered dialog from its content width. */
static int clamp_cols(int content_w)
{
    int cols = content_w + 4;        /* 2 borders + 1 inner margin each */
    if (cols > 76) cols = 76;
    if (cols < 12) cols = 12;
    return cols;
}

/* -------------------------------------------------------------------------
 * Shared single-line text editor (used by every input field)
 * -----------------------------------------------------------------------------
 * Full line editing: Left/Right, Home/End, Del, Backspace (mid-line) and
 * insert at the caret. The visible window scrolls horizontally to keep the
 * caret on screen. Drawing and key handling are split so multi-field dialogs
 * (the connect form) can drive several fields from one keyboard loop.
 * ---------------------------------------------------------------------- */
struct EditField {
    char *buf;        /* edited buffer (size maxlen+1)        */
    int   maxlen;     /* maximum characters                   */
    int   len;        /* current length                       */
    int   caret;      /* caret position 0..len                */
    int   start;      /* index of the first visible character */
};

static void editfield_init(EditField *f, char *buf, int maxlen)
{
    f->buf = buf;
    f->maxlen = maxlen;
    f->len = (int)strlen(buf);
    if (f->len > maxlen) { f->len = maxlen; buf[f->len] = '\0'; }
    f->caret = f->len;
    f->start = 0;
}

/* Draw the field into a row/col/width region; keeps the caret visible. When
 * focused, positions the hardware cursor (caller must have show_cursor(1)). */
static void editfield_draw(EditField *f, int row, int col, int width,
                           unsigned char attr, int is_password, int focused)
{
    int vis, j;
    if (width < 1) width = 1;
    if (f->caret < f->start)                 f->start = f->caret;
    else if (f->caret > f->start + width - 1) f->start = f->caret - width + 1;
    if (f->start < 0) f->start = 0;

    vis = f->len - f->start;
    if (vis > width) vis = width;
    fill_rect(row, col, 1, width, ' ', attr);
    for (j = 0; j < vis; j++) {
        char c = is_password ? '*' : f->buf[f->start + j];
        putchar_at(row, col + j, c, attr);
    }
    if (focused) set_cursor(row, col + (f->caret - f->start));
}

/* Handle one key. Returns 1 if it was an edit/caret key (consumed), 0 if the
 * caller should handle it (Enter/Tab/Esc/Space-on-checkbox/...). */
static int editfield_key(EditField *f, int k)
{
    switch (k) {
    case KEY_LEFT:  if (f->caret > 0)      f->caret--; return 1;
    case KEY_RIGHT: if (f->caret < f->len) f->caret++; return 1;
    case KEY_HOME:  f->caret = 0;          return 1;
    case KEY_END:   f->caret = f->len;     return 1;
    case KEY_BACKSP:
        if (f->caret > 0) {
            memmove(f->buf + f->caret - 1, f->buf + f->caret,
                    (unsigned)(f->len - f->caret + 1));   /* tail incl. NUL */
            f->caret--; f->len--;
        }
        return 1;
    case KEY_DEL:
        if (f->caret < f->len) {
            memmove(f->buf + f->caret, f->buf + f->caret + 1,
                    (unsigned)(f->len - f->caret));        /* tail incl. NUL */
            f->len--;
        }
        return 1;
    default:
        if (k >= 0x20 && k <= 0x7E && f->len < f->maxlen) {
            memmove(f->buf + f->caret + 1, f->buf + f->caret,
                    (unsigned)(f->len - f->caret + 1));    /* shift incl. NUL */
            f->buf[f->caret] = (char)k;
            f->caret++; f->len++;
            return 1;
        }
        return 0;
    }
}

/* -------------------------------------------------------------------------
 * Notice/error box
 * ---------------------------------------------------------------------- */
void dlg_message(const char *title, const char *msg, int is_error)
{
    char lines[DLG_MAX_LINES][72];
    int nlines = split_lines(msg, lines);
    int w = 0, i, cols, rows, top, left, bcol;
    unsigned char bg = is_error ? ATTR_ERROR : ATTR_DIALOG_BG;
    static const char *lbl = "OK";

    if (title) { int t = (int)strlen(title); if (t > w) w = t; }
    for (i = 0; i < nlines; i++) {
        int l = (int)strlen(lines[i]);
        if (l > w) w = l;
    }
    if (button_width(lbl) > w) w = button_width(lbl);

    cols = clamp_cols(w);
    rows = nlines + 4;               /* border + lines + blank line + button + border */
    top  = (SCREEN_ROWS - rows) / 2;
    left = (SCREEN_COLS - cols) / 2;

    save_screen(dlg_screen);
    draw_dialog_frame(top, left, rows, cols, title, bg);
    for (i = 0; i < nlines; i++)
        draw_text(top + 1 + i, left + 2, lines[i], bg, cols - 4);

    bcol = left + (cols - button_width(lbl)) / 2;
    draw_button(top + rows - 2, bcol, lbl, 1);

    for (;;) {
        int k = readkey();
        if (k == KEY_ENTER || k == KEY_ESC || k == KEY_SPACE)
            break;
    }
    restore_screen(dlg_screen);
}

void dlg_error(const char *title, const char *msg)
{
    dlg_message(title, msg, 1);
}

/* -------------------------------------------------------------------------
 * Yes/No prompt
 * ---------------------------------------------------------------------- */
/* Yes/No prompt with a selectable default focus (file-internal; publicly
 * exposed only via dlg_confirm with the default set to "Yes"). */
static int dlg_confirm_def(const char *title, const char *msg, int default_yes)
{
    char lines[DLG_MAX_LINES][72];
    int nlines = split_lines(msg, lines);
    int w = 0, i, cols, rows, top, left;
    int btnrow, bw_ja, bw_nein, gap = 3, btotal, b0;
    int focus = default_yes ? 0 : 1; /* 0 = "Yes", 1 = "No" (safe default) */
    int result;
    unsigned char bg = ATTR_DIALOG_BG;
    const char *lbl_yes = L("Yes", "Ja");
    const char *lbl_no  = L("No", "Nein");

    bw_ja   = button_width(lbl_yes);
    bw_nein = button_width(lbl_no);
    btotal  = bw_ja + gap + bw_nein;

    if (title) { int t = (int)strlen(title); if (t > w) w = t; }
    for (i = 0; i < nlines; i++) {
        int l = (int)strlen(lines[i]);
        if (l > w) w = l;
    }
    if (btotal > w) w = btotal;

    cols   = clamp_cols(w);
    rows   = nlines + 4;
    top    = (SCREEN_ROWS - rows) / 2;
    left   = (SCREEN_COLS - cols) / 2;
    btnrow = top + rows - 2;
    b0     = left + (cols - btotal) / 2;

    save_screen(dlg_screen);
    draw_dialog_frame(top, left, rows, cols, title, bg);
    for (i = 0; i < nlines; i++)
        draw_text(top + 1 + i, left + 2, lines[i], bg, cols - 4);

    for (;;) {
        int k;
        draw_button(btnrow, b0, lbl_yes, focus == 0);
        draw_button(btnrow, b0 + bw_ja + gap, lbl_no, focus == 1);

        k = readkey();
        if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_TAB) {
            focus = !focus;
        } else if (k == 'j' || k == 'J' || k == 'y' || k == 'Y') {
            result = 1; break;
        } else if (k == 'n' || k == 'N' || k == KEY_ESC) {
            result = 0; break;
        } else if (k == KEY_ENTER) {
            result = (focus == 0) ? 1 : 0; break;
        }
    }
    restore_screen(dlg_screen);
    return result;
}

int dlg_confirm(const char *title, const char *msg)
{
    return dlg_confirm_def(title, msg, 1);   /* default: "Yes" */
}

/* -------------------------------------------------------------------------
 * Single-line input field
 * ---------------------------------------------------------------------- */
int dlg_input(const char *title, const char *prompt,
              char *buf, int maxlen, int is_password)
{
    const char *hint = L("[Enter] OK   [Esc] Cancel", "[Enter] OK   [Esc] Abbruch");
    int promptlen = prompt ? (int)strlen(prompt) : 0;
    int hintlen   = (int)strlen(hint);
    int fieldw, w, cols, rows, top, left, frow, fcol;
    int result;
    EditField ef;
    unsigned char bg = ATTR_DIALOG_BG;

    fieldw = maxlen;
    if (fieldw > 50) fieldw = 50;
    if (fieldw < 8)  fieldw = 8;

    w = fieldw;
    if (promptlen > w) w = promptlen;
    if (hintlen   > w) w = hintlen;
    if (title) { int t = (int)strlen(title); if (t > w) w = t; }

    cols = clamp_cols(w);
    rows = 6;                        /* border, prompt, field, blank, hint, border */
    top  = (SCREEN_ROWS - rows) / 2;
    left = (SCREEN_COLS - cols) / 2;
    frow = top + 2;
    fcol = left + 2;

    editfield_init(&ef, buf, maxlen);

    save_screen(dlg_screen);
    draw_dialog_frame(top, left, rows, cols, title, bg);
    if (prompt) draw_text(top + 1, left + 2, prompt, bg, cols - 4);
    draw_text(top + 4, left + 2, hint, bg, cols - 4);

    show_cursor(1);
    for (;;) {
        int k;
        editfield_draw(&ef, frow, fcol, fieldw, ATTR_DIALOG_HL, is_password, 1);
        k = readkey();
        if (k == KEY_ENTER) { result = 1; break; }
        if (k == KEY_ESC)   { result = 0; break; }
        editfield_key(&ef, k);
    }
    show_cursor(0);
    restore_screen(dlg_screen);
    return result;
}

/* -------------------------------------------------------------------------
 * Checksum result dialog: two read-only sum lines + editable filename field.
 * Shares the dlg_input edit-loop logic (single line, no password).
 * ---------------------------------------------------------------------- */
int dlg_checksum(const char *title, const char *crc, const char *md5,
                 char *fnamebuf, int maxlen)
{
    const char *crclbl = "CRC32: ";
    const char *md5lbl = "MD5:   ";
    const char *savelbl = L("Save to file:", "In Datei speichern:");
    const char *hint = L("[Enter] Save   [Esc] Close (no file)",
                         "[Enter] Speichern   [Esc] Schlie" ss "en (keine Datei)");
    char crcline[16 + 12];
    char md5line[40 + 12];
    int fieldw, w, cols, rows, top, left, frow, fcol;
    int result;
    EditField ef;
    unsigned char bg = ATTR_DIALOG_BG;

    sprintf(crcline, "%s%s", crclbl, crc ? crc : "");
    sprintf(md5line, "%s%s", md5lbl, md5 ? md5 : "");

    fieldw = maxlen;
    if (fieldw > 40) fieldw = 40;
    if (fieldw < 8)  fieldw = 8;

    w = (int)strlen(md5line);                       /* widest fixed line */
    if ((int)strlen(crcline) > w) w = (int)strlen(crcline);
    if ((int)strlen(hint)    > w) w = (int)strlen(hint);
    if ((int)strlen(savelbl) > w) w = (int)strlen(savelbl);
    if (fieldw > w) w = fieldw;
    if (title) { int t = (int)strlen(title); if (t > w) w = t; }

    cols = clamp_cols(w);
    rows = 9;            /* border,crc,md5,blank,label,field,blank,hint,border */
    top  = (SCREEN_ROWS - rows) / 2;
    left = (SCREEN_COLS - cols) / 2;
    frow = top + 5;
    fcol = left + 2;

    editfield_init(&ef, fnamebuf, maxlen);

    save_screen(dlg_screen);
    draw_dialog_frame(top, left, rows, cols, title, bg);
    draw_text(top + 1, left + 2, crcline,  bg, cols - 4);
    draw_text(top + 2, left + 2, md5line,  bg, cols - 4);
    draw_text(top + 4, left + 2, savelbl,  bg, cols - 4);
    draw_text(top + 7, left + 2, hint,     bg, cols - 4);

    show_cursor(1);
    for (;;) {
        int k;
        editfield_draw(&ef, frow, fcol, fieldw, ATTR_DIALOG_HL, 0, 1);
        k = readkey();
        if (k == KEY_ENTER) { result = 1; break; }
        if (k == KEY_ESC)   { result = 0; break; }
        editfield_key(&ef, k);
    }
    show_cursor(0);
    restore_screen(dlg_screen);
    return result;
}

/* -------------------------------------------------------------------------
 * Progress dialog (non-modal, updated via callback)
 * -----------------------------------------------------------------------------
 * Shares the dlg_screen save buffer with the other dialogs: the progress
 * dialog is never open at the same time as another one (dlg_input closes
 * before the transfer starts, dlg_error only opens afterwards).
 * ---------------------------------------------------------------------- */
static int  prog_active = 0;
static int  prog_top, prog_left, prog_cols, prog_rows;
static int  prog_batch;            /* 1 => the batch total lines are present  */
static int  prog_barw;
/* Absolute screen rows of the content lines (computed in begin()). */
static int  prog_filerow, prog_barrow, prog_raterow;
static int  prog_totalrow, prog_batchbarrow, prog_footrow;
static long prog_lastpct;          /* last drawn per-file %, -1 = none        */
static long prog_lastbatchpct;     /* last drawn batch %, -1 = none           */

/* Draw a "[####....] NNN%" bar on screen row 'row' (0xDB full, 0xB0 empty). */
static void prog_draw_bar(int row, long pct)
{
    char bar[84];
    int  i, o = 0, fill;

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    fill = (int)((long)prog_barw * pct / 100);
    if (fill > prog_barw) fill = prog_barw;

    bar[o++] = '[';
    for (i = 0; i < prog_barw; i++)
        bar[o++] = (char)((i < fill) ? 0xDB : 0xB0);
    bar[o++] = ']';
    o += sprintf(bar + o, " %3ld%%", pct);
    bar[o] = '\0';
    draw_text(row, prog_left + 2, bar, ATTR_DIALOG_BG, prog_cols - 4);
}

/* bytes/sec -> compact "1.2M/s" / "523K/s" / "12B/s". */
static void format_rate(unsigned long bps, char *out)
{
    char dec = g_locale.decimal_sep;
    if (bps >= SZ_MB)
        sprintf(out, "%lu%c%luM/s", bps / SZ_MB,
                dec, (bps % SZ_MB) * 10UL / SZ_MB);
    else if (bps >= SZ_KB)
        sprintf(out, "%luK/s", bps / SZ_KB);
    else
        sprintf(out, "%luB/s", bps);
}

/* seconds -> "MM:SS" or "H:MM:SS"; "--:--" when negative/unknown. */
static void format_eta(long sec, char *out)
{
    if (sec < 0) { strcpy(out, "--:--"); return; }
    if (sec >= 3600L)
        sprintf(out, "%ld:%02ld:%02ld", sec / 3600L, (sec / 60L) % 60L, sec % 60L);
    else
        sprintf(out, "%02ld:%02ld", sec / 60L, sec % 60L);
}

/* bytes -> compact "12.5M" / "999K" / "512" (binary, 1024-based). */
static void format_bytes_compact(unsigned long n, char *out)
{
    char dec = g_locale.decimal_sep;
    if (n >= SZ_GB)
        sprintf(out, "%lu%c%luG", n / SZ_GB, dec, (n % SZ_GB) / (SZ_GB / 10UL));
    else if (n >= SZ_MB)
        sprintf(out, "%lu%c%luM", n / SZ_MB, dec, (n % SZ_MB) / (SZ_MB / 10UL));
    else if (n >= SZ_KB)
        sprintf(out, "%luK", n / SZ_KB);
    else
        sprintf(out, "%lu", n);
}

/* Overflow-safe percentage (handles >42 MB totals on 16-bit). */
static long prog_pct(unsigned long sofar, unsigned long total)
{
    if (sofar >= total)          return 100;
    if (total > 42000000UL)      return (long)(sofar / (total / 100UL));
    return (long)((sofar * 100UL) / total);
}

void dlg_progress_begin(const char *title, int batch)
{
    unsigned char bg = ATTR_DIALOG_BG;
    int row;

    prog_cols = 50;
    if (prog_cols > SCREEN_COLS - 2) prog_cols = SCREEN_COLS - 2;
    prog_batch = batch ? 1 : 0;
    /* content rows: file, bar, rate [, total, batch-bar], footer */
    prog_rows = prog_batch ? 8 : 6;
    prog_top  = (SCREEN_ROWS - prog_rows) / 2;
    prog_left = (SCREEN_COLS - prog_cols) / 2;
    prog_barw = (prog_cols - 4) - 7;        /* remainder for "[" "]" " 100%" */
    if (prog_barw < 4) prog_barw = 4;

    row = prog_top + 1;
    prog_filerow = row++;
    prog_barrow  = row++;
    prog_raterow = row++;
    if (prog_batch) {
        prog_totalrow    = row++;
        prog_batchbarrow = row++;
    } else {
        prog_totalrow = prog_batchbarrow = -1;
    }
    prog_footrow = row++;

    prog_lastpct      = -1;
    prog_lastbatchpct = -1;
    prog_active       = 1;

    save_screen(prog_screen);
    draw_dialog_frame(prog_top, prog_left, prog_rows, prog_cols, title, bg);

    draw_text(prog_filerow, prog_left + 2, L("File:", "Datei:"), bg, prog_cols - 4);
    prog_draw_bar(prog_barrow, 0);
    /* The rate line and the "P=Pause ESC=Cancel" footer are drawn by
     * dlg_progress_update(), i.e. only for real transfers (not delete/move). */
}

void dlg_progress_update(const ProgressInfo *pi)
{
    unsigned char bg = ATTR_DIALOG_BG;
    char buf[80], a[24], b[24], c[24];

    if (!prog_active || !pi) return;

    /* Per-file bar (or a byte counter when the size is unknown). */
    if (pi->file_total > 0) {
        long pct = prog_pct(pi->file_sofar, pi->file_total);
        if (pct != prog_lastpct) { prog_lastpct = pct; prog_draw_bar(prog_barrow, pct); }
    } else {
        format_bytes_compact(pi->file_sofar, a);
        sprintf(buf, L("%s transferred ...", "%s " ue "bertragen ..."), a);
        fill_rect(prog_barrow, prog_left + 2, 1, prog_cols - 4, ' ', bg);
        draw_text(prog_barrow, prog_left + 2, buf, bg, prog_cols - 4);
    }

    /* Current / average rate and per-file ETA. */
    format_rate(pi->cur_speed, a);
    format_rate(pi->avg_speed, b);
    format_eta(pi->file_eta_sec, c);
    /* German "avg" symbol: CP850/German-DOS char 155 = 'ø'. Own literal so the
     * \x escape does not absorb the following space as a hex digit. */
    sprintf(buf, L("%s  avg %s  ETA %s", "%s  " "\x9B" " %s  Rest %s"), a, b, c);
    fill_rect(prog_raterow, prog_left + 2, 1, prog_cols - 4, ' ', bg);
    draw_text(prog_raterow, prog_left + 2, buf, bg, prog_cols - 4);

    /* All-files (batch) total + bar. */
    if (prog_batch && prog_totalrow >= 0) {
        format_bytes_compact(pi->batch_sofar, a);
        if (pi->batch_total > 0) {
            format_bytes_compact(pi->batch_total, b);
            format_eta(pi->batch_eta_sec, c);
            sprintf(buf, L("All: %s / %s  ETA %s", "Ges: %s / %s  Rest %s"), a, b, c);
        } else {
            sprintf(buf, L("All: %s", "Ges: %s"), a);
        }
        fill_rect(prog_totalrow, prog_left + 2, 1, prog_cols - 4, ' ', bg);
        draw_text(prog_totalrow, prog_left + 2, buf, bg, prog_cols - 4);

        if (pi->batch_total > 0) {
            long pct = prog_pct(pi->batch_sofar, pi->batch_total);
            if (pct != prog_lastbatchpct) {
                prog_lastbatchpct = pct;
                prog_draw_bar(prog_batchbarrow, pct);
            }
        }
    }

    /* Footer: paused banner or the key hints. */
    fill_rect(prog_footrow, prog_left + 2, 1, prog_cols - 4, ' ', bg);
    if (pi->paused)
        draw_text(prog_footrow, prog_left + 2,
                  L("*** PAUSED ***  P=resume  ESC=cancel",
                    "*** PAUSE ***  P=weiter  ESC=Abbruch"), ATTR_DIALOG_HL, prog_cols - 4);
    else
        draw_text(prog_footrow, prog_left + 2,
                  L("P=Pause  ESC=Cancel", "P=Pause  ESC=Abbruch"), bg, prog_cols - 4);
}

void dlg_progress_setfile(const char *name, int idx, int count)
{
    char buf[80];
    unsigned char bg = ATTR_DIALOG_BG;
    if (!prog_active) return;

    prog_lastpct = -1;

    fill_rect(prog_filerow, prog_left + 2, 1, prog_cols - 4, ' ', bg);
    if (count > 1)
        sprintf(buf, L("File: %.26s (%d/%d)", "Datei: %.26s (%d/%d)"),
                name ? name : "", idx, count);
    else
        sprintf(buf, L("File: %.34s", "Datei: %.34s"), name ? name : "");
    draw_text(prog_filerow, prog_left + 2, buf, bg, prog_cols - 4);
    prog_draw_bar(prog_barrow, 0);
}

void dlg_progress_end(void)
{
    if (!prog_active) return;
    restore_screen(prog_screen);
    prog_active = 0;
}

/* -------------------------------------------------------------------------
 * Choice dialog (message + vertical option list)
 * ---------------------------------------------------------------------- */
int dlg_choice(const char *title, const char *msg,
               const char *const *items, int count)
{
    char lines[DLG_MAX_LINES][72];
    int  nlines = split_lines(msg, lines);
    int  w = 0, i, cols, rows, top, left, optrow0, sel = 0, result;
    unsigned char bg = ATTR_DIALOG_BG;

    if (count <= 0) return -1;

    if (title) { int t = (int)strlen(title); if (t > w) w = t; }
    for (i = 0; i < nlines; i++) { int l = (int)strlen(lines[i]); if (l > w) w = l; }
    for (i = 0; i < count;  i++) { int l = (int)strlen(items[i]) + 2; if (l > w) w = l; }

    cols    = clamp_cols(w);
    rows    = nlines + count + 3;   /* border, text, blank line, options, border */
    top     = (SCREEN_ROWS - rows) / 2;
    left    = (SCREEN_COLS - cols) / 2;
    optrow0 = top + 1 + nlines + 1;

    save_screen(dlg_screen);
    draw_dialog_frame(top, left, rows, cols, title, bg);
    for (i = 0; i < nlines; i++)
        draw_text(top + 1 + i, left + 2, lines[i], bg, cols - 4);

    for (;;) {
        int k;
        for (i = 0; i < count; i++) {
            unsigned char a = (i == sel) ? ATTR_DIALOG_HL : bg;
            fill_rect(optrow0 + i, left + 2, 1, cols - 4, ' ', a);
            draw_text(optrow0 + i, left + 3, items[i], a, cols - 5);
        }
        k = readkey();
        if (k == KEY_ESC)        { result = -1;  break; }
        if (k == KEY_ENTER)      { result = sel; break; }
        if (k == KEY_UP)         { if (sel > 0) sel--; }
        else if (k == KEY_DOWN)  { if (sel < count - 1) sel++; }
        else if (k == KEY_HOME)  { sel = 0; }
        else if (k == KEY_END)   { sel = count - 1; }
    }

    restore_screen(dlg_screen);
    return result;
}

/* -------------------------------------------------------------------------
 * Vertical selection menu
 * ---------------------------------------------------------------------- */
int dlg_menu(const char *title, const char *const *items, int count, int initial)
{
    int w = 0, i, cols, rows, top, left, vis, maxvis, topidx, sel, result;
    unsigned char bg = ATTR_DIALOG_BG;

    if (count <= 0) return -1;

    if (title) { int t = (int)strlen(title); if (t > w) w = t; }
    for (i = 0; i < count; i++) {
        int l = (int)strlen(items[i]);
        if (l > w) w = l;
    }

    cols   = clamp_cols(w);
    maxvis = SCREEN_ROWS - 6;
    if (maxvis < 1) maxvis = 1;
    vis    = (count < maxvis) ? count : maxvis;
    rows   = vis + 2;                         /* top/bottom border + entries */
    top    = (SCREEN_ROWS - rows) / 2;
    left   = (SCREEN_COLS - cols) / 2;

    sel = initial;
    if (sel < 0) sel = 0;
    if (sel >= count) sel = count - 1;
    topidx = 0;

    save_screen(dlg_screen);
    draw_dialog_frame(top, left, rows, cols, title, bg);

    for (;;) {
        int k;

        if (sel < topidx)            topidx = sel;
        if (sel >= topidx + vis)     topidx = sel - vis + 1;

        for (i = 0; i < vis; i++) {
            int idx = topidx + i;
            unsigned char a = (idx == sel) ? ATTR_DIALOG_HL : bg;
            fill_rect(top + 1 + i, left + 2, 1, cols - 4, ' ', a);
            if (idx < count)
                draw_text(top + 1 + i, left + 2, items[idx], a, cols - 4);
        }

        k = readkey();
        if (k == KEY_ESC)        { result = -1;  break; }
        if (k == KEY_ENTER)      { result = sel; break; }
        if (k == KEY_UP)         { if (sel > 0) sel--; }
        else if (k == KEY_DOWN)  { if (sel < count - 1) sel++; }
        else if (k == KEY_HOME)  { sel = 0; }
        else if (k == KEY_END)   { sel = count - 1; }
        else if (k == KEY_PGUP)  { sel -= vis; if (sel < 0) sel = 0; }
        else if (k == KEY_PGDN)  { sel += vis; if (sel > count - 1) sel = count - 1; }
        else if (k >= 0x20 && k < 0x7F) {
            /* Direct selection: first entry starting with the matching letter. */
            int up = toupper((unsigned char)k);
            int hit = -1;
            for (i = 0; i < count && hit < 0; i++)
                if (toupper((unsigned char)items[i][0]) == up) hit = i;
            if (hit >= 0) { result = hit; break; }
        }
    }

    restore_screen(dlg_screen);
    return result;
}

/* -------------------------------------------------------------------------
 * FTP connect form: Host/Port/User/Pass + save checkboxes
 *
 * Layout (cols=50, rows=11):
 *  Row 0:  border with title
 *  Row 1:  host field
 *  Row 2:  port field
 *  Row 3:  user field
 *  Row 4:  pass field (masked)
 *  Row 5:  divider
 *  Row 6:  [X] Save connection data
 *  Row 7:  [ ] Save password (insecure)
 *  Row 8:  divider
 *  Row 9:  buttons [ Connect ]  [ Cancel ]
 *  Row 10: bottom border
 *
 * Focus order: Host(0) Port(1) User(2) Pass(3)
 *              chk_save(4) chk_pass(5) btn_ok(6) btn_cancel(7)
 * Tab/Down forward, Up backward. Space/Enter on a checkbox toggles it.
 * ---------------------------------------------------------------------- */
int dlg_connect(const char *title,
                char *host, int host_max,
                char *port, int port_max,
                char *user, int user_max,
                char *pass, int pass_max,
                int *save_conn,
                int *save_pass)
{
    int cols   = 50;
    int rows   = 11;
    int top    = (SCREEN_ROWS - rows) / 2;
    int left   = (SCREEN_COLS - cols) / 2;
    int inner  = cols - 4;          /* usable inner area                     */
    int fdw    = inner - 6;         /* field display width: inner - "Host: " */
    int fcol   = left + 2 + 6;      /* left edge of the input fields         */
    int NFOCUS = 8;
    int focus  = (host[0] != '\0') ? 6 : 0;  /* pre-filled: focus Connect directly */

    /* Text fields */
    char *fbufs[4];
    int   fmaxs[4];
    EditField efs[4];
    int   frows[4];
    const char *flbls[4];
    int   is_pw[4];
    int i;

    fbufs[0] = host; fmaxs[0] = host_max; frows[0] = top + 1;
    fbufs[1] = port; fmaxs[1] = port_max; frows[1] = top + 2;
    fbufs[2] = user; fmaxs[2] = user_max; frows[2] = top + 3;
    fbufs[3] = pass; fmaxs[3] = pass_max; frows[3] = top + 4;

    flbls[0] = "Host: ";
    flbls[1] = "Port: ";
    flbls[2] = L("User: ", "User: ");
    flbls[3] = L("Pass: ", "Pass: ");

    is_pw[0] = 0; is_pw[1] = 0; is_pw[2] = 0; is_pw[3] = 1;

    for (i = 0; i < 4; i++)
        editfield_init(&efs[i], fbufs[i], fmaxs[i]);

    /* Checkbox state (local; only written to *save_conn/*save_pass on OK) */
    int chk_save = *save_conn;
    int chk_pass = *save_pass;
    if (!chk_save) chk_pass = 0;

    /* Buttons */
    const char *lbl_ok     = L("Connect", "Verbinden");
    const char *lbl_cancel = L("Cancel", "Abbrechen");
    int bw_ok     = button_width(lbl_ok);
    int bw_cancel = button_width(lbl_cancel);
    int gap       = 4;
    int btotal    = bw_ok + gap + bw_cancel;
    int b0        = left + (cols - btotal) / 2;

    unsigned char bg = ATTR_DIALOG_BG;
    int result = 0;

    /* Save the screen and draw the frame */
    save_screen(dlg_screen);
    draw_dialog_frame(top, left, rows, cols, title, bg);

    /* Static labels */
    for (i = 0; i < 4; i++)
        draw_text(frows[i], left + 2, flbls[i], bg, 6);

    /* Divider lines */
    draw_hsep(top + 5, left, cols, bg, 1);
    draw_hsep(top + 8, left, cols, bg, 1);

    show_cursor(1);

    for (;;) {
        int k;
        char tmp[56];

        /* Redraw the text fields */
        for (i = 0; i < 4; i++) {
            int is_focused = (focus == i);
            unsigned char fa = is_focused ? ATTR_DIALOG_HL : bg;
            editfield_draw(&efs[i], frows[i], fcol, fdw, fa, is_pw[i], is_focused);
        }

        /* Checkboxes */
        {
            unsigned char a4 = (focus == 4) ? ATTR_DIALOG_HL : bg;
            unsigned char a5 = (focus == 5) ? ATTR_DIALOG_HL : bg;
            sprintf(tmp, "[%c] %s", chk_save ? 'X' : ' ',
                    L("Save connection data", "Verbindungsdaten speichern"));
            fill_rect(top + 6, left + 2, 1, inner, ' ', a4);
            draw_text(top + 6, left + 2, tmp, a4, inner);
            sprintf(tmp, "[%c] %s", chk_pass ? 'X' : ' ',
                    L("Save password (insecure)", "Passwort speichern (unsicher)"));
            fill_rect(top + 7, left + 2, 1, inner, ' ', a5);
            draw_text(top + 7, left + 2, tmp, a5, inner);
        }

        /* Buttons */
        draw_button(top + 9, b0,               lbl_ok,     focus == 6);
        draw_button(top + 9, b0 + bw_ok + gap, lbl_cancel, focus == 7);

        k = readkey();

        if (k == KEY_ESC) { result = 0; break; }

        /* Forward navigation */
        if (k == KEY_TAB || k == KEY_DOWN)
        { focus = (focus + 1) % NFOCUS; continue; }
        /* Backward navigation (Up or Shift+Tab = 0x10F) */
        if (k == KEY_UP || k == 0x10F)
        { focus = (focus + NFOCUS - 1) % NFOCUS; continue; }

        /* Focus-specific input */
        if (focus >= 0 && focus <= 3) {
            if (k == KEY_ENTER) focus = (focus + 1) % NFOCUS;
            else                editfield_key(&efs[focus], k);
        } else if (focus == 4) {
            if (k == KEY_ENTER || k == ' ') {
                chk_save = !chk_save;
                if (!chk_save) chk_pass = 0;
            }
        } else if (focus == 5) {
            if ((k == KEY_ENTER || k == ' ') && chk_save)
                chk_pass = !chk_pass;
        } else if (focus == 6) {
            if (k == KEY_ENTER) { result = 1; break; }
        } else { /* focus == 7 */
            if (k == KEY_ENTER) { result = 0; break; }
        }
    }

    show_cursor(0);
    restore_screen(dlg_screen);

    if (result) {
        *save_conn = chk_save;
        *save_pass = chk_pass;
    }
    return result;
}

/* -------------------------------------------------------------------------
 * Splash screen
 * ---------------------------------------------------------------------- */
void dlg_splash(const char *version)
{
    int r = 7, c = 15, h = 11, w = 50;
    int inner = w - 2;
    unsigned char bg = ATTR_DIALOG_BG;
    char titlebuf[40];
    clock_t t0;

    /* Helper macro: draw text centered in the box */
#define SPLASH_LINE(row, text, attr) \
    { const char *_t = (text); int _l = (int)strlen(_t); \
      draw_text((row), (c) + 1 + (inner - _l) / 2, _t, (attr), _l); }

    sprintf(titlebuf, "FTP4DOS  v%s", version ? version : "");

    save_screen(dlg_screen);

    fill_rect(r, c, h, w, ' ', bg);
    draw_box(r, c, h, w, bg, 1);

    SPLASH_LINE(r + 2, titlebuf,
                ATTR_DIALOG_HL)
    SPLASH_LINE(r + 3, L("Dual-Panel FTP Client for DOS",
                          "Dual-Panel FTP Client f" ue "r DOS"),
                bg)
    SPLASH_LINE(r + 5, "(c) 2026  Projanglez",             bg)
    SPLASH_LINE(r + 6, "GNU General Public License v3",    bg)
    SPLASH_LINE(r + 8, L("[Any key to start]",
                          "[Beliebige Taste zum Starten]"),
                bg)

#undef SPLASH_LINE

    t0 = clock();
    while (!key_pending() && (clock() - t0) < (clock_t)(CLOCKS_PER_SEC * 10))
        ;
    if (key_pending()) getch();

    restore_screen(dlg_screen);
}
