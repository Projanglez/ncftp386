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
 * 'full'/'fullcap' (optional) receive the untruncated name; pass 0 to ignore.
 * Returns 1 = recognized (e filled in: name, size, date, is_dir, marked=0,
 *         fullname=0), 0 = line not recognizable as an entry. Also used by the
 * recursive directory download (dircopy.cpp). */
int ftp_parse_list_line(const char *line, int curYear, PanelEntry *e,
                        char *full = 0, int fullcap = 0);

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

    /* Pool of full (untruncated) names for the current listing. PanelEntry
     * records point into it via 'fullname'. Allocated lazily, reused (reset)
     * on every refresh; stable while the listing lives, so the pointers in
     * entries[] stay valid across sort_entries(). */
    char    *namePool;
    unsigned poolUsed;
    unsigned poolSize;
    /* Copy 's' into the pool; returns a pointer into it, or 0 if it doesn't
     * fit (caller then leaves fullname=0 and falls back to the short name). */
    char *pool_store(const char *s);

    /* LIST callback (ftpcli calls this for every raw line). */
    static void on_line(void *ctx, const char *line);
    void add_line(const char *line);
};

#endif /* RPANEL_H */
