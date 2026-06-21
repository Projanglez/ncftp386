/* =============================================================================
 * panel.h - Abstract panel base class for NCFTP386
 * -----------------------------------------------------------------------------
 * Logic shared by the two file-list panels (local & FTP remote):
 *   - Entry list (name, size, date, directory flag)
 *   - Cursor (selected entry) + scroll offset
 *   - Keyboard navigation (up/down, paging, Home/End)
 *   - Full rendering into a screen region (Norton Commander look)
 *
 * refresh() is purely virtual: the local panel reads the DOS filesystem,
 * the FTP panel the remote LIST output. Everything else is shared here.
 *
 * Memory note: each panel object contains the entry array directly
 * (~25 KB). When linking (step 3), set -zt<n> as needed so Open Watcom
 * places the large objects in FAR data segments and DGROUP (64 KB) doesn't
 * overflow.
 * ===========================================================================*/
#ifndef PANEL_H
#define PANEL_H

#define PANEL_NAME_MAX    40   /* holds local 8.3 AND longer FTP names        */
#define PANEL_MAX_ENTRIES 512  /* max. entries per directory                  */
#define PANEL_HEADER_MAX  80   /* path/title line (DOS path max. ~64+drive)   */

/* A directory entry (POD - re-sortable via qsort). */
struct PanelEntry {
    char          name[PANEL_NAME_MAX];
    unsigned long size;       /* size in bytes (0 for directories)            */
    unsigned      date;       /* DOS date word (bits: YYYYYYY MMMM DDDDD)     */
    unsigned      time;       /* DOS time word (bits: HHHHH MMMMMM SSSSS)     */
    unsigned char is_dir;     /* 1 = directory                                */
    unsigned char is_parent;  /* 1 = ".." entry (always sorted first)         */
    unsigned char marked;     /* 1 = marked by the user (Insert key)          */
};

class Panel {
public:
    Panel();
    virtual ~Panel();

    /* Set the screen region (top-left row/column, height, width). */
    void set_region(int top_, int left_, int height_, int width_);

    /* Active status (cursor highlight + header color). */
    void set_active(int a) { active = a; }

    /* Re-read the content - implemented by the subclass.
     * Returns: number of entries. */
    virtual int refresh() = 0;

    /* Actions (overridden by the subclass; base = no-op).
     * enter_selected(): Enter on the selected entry.
     *   Returns 1 = entered a directory (list refreshed), 0 = file/no action.
     * go_parent(): switch to the parent directory. */
    virtual int  enter_selected();
    virtual void go_parent();

    /* Draw the whole panel (border, header, columns, entries). */
    void draw();

    /* Navigation (only updates state; the caller calls draw() afterwards). */
    void page_up();
    void page_down();
    void move_home();
    void move_end();

    /* Move the cursor by delta (+1/-1) and draw FLICKER-FREE: for plain
     * movement within the visible area, only the old and new cursor row
     * are redrawn; a full rebuild only happens when scrolling at the edge. */
    void move_step(int delta);

    /* Currently selected entry (0 if the list is empty). */
    PanelEntry *selected();
    int         selected_index() const { return cursor; }
    int         entry_count()    const { return count; }

    /* Set the cursor to the entry with this name (case-insensitive). If it
     * is not found, the cursor stays within the valid range. Used to "keep
     * the cursor on the item after the operation". */
    void select_by_name(const char *name);
    /* Set the cursor directly to an index (clamped to the valid range).
     * Used to "stay nearby after deleting". */
    void set_cursor_index(int idx);

    /* Entry by index (0 if outside the valid range). */
    PanelEntry *entry_at(int i);

    /* --- Multi-selection (Norton Commander style, Insert key) --- */
    /* Toggle the mark on the current entry and move the cursor down.
     * The ".." entry cannot be marked. */
    void          toggle_mark();
    void          invert_marks();  /* numpad *: invert all marks                  */
    /* Numpad +: in the active panel, mark all entries that don't exist in
     * `other` (name, case-insensitive) or exist there with a different size
     * (files only). If `other` is empty/null: mark everything. */
    void          compare_mark(const Panel *other);
    void          clear_marks();
    int           marked_count()     const; /* number of marked entries      */
    unsigned long marked_size()      const; /* sum of sizes (files only)     */
    int           marked_dir_count() const; /* number of marked directories  */

    /* 1 if an entry with this name exists (".." excluded), case-insensitive.
     * Used for the overwrite prompt during upload. */
    int         has_entry(const char *name) const;

    /* Panel title/path line (local: directory, remote: FTP path). */
    const char *title() const { return header; }

protected:
    /* Index of the entry with this name (-1 = not found, case-insensitive).
     * Internal helper for has_entry and compare_mark. */
    int         find_entry(const char *name) const;
    /* --- Screen region --- */
    int top, left, height, width;

    /* --- Content --- */
    PanelEntry entries[PANEL_MAX_ENTRIES];
    int        count;       /* number of valid entries              */
    int        cursor;      /* index of the selected entry          */
    int        topentry;    /* index of the first visible entry     */
    int        active;      /* 1 = active panel                     */
    char       header[PANEL_HEADER_MAX];  /* path/title line        */

    /* --- Helper functions --- */
    int  visible_rows() const;   /* number of visible entry rows           */
    void clamp_scroll();         /* adjust topentry so the cursor is visible */
    void draw_entry_row(int idx);/* redraw just one entry row (idx)        */
    void format_entry(const PanelEntry *e, char *out, int inner) const;
    virtual unsigned char frame_attr() const;  /* border color (overridable) */
    /* Display case convention: 1 = Norton style (UPPERCASE directories,
     * lowercase files); 0 = show the name verbatim. The local panel keeps
     * the Norton convention; the FTP panel overrides this to 0 because Unix
     * servers are case-sensitive (see RemotePanel). */
    virtual int nc_case() const;
};

#endif /* PANEL_H */
