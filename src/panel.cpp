/* =============================================================================
 * panel.cpp - Abstract panel base class: navigation & rendering
 * -----------------------------------------------------------------------------
 * Column layout in the 38-character-wide inner area (40-column panel):
 *
 *   Col.0          : mark column (reserved)
 *   Name           : left-aligned, truncated
 *   Size/<DIR>     : right-aligned
 *   Date MM-DD-YY  : right-aligned at the right edge
 *
 * Column widths are computed from the actual inner width, so the layout
 * stays correct even with a different panel width.
 * ===========================================================================*/
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "panel.h"
#include "tui.h"
#include "i18n.h"
#include "umlaut.h"   /* always include last */

/* -------------------------------------------------------------------------
 * Column layout
 * ---------------------------------------------------------------------- */
struct ColLayout {
    int name_off, name_w;
    int size_off, size_w;
    int date_off, date_w;
};

static void columns(int inner, ColLayout *c)
{
    c->date_w   = 14;                               /* "DD-MM-YY HH:MM"       */
    c->date_off = inner - c->date_w;
    c->size_w   = 9;                                /* "9,999,999" / "xx.xxxk"*/
    c->size_off = c->date_off - 1 - c->size_w;
    c->name_off = 1;                                /* col.0 = mark column    */
    c->name_w   = c->size_off - 1 - c->name_off;   /* = 12, max 8.3 name     */
    if (c->name_w < 1) c->name_w = 1;               /* guard for mini panels  */
}

/* Write number n into buf with thousands separators (EN: comma, DE: dot).
 * n > 9,999,999: output in kilobytes, e.g. "20,000k". */
static void fmt_size(char *buf, unsigned long n)
{
    unsigned long val = n;
    char suffix = '\0';
    char raw[16];
    const char *sep = L(",", ".");
    int len, i, pos;

    if (n > 9999999UL) { val = n / 1000UL; suffix = 'k'; }
    sprintf(raw, "%lu", val);
    len = (int)strlen(raw);
    pos = 0;
    for (i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) buf[pos++] = sep[0];
        buf[pos++] = raw[i];
    }
    if (suffix) buf[pos++] = suffix;
    buf[pos] = '\0';
}

/* Place text in 'out' at offset 'off', left- or right-aligned within width
 * 'w'. Writes NO NUL terminator (fields sit directly next to each other). */
static void place(char *out, int off, const char *s, int w, int rightalign)
{
    int len = 0;
    int start, i;

    while (s[len] != '\0') len++;
    if (len > w) len = w;                           /* truncate              */

    start = rightalign ? (off + (w - len)) : off;
    for (i = 0; i < len; i++)
        out[start + i] = s[i];
}

/* -------------------------------------------------------------------------
 * Constructor / destructor / geometry
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

/* Default actions: no-op. Subclasses override. */
int  Panel::enter_selected() { return 0; }
void Panel::go_parent()      { }

/* -------------------------------------------------------------------------
 * Navigation
 * ---------------------------------------------------------------------- */
