/* =============================================================================
 * editor.cpp - Minimal full-screen text editor (F4)
 * -----------------------------------------------------------------------------
 * Data model: an array of lines (EdLine) on the far heap; each line owns
 * its own growable far buffer. At most ED_LOAD_MAX bytes are loaded; larger
 * files are rejected (no partial editing).
 *
 * Controls:
 *   Arrows / Page Up-Down / Home / End  - navigation
 *   printable characters                - insert
 *   Backspace / Delete                  - delete (at the line edge: join lines)
 *   Enter                               - split the line at the cursor position
 *   F2                                  - save (DOS line endings \r\n)
 *   Esc                                 - quit (confirms if there are changes)
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor.h"
#include "tui.h"
#include "keymap.h"
#include "dialog.h"
#include "i18n.h"
#include "umlaut.h"   /* always the last include */

#define ED_LOAD_MAX     32000u   /* max. bytes read in                      */
#define ED_MAX_LINES     2000    /* max. lines                             */
#define ED_MAX_LINELEN   1000    /* max. characters per line               */

typedef struct {
    char far *txt;   /* line text (NO NUL termination needed) */
    int       len;   /* characters in use                     */
    int       cap;   /* capacity of txt in characters          */
} EdLine;

static EdLine far *ed_lines;     /* line array (far heap)           */
static int ed_n;                 /* number of lines                 */
static int ed_mod;               /* 1 = unsaved changes             */
static int ed_cx, ed_cy;         /* cursor: column, line (0-based)  */
static int ed_top, ed_hoff;      /* first visible: line, column     */

/* -------------------------------------------------------------------------
 * Memory management
 * ---------------------------------------------------------------------- */
static void ed_free(void)
{
    int i;
    if (ed_lines) {
        for (i = 0; i < ed_n; i++)
            if (ed_lines[i].txt) free(ed_lines[i].txt);
        free(ed_lines);
        ed_lines = 0;
    }
    ed_n = 0;
}

/* Grow a line's capacity to at least 'need' characters. */
static int ed_ensure(EdLine far *l, int need)
{
    int       nc;
    char far *p;

    if (need > ED_MAX_LINELEN) return 0;        /* line too long */
    if (l->cap >= need) return 1;

    nc = l->cap ? l->cap : 16;
    while (nc < need) nc += (nc >> 1) + 8;
    if (nc > ED_MAX_LINELEN) nc = ED_MAX_LINELEN;

    p = (char far *)realloc(l->txt, (unsigned)nc + 1);
    if (!p) return 0;
    l->txt = p;
    l->cap = nc;
    return 1;
}

/* Insert an empty line at index 'idx'. */
static int ed_newline_at(int idx)
{
    int i;
    if (ed_n >= ED_MAX_LINES) return 0;
    for (i = ed_n; i > idx; i--) ed_lines[i] = ed_lines[i - 1];
    ed_lines[idx].txt = 0;
    ed_lines[idx].len = 0;
    ed_lines[idx].cap = 0;
    ed_n++;
    return 1;
}

/* Append a line of 'len' bytes from 'src' at the end (load phase). */
static int ed_addline(const char far *src, int len)
{
    EdLine far *l;
    if (ed_n >= ED_MAX_LINES) return 0;
    if (len > ED_MAX_LINELEN) len = ED_MAX_LINELEN;   /* truncate */
    l = &ed_lines[ed_n];
    l->txt = 0; l->len = 0; l->cap = 0;
    if (len > 0) {
        if (!ed_ensure(l, len)) return 0;
        _fmemcpy(l->txt, src, len);
        l->len = len;
    }
    ed_n++;
    return 1;
}

/* -------------------------------------------------------------------------
 * Load / save
 *   ed_load return value: 0 = ok, -1 = out of memory, -2 = file too large
 * ---------------------------------------------------------------------- */
static int ed_load(const char *path)
{
    FILE     *f;
    char far *buf;
    unsigned  n, pos, ls;

    ed_lines = (EdLine far *)malloc((unsigned)ED_MAX_LINES * sizeof(EdLine));
    if (!ed_lines) return -1;
    ed_n = 0;

    f = fopen(path, "rb");
    if (!f) return 0;                 /* new/empty file: 0 lines */

    buf = (char far *)malloc(ED_LOAD_MAX);
    if (!buf) { fclose(f); return -1; }

    n = (unsigned)fread(buf, 1, ED_LOAD_MAX, f);
    if (n == ED_LOAD_MAX && fgetc(f) != EOF) {   /* more data follows -> too large */
        free(buf);
        fclose(f);
        return -2;
    }
    fclose(f);

    /* Split into lines ('\n' separates; a trailing '\r' is discarded). */
    ls = 0;
    for (pos = 0; pos < n; pos++) {
        if (buf[pos] == '\n') {
            int linelen = (int)(pos - ls);
            if (linelen > 0 && buf[ls + linelen - 1] == '\r') linelen--;
            if (!ed_addline(buf + ls, linelen)) break;
            ls = pos + 1;
        }
    }
    if (ls < n || ed_n == 0) {        /* remainder after the last '\n', or an empty file */
        int linelen = (int)(n - ls);
        if (linelen > 0 && buf[ls + linelen - 1] == '\r') linelen--;
        ed_addline(buf + ls, linelen);
    }

    free(buf);
    return 0;
}

