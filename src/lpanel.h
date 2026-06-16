/* =============================================================================
 * lpanel.h - Local filesystem panel (left side of the screen)
 * -----------------------------------------------------------------------------
 * Reads the current DOS directory via _dos_findfirst/_dos_findnext, sorts it
 * (".." first, then directories, then files - each alphabetically) and
 * renders it as a Norton Commander panel. Enter switches into directories.
 * ===========================================================================*/
#ifndef LPANEL_H
#define LPANEL_H

#include "panel.h"

class LocalPanel : public Panel {
public:
    LocalPanel();

    int  refresh();             /* re-read the current directory             */
    int  enter_selected();      /* override: 1 = changed directory, 0 = file */
    void go_parent();           /* override: switch to the parent directory  */

    const char *path() const { return cwd; }

private:
    char cwd[PANEL_HEADER_MAX]; /* current working directory (with drive)    */

    void read_cwd();            /* fetch cwd from DOS                         */
    static int compare(const void *a, const void *b); /* qsort comparator    */
};

#endif /* LPANEL_H */
