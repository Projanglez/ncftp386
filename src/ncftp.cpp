/* =============================================================================
 * ncftp.cpp - NCFTP386: Main, screen layout and event loop
 * -----------------------------------------------------------------------------
 * Norton-Commander-style dual-panel file manager: local DOS filesystem on the
 * left, FTP remote via mTCP on the right. The main loop works polymorphically
 * over Panel*.
 *
 * Screen (80x25) - no menu/command line, maximum panel content:
 *   Rows 0-22 : two panels (each 40 columns wide)
 *   Row  23   : status bar (file info + connection status)
 *   Row  24   : function key bar
 *
 * Language (German/English) is derived at startup from the DOS country
 * setting; all visible text goes through L("en","de").
 * Command line: NCFTP EN  (or /EN, -EN) -> forces English.
 * ===========================================================================*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <direct.h>
#include <dos.h>
#include <ctype.h>
#include <time.h>

#include "tui.h"
#include "panel.h"
#include "lpanel.h"
#include "rpanel.h"
#include "ftpcli.h"
#include "keymap.h"
#include "dialog.h"
#include "viewer.h"
#include "editor.h"
#include "dircopy.h"
#include "connsave.h"
#include "i18n.h"
#include "umlaut.h"   /* always include last */

#define APP_VERSION "0.9.2"

/* ---- Screen layout ---- */
#define PANEL_TOP     0
#define PANEL_ROWS    23           /* rows 0..22                       */
#define PANEL_COLS    40
#define ROW_STATUS    23
#define ROW_FKEYS     24

/* ---- Global panels ---- */
/* Large objects (~25 KB per panel): when compiling ncftp.cpp, set -zt256
 * so Open Watcom places them in FAR data segments (DGROUP < 64 KB).
 * Left is local, right is FTP remote. The main loop works polymorphically
 * over Panel*. */
static LocalPanel  g_left;
static RemotePanel g_right;
static Panel      *g_active = 0;

/* FTP client (controls the remote side). g_ftp_ready=1 once the mTCP
 * stack has been successfully initialized. */
static FtpClient g_ftp;
static int       g_ftp_ready = 0;

/* Data for the most recently used/remembered connection. Kept at file scope
 * so main() (loading from NCFTP.SAV, command line) and do_connect() (dialog,
 * saving) can both access it. */
static char g_host[FTP_HOST_MAX] = "";
static char g_portStr[8]         = "21";
static char g_user[40]           = "anonymous";
static char g_pass[40]           = "";
static int  g_savepw      = 1;   /* remember password (0 = host/user only)     */
static int  g_saveconn    = 1;   /* 0 = do not save this session in NCFTP.SAV  */
static int  g_autoconnect = 0;   /* auto-connect via command line (-h)         */
static int  g_nosplash    = 0;   /* /Q: skip splash screen                     */
static int  g_swapped     = 0;   /* Ctrl-U: local panel on the right (saved)   */
static int  g_video_pref  = -1;  /* -1 auto, 0 force color, 1 force mono        */

/* Critical-error handler (INT 24h): prevents the DOS "Abort, Retry, Fail?"
 * prompt - e.g. for an empty floppy drive. Instead of letting it wreck the
 * screen, we simply let the failed DOS operation return with an error
 * ("Fail"). Registered once in main(). */
static int ncftp_harderr(unsigned deverr, unsigned errcode, unsigned *devhdr)
{
    (void)deverr; (void)errcode; (void)devhdr;
    return _HARDERR_FAIL;           /* failed DOS operation: just fail */
}

/* -------------------------------------------------------------------------
 * Screen chrome
 * ---------------------------------------------------------------------- */

/* Function key labels per language. F6 = "Move", F7 = "MkDir". */
static const char *fkey_label(int i)
{
    static const char *de[10] = {
        "Hilfe", "Verb", "Anzeig", "Edit", "Kopier",
        "Versch", "MkDir", "L" oe "sch", "Laufw", "Ende"
    };
    static const char *en[10] = {
        "Help", "Conn", "View", "Edit", "Copy",
        "Move", "MkDir", "Del", "Drive", "Quit"
    };
    return g_english ? en[i] : de[i];
}

/* Alternate labels shown while ALT is held (Norton Commander style). Only the
 * keys that have an ALT action are labelled; "" leaves the cell blank. */
static const char *fkey_alt_label(int i)
{
    static const char *de[10] = {
        "Laufw", "", "", "", "",
        "Umben", "", "", "", ""
    };
    static const char *en[10] = {
        "Drive", "", "", "", "",
        "Rename", "", "", "", ""
    };
    return g_english ? en[i] : de[i];
}

/* Reflects whether the function-key bar currently shows the ALT labels, so
 * the idle loop only repaints row 24 when the ALT state actually changes. */
static int g_altbar = 0;

static void draw_fkeybar(int alt)
{
    int i;
    fill_rect(ROW_FKEYS, 0, 1, SCREEN_COLS, ' ', ATTR_FNKEY_LBL);
    for (i = 0; i < 10; i++) {
        int col = i * 8;            /* 10 cells of 8 columns = 80 */
        char num[4];
        int nlen;
        const char *lbl = alt ? fkey_alt_label(i) : fkey_label(i);
        sprintf(num, "%d", i + 1);  /* 1..10 */
        nlen = (int)strlen(num);
        draw_text(ROW_FKEYS, col,        num, ATTR_FNKEY_NUM, nlen);
        draw_text(ROW_FKEYS, col + nlen, lbl, ATTR_FNKEY_LBL, 8 - nlen);
    }
    g_altbar = alt;
}

/* "1234567" -> "1,234,567" (en) or "1.234.567" (de). out >= 20 characters. */
static void format_thousands(unsigned long v, char *out)
{
    char tmp[16];
    char sep = g_english ? ',' : '.';
    int  ndig = sprintf(tmp, "%lu", v);
    int  i, o = 0;
    for (i = 0; i < ndig; i++) {
        if (i > 0 && ((ndig - i) % 3) == 0) out[o++] = sep;
        out[o++] = tmp[i];
    }
    out[o] = '\0';
}

/* Bytes -> "(123 KB)" or "(1.2 MB)". MB once > 1000 KB. out >= 24 characters. */
static void format_human(unsigned long bytes, char *out)
{
    char dec = g_english ? '.' : ',';
    unsigned long kb = bytes / 1024UL;
    if (kb > 1000UL) {
        unsigned long MB    = 1048576UL;
        unsigned long whole = bytes / MB;
        unsigned long frac  = (bytes % MB) * 10UL / MB;
        sprintf(out, "(%lu%c%lu MB)", whole, dec, frac);
    } else {
        sprintf(out, "(%lu KB)", kb);
    }
}

