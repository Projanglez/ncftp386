/* =============================================================================
 * lpanel.cpp - Lokales Dateisystem-Panel
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <dos.h>      /* _dos_findfirst/_dos_findnext, _A_* , struct find_t  */
#include <direct.h>   /* getcwd, chdir                                       */
#include <string.h>   /* strncpy, strcpy, stricmp                            */
#include <stdlib.h>   /* qsort                                               */

#include "lpanel.h"

/* Letzten Pfadbestandteil ("Blattname") ermitteln. "C:\FOO\BAR" -> "BAR",
 * "C:\" -> "" (Wurzel hat kein Blatt). */
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

/* Aktuelles Arbeitsverzeichnis (inkl. Laufwerk) ermitteln. */
void LocalPanel::read_cwd()
{
    if (getcwd(cwd, PANEL_HEADER_MAX) == 0) {
        /* Fallback, falls getcwd scheitert. */
        strcpy(cwd, "C:\\");
    }
}

/* qsort-Vergleich: ".." zuerst, dann Verzeichnisse, dann Dateien,
 * jeweils alphabetisch (gross/klein egal). */
int LocalPanel::compare(const void *a, const void *b)
{
    const PanelEntry *ea = (const PanelEntry *)a;
    const PanelEntry *eb = (const PanelEntry *)b;

    if (ea->is_parent != eb->is_parent)
        return ea->is_parent ? -1 : 1;
    if (ea->is_dir != eb->is_dir)
        return ea->is_dir ? -1 : 1;
    return stricmp(ea->name, eb->name);
}

/* Verzeichnis neu einlesen. Rueckgabe: Anzahl Eintraege. */
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
        /* "." (aktuelles Verzeichnis) ueberspringen. */
        if (!(ff.name[0] == '.' && ff.name[1] == '\0')) {
            PanelEntry *e = &entries[count];
            strncpy(e->name, ff.name, PANEL_NAME_MAX - 1);
            e->name[PANEL_NAME_MAX - 1] = '\0';
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

    qsort(entries, count, sizeof(PanelEntry), compare);

    cursor = 0;
    topentry = 0;
    return count;
}

/* Enter auf dem markierten Eintrag.
 * Rueckgabe: 1 = Verzeichnis betreten (Panel neu eingelesen),
 *            0 = Datei (Aufrufer behandelt Anzeigen/Starten). */
int LocalPanel::enter_selected()
{
    PanelEntry *e = selected();
    if (e == 0)        return 0;
    if (!e->is_dir)    return 0;

    if (e->is_parent) {
        /* Hochwechseln: danach Cursor auf das verlassene Verzeichnis. */
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

/* Backspace: ins uebergeordnete Verzeichnis (am Wurzelverzeichnis wirkungslos).
 * Danach steht der Cursor auf dem soeben verlassenen Verzeichnis. */
void LocalPanel::go_parent()
{
    char leaf[PANEL_NAME_MAX];
    path_leaf(cwd, leaf, sizeof(leaf));
    chdir("..");
    refresh();
    select_by_name(leaf);
}
