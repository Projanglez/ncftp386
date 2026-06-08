/* =============================================================================
 * viewer.cpp - Vollbild-Textbetrachter (F3)
 * -----------------------------------------------------------------------------
 * Liest bis VIEW_BUF_MAX Bytes der Datei in den FAR-Heap, baut einen
 * Zeilenindex auf und zeigt die Datei scrollbar an. Bewusst speicherbegrenzt:
 * eine einzelne malloc-Anforderung (<64 KB im Large Model = FAR-Heap) plus
 * ein Offset-Array. Kein Laden der gesamten Datei.
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
#include "umlaut.h"   /* immer als letzter Include */

#define VIEW_BUF_MAX    32000u   /* max. eingelesene Bytes (schont knappen RAM) */
#define VIEW_MAX_LINES   4000    /* max. indizierte Zeilen                     */

void view_file(const char *path, const char *title)
{
    FILE          *f;
    char      far *buf;
    unsigned  far *lstart;       /* Offset jedes Zeilenanfangs in buf */
    long           nlines = 0;
    int            truncated = 0;
    size_t         n;

    f = fopen(path, "rb");
    if (!f) {
        dlg_error(L("Anzeigen", "View"),
                  L("Datei nicht lesbar.", "Cannot open file."));
        return;
    }

    buf    = (char far *)malloc(VIEW_BUF_MAX);
    lstart = (unsigned far *)malloc((unsigned)VIEW_MAX_LINES * sizeof(unsigned));
    if (!buf || !lstart) {
        if (buf)    free(buf);
        if (lstart) free(lstart);
        fclose(f);
        dlg_error(L("Anzeigen", "View"),
                  L("Zu wenig Speicher.", "Out of memory."));
        return;
    }

    n = fread(buf, 1, VIEW_BUF_MAX - 1, f);
    if (n == VIEW_BUF_MAX - 1 && fgetc(f) != EOF) truncated = 1;
    fclose(f);

    /* --- Zeilenindex aufbauen (Zeilenanfaenge merken) --- */
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

    /* --- Anzeige-Schleife --- */
    {
        int  content_rows = SCREEN_ROWS - 2;   /* Kopf (0) + Inhalt + Fuss (24) */
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

            /* Kopfzeile (Dateiname + Zeilenposition). */
            lastvis = top + content_rows;
            if (lastvis > nlines) lastvis = nlines;
            sprintf(head, " %.38s   %s %ld-%ld/%ld%s",
                    title ? title : "",
                    L("Zeile", "Line"),
                    top + 1, lastvis, nlines,
                    truncated ? L("  [gek" ue "rzt]", "  [truncated]") : "");
            fill_rect(0, 0, 1, SCREEN_COLS, ' ', ATTR_MENUBAR);
            draw_text(0, 0, head, ATTR_MENUBAR, SCREEN_COLS);

            /* Inhaltszeilen. */
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

            /* Fusszeile (Tastenhinweis). */
            sprintf(foot, " %s",
                    L("Pfeile/Bild scrollen   <-/-> seitlich   Esc Ende",
                      "Arrows/PgUp-Dn scroll   <-/-> sideways   Esc Quit"));
            fill_rect(SCREEN_ROWS - 1, 0, 1, SCREEN_COLS, ' ', ATTR_MENUBAR);
            draw_text(SCREEN_ROWS - 1, 0, foot, ATTR_MENUBAR, SCREEN_COLS);

            /* Eingabe. */
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