static void draw_statusbar(void)
{
    char info[120];
    char conn[48];
    int  clen;
    PanelEntry *e = g_active ? g_active->selected() : 0;

    if (g_ftp.is_connected())
        sprintf(conn, "%s %.20s", L("Connected:", "Verbunden:"), g_ftp.host_name());
    else
        strcpy(conn, L("Not connected", "Nicht verbunden"));
    clen = (int)strlen(conn);

    fill_rect(ROW_STATUS, 0, 1, SCREEN_COLS, ' ', ATTR_STATUSBAR);

    /* Marked entries take precedence over the single-file info. */
    if (g_active && g_active->marked_count() > 0) {
        int nm = g_active->marked_count();
        int nd = g_active->marked_dir_count();
        unsigned long ms = g_active->marked_size();
        if (ms > 0) {
            char num[20], hum[24];
            format_thousands(ms, num);
            if (ms >= 1024UL) {
                format_human(ms, hum);
                if (nd > 0)
                    sprintf(info, L(" %d marked   %s bytes %s + %d Dir(s)",
                                    " %d markiert   %s Bytes %s + %d Dir(s)"),
                            nm, num, hum, nd);
                else
                    sprintf(info, L(" %d marked   %s bytes %s",
                                    " %d markiert   %s Bytes %s"),
                            nm, num, hum);
            } else {
                if (nd > 0)
                    sprintf(info, L(" %d marked   %s bytes + %d Dir(s)",
                                    " %d markiert   %s Bytes + %d Dir(s)"),
                            nm, num, nd);
                else
                    sprintf(info, L(" %d marked   %s bytes",
                                    " %d markiert   %s Bytes"),
                            nm, num);
            }
        } else {
            if (nd > 0)
                sprintf(info, L(" %d marked + %d Dir(s)", " %d markiert + %d Dir(s)"), nm, nd);
            else
                sprintf(info, L(" %d marked", " %d markiert"), nm);
        }
        draw_text(ROW_STATUS, 0, info, ATTR_STATUSBAR, SCREEN_COLS - 2 - clen);
        draw_text(ROW_STATUS, SCREEN_COLS - clen, conn, ATTR_STATUSBAR, clen);
        return;
    }

    if (e) {
        if (e->is_dir) {
            sprintf(info, " %-12s   <DIR>", e->name);
        } else if (e->size > 999UL) {
            char num[20];
            format_thousands(e->size, num);
            if (e->size >= 1024UL) {
                char hum[24];
                format_human(e->size, hum);
                sprintf(info, " %-12s   %s Bytes %s", e->name, num, hum);
            } else {
                sprintf(info, " %-12s   %s Bytes", e->name, num);
            }
        } else {
            sprintf(info, " %-12s   %lu Bytes", e->name, e->size);
        }
    } else {
        strcpy(info, L(" (empty)", " (leer)"));
    }
    draw_text(ROW_STATUS, 0, info, ATTR_STATUSBAR, SCREEN_COLS - 2 - clen);
    draw_text(ROW_STATUS, SCREEN_COLS - clen, conn, ATTR_STATUSBAR, clen);
}

/* Brief message in the status bar (until the next refresh). */
static void flash_status(const char *msg)
{
    fill_rect(ROW_STATUS, 0, 1, SCREEN_COLS, ' ', ATTR_STATUSBAR);
    draw_text(ROW_STATUS, 1, msg, ATTR_STATUSBAR, SCREEN_COLS - 2);
}

static void set_active(Panel *p)
{
    g_active = p;
    g_left.set_active(p == (Panel *)&g_left);
    g_right.set_active(p == (Panel *)&g_right);
}

/* Date + time in the top right (in the topmost border row). Ends at column
 * 77, so 2 border characters (columns 78/79) remain visible on the right.
 * Format depends on language (24-h): EN MM/DD/YYYY, DE DD.MM.YYYY. */
static void draw_clock(void)
{
    struct dosdate_t d;
    struct dostime_t t;
    char  buf[24];
    int   len, col;

    _dos_getdate(&d);
    _dos_gettime(&t);
    if (g_english)
        sprintf(buf, "%02d/%02d/%04d %02d:%02d:%02d",
                d.month, d.day, d.year, t.hour, t.minute, t.second);
    else
        sprintf(buf, "%02d.%02d.%04d %02d:%02d:%02d",
                d.day, d.month, d.year, t.hour, t.minute, t.second);

    len = (int)strlen(buf);
    col = 78 - len;                 /* last character at column 77 */
    if (col < 1) col = 1;
    draw_text(0, col, buf, ATTR_BORDER, len);
}

static void redraw_all(void)
{
    g_left.draw();
    g_right.draw();
    draw_statusbar();
    draw_fkeybar(0);
    draw_clock();
}

/* Position the two panels. Normally the local panel is on the left and the
 * remote panel on the right; when g_swapped is set they switch sides (the
 * local HDD ends up on the right). Only the screen columns change - the panel
 * objects (and thus copy direction / Tab handling) stay the same. */
static void apply_panel_regions(void)
{
    int lcol = g_swapped ? PANEL_COLS : 0;          /* local panel column  */
    int rcol = g_swapped ? 0          : PANEL_COLS; /* remote panel column */
    g_left.set_region(PANEL_TOP, lcol, PANEL_ROWS, PANEL_COLS);
    g_right.set_region(PANEL_TOP, rcol, PANEL_ROWS, PANEL_COLS);
}

/* Ctrl-U: swap the panels left<->right and remember the choice. */
static void do_swap_panels(void)
{
    g_swapped = !g_swapped;
    apply_panel_regions();
    redraw_all();
    if (g_saveconn)
        connsave_store(g_host, g_portStr, g_user, g_pass, g_savepw, g_swapped);
}

/* Called when the FTP connection was lost while idle. */
static void handle_disconnect(void)
{
    g_right.refresh();              /* shows "(not connected)" */
    redraw_all();                   /* fully update the UI before the popup */
    dlg_error(L("Connection lost", "Verbindung getrennt"),
              L("The server closed the connection.",
                "Der Server hat die Verbindung beendet."));
}

/* -------------------------------------------------------------------------
 * F2 - Establish / close the FTP connection
 * Connection data lives in g_host/g_portStr/g_user/g_pass (pre-filled from
 * NCFTP.SAV or the command line, editable in the dialog). On success it is
 * saved again - unless suppressed via -n (password only if g_savepw is
 * set). The actual connect call is blocking.
 * ---------------------------------------------------------------------- */

/* Connect using the current g_* data. On success: list the right panel,
 * move focus there, save the data. Returns FTP_OK or an error code.
 * Does NOT redraw itself (the caller decides on redraw/error dialog).
 *
 * Transient errors are retried automatically ONCE: on real hardware the
 * very first TCP SYN after a cold start is occasionally lost (switch/NIC
 * wake-up); mTCP's own SYN retry only kicks in after ~10s. A short settle
 * period + a fresh SYN reliably fixes this - applies to both F2 and
 * auto-connect. */
