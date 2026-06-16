/* =============================================================================
 * editor.h - Minimal full-screen text editor (F4), local files only
 * -----------------------------------------------------------------------------
 * Loads a text file (up to ~32 KB) into a line buffer on the far heap and
 * allows simple editing: inserting/deleting characters, splitting/joining
 * lines, scrolling. Save with F2, quit with Esc (confirms if there are
 * unsaved changes). Deliberately minimal: no undo, no search, no remote
 * editing.
 * ===========================================================================*/
#ifndef EDITOR_H
#define EDITOR_H

/* path  = full path of the (local) file.
 * title = display name in the header line (e.g. just the file name).
 * Returns: 1 = saved at least once, otherwise 0. */
int edit_file(const char *path, const char *title);

#endif /* EDITOR_H */
