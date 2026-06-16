/* =============================================================================
 * viewer.cpp - Full-screen text viewer (F3)
 * -----------------------------------------------------------------------------
 * Reads up to VIEW_BUF_MAX bytes of the file into the far heap, builds a
 * line index, and displays the file with scrolling. Deliberately memory-
 * limited: a single malloc request (<64 KB in the large model = far heap)
 * plus an offset array. Does not load the entire file.
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>

#include "viewer.h"
#include "tui.h"
#include "keymap.h"
#include "dialog.h"
#include "i18n.h"
#include "umlaut.h"   /* always the last include */

#define VIEW_BUF_MAX    32000u   /* max. bytes read in (spares scarce RAM) */
#define VIEW_MAX_LINES   4000    /* max. indexed lines                     */

void view_file(const char *path, const char *title)
{
    FILE          *f;
    char      far *buf;
    unsigned  far *lstart;       /* offset of each line start in buf */
    long           nlines = 0;
    int            truncated = 0;
    size_t         n;

    f = fopen(path, "rb");
    if (!f) {
        dlg_error(L("View", "Anzeigen"),
                  L("Cannot open file.", "Datei nicht lesbar."));
        return;
    }

    buf    = (char far *)malloc(VIEW_BUF_MAX);
    lstart = (unsigned far *)malloc((unsigned)VIEW_MAX_LINES * sizeof(unsigned));
    if (!buf || !lstart) {
        if (buf)    free(buf);
        if (lstart) free(lstart);
        fclose(f);
        dlg_error(L("View", "Anzeigen"),
                  L("Out of memory.", "Zu wenig Speicher."));
        return;
    }

    n = fread(buf, 1, VIEW_BUF_MAX - 1, f);
    if (n == VIEW_BUF_MAX - 1 && fgetc(f) != EOF) truncated = 1;
    fclose(f);

    /* --- Build the line index (remember the start of each line) --- */
    {
        unsigned pos = 0;
        lstart[nlines++] = 0;
        while (pos < n && nlines < VIEW_MAX_LINES) {
            if (buf[pos] == '\n')
                lstart[nlines++] = (unsigned)(pos + 1);
            pos++;
        }
        if (nlines >= VIEW_MAX_LINES) truncated = 1;
    }

    /* --- Display loop --- */
    {
        int  content_rows = SCREEN_ROWS - 2;   /* header (0) + content + footer (24) */
        long top  = 0;
        int  hoff = 0;
        int  running = 1;

        show_cursor(0);
        while (running) {
            char head[120];
            char foot[120];
            long lastvis;
            int  r, k;
            long maxtop;

            /* Header line (file name + line position). */
            lastvis = top + content_rows;
            if (lastvis > nlines) lastvis = nlines;
            sprintf(head, " %.38s   %s %ld-%ld/%ld%s",
                    title ? title : "",
                    L("Line", "Zeile"),
                    top + 1, lastvis, nlines,
                    truncated ? L("  [truncated]", "  [gek" ue "rzt]") : "");
            fill_rect(0, 0, 1, SCREEN_COLS, ' ', ATTR_MENUBAR);
            draw_text(0, 0, head, ATTR_MENUBAR, SCREEN_COLS);

            /* Content lines. */
            for (r = 0; r < content_rows; r++) {
                int  row = 1 + r;
                long li  = top + r;
                fill_rect(row, 0, 1, SCREEN_COLS, ' ', ATTR_PANEL);
                if (li < nlines) {
                    unsigned s   = lstart[li];
                    unsigned e   = (li + 1 < nlines) ? lstart[li + 1] : (unsigned)n;
                    int      len = (int)(e - s);
                    int      col;
                    while (len > 0 &&
                           (buf[s + len - 1] == '\n' || buf[s + len - 1] == '\r'))
                        len--;
                    for (col = 0; col < SCREEN_COLS; col++) {
                        int  srcidx = hoff + col;
                        char c;
                        if (srcidx < len) {
                            unsigned char ch = (unsigned char)buf[s + srcidx];
                            if (ch == '\t')              c = ' ';
                            else if (ch < 32 || ch == 127) c = '.';
                            else                          c = (char)ch;
                        } else {
                            c = ' ';
                        }
                        putchar_at(row, col, c, ATTR_PANEL);
                    }
                }
            }

            /* Footer line (key hint). */
            sprintf(foot, " %s",
                    L("Arrows/PgUp-Dn scroll   <-/-> sideways   Esc Quit",
                      "Pfeile/Bild scrollen   <-/-> seitlich   Esc Ende"));
            fill_rect(SCREEN_ROWS - 1, 0, 1, SCREEN_COLS, ' ', ATTR_MENUBAR);
            draw_text(SCREEN_ROWS - 1, 0, foot, ATTR_MENUBAR, SCREEN_COLS);

            /* Input. */
            maxtop = nlines - content_rows;
            if (maxtop < 0) maxtop = 0;
            k = readkey();
            switch (k) {
            case KEY_ESC: case 'q': case 'Q':  running = 0; break;
            case KEY_UP:    if (top > 0)      top--; break;
            case KEY_DOWN:  if (top < maxtop) top++; break;
            case KEY_PGUP:  top -= content_rows; if (top < 0) top = 0; break;
            case KEY_PGDN:  top += content_rows; if (top > maxtop) top = maxtop; break;
            case KEY_HOME:  top = 0; hoff = 0; break;
            case KEY_END:   top = maxtop; break;
            case KEY_LEFT:  hoff -= 8; if (hoff < 0)   hoff = 0;   break;
            case KEY_RIGHT: hoff += 8; if (hoff > 240) hoff = 240; break;
            default: break;
            }
        }
    }

    free(buf);
    free(lstart);
}
