/* =============================================================================
 * tui.h - TUI-Engine fuer NCFTP386
 * -----------------------------------------------------------------------------
 * Direkter Zugriff auf den CGA/EGA/VGA-Textbildschirmspeicher (0xB800:0000),
 * Box-Drawing, Text-Ausgabe, Cursor-Steuerung. Kein curses, kein BIOS-TTY -
 * alles per far-Pointer in den Videospeicher fuer maximale Geschwindigkeit
 * auf echter 386-Hardware.
 *
 * Koordinaten: row = Zeile (0..24), col = Spalte (0..79). 0/0 = oben links.
 * Attribut-Byte: Bits 0-3 Vordergrund, 4-6 Hintergrund, 7 Blinken.
 * ===========================================================================*/
#ifndef TUI_H
#define TUI_H

/* ---- Bildschirmgeometrie (80x25 Farb-Textmodus) ---- */
#define SCREEN_COLS  80
#define SCREEN_ROWS  25
#define SCREEN_CELLS (SCREEN_ROWS * SCREEN_COLS)  /* 2000 Zeichen/Attribut-Paare */

/* ---- Farb-Attribute (DOS attribute bytes), vgl. CLAUDE.md ---- */
#define ATTR_PANEL      0x1F  /* weiss auf blau   (Panelinhalt)        */
#define ATTR_SELECTED   0x30  /* schwarz auf cyan (Cursor-Zeile)       */
#define ATTR_MARKED     0x1E  /* gelb auf blau    (markierter Eintrag) */
#define ATTR_MARKED_SEL 0x3E  /* gelb auf cyan    (markiert + Cursor)  */
#define ATTR_HEADER     0x30  /* schwarz auf cyan (Pfad-Header)        */
#define ATTR_COLHDR     0x1A  /* hellgruen auf blau (Spaltenkoepfe)    */
#define ATTR_BORDER     0x1F  /* weiss auf blau   (Rahmen)             */
#define ATTR_MENUBAR    0x70  /* schwarz auf hellgrau                  */
#define ATTR_STATUSBAR  0x70  /* schwarz auf hellgrau                  */
#define ATTR_FNKEY_NUM  0x30  /* schwarz auf cyan (F1, F2, ...)        */
#define ATTR_FNKEY_LBL  0x70  /* schwarz auf hellgrau (Label)          */
#define ATTR_DIALOG_BG  0x70  /* Dialog-Hintergrund                    */
#define ATTR_DIALOG_HL  0x0F  /* Dialog-Highlight (weiss auf schwarz)  */
#define ATTR_ERROR      0x4F  /* weiss auf rot (Fehlerdialoge)         */

/* ---- CP437 Box-Drawing-Zeichen ---- */
/* Doppelrahmen */
#define BOX_D_HLINE  0xCD  /* = */
#define BOX_D_VLINE  0xBA  /* | */
#define BOX_D_TL     0xC9  /* obere linke  Ecke   */
#define BOX_D_TR     0xBB  /* obere rechte Ecke   */
#define BOX_D_BL     0xC8  /* untere linke Ecke   */
#define BOX_D_BR     0xBC  /* untere rechte Ecke  */
#define BOX_D_TLEFT  0xCC  /* T nach rechts (|=)  linker Rand     */
#define BOX_D_TRIGHT 0xB9  /* T nach links        rechter Rand    */
#define BOX_D_TTOP   0xCB  /* T nach unten        oberer Rand     */
#define BOX_D_TBOT   0xCA  /* T nach oben         unterer Rand    */
/* Einfachrahmen */
#define BOX_S_HLINE  0xC4  /* - */
#define BOX_S_VLINE  0xB3  /* | */
#define BOX_S_TL     0xDA
#define BOX_S_TR     0xBF
#define BOX_S_BL     0xC0
#define BOX_S_BR     0xD9
#define BOX_S_TLEFT  0xC3  /* T nach rechts */
#define BOX_S_TRIGHT 0xB4  /* T nach links  */

/* ---- Initialisierung / Aufraeumen ---- */
void tui_init(void);       /* 80x25-Textmodus erzwingen, Cursor ausblenden     */
void tui_shutdown(void);   /* Cursor wieder an, Bildschirm normal loeschen     */

/* ---- Grundlegende Ausgaberoutinen (Direktzugriff Videospeicher) ---- */
void clear_screen(unsigned char attr);
void putchar_at(int row, int col, char ch, unsigned char attr);
void putattr_at(int row, int col, unsigned char attr);  /* nur Attribut aendern */
void fill_rect(int row, int col, int rows, int cols, char ch, unsigned char attr);
void hline(int row, int col, int len, char ch, unsigned char attr);
void vline(int row, int col, int len, char ch, unsigned char attr);

/* ---- Rahmen & Text ---- */
/* draw_box: zeichnet nur den Rahmen (Inneres unberuehrt). dbl!=0 -> Doppellinie */
void draw_box(int row, int col, int rows, int cols, unsigned char attr, int dbl);
/* draw_hsep: horizontale Trennlinie mit T-Stuecken, passt in eine Box der
 * Breite 'cols' deren linker Rand bei 'col' liegt (z.B. Panel-Header-Trenner) */
void draw_hsep(int row, int col, int cols, unsigned char attr, int dbl);
/* draw_text: schreibt Text ab (row,col). Stoppt bei NUL, bei Spalte 80 und -
 * falls maxlen>0 - nach maxlen Zeichen. Kein Auffuellen (dafuer fill_rect). */
void draw_text(int row, int col, const char far *text, unsigned char attr, int maxlen);

/* ---- Cursor (BIOS INT 10h) ---- */
void set_cursor(int row, int col);
void show_cursor(int visible);   /* 0 = ausblenden, !=0 = einblenden */

/* ---- Bildschirm sichern/wiederherstellen (fuer modale Dialoge) ---- */
/* buf muss mindestens SCREEN_CELLS*2 Bytes gross sein. */
void save_screen(unsigned char far *buf);
void restore_screen(const unsigned char far *buf);

#endif /* TUI_H */
