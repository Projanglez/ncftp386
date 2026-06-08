/* =============================================================================
 * panel.cpp - Abstrakte Panel-Basisklasse: Navigation & Rendering
 * -----------------------------------------------------------------------------
 * Spaltenlayout im 38 Zeichen breiten Innenbereich (40-Spalten-Panel):
 *
 *   Sp.0          : Markierungsspalte (reserviert)
 *   Name          : links, abgeschnitten
 *   Groesse/<DIR> : rechtsbuendig
 *   Datum MM-TT-JJ: rechtsbuendig am rechten Rand
 *
 * Spaltenbreiten werden aus der tatsaechlichen Innenbreite berechnet, damit
 * das Layout auch bei abweichender Panelbreite stimmt.
 * ===========================================================================*/
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "panel.h"
#include "tui.h"
#include "i18n.h"
#include "umlaut.h"   /* immer als letzter Include */

/* -------------------------------------------------------------------------
 * Spaltenlayout
 * ---------------------------------------------------------------------- */
struct ColLayout {
    int name_off, name_w;
    int size_off, size_w;
    int date_off, date_w;
};

static void columns(int inner, ColLayout *c)
{
    c->date_w   = 11;                               /* "MM-TT HH:MM"         */
    c->date_off = inner - c->date_w;
    c->size_w   = 10;                               /* "99.999.999" + Reserve*/
    c->size_off = c->date_off - 1 - c->size_w;
    c->name_off = 1;                                /* Sp.0 = Markierung     */
    c->name_w   = c->size_off - 1 - c->name_off;
    if (c->name_w < 1) c->name_w = 1;               /* Schutz fuer Mini-Panel*/
}

/* Zahl n mit Tausenderpunkten in buf schreiben (DE: Punkt, EN: Komma). */
static void fmt_size(char *buf, unsigned long n)
{
    char raw[16];
    const char *sep = L(".", ",");
    int len, i, pos;
    sprintf(raw, "%lu", n);
    len = (int)strlen(raw);
    pos = 0;
    for (i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) buf[pos++] = sep[0];
        buf[pos++] = raw[i];
    }
    buf[pos] = '\0';
}

/* Text in 'out' an Offset 'off' platzieren, links- oder rechtsbuendig in der
 * Breite 'w'. Schreibt KEINE NUL (Felder liegen direkt aneinander). */
static void place(char *out, int off, const char *s, int w, int rightalign)
{
    int len = 0;
    int start, i;

    while (s[len] != '\0') len++;
    if (len > w) len = w;                           /* abschneiden           */

    start = rightalign ? (off + (w - len)) : off;
    for (i = 0; i < len; i++)
        out[start + i] = s[i];
}

/* -------------------------------------------------------------------------
 * Konstruktor / Destruktor / Geometrie
 * ---------------------------------------------------------------------- */
Panel::Panel()
{
    top = 1; left = 0; height = 21; width = 40;
    count = 0; cursor = 0; topentry = 0; active = 0;
    header[0] = '\0';
}

Panel::~Panel()
{
}

void Panel::set_region(int top_, int left_, int height_, int width_)
{
    top = top_; left = left_; height = height_; width = width_;
}

unsigned char Panel::frame_attr() const
{
    return ATTR_BORDER;
}

/* Default-Aktionen: wirkungslos. Unterklassen ueberschreiben. */
int  Panel::enter_selected() { return 0; }
void Panel::go_parent()      { }

/* -------------------------------------------------------------------------
 * Navigation
 * ---------------------------------------------------------------------- */
int Panel::visible_rows() const
{
    /* Aufbau: oben Rahmen + Pfad + Trenner + Spaltenkopf, unten Rahmen. */
    return (height > 5) ? (height - 5) : 0;
}

void Panel::clamp_scroll()
{
    int vr = visible_rows();

    if (count <= 0) { cursor = 0; topentry = 0; return; }

    if (cursor < 0)        cursor = 0;
    if (cursor >= count)   cursor = count - 1;

    if (cursor < topentry)            topentry = cursor;
    if (cursor >= topentry + vr)      topentry = cursor - vr + 1;
    if (topentry < 0)                 topentry = 0;
    if (topentry > count - vr)        topentry = count - vr;
    if (topentry < 0)                 topentry = 0;   /* count < vr */
}