static int ed_save(const char *path)
{
    FILE *f;
    int   i;
    f = fopen(path, "wb");
    if (!f) return 0;
    for (i = 0; i < ed_n; i++) {
        if (ed_lines[i].len > 0)
            fwrite(ed_lines[i].txt, 1, (unsigned)ed_lines[i].len, f);
        fputc('\r', f);
        fputc('\n', f);
    }
    if (fclose(f) != 0) return 0;
    ed_mod = 0;
    return 1;
}

/* -------------------------------------------------------------------------
 * Edit operations
 * ---------------------------------------------------------------------- */
static void ed_insert_char(int c)
{
    EdLine far *l = &ed_lines[ed_cy];
    int i;
    if (!ed_ensure(l, l->len + 1)) return;       /* line full -> ignore */
    for (i = l->len; i > ed_cx; i--) l->txt[i] = l->txt[i - 1];
    l->txt[ed_cx] = (char)c;
    l->len++;
    ed_cx++;
    ed_mod = 1;
}

/* Enter: split the current line at the cursor position. */
static void ed_split(void)
{
    EdLine far *l;
    EdLine far *nl;
    int tail;

    if (!ed_newline_at(ed_cy + 1)) return;
    l  = &ed_lines[ed_cy];           /* ed_cy stays unchanged */
    nl = &ed_lines[ed_cy + 1];
    tail = l->len - ed_cx;
    if (tail > 0) {
        if (ed_ensure(nl, tail)) {
            _fmemcpy(nl->txt, l->txt + ed_cx, (unsigned)tail);
            nl->len = tail;
            l->len  = ed_cx;
        }
    }
    ed_cy++;
    ed_cx = 0;
    ed_mod = 1;
}

static void ed_remove_line(int idx)
{
    int i;
    if (ed_lines[idx].txt) free(ed_lines[idx].txt);
    for (i = idx; i < ed_n - 1; i++) ed_lines[i] = ed_lines[i + 1];
    ed_n--;
}

static void ed_backspace(void)
{
    EdLine far *l = &ed_lines[ed_cy];
    if (ed_cx > 0) {
        int i;
        for (i = ed_cx; i < l->len; i++) l->txt[i - 1] = l->txt[i];
        l->len--;
        ed_cx--;
        ed_mod = 1;
    } else if (ed_cy > 0) {                       /* join with the previous line */
        EdLine far *p = &ed_lines[ed_cy - 1];
        int pl = p->len;
        if (l->len > 0) {
            if (!ed_ensure(p, pl + l->len)) return;   /* too long -> abort */
            _fmemcpy(p->txt + pl, l->txt, (unsigned)l->len);
            p->len = pl + l->len;
        }
        ed_remove_line(ed_cy);
        ed_cy--;
        ed_cx = pl;
        ed_mod = 1;
    }
}

static void ed_del(void)
{
    EdLine far *l = &ed_lines[ed_cy];
    if (ed_cx < l->len) {
        int i;
        for (i = ed_cx; i < l->len - 1; i++) l->txt[i] = l->txt[i + 1];
        l->len--;
        ed_mod = 1;
    } else if (ed_cy < ed_n - 1) {                /* append the following line */
        EdLine far *nx = &ed_lines[ed_cy + 1];
        if (nx->len > 0) {
            if (!ed_ensure(l, l->len + nx->len)) return;
            _fmemcpy(l->txt + l->len, nx->txt, (unsigned)nx->len);
            l->len += nx->len;
        }
        ed_remove_line(ed_cy + 1);
        ed_mod = 1;
    }
}

/* -------------------------------------------------------------------------
 * Scrolling / drawing
 * ---------------------------------------------------------------------- */
static void ed_scroll(int content_rows)
{
    if (ed_cy < 0)        ed_cy = 0;
    if (ed_cy >= ed_n)    ed_cy = ed_n - 1;
    if (ed_cx < 0)        ed_cx = 0;
    if (ed_cx > ed_lines[ed_cy].len) ed_cx = ed_lines[ed_cy].len;

    if (ed_cy < ed_top)                      ed_top = ed_cy;
    if (ed_cy >= ed_top + content_rows)      ed_top = ed_cy - content_rows + 1;
    if (ed_top < 0)                          ed_top = 0;

    if (ed_cx < ed_hoff)                     ed_hoff = ed_cx;
    if (ed_cx >= ed_hoff + SCREEN_COLS)      ed_hoff = ed_cx - SCREEN_COLS + 1;
    if (ed_hoff < 0)                         ed_hoff = 0;
}

