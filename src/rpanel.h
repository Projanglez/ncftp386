/* =============================================================================
 * rpanel.h - FTP remote panel (right side of the screen)
 * -----------------------------------------------------------------------------
 * Fetches the directory listing via FtpClient::list() and converts the raw
 * LIST text lines into PanelEntry records. Supports the Unix "ls -l" format
 * and (as a bonus) the MS-DOS/IIS format. Enter changes directories via CWD,
 * Backspace goes up via CDUP.
 *
 * The panel only holds a pointer to the (externally managed) FtpClient.
 * Without a connection it shows an empty list with a note in the header.
 * ===========================================================================*/
#ifndef RPANEL_H
#define RPANEL_H

#include "panel.h"
#include "ftpcli.h"

/* Parse one raw LIST line into a PanelEntry (Unix or DOS/IIS format).
 * curYear supplies the year for lines that only carry a time (no year).
 * Returns 1 = recognized (e filled in: name, size, date, is_dir, marked=0),
 *         0 = line not recognizable as an entry. Also used by the recursive
 * directory download (dircopy.cpp). */
int ftp_parse_list_line(const char *line, int curYear, PanelEntry *e);

class RemotePanel : public Panel {
public:
    RemotePanel();

    /* Attach the (externally created) FTP client. */
    void attach(FtpClient *client) { ftp = client; }

    int  refresh();             /* re-list the current remote directory       */
    int  enter_selected();      /* override: 1 = changed directory, 0 = file  */
    void go_parent();           /* override: CDUP                             */

    const char *path() const { return cwd; }

    /* Did the last navigation/listing action fail? + error text. */
    int         nav_failed() const { return navFailed; }
    const char *last_error() const { return ftp ? ftp->last_error() : ""; }

private:
    /* FTP servers (Unix) are case-sensitive: show names verbatim instead of
     * forcing the Norton UPPERCASE-dir / lowercase-file convention. */
    int nc_case() const { return 0; }

    FtpClient *ftp;
    char cwd[PANEL_HEADER_MAX];  /* current remote path (via PWD)              */
    int  navFailed;             /* 1 = the last action reported an error       */
    int  curYear;               /* current year (for date lines with a time)  */

    /* LIST callback (ftpcli calls this for every raw line). */
    static void on_line(void *ctx, const char *line);
    void add_line(const char *line);

    static int compare(const void *a, const void *b);  /* qsort comparator */
};

#endif /* RPANEL_H */