void Panel::move_up()    { cursor--;                clamp_scroll(); }
void Panel::move_down()  { cursor++;                clamp_scroll(); }

/* Flimmerfreie Cursor-Bewegung: nur die zwei betroffenen Zeilen neu zeichnen,
 * sofern nicht gescrollt wurde (dann ist ein Vollaufbau noetig). */
void Panel::move_step(int delta)
{
    int old_cursor = cursor;
    int old_top    = topentry;

    cursor += delta;
    clamp_scroll();

    if (topentry != old_top) {        /* am Rand gescrollt -> alles neu */
        draw();
        return;
    }
    if (cursor != old_cursor) {       /* nur alte + neue Cursorzeile */
        draw_entry_row(old_cursor);
        draw_entry_row(cursor);
    }
}
void Panel::page_up()    { cursor -= visible_rows();clamp_scroll(); }
void Panel::page_down()  { cursor += visible_rows();clamp_scroll(); }
void Panel::move_home()  { cursor = 0;  topentry = 0; clamp_scroll(); }
void Panel::move_end()   { cursor = count - 1;        clamp_scroll(); }

PanelEntry *Panel::selected()
{
    if (cursor < 0 || cursor >= count) return 0;
    return &entries[cursor];
}

PanelEntry *Panel::entry_at(int i)
{
    if (i < 0 || i >= count) return 0;
    return &entries[i];
}

void Panel::select_by_name(const char *name)
{
    int i;
    if (name && name[0]) {
        for (i = 0; i < count; i++) {
            if (stricmp(entries[i].name, name) == 0) {
                cursor = i;
                clamp_scroll();
                return;
            }
        }
    }
    clamp_scroll();              /* nicht gefunden -> Cursor gueltig halten */
}

void Panel::set_cursor_index(int idx)
{
    cursor = idx;
    clamp_scroll();
}

/* --- Mehrfachauswahl ---------------------------------------------------- */
void Panel::toggle_mark()
{
    int marked_idx = cursor;
    int old_top    = topentry;
    PanelEntry *e  = selected();

    if (e && !e->is_parent)
        e->marked = (unsigned char)(e->marked ? 0 : 1);

    cursor++;                    /* wie Norton Commander: weiter nach unten */
    clamp_scroll();

    if (topentry != old_top) {   /* gescrollt -> alles neu */
        draw();
        return;
    }
    /* markierte (alte) Zeile + ggf. neue Cursorzeile flimmerfrei neu malen */
    draw_entry_row(marked_idx);
    if (cursor != marked_idx)
        draw_entry_row(cursor);
}

void Panel::clear_marks()
{
    int i;
    for (i = 0; i < count; i++) entries[i].marked = 0;
}

int Panel::marked_count() const
{
    int i, n = 0;
    for (i = 0; i < count; i++) if (entries[i].marked) n++;
    return n;
}

unsigned long Panel::marked_size() const
{
    unsigned long s = 0;
    int i;
    for (i = 0; i < count; i++)
        if (entries[i].marked && !entries[i].is_dir) s += entries[i].size;
    return s;
}