static int perform_connect(void)
{
    unsigned port = (unsigned)atoi(g_portStr);
    int rc = FTP_ERR_GENERAL;
    int attempt;

    if (port == 0) port = 21;

    for (attempt = 0; attempt < 2; attempt++) {
        flash_status(attempt == 0
            ? L(" Connecting to FTP server ...", " Verbinde mit FTP-Server ...")
            : L(" Retrying connection ...", " Erneuter Verbindungsversuch ..."));

        rc = g_ftp.connect(g_host, port, g_user, g_pass);
        if (rc == FTP_OK) break;

        /* Only retry transient network errors; do NOT retry real errors
         * (login rejected). DNS stays included: a lost first DNS packet is
         * the same cold-start case. */
        if (rc != FTP_ERR_TIMEOUT && rc != FTP_ERR_DNS &&
            rc != FTP_ERR_CONNECT && rc != FTP_ERR_DATACONN)
            break;
        if (attempt == 0)
            FtpClient::stack_poll(500);   /* brief settle, then fresh SYN */
    }
    if (rc != FTP_OK) return rc;

    g_right.refresh();
    set_active((Panel *)&g_right);
    if (g_saveconn)
        connsave_store(g_host, g_portStr, g_user, g_pass, g_savepw, g_swapped);
    return FTP_OK;
}

