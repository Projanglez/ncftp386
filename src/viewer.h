/* =============================================================================
 * viewer.h - Simple full-screen text viewer (F3)
 * -----------------------------------------------------------------------------
 * Displays a file (up to ~60 KB) with scrolling. Controls: arrows, Page Up/
 * Down, Home/End, Left/Right (horizontal), Esc/Q quits. Larger files are
 * truncated (a "[truncated]" note is shown). Control characters are shown
 * as '.'.
 * ===========================================================================*/
#ifndef VIEWER_H
#define VIEWER_H

/* path  = full path of the (local) file.
 * title = display name in the header line (e.g. just the file name). */
void view_file(const char *path, const char *title);

#endif /* VIEWER_H */