int Panel::has_entry(const char *name) const
{
    int i;
    if (name == 0 || name[0] == '\0') return 0;
    for (i = 0; i < count; i++) {
        if (entries[i].is_parent) continue;
        if (stricmp(entries[i].name, name) == 0) return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Eintrag formatieren
 * ---------------------------------------------------------------------- */
void Panel::format_entry(const PanelEntry *e, char *out, int inner) const
{
    ColLayout c;
    int i;
    char tmp[16];
    char dispname[PANEL_NAME_MAX];

    columns(inner, &c);

    for (i = 0; i < inner; i++) out[i] = ' ';
    out[inner] = '\0';

    /* Name: Verzeichnisse GROSSBUCHSTABEN, Dateien kleinbuchstaben. */
    for (i = 0; e->name[i] && i < PANEL_NAME_MAX - 1; i++)
        dispname[i] = (char)(e->is_dir
                             ? toupper((unsigned char)e->name[i])
                             : tolower((unsigned char)e->name[i]));
    dispname[i] = '\0';
    place(out, c.name_off, dispname, c.name_w, 0);

    /* Groesse oder <DIR> (rechts). */
    if (e->is_dir) {
        place(out, c.size_off, "<DIR>", c.size_w, 1);
    } else {
        fmt_size(tmp, e->size);
        place(out, c.size_off, tmp, c.size_w, 1);
    }

    /* Datum+Zeit "MM-TT HH:MM" (rechts). Beim ".."-Eintrag leer lassen. */
    if (!e->is_parent) {
        int month = (int)((e->date >> 5) & 0x0F);
        int day   = (int)(e->date & 0x1F);
        int hh    = (int)((e->time >> 11) & 0x1F);
        int mm    = (int)((e->time >> 5)  & 0x3F);
        sprintf(tmp, "%02d-%02d %02d:%02d", month, day, hh, mm);
        place(out, c.date_off, tmp, c.date_w, 1);
    }
}

/* -------------------------------------------------------------------------
 * Zeichnen
 * ---------------------------------------------------------------------- */

/* Eine einzelne Eintragszeile (entry-Index idx) neu zeichnen. Liegt idx
 * ausserhalb des sichtbaren Fensters, passiert nichts. Leere Zeilen (idx jen-
 * seits der Liste) werden in Panelfarbe geleert. Basis fuer den flimmerfreien
 * Cursor-Redraw und fuer den Voll-draw(). */
void Panel::draw_entry_row(int idx)
{
    int  inner = width - 2;
    int  vr    = visible_rows();
    int  rel   = idx - topentry;
    int  row;
    char buf[PANEL_HEADER_MAX];

    if (inner < 1) return;
    if (rel < 0 || rel >= vr) return;        /* nicht sichtbar */
    row = top + 4 + rel;

    if (idx >= 0 && idx < count) {
        int is_cur = (active && idx == cursor);
        int is_mk  = entries[idx].marked;
        unsigned char a;
        if (is_cur) a = is_mk ? ATTR_MARKED_SEL : ATTR_SELECTED;
        else        a = is_mk ? ATTR_MARKED     : ATTR_PANEL;
        fill_rect(row, left + 1, 1, inner, ' ', a);
        format_entry(&entries[idx], buf, inner);
        draw_text(row, left + 1, buf, a, inner);
    } else {
        fill_rect(row, left + 1, 1, inner, ' ', ATTR_PANEL);
    }
}

void Panel::draw()
{
    int inner = width - 2;
    unsigned char fa = frame_attr();
    unsigned char ha = ATTR_PANEL;   /* Pfad-Header: immer weiss auf blau */
    int vr = visible_rows();
    int i;
    char buf[PANEL_HEADER_MAX];

    if (inner < 1) return;

    /* 1) Gesamte Panelflaeche in Panelfarbe loeschen. */
    fill_rect(top, left, height, width, ' ', ATTR_PANEL);

    /* 2) Doppelrahmen + Trennlinie unter dem Pfad-Header. */
    draw_box(top, left, height, width, fa, 1);
    draw_hsep(top + 2, left, width, fa, 1);

    /* 3) Pfad-Header (Zeile top+1), zentriert. Bei Ueberlaenge das Ende zeigen. */
    {
        const char far *h = header;
        const char far *p = h;
        int len = 0, pad;
        while (h[len] != '\0') len++;
        if (len > inner) { p = h + (len - inner); len = inner; }
        pad = (inner - len) / 2;
        fill_rect(top + 1, left + 1, 1, inner, ' ', ha);
        draw_text(top + 1, left + 1 + pad, p, ha, len);
    }

    /* 4) Spaltenkopf (Zeile top+3). */
    {
        ColLayout c;
        columns(inner, &c);
        for (i = 0; i < inner; i++) buf[i] = ' ';
        buf[inner] = '\0';
        place(buf, c.name_off, L("Name", "Name"), c.name_w, 0);
        place(buf, c.size_off, L("Gr" oe ss "e", "Size"), c.size_w, 1);
        place(buf, c.date_off, L("Datum  Zeit", "Date  Time"), c.date_w, 1);
        fill_rect(top + 3, left + 1, 1, inner, ' ', ATTR_COLHDR);
        draw_text(top + 3, left + 1, buf, ATTR_COLHDR, inner);
    }

    /* 5) Eintraege (Zeilen top+4 .. top+height-2). */
    for (i = 0; i < vr; i++)
        draw_entry_row(topentry + i);
}