static void ed_draw(const char *title, int content_rows)
{
    char head[120];
    char foot[120];
    int  r;

    /* Header line: file, line/column, modified marker. */
    sprintf(head, " %.28s   %s %d/%d  %s %d%s",
            title ? title : "",
            L("Ln", "Z"), ed_cy + 1, ed_n,
            L("Col", "Sp"), ed_cx + 1,
            ed_mod ? L("  *modified", "  *ge" ae "ndert") : "");
    fill_rect(0, 0, 1, SCREEN_COLS, ' ', ATTR_MENUBAR);
    draw_text(0, 0, head, ATTR_MENUBAR, SCREEN_COLS);

    /* Content lines. */
    for (r = 0; r < content_rows; r++) {
        int  row = 1 + r;
        int  li  = ed_top + r;
        fill_rect(row, 0, 1, SCREEN_COLS, ' ', ATTR_PANEL);
        if (li < ed_n) {
            EdLine far *l = &ed_lines[li];
            int col;
            for (col = 0; col < SCREEN_COLS; col++) {
                int srcidx = ed_hoff + col;
                if (srcidx < l->len) {
                    unsigned char ch = (unsigned char)l->txt[srcidx];
                    char c;
                    if (ch == '\t')                 c = ' ';
                    else if (ch < 32 || ch == 127)  c = '.';
                    else                            c = (char)ch;
                    putchar_at(row, col, c, ATTR_PANEL);
                }
            }
        }
    }

    /* Footer line: key hint. */
    sprintf(foot, " %s",
            L("F2 Save   Esc Quit   Enter splits line",
              "F2 Speichern   Esc Ende   Enter teilt Zeile"));
    fill_rect(SCREEN_ROWS - 1, 0, 1, SCREEN_COLS, ' ', ATTR_MENUBAR);
    draw_text(SCREEN_ROWS - 1, 0, foot, ATTR_MENUBAR, SCREEN_COLS);
}

/* -------------------------------------------------------------------------
 * Main loop
 * ---------------------------------------------------------------------- */
int edit_file(const char *path, const char *title)
{
    int content_rows = SCREEN_ROWS - 2;   /* header (0) + content + footer (24) */
    int running = 1;
    int saved = 0;
    int rc;

    ed_lines = 0; ed_n = 0; ed_mod = 0;
    ed_cx = ed_cy = ed_top = ed_hoff = 0;

    rc = ed_load(path);
    if (rc == -1) {
        ed_free();
        dlg_error(L("Edit", "Bearbeiten"),
                  L("Out of memory.", "Zu wenig Speicher."));
        return 0;
    }
    if (rc == -2) {
        ed_free();
        dlg_error(L("Edit", "Bearbeiten"),
                  L("File too large to edit.",
                    "Datei zu gro" ss " zum Bearbeiten."));
        return 0;
    }
    if (ed_n == 0) ed_addline("", 0);     /* always at least one line */

    while (running) {
        int k;
        ed_scroll(content_rows);
        ed_draw(title, content_rows);
        show_cursor(1);
        set_cursor(1 + (ed_cy - ed_top), ed_cx - ed_hoff);

        k = readkey();
        switch (k) {
        case KEY_ESC:
            if (!ed_mod ||
                dlg_confirm(L("Edit", "Bearbeiten"),
                            L("Discard unsaved changes?",
                              "Ungespeicherte " Ae "nderungen verwerfen?")))
                running = 0;
            break;
        case KEY_F2:
            if (ed_save(path)) saved = 1;
            else dlg_error(L("Save", "Speichern"),
                           L("Write failed.", "Schreiben fehlgeschlagen."));
            break;
        case KEY_UP:    ed_cy--; break;
        case KEY_DOWN:  ed_cy++; break;
        case KEY_LEFT:
            if (ed_cx > 0) ed_cx--;
            else if (ed_cy > 0) { ed_cy--; ed_cx = ed_lines[ed_cy].len; }
            break;
        case KEY_RIGHT:
            if (ed_cx < ed_lines[ed_cy].len) ed_cx++;
            else if (ed_cy < ed_n - 1) { ed_cy++; ed_cx = 0; }
            break;
        case KEY_HOME:  ed_cx = 0; break;
        case KEY_END:   ed_cx = ed_lines[ed_cy].len; break;
        case KEY_PGUP:  ed_cy -= content_rows; ed_top -= content_rows; break;
        case KEY_PGDN:  ed_cy += content_rows; ed_top += content_rows; break;
        case KEY_ENTER:   ed_split();     break;
        case KEY_BACKSP:  ed_backspace(); break;
        case KEY_DEL:     ed_del();       break;
        default:
            if (k >= 32 && k != 127 && k < 0x100) ed_insert_char(k);
            break;
        }
    }

    show_cursor(0);
    ed_free();
    return saved;
}