int Panel::visible_rows() const
{
    /* Layout: border + path + separator on top, border at the bottom. */
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

/* Flicker-free cursor movement: only redraw the two affected rows, unless
 * scrolling occurred (in which case a full rebuild is needed). */
void Panel::move_step(int delta)
{
    int old_cursor = cursor;
    int old_top    = topentry;

    cursor += delta;
    clamp_scroll();

    if (topentry != old_top) {        /* scrolled at the edge -> redraw everything */
        draw();
        return;
    }
    if (cursor != old_cursor) {       /* only the old + new cursor row */
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
    clamp_scroll();              /* not found -> keep the cursor valid */
}

void Panel::set_cursor_index(int idx)
{
    cursor = idx;
    clamp_scroll();
}

/* --- Multi-selection ----------------------------------------------------- */
void Panel::toggle_mark()
{
    int marked_idx = cursor;
    int old_top    = topentry;
    PanelEntry *e  = selected();

    if (e && !e->is_parent)
        e->marked = (unsigned char)(e->marked ? 0 : 1);

    cursor++;                    /* like Norton Commander: move on down */
    clamp_scroll();

    if (topentry != old_top) {   /* scrolled -> redraw everything */
        draw();
        return;
    }
    /* flicker-free redraw of the (old) marked row + the new cursor row, if any */
    draw_entry_row(marked_idx);
    if (cursor != marked_idx)
        draw_entry_row(cursor);
}

void Panel::invert_marks()
{
    int i;
    for (i = 0; i < count; i++)
        if (!entries[i].is_parent)
            entries[i].marked = entries[i].marked ? 0 : 1;
    draw();
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

int Panel::marked_dir_count() const
{
    int i, n = 0;
    for (i = 0; i < count; i++) if (entries[i].marked && entries[i].is_dir) n++;
    return n;
}

int Panel::find_entry(const char *name) const
{
    int i;
    if (name == 0 || name[0] == '\0') return -1;
    for (i = 0; i < count; i++) {
        if (entries[i].is_parent) continue;
        if (stricmp(entries[i].name, name) == 0) return i;
    }
    return -1;
}

int Panel::has_entry(const char *name) const
{
    return find_entry(name) >= 0;
}

void Panel::compare_mark(const Panel *other)
{
    int i;
    int other_empty = (other == 0 || other->count == 0);
    for (i = 0; i < count; i++) {
        PanelEntry *e = &entries[i];
        if (e->is_parent) continue;
        if (other_empty) { e->marked = 1; continue; }
        int idx = other->find_entry(e->name);
        if (idx < 0) {
            e->marked = 1;
        } else if (!e->is_dir && other->entries[idx].size != e->size) {
            e->marked = 1;
        }
    }
    draw();
}

/* -------------------------------------------------------------------------
 * Format an entry
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

    /* Name: directories UPPERCASE, files lowercase. */
    for (i = 0; e->name[i] && i < PANEL_NAME_MAX - 1; i++)
        dispname[i] = (char)(e->is_dir
                             ? toupper((unsigned char)e->name[i])
                             : tolower((unsigned char)e->name[i]));
    dispname[i] = '\0';
    place(out, c.name_off, dispname, c.name_w, 0);

    /* Size or <DIR> (right-aligned). */
    if (e->is_dir) {
        place(out, c.size_off, "<DIR>", c.size_w, 1);
    } else {
        fmt_size(tmp, e->size);
        place(out, c.size_off, tmp, c.size_w, 1);
    }

    /* Date+time (right-aligned). Left empty for the ".." entry.
     * EN: "MM-DD-YY HH:MM"  /  DE: "DD-MM-YY HH:MM"  (both 14 characters) */
    if (!e->is_parent) {
        int year  = (int)((1980 + (int)((e->date >> 9) & 0x7F)) % 100);
        int month = (int)((e->date >> 5) & 0x0F);
        int day   = (int)(e->date & 0x1F);
        int hh    = (int)((e->time >> 11) & 0x1F);
        int mm    = (int)((e->time >> 5)  & 0x3F);
        if (g_english)
            sprintf(tmp, "%02d-%02d-%02d %02d:%02d", month, day, year, hh, mm);
        else
            sprintf(tmp, "%02d-%02d-%02d %02d:%02d", day, month, year, hh, mm);
        place(out, c.date_off, tmp, c.date_w, 1);
    }
}

/* -------------------------------------------------------------------------
 * Drawing
 * ---------------------------------------------------------------------- */

/* Redraw a single entry row (entry index idx). If idx is outside the
 * visible window, nothing happens. Empty rows (idx beyond the list) are
 * cleared in panel color. Basis for the flicker-free cursor redraw and for
 * the full draw(). */
void Panel::draw_entry_row(int idx)
{
    int  inner = width - 2;
    int  vr    = visible_rows();
    int  rel   = idx - topentry;
    int  row;
    char buf[PANEL_HEADER_MAX];

    if (inner < 1) return;
    if (rel < 0 || rel >= vr) return;        /* not visible */
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
    unsigned char ha = ATTR_PANEL;   /* path header: always white on blue */
    int vr = visible_rows();
    int i;
    char buf[PANEL_HEADER_MAX];

    if (inner < 1) return;

    /* 1) Clear the whole panel area in panel color. */
    fill_rect(top, left, height, width, ' ', ATTR_PANEL);

    /* 2) Double border + divider below the path header. */
    draw_box(top, left, height, width, fa, 1);
    draw_hsep(top + 2, left, width, fa, 1);

    /* 3) Path header (row top+1), centered. If too long, show the end. */
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

    /* 4) Column header (row top+3). */
    {
        ColLayout c;
        columns(inner, &c);
        for (i = 0; i < inner; i++) buf[i] = ' ';
        buf[inner] = '\0';
        place(buf, c.name_off, L("Name", "Name"), c.name_w, 0);
        place(buf, c.size_off, L("Size", "Gr" oe ss "e"), c.size_w, 1);
        place(buf, c.date_off, L("Date      Time", "Datum     Zeit"), c.date_w, 1);
        fill_rect(top + 3, left + 1, 1, inner, ' ', ATTR_COLHDR);
        draw_text(top + 3, left + 1, buf, ATTR_COLHDR, inner);
    }

    /* 5) Entries (rows top+4 .. top+height-2). */
    for (i = 0; i < vr; i++)
        draw_entry_row(topentry + i);
}