static void do_connect(void)
{
    if (!g_ftp_ready) {
        dlg_error(L("FTP unavailable", "FTP nicht verf" ue "gbar"),
                  L("TCP/IP could not be started.\n"
                    "Check MTCPCFG and restart the program.",
                    "TCP/IP konnte nicht gestartet werden.\n"
                    "MTCPCFG pruefen und Programm neu starten."));
        redraw_all();
        return;
    }

    /* Already connected -> offer to disconnect. */
    if (g_ftp.is_connected()) {
        if (dlg_confirm(L("Disconnect", "Trennen"),
                        L("Close the current FTP connection?",
                          "Bestehende FTP-Verbindung trennen?"))) {
            g_ftp.disconnect();
            g_right.refresh();
        }
        redraw_all();
        return;
    }

    if (!dlg_connect(L("Connect to FTP Server", "FTP-Verbindung herstellen"),
                     g_host,    FTP_HOST_MAX - 1,
                     g_portStr, (int)sizeof(g_portStr) - 1,
                     g_user,    (int)sizeof(g_user) - 1,
                     g_pass,    (int)sizeof(g_pass) - 1,
                     &g_saveconn, &g_savepw))
    { redraw_all(); return; }
    if (g_host[0] == '\0') { redraw_all(); return; }

    redraw_all();
    if (perform_connect() != FTP_OK) {
        dlg_error(L("Connection failed", "Verbindung fehlgeschlagen"), g_ftp.last_error());
        redraw_all();
        return;
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F5 - Copy between the local and remote panel
 * The direction follows the active panel:
 *   active = local  -> Upload   (STOR) local  -> remote
 *   active = remote -> Download (RETR) remote -> local
 *
 * All marked entries are copied (Insert key); without marks, the entry
 * under the cursor. Directories are copied recursively including all
 * subdirectories (dircopy.cpp). When copying a single file without marks,
 * the target name can be edited beforehand.
 * ---------------------------------------------------------------------- */

/* Progress callback for FtpClient::retr/stor (during the transfer). */
static void copy_progress(void *ctx, unsigned long sofar, unsigned long total)
{
    (void)ctx;
    dlg_progress_update(sofar, total);
}

/* Callback for dircopy: show the file/directory currently being processed. */
static void copy_item(void *ctx, const char *name, int is_dir)
{
    (void)ctx; (void)is_dir;
    dlg_progress_setfile(name);
}

/* State of a (recursive) copy operation - created fresh per do_copy() call,
 * i.e. "Overwrite all" only applies to the current operation. */
struct CopyCtx {
    int overwrite_all;
};

/* 4-option prompt on file conflict. Returns like dlg_choice (0..3 / -1). */
static int dlg_overwrite(const char *name)
{
    char        msg[120];
    const char *items[4];
    sprintf(msg, L("File already exists:\n%.40s",
                   "Datei existiert bereits:\n%.40s"), name);
    items[0] = L("Overwrite",      Ue "berschreiben");
    items[1] = L("Skip file", "Datei " ue "berspringen");
    items[2] = L("Overwrite all", "Alle " ue "berschreiben");
    items[3] = L("Cancel operation",   "Vorgang abbrechen");
    return dlg_choice(L("Overwrite?", Ue "berschreiben?"), msg, items, 4);
}

/* Conflict callback for dircopy + the single-file batch cases. */
static int copy_conflict(void *vctx, const char *name)
{
    CopyCtx *c = (CopyCtx *)vctx;
    int r;
    if (c && c->overwrite_all) return DC_OVERWRITE;

    r = dlg_overwrite(name);
    if (r == 0) return DC_OVERWRITE;                         /* Overwrite     */
    if (r == 1) return DC_SKIP;                              /* Skip          */
    if (r == 2) { if (c) c->overwrite_all = 1; return DC_OVERWRITE; }  /* All */
    return DC_ABORT;                                         /* Cancel/Esc    */
}

/* 1 if the local file exists (for the overwrite prompt). */
static int local_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Build local path "dir\name" (respecting root "C:\"), length-safe. */
static void join_local(char *out, int outsz, const char *dir, const char *name)
{
    int n;
    strncpy(out, dir, outsz - 1);
    out[outsz - 1] = '\0';
    n = (int)strlen(out);
    if (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/' && out[n - 1] != ':'
        && n < outsz - 1) {
        out[n++] = '\\';
        out[n] = '\0';
    }
    strncat(out, name, outsz - 1 - (int)strlen(out));
}

/* Copy a single file interactively (target name editable, overwrite prompt).
 * to_remote != 0 => upload (local -> remote), otherwise download. */
static void copy_single_file_interactive(int to_remote, PanelEntry *e)
{
    char target[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
    char prompt[64];
    int  rc;

    if (!to_remote) {
        /* --- Download: remote -> local --- */
        join_local(target, (int)sizeof(target), g_left.path(), e->name);
        sprintf(prompt, L("Download \"%.20s\" to:", "\"%.20s\" laden nach:"), e->name);
        if (!dlg_input(L("Download", "Download"), prompt, target, (int)sizeof(target) - 1, 0)) { redraw_all(); return; }
        if (target[0] == '\0') { redraw_all(); return; }

        if (local_exists(target)) {
            char q[120];
            sprintf(q, L("Local file already exists:\n%.40s\nOverwrite?",
                         "Lokale Datei existiert bereits:\n%.40s\n" Ue "berschreiben?"), target);
            if (!dlg_confirm(L("Download", "Download"), q)) { redraw_all(); return; }
        }

        redraw_all();
        dlg_progress_begin(L("Download", "Download"), e->name);
        rc = g_ftp.retr(e->name, target, copy_progress, 0);
        dlg_progress_end();

        if (rc != FTP_OK) dlg_error(L("Download failed", "Download fehlgeschlagen"), g_ftp.last_error());
        g_left.refresh();
    } else {
        /* --- Upload: local -> remote --- */
        char localpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(localpath, (int)sizeof(localpath), g_left.path(), e->name);
        strncpy(target, e->name, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
        sprintf(prompt, L("Upload \"%.20s\" as:", "\"%.20s\" senden als:"), e->name);
        if (!dlg_input(L("Upload", "Upload"), prompt, target, (int)sizeof(target) - 1, 0)) { redraw_all(); return; }
        if (target[0] == '\0') { redraw_all(); return; }

        if (g_right.has_entry(target)) {
            char q[120];
            sprintf(q, L("Remote file already exists:\n%.40s\nOverwrite?",
                         "Remote-Datei existiert bereits:\n%.40s\n" Ue "berschreiben?"), target);
            if (!dlg_confirm(L("Upload", "Upload"), q)) { redraw_all(); return; }
        }

        redraw_all();
        dlg_progress_begin(L("Upload", "Upload"), e->name);
        rc = g_ftp.stor(localpath, target, copy_progress, 0);
        dlg_progress_end();

        if (rc != FTP_OK) dlg_error(L("Upload failed", "Upload fehlgeschlagen"), g_ftp.last_error());
        g_right.refresh();
    }
    redraw_all();
}

/* Copy one entry (file or directory) in batch mode. Target name = source
 * name in the other panel's directory. Directories are recursive.
 * If the target file exists, copy_conflict is asked; returns FTP_OK,
 * FTP_ERR_ABORT (user cancel) or another FTP_ERR_* code. */
static int copy_one_entry(int to_remote, PanelEntry *e, CopyCtx *cc)
{
    char localpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];

    join_local(localpath, (int)sizeof(localpath), g_left.path(), e->name);

    if (to_remote) {
        if (e->is_dir)
            return dircopy_upload(&g_ftp, localpath, e->name,
                                  copy_item, copy_progress, copy_conflict, cc);
        /* Single file: does it already exist remotely? */
        if (g_ftp.remote_file_exists(e->name)) {
            int d = copy_conflict(cc, e->name);
            if (d == DC_ABORT) return FTP_ERR_ABORT;
            if (d == DC_SKIP)  return FTP_OK;
        }
        copy_item(0, e->name, 0);
        return g_ftp.stor(localpath, e->name, copy_progress, cc);
    } else {
        if (e->is_dir)
            return dircopy_download(&g_ftp, e->name, localpath,
                                    copy_item, copy_progress, copy_conflict, cc);
        /* Single file: does it already exist locally? */
        if (local_exists(localpath)) {
            int d = copy_conflict(cc, e->name);
            if (d == DC_ABORT) return FTP_ERR_ABORT;
            if (d == DC_SKIP)  return FTP_OK;
        }
        copy_item(0, e->name, 0);
        return g_ftp.retr(e->name, localpath, copy_progress, cc);
    }
}

static void do_copy(void)
{
    PanelEntry *cur;
    CopyCtx     cc;
    int to_remote, nmarked, total, i, rc;
    const char *destdir;
    char q[140];

    if (g_active == 0) return;

    if (!g_ftp.is_connected()) {
        dlg_error(L("Copy", "Kopieren"),
                  L("No FTP connection.\nConnect with F2 first.",
                    "Keine FTP-Verbindung.\nMit F2 zuerst verbinden."));
        redraw_all();
        return;
    }

    to_remote = (g_active == (Panel *)&g_left);   /* local active -> upload */
    nmarked   = g_active->marked_count();
    cur       = g_active->selected();

    /* --- Single file without marks: interactive convenience path --- */
    if (nmarked == 0) {
        if (cur == 0 || cur->is_parent) { redraw_all(); return; }
        if (!cur->is_dir) { copy_single_file_interactive(to_remote, cur); return; }
    }

    /* --- Batch/directory copy --- */
    total   = (nmarked > 0) ? nmarked : 1;
    destdir = to_remote ? g_right.path() : g_left.path();

    sprintf(q, L("Copy %d item(s) to:\n%.40s",
                 "%d Eintrag/Eintr" ae "ge kopieren nach:\n%.40s"), total, destdir);
    if (!dlg_confirm(L("Copy", "Kopieren"), q)) { redraw_all(); return; }

    cc.overwrite_all = 0;              /* fresh per operation (no "all" carries over) */

    redraw_all();
    dlg_progress_begin(L("Copy", "Kopieren"), "");

    rc = FTP_OK;
    if (nmarked > 0) {
        for (i = 0; i < g_active->entry_count(); i++) {
            PanelEntry *e = g_active->entry_at(i);
            if (!e || !e->marked || e->is_parent) continue;
            rc = copy_one_entry(to_remote, e, &cc);
            if (rc != FTP_OK) break;
        }
    } else {
        rc = copy_one_entry(to_remote, cur, &cc);   /* single directory */
    }

    dlg_progress_end();

    if (rc != FTP_OK && rc != FTP_ERR_ABORT)
        dlg_error(L("Copy failed", "Kopieren fehlgeschlagen"), g_ftp.last_error());

    /* Clear marks and re-read both sides. Keep the active (source) panel's
     * cursor on the copied item. */
    {
        char keepname[PANEL_NAME_MAX];
        PanelEntry *sel = g_active->selected();
        keepname[0] = '\0';
        if (sel) { strncpy(keepname, sel->name, sizeof(keepname) - 1); keepname[sizeof(keepname) - 1] = '\0'; }
        g_active->clear_marks();
        g_left.refresh();
        g_right.refresh();
        g_active->select_by_name(keepname);
    }
    redraw_all();
    if (rc == FTP_ERR_ABORT)
        flash_status(L(" Copy aborted.", " Kopieren abgebrochen."));
}

/* -------------------------------------------------------------------------
 * F3 - View file (local directly, remote via temporary download)
 * ---------------------------------------------------------------------- */
static void do_view(void)
{
    char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
    PanelEntry *e;

    if (g_active == 0) return;
    e = g_active->selected();
    if (e == 0 || e->is_parent || e->is_dir) { redraw_all(); return; }

    if (g_active == (Panel *)&g_left) {
        /* View local file directly. */
        join_local(path, (int)sizeof(path), g_left.path(), e->name);
        view_file(path, e->name);
    } else {
        /* Remote: download to a temporary local file, view it, then delete it. */
        int rc;
        if (!g_ftp.is_connected()) {
            dlg_error(L("View", "Anzeigen"),
                      L("No FTP connection.\nConnect with F2 first.",
                        "Keine FTP-Verbindung.\nMit F2 zuerst verbinden."));
            redraw_all();
            return;
        }
        join_local(path, (int)sizeof(path), g_left.path(), "$NCVIEW$.TMP");
        redraw_all();
        dlg_progress_begin(L("View", "Anzeigen"), e->name);
        rc = g_ftp.retr(e->name, path, copy_progress, 0);
        dlg_progress_end();
        if (rc != FTP_OK) {
            remove(path);   /* clean up any partially started temp file */
            dlg_error(L("View failed", "Anzeigen fehlgeschlagen"), g_ftp.last_error());
            redraw_all();
            return;
        }
        view_file(path, e->name);
        remove(path);
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F4 - Edit file (local only)
 * ---------------------------------------------------------------------- */
static void do_edit(void)
{
    char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
    char keep[PANEL_NAME_MAX];
    PanelEntry *e;

    if (g_active == 0) return;
    e = g_active->selected();
    if (e == 0 || e->is_parent || e->is_dir) { redraw_all(); return; }

    if (g_active != (Panel *)&g_left) {
        dlg_error(L("Edit", "Bearbeiten"),
                  L("Only local files can be edited.",
                    "Nur lokale Dateien k" oe "nnen bearbeitet werden."));
        redraw_all();
        return;
    }

    join_local(path, (int)sizeof(path), g_left.path(), e->name);
    strcpy(keep, e->name);
    edit_file(path, e->name);

    /* Size/date may have changed -> re-read, keep the cursor. */
    g_left.refresh();
    g_left.select_by_name(keep);
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F7 - Make directory (local or remote)
 * ---------------------------------------------------------------------- */
static void do_mkdir(void)
{
    char name[PANEL_NAME_MAX];
    int rc;

    if (g_active == 0) return;
    name[0] = '\0';
    if (!dlg_input(L("Make Directory", "Verzeichnis erstellen"),
                   L("Name:", "Name:"), name, PANEL_NAME_MAX - 1, 0)) {
        redraw_all(); return;
    }
    if (name[0] == '\0') { redraw_all(); return; }

    if (g_active == (Panel *)&g_right) {
        if (!g_ftp.is_connected()) {
            dlg_error(L("Make Directory", "Verzeichnis erstellen"),
                      L("No FTP connection.\nConnect with F2 first.",
                        "Keine FTP-Verbindung.\nMit F2 zuerst verbinden."));
            redraw_all(); return;
        }
        rc = g_ftp.make_dir(name);
        if (rc != FTP_OK)
            dlg_error(L("Make Directory", "Verzeichnis erstellen"), g_ftp.last_error());
        else
            { g_right.refresh(); g_right.select_by_name(name); }
    } else {
        char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(path, (int)sizeof(path), g_left.path(), name);
        if (_mkdir(path) != 0)
            dlg_error(L("Make Directory", "Verzeichnis erstellen"),
                      L("Could not create directory.", "Konnte Verzeichnis nicht anlegen."));
        else
            { g_left.refresh(); g_left.select_by_name(name); }
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F8 - Delete with confirmation (file or empty directory)
 * ---------------------------------------------------------------------- */
/* Delete a single entry (file or EMPTY directory).
 * Returns 0 = success, otherwise error (the error text is then in g_ftp,
 * or the caller reports a generic message). on_remote != 0 => remote side. */
static int delete_one_entry(int on_remote, PanelEntry *e)
{
    if (on_remote) {
        return e->is_dir ? g_ftp.remove_dir(e->name) : g_ftp.remove_file(e->name);
    } else {
        char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(path, (int)sizeof(path), g_left.path(), e->name);
        if (e->is_dir) return (_rmdir(path) == 0) ? 0 : -1;
        return (remove(path) == 0) ? 0 : -1;
    }
}

/* Delete an entry recursively (file or whole directory tree).
 * Returns 0 = success, otherwise error. */
static int delete_one_recursive(int on_remote, PanelEntry *e)
{
    if (!e->is_dir) {
        copy_item(0, e->name, 0);
        return delete_one_entry(on_remote, e);
    }
    if (on_remote)
        return dircopy_delete_remote(&g_ftp, e->name, copy_item, 0);
    {
        char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(path, (int)sizeof(path), g_left.path(), e->name);
        return dircopy_delete_local(path, copy_item, 0);
    }
}

/* Adds the tree of an entry to *nf/*nd (a file counts as 1 file). */
static void count_one(int on_remote, PanelEntry *e, unsigned *nf, unsigned *nd)
{
    if (!e->is_dir) { (*nf)++; return; }
    if (on_remote) {
        dircopy_count_remote(&g_ftp, e->name, nf, nd);
    } else {
        char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(path, (int)sizeof(path), g_left.path(), e->name);
        dircopy_count_local(path, nf, nd);
    }
}

static void do_delete(void)
{
    char        prompt[140];
    PanelEntry *cur;
    int         on_remote, nmarked, i, errors, keepidx;
    unsigned    nfiles = 0, ndirs = 0;

    if (g_active == 0) return;
    on_remote = (g_active == (Panel *)&g_right);

    if (on_remote && !g_ftp.is_connected()) {
        dlg_error(L("Delete", "L" oe "schen"),
                  L("No FTP connection.\nConnect with F2 first.",
                    "Keine FTP-Verbindung.\nMit F2 zuerst verbinden."));
        redraw_all(); return;
    }

    nmarked = g_active->marked_count();
    cur     = g_active->selected();
    keepidx = g_active->selected_index();   /* stay here after deleting */

    /* --- Convenience path: single file (no tree) --- */
    if (nmarked == 0) {
        if (cur == 0 || cur->is_parent) { redraw_all(); return; }
        if (!cur->is_dir) {
            sprintf(prompt, L("Delete file\n\"%.32s\"?",
                              "Datei \"%.32s\"\nl" oe "schen?"), cur->name);
            if (!dlg_confirm(L("Delete", "L" oe "schen"), prompt)) { redraw_all(); return; }
            if (delete_one_entry(on_remote, cur) != 0) {
                if (on_remote)
                    dlg_error(L("Delete failed", "L" oe "schen fehlgeschlagen"), g_ftp.last_error());
                else
                    dlg_error(L("Delete failed", "L" oe "schen fehlgeschlagen"),
                              L("Could not delete\nthe file.",
                                "Datei konnte nicht\ngel" oe "scht werden."));
            } else {
                if (on_remote) g_right.refresh(); else g_left.refresh();
                g_active->set_cursor_index(keepidx);
            }
            redraw_all();
            return;
        }
    }

    /* --- Tree/batch deletion: count first, then warn --- */
    flash_status(L(" Counting ...", " Ermittle Anzahl ..."));
    if (nmarked > 0) {
        for (i = 0; i < g_active->entry_count(); i++) {
            PanelEntry *e = g_active->entry_at(i);
            if (!e || !e->marked || e->is_parent) continue;
            count_one(on_remote, e, &nfiles, &ndirs);
        }
    } else {
        count_one(on_remote, cur, &nfiles, &ndirs);   /* single directory */
    }

    sprintf(prompt, L("Permanently delete\n%u file(s) and %u director(y/ies)?",
                      "%u Datei(en) und %u Verzeichnis(se)\nunwiderruflich l" oe "schen?"),
            nfiles, ndirs);
    if (!dlg_confirm(L("Delete", "L" oe "schen"), prompt)) { redraw_all(); return; }

    redraw_all();
    dlg_progress_begin(L("Delete", "L" oe "schen"), "");

    errors = 0;
    if (nmarked > 0) {
        for (i = 0; i < g_active->entry_count(); i++) {
            PanelEntry *e = g_active->entry_at(i);
            if (!e || !e->marked || e->is_parent) continue;
            if (delete_one_recursive(on_remote, e) != 0) errors++;
        }
    } else {
        if (delete_one_recursive(on_remote, cur) != 0) errors++;
    }

    dlg_progress_end();

    g_active->clear_marks();
    if (on_remote) g_right.refresh(); else g_left.refresh();
    g_active->set_cursor_index(keepidx);
    if (errors)
        dlg_error(L("Delete", "L" oe "schen"),
                  L("Some items could not be\nfully deleted.",
                    "Einige Eintr" ae "ge konnten nicht\nvollst" ae "ndig gel" oe "scht werden."));
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F6 - Move (copy to the other panel, then delete the source)
 * Works on a single entry or a multi-selection, files and recursive
 * directories, in both directions. Reuses the copy and delete machinery.
 * A source is deleted only after its copy reported success, so a failed
 * transfer never loses data. (Note: choosing "Skip" on an existing target
 * still counts as success and removes that source - the same trade-off the
 * underlying copy makes.)
 * ---------------------------------------------------------------------- */
static void do_move(void)
{
    PanelEntry *cur;
    CopyCtx     cc;
    int to_remote, src_remote, nmarked, total, i, rc, keepidx;
    const char *destdir;
    char q[140];

    if (g_active == 0) return;

    if (!g_ftp.is_connected()) {
        dlg_error(L("Move", "Verschieben"),
                  L("No FTP connection.\nConnect with F2 first.",
                    "Keine FTP-Verbindung.\nMit F2 zuerst verbinden."));
        redraw_all();
        return;
    }

    to_remote  = (g_active == (Panel *)&g_left);   /* local active -> upload     */
    src_remote = !to_remote;                       /* delete sources on this side */
    nmarked    = g_active->marked_count();
    cur        = g_active->selected();
    keepidx    = g_active->selected_index();

    if (nmarked == 0) {
        if (cur == 0 || cur->is_parent) { redraw_all(); return; }
    }

    total   = (nmarked > 0) ? nmarked : 1;
    destdir = to_remote ? g_right.path() : g_left.path();

    sprintf(q, L("Move %d item(s) to:\n%.40s",
                 "%d Eintrag/Eintr" ae "ge verschieben nach:\n%.40s"), total, destdir);
    if (!dlg_confirm(L("Move", "Verschieben"), q)) { redraw_all(); return; }

    cc.overwrite_all = 0;              /* fresh per operation */

    redraw_all();
    dlg_progress_begin(L("Move", "Verschieben"), "");

    rc = FTP_OK;
    if (nmarked > 0) {
        for (i = 0; i < g_active->entry_count(); i++) {
            PanelEntry *e = g_active->entry_at(i);
            if (!e || !e->marked || e->is_parent) continue;
            rc = copy_one_entry(to_remote, e, &cc);
            if (rc != FTP_OK) break;              /* keep source on failure/abort */
            delete_one_recursive(src_remote, e);  /* copy ok -> remove source      */
        }
    } else {
        rc = copy_one_entry(to_remote, cur, &cc);
        if (rc == FTP_OK) delete_one_recursive(src_remote, cur);
    }

    dlg_progress_end();

    if (rc != FTP_OK && rc != FTP_ERR_ABORT)
        dlg_error(L("Move failed", "Verschieben fehlgeschlagen"), g_ftp.last_error());

    g_active->clear_marks();
    g_left.refresh();
    g_right.refresh();
    g_active->set_cursor_index(keepidx);
    redraw_all();
    if (rc == FTP_ERR_ABORT)
        flash_status(L(" Move aborted.", " Verschieben abgebrochen."));
}

/* -------------------------------------------------------------------------
 * Alt+F6 - Rename (file or directory, local or remote)
 * Plain rename within the same directory (no moving between panels).
 * ---------------------------------------------------------------------- */
static void do_rename(void)
{
    char newname[PANEL_NAME_MAX];
    char prompt[64];
    PanelEntry *e;
    int rc;

    if (g_active == 0) return;
    e = g_active->selected();
    if (e == 0 || e->is_parent) { redraw_all(); return; }

    strncpy(newname, e->name, sizeof(newname) - 1);
    newname[sizeof(newname) - 1] = '\0';
    sprintf(prompt, L("Rename \"%.20s\" to:", "\"%.20s\" umbenennen in:"), e->name);
    if (!dlg_input(L("Rename", "Umbenennen"), prompt, newname, PANEL_NAME_MAX - 1, 0)) { redraw_all(); return; }
    if (newname[0] == '\0')            { redraw_all(); return; }
    if (strcmp(newname, e->name) == 0) { redraw_all(); return; }   /* unchanged */

    if (g_active == (Panel *)&g_right) {
        if (!g_ftp.is_connected()) {
            dlg_error(L("Rename", "Umbenennen"),
                      L("No FTP connection.\nConnect with F2 first.",
                        "Keine FTP-Verbindung.\nMit F2 zuerst verbinden."));
            redraw_all(); return;
        }
        rc = g_ftp.rename(e->name, newname);
        if (rc != FTP_OK)
            dlg_error(L("Rename failed", "Umbenennen fehlgeschlagen"), g_ftp.last_error());
        else
            { g_right.refresh(); g_right.select_by_name(newname); }
    } else {
        char oldpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        char newpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(oldpath, (int)sizeof(oldpath), g_left.path(), e->name);
        join_local(newpath, (int)sizeof(newpath), g_left.path(), newname);
        if (rename(oldpath, newpath) != 0)
            dlg_error(L("Rename failed", "Umbenennen fehlgeschlagen"),
                      L("Could not rename.\nName invalid or already exists.",
                        "Konnte nicht umbenennen.\nName ung" ue "ltig oder existiert bereits."));
        else
            { g_left.refresh(); g_left.select_by_name(newname); }
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F9 - Change local drive
 * Only meaningful for the local panel (the FTP side has no drives). Only
 * drives that actually exist are offered.
 * ---------------------------------------------------------------------- */

/* 1 if drive 'd' (1=A, 2=B, ...) exists. INT 21h/AX=4409h
 * ("is block device remote?") returns carry for an invalid drive. */
static int drive_present(int d)
{
    union REGS r;
    r.x.ax = 0x4409;
    r.h.bl = (unsigned char)d;
    int86(0x21, &r, &r);
    return r.x.cflag ? 0 : 1;
}

static void do_drives(void)
{
    char        labels[26][4];
    const char *items[26];
    int         letters[26];
    int         n = 0, d, cur, initial, sel, newd;

    for (d = 1; d <= 26; d++) {
        if (drive_present(d)) {
            sprintf(labels[n], "%c:", 'A' + d - 1);
            items[n]   = labels[n];
            letters[n] = d;
            n++;
        }
    }
    if (n == 0) {
        dlg_error(L("Drive", "Laufwerk"),
                  L("No drives found.", "Keine Laufwerke gefunden."));
        redraw_all(); return;
    }

    cur = _getdrive();                 /* current drive (1=A) */
    initial = 0;
    { int i; for (i = 0; i < n; i++) if (letters[i] == cur) initial = i; }

    sel = dlg_menu(L("Select Drive", "Laufwerk w" ae "hlen"), items, n, initial);
    if (sel < 0) { redraw_all(); return; }

    newd = letters[sel];
    if (newd != cur) {
        char test[PANEL_HEADER_MAX];
        _chdrive(newd);
        /* getcwd fails if the drive isn't ready (empty floppy). The harderr
         * handler prevents the DOS prompt. */
        if (getcwd(test, sizeof(test)) == 0) {
            _chdrive(cur);             /* revert to the old drive */
            dlg_error(L("Drive", "Laufwerk"),
                      L("Drive not ready.", "Laufwerk nicht bereit."));
            redraw_all(); return;
        }
        g_left.refresh();
    }
    set_active((Panel *)&g_left);      /* drive change affects the local panel */
    redraw_all();
}

/* Brief help (/?) on stdout - runs before tui_init, so plain output. */
static void print_usage(void)
{
    printf("FTP4DOS v" APP_VERSION " - Dual-Panel FTP Client for DOS\n");
    printf("(c) 2026 Projanglez -- https://github.com/Projanglez/ftp4dos\n\n");
    printf("Usage: FTP4DOS [/L:EN|DE] [/H:HOST] [/P:PORT] [/U:USER] [/W:PASS] [/S:ALL|NOPASS|OFF] [/Q] [/MONO|/COLOR]\n");
    printf("       ('-' may be used instead of '/'; flags are case-insensitive)\n\n");
    printf("  /L:EN|DE        force English or German user interface\n");
    printf("  /H:HOST         connect to HOST automatically on startup\n");
    printf("  /P:PORT         port (default 21)\n");
    printf("  /U:USER         user name (default anonymous)\n");
    printf("  /W:PASS         password  (WARNING: stored in cleartext in the batch file)\n");
    printf("  /S:ALL          save connection incl. password to FTP4DOS.SAV (default)\n");
    printf("  /S:NOPASS       save connection but not the password\n");
    printf("  /S:OFF          do not save this connection\n");
    printf("  /Q              skip splash screen\n");
    printf("  /MONO           force monochrome display (MDA/Hercules)\n");
    printf("  /COLOR          force color display (default: auto-detect)\n");
    printf("  /?              this help\n");
}

/* -------------------------------------------------------------------------
 * Main / event loop
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int running = 1;
    int i;

    /* Determine language from the DOS country setting (before any output). */
    i18n_init();

    /* Load the remembered connection (NCFTP.SAV next to the EXE). Fills
     * g_host etc.; if the file is missing, the defaults remain. */
    connsave_init(argv[0]);
    connsave_load(g_host, (int)sizeof(g_host), g_portStr, (int)sizeof(g_portStr),
                  g_user, (int)sizeof(g_user), g_pass, (int)sizeof(g_pass),
                  &g_savepw, &g_swapped);

    /* Parse the command line (overrides the loaded values). Uniform syntax
     * /X or /X:value; '-' is also allowed instead of '/'. The flag letter
     * is case-insensitive, the VALUE is taken verbatim (so password and
     * user stay exactly as cased).
     *   /L:DE|EN  language     /H:Host (+auto-connect)  /P:Port
     *   /U:User   /W:Password  /S:ALL|NOPASS|OFF        /? Help */
    {
        int want_help = 0;
        for (i = 1; i < argc; i++) {
            const char *o = argv[i];
            const char *val;
            char f;

            if (*o == '/' || *o == '-') o++;
            f = (char)tolower((unsigned char)o[0]);
            /* Value = remainder after ':' (if present), otherwise empty. */
            val = (o[1] == ':') ? (o + 2) : "";

            switch (f) {
            case 'l':   /* language: value is case-insensitive */
                if (val[0] == 'e' || val[0] == 'E') g_english = 1;
                else if (val[0] == 'd' || val[0] == 'D') g_english = 0;
                break;
            case 'h':
                strncpy(g_host, val, sizeof(g_host) - 1); g_host[sizeof(g_host) - 1] = 0;
                if (g_host[0]) g_autoconnect = 1;
                break;
            case 'p':
                strncpy(g_portStr, val, sizeof(g_portStr) - 1); g_portStr[sizeof(g_portStr) - 1] = 0;
                break;
            case 'u':
                strncpy(g_user, val, sizeof(g_user) - 1); g_user[sizeof(g_user) - 1] = 0;
                break;
            case 'w':
                strncpy(g_pass, val, sizeof(g_pass) - 1); g_pass[sizeof(g_pass) - 1] = 0;
                break;
            case 's':
                if      (stricmp(val, "OFF")    == 0) g_saveconn = 0;
                else if (stricmp(val, "NOPASS") == 0) { g_saveconn = 1; g_savepw = 0; }
                else if (stricmp(val, "ALL")    == 0) { g_saveconn = 1; g_savepw = 1; }
                break;
            case 'q':
                g_nosplash = 1;
                break;
            case 'm':       /* /MONO  : force monochrome (MDA/Hercules) */
                g_video_pref = 1;
                break;
            case 'c':       /* /COLOR : force color adapter */
                g_video_pref = 0;
                break;
            case '?':
                want_help = 1;
                break;
            default:
                break;
            }
        }
        if (want_help) { print_usage(); return 0; }
    }

    /* Make critical DOS errors (empty drive etc.) fail automatically
     * instead of letting "Abort, Retry, Fail?" wreck the TUI. */
    _harderr(ncftp_harderr);

    /* Start the mTCP stack BEFORE tui_init: parseEnv/initStack may write to
     * stderr on error - tui_init then clears the screen. If it fails (e.g.
     * MTCPCFG missing / no packet driver), the program keeps running as a
     * plain local file manager; F2 then reports the error. */
    g_ftp_ready = (FtpClient::init_stack() == FTP_OK) ? 1 : 0;
    g_right.attach(&g_ftp);

    tui_init(g_video_pref);

    apply_panel_regions();          /* honors g_swapped loaded from FTP4DOS.SAV */
    g_left.refresh();
    g_right.refresh();
    set_active((Panel *)&g_left);

    redraw_all();

    if (!g_nosplash) dlg_splash(APP_VERSION);

    /* Auto-connect if a host was given on the command line (-h).
     * Right after startup the packet driver has only just been hooked in
     * and the link is still cold - so let the stack warm up briefly before
     * the first connection attempt. perform_connect() itself handles the
     * transient retry (so this also applies to F2). */
    if (g_autoconnect) {
        if (g_ftp_ready) {
            FtpClient::stack_poll(750);            /* let the driver/link settle */
            if (perform_connect() != FTP_OK)
                dlg_error(L("Connection failed", "Verbindung fehlgeschlagen"), g_ftp.last_error());
            redraw_all();
        } else {
            flash_status(L(" FTP unavailable (MTCPCFG?).",
                           " FTP nicht verf" ue "gbar (MTCPCFG?)."));
        }
    }

    {
    time_t last_noop  = time(0);
    time_t last_clock = 0;

    while (running) {
        int key;

        /* Idle: wait for a key while keeping the connection alive.
         *  - update the clock every second,
         *  - while connected, drive the stack + detect disconnects,
         *  - send a NOOP every 60 s (keepalive against server idle timeout). */
        while (running && !key_pending()) {
            time_t now = time(0);

            /* Norton-style ALT bar: while ALT is held, row 24 shows the
             * alternate labels (Alt+F1 Drive, Alt+F6 Rename). BIOS keyboard
             * flag at 0040:0017, bit 3 = ALT down. Only repaint on change. */
            {
                unsigned char far *kbflag = (unsigned char far *)MK_FP(0x0040, 0x0017);
                int alt = (*kbflag & 0x08) ? 1 : 0;
                if (alt != g_altbar) draw_fkeybar(alt);
            }

            if (now != last_clock) { draw_clock(); last_clock = now; }
            if (g_ftp.is_connected()) {
                if (!g_ftp.idle_drive()) {
                    handle_disconnect();
                } else if (now - last_noop >= 60) {
                    last_noop = now;
                    if (g_ftp.noop() != FTP_OK) handle_disconnect();
                }
            }
        }

        key = readkey();
        last_noop = time(0);    /* key activity -> restart the keepalive timer */

        switch (key) {
        case KEY_TAB:
            set_active(g_active == (Panel *)&g_left
                       ? (Panel *)&g_right : (Panel *)&g_left);
            g_left.draw();
            g_right.draw();
            draw_statusbar();
            break;

        case KEY_CTRL_U:        /* swap panels left<->right (remembered) */
            do_swap_panels();
            break;

        case KEY_UP:   g_active->move_step(-1); draw_statusbar(); break;
        case KEY_DOWN: g_active->move_step(+1); draw_statusbar(); break;
        case KEY_PGUP: g_active->page_up();   g_active->draw(); draw_statusbar(); break;
        case KEY_PGDN: g_active->page_down(); g_active->draw(); draw_statusbar(); break;
        case KEY_HOME: g_active->move_home(); g_active->draw(); draw_statusbar(); break;
        case KEY_END:  g_active->move_end();  g_active->draw(); draw_statusbar(); break;

        case KEY_INS:  /* toggle mark + move cursor down (Norton style) */
            g_active->toggle_mark(); draw_statusbar(); break;
        case KEY_STAR: /* numpad *: invert all marks (NC style) */
            g_active->invert_marks(); draw_statusbar(); break;
        case KEY_PLUS: { /* numpad +: mark missing/different files */
            const Panel *other = (g_active == (Panel *)&g_left)
                                 ? (const Panel *)&g_right
                                 : (const Panel *)&g_left;
            g_active->compare_mark(other);
            draw_statusbar();
            break;
        }

        case KEY_ENTER:
            if (g_active->enter_selected()) {
                g_active->draw();
                draw_statusbar();
                if (g_active == (Panel *)&g_right && g_right.nav_failed())
                    flash_status(g_right.last_error());
            }
            /* otherwise: file -> view (like F3). */
            else {
                do_view();
            }
            break;

        case KEY_BACKSP:
            g_active->go_parent();
            g_active->draw();
            draw_statusbar();
            if (g_active == (Panel *)&g_right && g_right.nav_failed())
                flash_status(g_right.last_error());
            break;

        /* Function keys. */
        case KEY_F1:
            dlg_message(L("Help", "Hilfe"),
                L("Tab        Switch panel    Ctrl+U  Swap panels\n"
                  "Insert     Mark item (copy/move/delete several)\n"
                  "*          Invert selection\n"
                  "+          Mark files missing or different in other panel\n"
                  "Enter      Enter directory / view file\n"
                  "Backspace  Parent directory\n"
                  "F2 Connect  F3 View  F4 Edit  F5 Copy (recursive)\n"
                  "F6 Move (recursive)  Alt+F6 Rename\n"
                  "F7 MkDir  F8 Delete  F9/Alt+F1 Drive  F10 Quit",
                  "Tab        Panel wechseln   Strg+U  Panels tauschen\n"
                  "Einfg      Eintrag markieren (mehrere kop./versch./l" oe "schen)\n"
                  "*          Markierung invertieren\n"
                  "+          Fehlende/abweichende Dateien gg. anderem Panel markieren\n"
                  "Enter      Verzeichnis betreten / Datei anzeigen\n"
                  "Backspace  " Ue "bergeordnetes Verzeichnis\n"
                  "F2 Verbinden  F3 Anzeigen  F4 Bearbeiten  F5 Kopieren\n"
                  "F6 Verschieben (rekursiv)  Alt+F6 Umbenennen\n"
                  "F7 MkDir  F8 L" oe "schen  F9/Alt+F1 Laufwerk  F10 Ende"), 0);
            break;
        case KEY_F2:  do_connect(); break;
        case KEY_F3:  do_view(); break;
        case KEY_F4:  do_edit(); break;
        case KEY_F5:  do_copy(); break;
        case KEY_F6:  do_move(); break;
        case KEY_F7:  do_mkdir(); break;
        case KEY_F8:  do_delete(); break;
        case KEY_F9:  do_drives(); break;
        case KEY_ALT_F1: do_drives(); break;   /* secret: same as F9 (for NC veterans) */
        case KEY_ALT_F6: do_rename(); break;   /* F6 is Move; rename moved to Alt+F6  */

        case KEY_F10:
            if (dlg_confirm(L("Quit", "Beenden"),
                            L("Really quit FTP4DOS?", "FTP4DOS wirklich beenden?")))
                running = 0;
            break;

        default:
            break;
        }
    }
    }   /* end of idle/event block */

    /* Disconnect cleanly and release the mTCP stack. */
    if (g_ftp.is_connected())
        g_ftp.disconnect();
    if (g_ftp_ready)
        FtpClient::shutdown_stack();

    tui_shutdown();
    return 0;
}
