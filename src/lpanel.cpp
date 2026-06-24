/* =============================================================================
 * lpanel.cpp - Local filesystem panel
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <dos.h>      /* _dos_findfirst/_dos_findnext, _A_* , struct find_t  */
#include <direct.h>   /* getcwd, chdir                                       */
#include <string.h>   /* strncpy, strcpy, stricmp                            */
#include <stdlib.h>   /* qsort                                               */

#include "lpanel.h"

/* Determine the last path component ("leaf name"). "C:\FOO\BAR" -> "BAR",
 * "C:\" -> "" (the root has no leaf). */
static void path_leaf(const char *path, char *out, int outsz)
{
    const char *p, *leaf = path;
    int n = 0;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') leaf = p + 1;
    while (leaf[n] && n < outsz - 1) { out[n] = leaf[n]; n++; }
    out[n] = '\0';
}

LocalPanel::LocalPanel()
{
    cwd[0] = '\0';
}

/* Determine the current working directory (including drive). */
void LocalPanel::read_cwd()
{
    if (getcwd(cwd, PANEL_HEADER_MAX) == 0) {
        /* Fallback in case getcwd fails. */
        strcpy(cwd, "C:\\");
    }
}

/* Re-read the directory. Returns: number of entries. */
int LocalPanel::refresh()
{
    struct find_t ff;
    unsigned amask = _A_SUBDIR | _A_HIDDEN | _A_SYSTEM | _A_RDONLY | _A_ARCH;
    unsigned rc;

    count = 0;
    read_cwd();
    strncpy(header, cwd, PANEL_HEADER_MAX - 1);
    header[PANEL_HEADER_MAX - 1] = '\0';

    rc = _dos_findfirst("*.*", amask, &ff);
    while (rc == 0 && count < PANEL_MAX_ENTRIES) {
        /* Skip "." (the current directory). */
        if (!(ff.name[0] == '.' && ff.name[1] == '\0')) {
            PanelEntry *e = &entries[count];
            strncpy(e->name, ff.name, PANEL_NAME_MAX - 1);
            e->name[PANEL_NAME_MAX - 1] = '\0';
            e->fullname  = 0;             /* 8.3 names always fit in 'name' */
            e->size      = ff.size;
            e->date      = ff.wr_date;
            e->time      = ff.wr_time;
            e->is_dir    = (ff.attrib & _A_SUBDIR) ? 1 : 0;
            e->is_parent = (ff.name[0] == '.' && ff.name[1] == '.' &&
                            ff.name[2] == '\0') ? 1 : 0;
            e->marked    = 0;
            count++;
        }
        rc = _dos_findnext(&ff);
    }

    sort_entries();

    cursor = 0;
    topentry = 0;
    return count;
}

/* Enter on the selected entry.
 * Returns: 1 = entered a directory (panel re-read),
 *          0 = file (caller handles viewing/launching it). */
int LocalPanel::enter_selected()
{
    PanelEntry *e = selected();
    if (e == 0)        return 0;
    if (!e->is_dir)    return 0;

    if (e->is_parent) {
        /* Going up: afterwards put the cursor on the directory we left. */
        char leaf[PANEL_NAME_MAX];
        path_leaf(cwd, leaf, sizeof(leaf));
        chdir("..");
        refresh();
        select_by_name(leaf);
    } else {
        chdir(e->name);
        refresh();
    }
    return 1;
}

/* Backspace: go to the parent directory (no effect at the root).
 * Afterwards the cursor sits on the directory just left. */
void LocalPanel::go_parent()
{
    char leaf[PANEL_NAME_MAX];
    path_leaf(cwd, leaf, sizeof(leaf));
    chdir("..");
    refresh();
    select_by_name(leaf);
}
