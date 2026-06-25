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
#include "extmem.h"
#include "viewer.h"
#include "editor.h"
#include "dircopy.h"
#include "connsave.h"
#include "sites.h"
#include "checksum.h"
#include "i18n.h"
#include "umlaut.h"   /* always include last */

#define APP_VERSION "0.9.5"

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
static int  g_fullscreen  = 0;   /* Alt+F8: active panel spans the full width   */
static int  g_video_pref  = -1;  /* -1 auto, 0 force color, 1 force mono        */
static int  g_exmem       = 0;   /* /EXMEM: 1 = use extended/expanded memory   */
static int  g_exmem_pref  = 0;   /* 0 auto, 1 XMS, 2 EMS                        */
static ExtStore *g_extStore = 0; /* remote panel's XMS/EMS store (when /EXMEM)  */

static void warn_truncated(Panel *p);   /* popup when a listing didn't all fit  */

/* Persisted UI state: FTP start directory + per-pane sort mode (FTP4DOS.SAV).
 * Zero-initialized: empty start dir, both panes use the default sort until the
 * user picks one. */
static UiState g_ui = { "", 0, 0, 0, 0, 0, 0 };

/* Set by perform_connect() when the optional start directory could not be
 * entered; the caller shows the note AFTER its post-connect redraw_all(). */
static int  g_startdir_warn = 0;

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
        "Versch", "MkDir", "L" oe "sch", "Aktual", "Ende"
    };
    static const char *en[10] = {
        "Help", "Conn", "View", "Edit", "Copy",
        "Move", "MkDir", "Del", "Refresh", "Quit"
    };
    return g_english ? en[i] : de[i];
}

/* Alternate labels shown while ALT is held (Norton Commander style). Only the
 * keys that have an ALT action are labelled; "" leaves the cell blank. */
static const char *fkey_alt_label(int i)
{
    static const char *de[10] = {
        "Laufw", "Detail", "Sort", "", "",
        "Umben", "Suchen", "Vollb", "Pr" ue "fsum", ""
    };
    static const char *en[10] = {
        "Drive", "Detail", "Sort", "", "",
        "Rename", "Search", "Full", "ChkSum", ""
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

/* "1234567" -> "1,234,567" (US) or "1.234.567" (NL/DE). out >= 20 characters. */
static void format_thousands(unsigned long v, char *out)
{
    char tmp[16];
    char sep = g_locale.thousands_sep;
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
    char dec = g_locale.decimal_sep;
    unsigned long kb = bytes / SZ_KB;
    if (kb > 1000UL) {
        unsigned long whole = bytes / SZ_MB;
        unsigned long frac  = (bytes % SZ_MB) * 10UL / SZ_MB;
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

    char ds = g_locale.date_sep;
    char ts = g_locale.time_sep;

    _dos_getdate(&d);
    _dos_gettime(&t);

    /* Date order + separators from the locale, 4-digit year. Time stays 24h. */
    if (g_locale.date_order == 1)             /* DMY */
        len = sprintf(buf, "%02d%c%02d%c%04d", d.day, ds, d.month, ds, d.year);
    else if (g_locale.date_order == 2)        /* YMD */
        len = sprintf(buf, "%04d%c%02d%c%02d", d.year, ds, d.month, ds, d.day);
    else                                      /* MDY */
        len = sprintf(buf, "%02d%c%02d%c%04d", d.month, ds, d.day, ds, d.year);
    sprintf(buf + len, " %02d%c%02d%c%02d",
            t.hour, ts, t.minute, ts, t.second);

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
    /* Full-screen: the active panel takes the whole width (so long FTP names
     * are readable); the other panel is given zero width and draws nothing. */
    if (g_fullscreen && g_active) {
        Panel *other = (g_active == (Panel *)&g_left) ? (Panel *)&g_right
                                                      : (Panel *)&g_left;
        g_active->set_region(PANEL_TOP, 0, PANEL_ROWS, SCREEN_COLS);
        other->set_region(PANEL_TOP, 0, PANEL_ROWS, 0);
        return;
    }
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
        connsave_store(g_host, g_portStr, g_user, g_pass, g_savepw, g_swapped, &g_ui);
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

    /* Optional start directory: CWD into it before listing. A bad/missing dir
     * is non-fatal - keep the connection at root and note it in the status
     * (the caller flashes it after its post-connect redraw). */
    g_startdir_warn = 0;
    if (g_ui.startdir[0] && g_ftp.change_dir(g_ui.startdir) != FTP_OK)
        g_startdir_warn = 1;

    g_right.refresh();
    set_active((Panel *)&g_right);
    if (g_saveconn)
        connsave_store(g_host, g_portStr, g_user, g_pass, g_savepw, g_swapped, &g_ui);
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
                     g_ui.startdir, (int)sizeof(g_ui.startdir) - 1,
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
    if (g_startdir_warn)
        flash_status(L(" Start dir not found - connected at root.",
                       " Startverzeichnis nicht gefunden - Wurzel."));
    warn_truncated((Panel *)&g_right);
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

/* 18.2 Hz BIOS tick counter at 0040:006Ch (wraps at midnight; harmless here).
 * Avoids pulling the mTCP timer headers into this translation unit. */
static unsigned long bios_ticks(void)
{
    unsigned long far *t = (unsigned long far *)MK_FP(0x40, 0x6C);
    return *t;
}

/* bytes/ms -> bytes/sec, overflow-safe for large byte counts on 16-bit. */
static unsigned long bytes_per_sec(unsigned long bytes, unsigned long ms)
{
    if (ms == 0) return 0;
    if (bytes < 4000000UL) return bytes * 1000UL / ms;
    return bytes / ms * 1000UL;            /* large: divide first (less precise) */
}

/* State of a (recursive) copy operation - created fresh per do_copy() call,
 * i.e. "Overwrite all" only applies to the current operation. It also carries
 * the live transfer telemetry shared with the progress dialog. */
struct CopyCtx {
    int           overwrite_all;
    /* batch (all-files) accounting */
    int           batch;            /* 1 => draw the all-files lines           */
    int           files_total;      /* 0 => unknown (directories involved)     */
    int           files_done;
    unsigned long batch_total;      /* sum of marked file sizes; 0 = unknown   */
    unsigned long batch_base;       /* bytes of fully finished files           */
    unsigned long cur_file_sofar;   /* bytes of the current file so far        */
    unsigned long file_total;       /* current file's known size (0 = unknown) */
    /* timing, in BIOS ticks (55 ms each) */
    unsigned long file_start;       /* tick the current file began             */
    unsigned long pause_ticks;      /* total ticks spent paused on this file   */
    unsigned long samp_tick, samp_bytes;  /* last instantaneous-speed sample   */
    unsigned long cur_speed;        /* last instantaneous bytes/sec            */
    unsigned long last_draw;        /* tick of the last dialog redraw          */
};

/* (Re)start the per-file timing for a fresh file/transfer. */
static void copyctx_file_start(CopyCtx *c)
{
    unsigned long now = bios_ticks();
    if (!c) return;
    c->cur_file_sofar = 0;
    c->file_total     = 0;
    c->file_start     = now;
    c->pause_ticks    = 0;
    c->samp_tick      = now;
    c->samp_bytes     = 0;
    c->cur_speed      = 0;
    c->last_draw      = 0;
}

/* Fill a ProgressInfo from the context + current per-file byte count. */
static void progress_fill(CopyCtx *c, unsigned long sofar, unsigned long total,
                          int paused, ProgressInfo *pi)
{
    memset(pi, 0, sizeof(*pi));
    pi->file_sofar    = sofar;
    pi->file_total    = total;
    pi->paused        = paused;
    pi->file_eta_sec  = -1;
    pi->batch_eta_sec = -1;
    if (c) {
        unsigned long el = bios_ticks() - c->file_start - c->pause_ticks;  /* ticks */
        unsigned long avg = bytes_per_sec(sofar, el * 55UL);
        unsigned long bsofar = c->batch_base + sofar;
        pi->cur_speed   = c->cur_speed;
        pi->avg_speed   = avg;
        if (total > sofar && avg) pi->file_eta_sec = (long)((total - sofar) / avg);
        pi->batch       = c->batch;
        pi->files_done  = c->files_done;
        pi->files_total = c->files_total;
        pi->batch_sofar = bsofar;
        pi->batch_total = c->batch_total;
        if (c->batch_total > bsofar && avg)
            pi->batch_eta_sec = (long)((c->batch_total - bsofar) / avg);
    }
}

/* Poll the keyboard during a transfer: ESC cancels (returns 1), P pauses.
 * The pause loop keeps the TCP stack alive (so the connection survives) but
 * does not touch the data socket - downloads throttle via a closing window,
 * uploads simply stop sending. Returns 1 to abort, 0 to continue. */
static int progress_keys(CopyCtx *c, unsigned long sofar, unsigned long total)
{
    int k;
    if (!key_pending()) return 0;
    k = readkey();
    if (k == KEY_ESC) return 1;
    if (k == 'p' || k == 'P') {
        unsigned long pstart = bios_ticks();
        ProgressInfo  pi;
        progress_fill(c, sofar, total, 1, &pi);
        dlg_progress_update(&pi);
        for (;;) {
            FtpClient::stack_poll(50);          /* keep mTCP/ARP/TCP alive */
            if (key_pending()) {
                int k2 = readkey();
                if (k2 == KEY_ESC) return 1;
                if (k2 == 'p' || k2 == 'P') break;
            }
        }
        if (c) {                                /* discount the paused time */
            unsigned long now = bios_ticks();
            c->pause_ticks += (now - pstart);
            c->samp_tick   = now;
            c->samp_bytes  = sofar;
            c->last_draw   = 0;
        }
        progress_fill(c, sofar, total, 0, &pi);
        dlg_progress_update(&pi);
    }
    return 0;
}

/* Progress callback for FtpClient::retr/stor (during the transfer).
 * Returns non-zero to abort (ESC); also handles P=pause. */
static int copy_progress(void *ctx, unsigned long sofar, unsigned long total)
{
    CopyCtx      *c = (CopyCtx *)ctx;
    unsigned long now = bios_ticks();

    if (c) {
        c->cur_file_sofar = sofar;
        c->file_total     = total;
        if ((now - c->samp_tick) >= 9UL) {      /* resample speed ~ every 0.5s */
            c->cur_speed  = bytes_per_sec(sofar - c->samp_bytes,
                                          (now - c->samp_tick) * 55UL);
            c->samp_tick  = now;
            c->samp_bytes = sofar;
        }
    }

    if (!c || (now - c->last_draw) >= 4UL || (total && sofar >= total)) {  /* throttle ~220ms */
        ProgressInfo pi;
        progress_fill(c, sofar, total, 0, &pi);
        dlg_progress_update(&pi);
        if (c) c->last_draw = now;
    }

    return progress_keys(c, sofar, total);
}

/* Callback for dircopy: show the file/directory currently being processed.
 * Rolls the just-finished file's bytes into the batch base and restarts the
 * per-file timing/counter. */
static void copy_item(void *ctx, const char *name, int is_dir)
{
    CopyCtx *c = (CopyCtx *)ctx;
    if (c) {
        c->batch_base += c->cur_file_sofar;     /* count the previous file */
        copyctx_file_start(c);
        if (!is_dir) c->files_done++;
    }
    dlg_progress_setfile(name, c ? c->files_done : 0, c ? c->files_total : 0);
}

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
    char    target[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
    char    prompt[64];
    int     rc;
    CopyCtx cc;
    const char *abortmsg = 0;

    memset(&cc, 0, sizeof(cc));
    cc.files_total = 1;
    cc.files_done  = 1;

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
        dlg_progress_begin(L("Download", "Download"), 0);
        dlg_progress_setfile(e->name, 1, 1);
        copyctx_file_start(&cc);
        rc = g_ftp.retr(entry_name(e), target, copy_progress, &cc);
        dlg_progress_end();

        if (rc == FTP_ERR_ABORT)   abortmsg = L(" Download aborted.", " Download abgebrochen.");
        else if (rc != FTP_OK)     dlg_error(L("Download failed", "Download fehlgeschlagen"), g_ftp.last_error());
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
        dlg_progress_begin(L("Upload", "Upload"), 0);
        dlg_progress_setfile(target, 1, 1);
        copyctx_file_start(&cc);
        rc = g_ftp.stor(localpath, target, copy_progress, &cc);
        dlg_progress_end();

        if (rc == FTP_ERR_ABORT)   abortmsg = L(" Upload aborted.", " Upload abgebrochen.");
        else if (rc != FTP_OK)     dlg_error(L("Upload failed", "Upload fehlgeschlagen"), g_ftp.last_error());
        g_right.refresh();
    }
    redraw_all();
    if (abortmsg) flash_status(abortmsg);
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
        copy_item(cc, e->name, 0);
        return g_ftp.stor(localpath, e->name, copy_progress, cc);
    } else {
        if (e->is_dir)
            return dircopy_download(&g_ftp, entry_name(e), localpath,
                                    copy_item, copy_progress, copy_conflict, cc);
        /* Single file: does it already exist locally? */
        if (local_exists(localpath)) {
            int d = copy_conflict(cc, e->name);
            if (d == DC_ABORT) return FTP_ERR_ABORT;
            if (d == DC_SKIP)  return FTP_OK;
        }
        copy_item(cc, e->name, 0);
        return g_ftp.retr(entry_name(e), localpath, copy_progress, cc);
    }
}

/* Defined further below (next to the delete helpers); used by the copy/move
 * pre-scan that runs before the confirm dialog. */
static int scan_one_entry(int from_remote, PanelEntry *e,
                          unsigned *nf, unsigned *nd, unsigned long *bytes);

static void do_copy(void)
{
    PanelEntry   *cur;
    CopyCtx       cc;
    int           to_remote, from_remote, nmarked, i, rc, scan_ok = 1, hasdir = 0;
    const char   *destdir;
    char          q[140];
    char          sz[24];
    unsigned      nfiles = 0, ndirs = 0;
    unsigned long nbytes = 0;

    if (g_active == 0) return;

    if (!g_ftp.is_connected()) {
        dlg_error(L("Copy", "Kopieren"),
                  L("No FTP connection.\nConnect with F2 first.",
                    "Keine FTP-Verbindung.\nMit F2 zuerst verbinden."));
        redraw_all();
        return;
    }

    to_remote   = (g_active == (Panel *)&g_left);   /* local active -> upload */
    from_remote = !to_remote;                       /* source side for the scan */
    nmarked     = g_active->marked_count();
    cur         = g_active->selected();

    /* --- Single file without marks: interactive convenience path --- */
    if (nmarked == 0) {
        if (cur == 0 || cur->is_parent) { redraw_all(); return; }
        if (!cur->is_dir) { copy_single_file_interactive(to_remote, cur); return; }
    }

    /* --- Batch/directory copy --- */
    destdir = to_remote ? g_right.path() : g_left.path();

    /* Pre-scan the source entries (BEFORE the confirm dialog) so it can show
     * file/dir counts and the total size. Directories are walked recursively
     * (local: instant; remote: a recursive LIST, shown as "Counting ..."). If
     * any scan fails we fall back to an indeterminate total / item wording. */
    for (i = 0; i < g_active->entry_count() && nmarked > 0; i++) {
        PanelEntry *e = g_active->entry_at(i);
        if (e && e->marked && !e->is_parent && e->is_dir) { hasdir = 1; break; }
    }
    if (nmarked == 0) hasdir = 1;          /* single directory copy */
    if (hasdir) flash_status(L(" Counting ...", " Ermittle Anzahl ..."));

    if (nmarked > 0) {
        for (i = 0; i < g_active->entry_count(); i++) {
            PanelEntry *e = g_active->entry_at(i);
            if (!e || !e->marked || e->is_parent) continue;
            if (scan_one_entry(from_remote, e, &nfiles, &ndirs, &nbytes) != FTP_OK)
                { scan_ok = 0; break; }
        }
    } else {
        if (scan_one_entry(from_remote, cur, &nfiles, &ndirs, &nbytes) != FTP_OK)
            scan_ok = 0;
    }

    redraw_all();
    if (scan_ok) {
        format_human(nbytes, sz);
        sprintf(q, L("Copy %u file(s) and %u director(y/ies) %s to:\n%.40s",
                     "%u Datei(en) und %u Verzeichnis(se) %s\nkopieren nach:\n%.40s"),
                nfiles, ndirs, sz, destdir);
    } else {
        sprintf(q, L("Copy %d item(s) to:\n%.40s",
                     "%d Eintrag/Eintr" ae "ge kopieren nach:\n%.40s"),
                (nmarked > 0) ? nmarked : 1, destdir);
    }
    if (!dlg_confirm(L("Copy", "Kopieren"), q)) { redraw_all(); return; }

    memset(&cc, 0, sizeof(cc));        /* fresh per operation (no "all" carries over) */
    cc.batch = 1;
    if (scan_ok) { cc.files_total = (int)nfiles; cc.batch_total = nbytes; }

    redraw_all();
    dlg_progress_begin(L("Copy", "Kopieren"), cc.batch);

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
        CopyCtx cc;
        memset(&cc, 0, sizeof(cc));
        cc.files_total = 1;
        cc.files_done  = 1;
        join_local(path, (int)sizeof(path), g_left.path(), "$NCVIEW$.TMP");
        redraw_all();
        dlg_progress_begin(L("View", "Anzeigen"), 0);
        dlg_progress_setfile(e->name, 1, 1);
        copyctx_file_start(&cc);
        rc = g_ftp.retr(entry_name(e), path, copy_progress, &cc);
        dlg_progress_end();
        if (rc != FTP_OK) {
            remove(path);   /* clean up any partially started temp file */
            if (rc == FTP_ERR_ABORT) {
                redraw_all();
                flash_status(L(" View aborted.", " Anzeigen abgebrochen."));
            } else {
                dlg_error(L("View failed", "Anzeigen fehlgeschlagen"), g_ftp.last_error());
                redraw_all();
            }
            return;
        }
        view_file(path, e->name);
        remove(path);
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * Alt+F9 - Checksum (CRC32 + MD5) of the selected file
 * -----------------------------------------------------------------------------
 * Local files are read directly; remote files are first downloaded to a temp
 * file (like F3/View), checksummed, then the temp file is removed. The result
 * popup shows both sums and can save them to a text file in the local dir.
 * ---------------------------------------------------------------------- */
static void do_checksum(void)
{
    char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
    char crc[16], md5[40];
    char fname[PANEL_NAME_MAX];
    char title[PANEL_NAME_MAX + 16];
    int  remote = 0, i;
    PanelEntry *e;

    if (g_active == 0) return;
    e = g_active->selected();
    if (e == 0 || e->is_parent || e->is_dir) { redraw_all(); return; }

    if (g_active == (Panel *)&g_left) {
        join_local(path, (int)sizeof(path), g_left.path(), e->name);
    } else {
        /* Remote: download to a temporary local file first. */
        int rc;
        CopyCtx cc;
        remote = 1;
        if (!g_ftp.is_connected()) {
            dlg_error(L("Checksum", "Pr" ue "fsumme"),
                      L("No FTP connection.\nConnect with F2 first.",
                        "Keine FTP-Verbindung.\nMit F2 zuerst verbinden."));
            redraw_all();
            return;
        }
        memset(&cc, 0, sizeof(cc));
        cc.files_total = 1;
        cc.files_done  = 1;
        join_local(path, (int)sizeof(path), g_left.path(), "$NCSUM$.TMP");
        redraw_all();
        dlg_progress_begin(L("Checksum", "Pr" ue "fsumme"), 0);
        dlg_progress_setfile(e->name, 1, 1);
        copyctx_file_start(&cc);
        rc = g_ftp.retr(entry_name(e), path, copy_progress, &cc);
        dlg_progress_end();
        if (rc != FTP_OK) {
            remove(path);   /* clean up any partial temp file */
            if (rc == FTP_ERR_ABORT) {
                redraw_all();
                flash_status(L(" Checksum aborted.", " Pr" ue "fsumme abgebrochen."));
            } else {
                dlg_error(L("Checksum failed", "Pr" ue "fsumme fehlgeschlagen"),
                          g_ftp.last_error());
                redraw_all();
            }
            return;
        }
    }

    flash_status(L(" Computing checksum ...", " Berechne Pr" ue "fsumme ..."));
    if (checksum_file(path, crc, md5) != 0) {
        if (remote) remove(path);
        dlg_error(L("Checksum", "Pr" ue "fsumme"),
                  L("Could not read the file.", "Datei konnte nicht gelesen werden."));
        redraw_all();
        return;
    }
    if (remote) remove(path);   /* temp no longer needed */

    /* Default save name: first 8 chars of the base name + ".CHK". */
    for (i = 0; i < 8 && e->name[i] && e->name[i] != '.'; i++)
        fname[i] = e->name[i];
    fname[i] = '\0';
    if (fname[0] == '\0') strcpy(fname, "CHECKSUM");
    strcat(fname, ".CHK");

    sprintf(title, "%s %.*s", L("Checksum:", "Pr" ue "fsumme:"),
            PANEL_NAME_MAX - 1, e->name);

    redraw_all();
    if (dlg_checksum(title, crc, md5, fname, (int)sizeof(fname) - 1)) {
        char savepath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        FILE *f;
        join_local(savepath, (int)sizeof(savepath), g_left.path(), fname);
        f = fopen(savepath, "w");
        if (f == 0) {
            dlg_error(L("Checksum", "Pr" ue "fsumme"),
                      L("Could not write the file.",
                        "Datei konnte nicht geschrieben werden."));
        } else {
            fprintf(f, "; FTP4DOS checksum - %s\n", e->name);
            fprintf(f, "CRC32: %s\n", crc);
            fprintf(f, "MD5:   %s\n", md5);
            fclose(f);
            g_left.refresh();   /* show the new file in the local panel */
        }
    }
    redraw_all();
}

/* Format n as a decimal count of 'unit' bytes with one fractional digit, using
 * the locale decimal separator (e.g. "576,7"). Overflow-safe for GB. */
static void fmt_unit(unsigned long n, unsigned long unit, char *out)
{
    unsigned long whole = n / unit;
    unsigned long rem   = n % unit;
    unsigned long frac;
    if (unit >= 100000000UL) frac = rem / (unit / 10UL);   /* GB: avoid rem*10 overflow */
    else                     frac = rem * 10UL / unit;      /* KB/MB: more precise        */
    if (frac > 9) frac = 9;
    sprintf(out, "%lu%c%lu", whole, g_locale.decimal_sep, frac);
}

/* -------------------------------------------------------------------------
 * Alt+F2 - Detail: full name + size (Bytes/KB/MB/GB) of the selected entry.
 * The panel column truncates long FTP names; this shows the complete name.
 * ---------------------------------------------------------------------- */
static void do_detail(void)
{
    PanelEntry *e;
    char msg[600];
    char wrapped[300];

    if (g_active == 0) return;
    e = g_active->selected();
    if (e == 0 || e->is_parent) { redraw_all(); return; }

    /* Show the full name (may exceed the panel column), wrapped to fit the
     * dialog: break it into <=60-char chunks on their own lines. */
    {
        const char *nm = entry_name(e);
        int o = 0, col = 0;
        while (*nm && o < (int)sizeof(wrapped) - 2) {
            wrapped[o++] = *nm++;
            if (++col >= 60 && *nm) { wrapped[o++] = '\n'; col = 0; }
        }
        wrapped[o] = '\0';
    }

    if (e->is_dir) {
        sprintf(msg, "Name:\n%s\n\n  <DIR>", wrapped);
    } else {
        char nbytes[24], kb[24], mb[24], gb[24];
        format_thousands(e->size, nbytes);
        fmt_unit(e->size, SZ_KB, kb);
        fmt_unit(e->size, SZ_MB, mb);
        fmt_unit(e->size, SZ_GB, gb);
        sprintf(msg, "Name:\n%s\n\n  Bytes: %s\n     KB: %s\n     MB: %s\n     GB: %s",
                wrapped, nbytes, kb, mb, gb);
    }
    dlg_message(L("File details", "Dateidetails"), msg, 0);
    redraw_all();
}

/* If the last listing didn't fit the panel's buffer, tell the user (once per
 * listing - refresh() clears the flag). Hint at /EXMEM for more room. */
static void warn_truncated(Panel *p)
{
    char msg[200];
    if (p == 0 || !p->is_truncated()) return;
    sprintf(msg,
        L("This directory has %d entries; only the\nfirst %d can be shown.\n\nStart with /EXMEM (XMS/EMS) to list them all.",
          "Dieses Verzeichnis hat %d Eintr" ae "ge; nur die\nersten %d sind anzeigbar.\n\nMit /EXMEM (XMS/EMS) alle anzeigen."),
        p->total_count(), p->entry_count());
    dlg_message(L("Directory truncated", "Verzeichnis gek" ue "rzt"), msg, 0);
}

/* -------------------------------------------------------------------------
 * F9 - Refresh: re-read the active panel (e.g. to see a new remote file).
 * ---------------------------------------------------------------------- */
static void do_refresh(void)
{
    if (g_active == 0) return;
    g_active->refresh();   /* remote: re-LISTs over FTP; local: re-reads the dir */
    redraw_all();
    flash_status(L(" Refreshed.", " Aktualisiert."));
    warn_truncated(g_active);
}

/* -------------------------------------------------------------------------
 * Alt+F7 / Ctrl+F - Search: jump to the next entry whose name starts with the
 * typed text. Search starts AFTER the cursor and wraps to the top.
 * ---------------------------------------------------------------------- */
static void do_search(void)
{
    char buf[80];
    int  idx;
    if (g_active == 0) return;
    buf[0] = '\0';
    if (!dlg_input(L("Search", "Suchen"),
                   L("Jump to name starting with:", "Springe zu Name, beginnend mit:"),
                   buf, (int)sizeof(buf) - 1, 0)) {
        redraw_all();
        return;
    }
    if (buf[0] == '\0') { redraw_all(); return; }

    idx = g_active->find_prefix(buf, g_active->selected_index() + 1);
    if (idx < 0) idx = g_active->find_prefix(buf, 0);   /* wrap around to the top */

    redraw_all();
    if (idx >= 0) {
        g_active->set_cursor_index(idx);
        g_active->draw();
        draw_statusbar();
    } else {
        flash_status(L(" Not found.", " Nicht gefunden."));
    }
}

/* -------------------------------------------------------------------------
 * Alt+F8 - Full-screen toggle: the active panel uses the whole 80 columns so
 * long remote file names are readable; toggle again to restore the split view.
 * ---------------------------------------------------------------------- */
static void do_fullscreen(void)
{
    g_fullscreen = !g_fullscreen;
    apply_panel_regions();
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
        return e->is_dir ? g_ftp.remove_dir(entry_name(e)) : g_ftp.remove_file(entry_name(e));
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
        return dircopy_delete_remote(&g_ftp, entry_name(e), copy_item, 0);
    {
        char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(path, (int)sizeof(path), g_left.path(), e->name);
        return dircopy_delete_local(path, copy_item, 0);
    }
}

/* Recursively add one panel entry to the running totals. A file adds 1 to *nf
 * (+ its size to *bytes); a directory adds 1 to *nd for itself, then recurses
 * via dircopy_measure_* for nested files/dirs/bytes. from_remote != 0 => the
 * entry lives on the remote panel. Returns FTP_OK or an FTP_ERR_* (a failed
 * remote LIST) so callers can fall back to an indeterminate total. */
static int scan_one_entry(int from_remote, PanelEntry *e,
                          unsigned *nf, unsigned *nd, unsigned long *bytes)
{
    if (!e->is_dir) { (*nf)++; *bytes += e->size; return FTP_OK; }
    (*nd)++;                              /* the top-level directory itself */
    if (from_remote) {
        return dircopy_measure_remote(&g_ftp, entry_name(e), nf, nd, bytes);
    } else {
        char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(path, (int)sizeof(path), g_left.path(), e->name);
        return dircopy_measure_local(path, nf, nd, bytes);
    }
}

static void do_delete(void)
{
    char          prompt[140];
    char          sz[24];
    PanelEntry   *cur;
    int           on_remote, nmarked, i, errors, keepidx;
    unsigned      nfiles = 0, ndirs = 0;
    unsigned long nbytes = 0;

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
            scan_one_entry(on_remote, e, &nfiles, &ndirs, &nbytes);
        }
    } else {
        scan_one_entry(on_remote, cur, &nfiles, &ndirs, &nbytes);  /* single dir */
    }

    format_human(nbytes, sz);
    sprintf(prompt, L("Permanently delete\n%u file(s) and %u director(y/ies) %s?",
                      "%u Datei(en) und %u Verzeichnis(se) %s\nunwiderruflich l" oe "schen?"),
            nfiles, ndirs, sz);
    if (!dlg_confirm(L("Delete", "L" oe "schen"), prompt)) { redraw_all(); return; }

    redraw_all();
    dlg_progress_begin(L("Delete", "L" oe "schen"), 0);

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
    PanelEntry   *cur;
    CopyCtx       cc;
    int           to_remote, src_remote, nmarked, i, rc, keepidx, scan_ok = 1, hasdir = 0;
    const char   *destdir;
    char          q[140];
    char          sz[24];
    unsigned      nfiles = 0, ndirs = 0;
    unsigned long nbytes = 0;

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

    destdir = to_remote ? g_right.path() : g_left.path();

    /* Pre-scan the source entries for the confirm dialog (counts + size). */
    for (i = 0; i < g_active->entry_count() && nmarked > 0; i++) {
        PanelEntry *e = g_active->entry_at(i);
        if (e && e->marked && !e->is_parent && e->is_dir) { hasdir = 1; break; }
    }
    if (nmarked == 0) hasdir = 1;
    if (hasdir) flash_status(L(" Counting ...", " Ermittle Anzahl ..."));

    if (nmarked > 0) {
        for (i = 0; i < g_active->entry_count(); i++) {
            PanelEntry *e = g_active->entry_at(i);
            if (!e || !e->marked || e->is_parent) continue;
            if (scan_one_entry(src_remote, e, &nfiles, &ndirs, &nbytes) != FTP_OK)
                { scan_ok = 0; break; }
        }
    } else {
        if (scan_one_entry(src_remote, cur, &nfiles, &ndirs, &nbytes) != FTP_OK)
            scan_ok = 0;
    }

    redraw_all();
    if (scan_ok) {
        format_human(nbytes, sz);
        sprintf(q, L("Move %u file(s) and %u director(y/ies) %s to:\n%.40s",
                     "%u Datei(en) und %u Verzeichnis(se) %s\nverschieben nach:\n%.40s"),
                nfiles, ndirs, sz, destdir);
    } else {
        sprintf(q, L("Move %d item(s) to:\n%.40s",
                     "%d Eintrag/Eintr" ae "ge verschieben nach:\n%.40s"),
                (nmarked > 0) ? nmarked : 1, destdir);
    }
    if (!dlg_confirm(L("Move", "Verschieben"), q)) { redraw_all(); return; }

    cc.overwrite_all = 0;              /* fresh per operation */

    redraw_all();
    dlg_progress_begin(L("Move", "Verschieben"), 0);

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
        rc = g_ftp.rename(entry_name(e), newname);
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
 * Alt+F3 - Sort the active panel (configurable key + direction)
 * One combined menu: 5 keys x ascending/descending, plus a "Default" entry.
 * Each panel keeps its own mode (Panel::set_sort), so left and right can be
 * sorted differently; the choice is remembered per pane in FTP4DOS.SAV and
 * "Default" forgets it again.
 * ---------------------------------------------------------------------- */
static void do_sort(void)
{
    static const char *en[11] = {
        "Name (A-Z)",       "Name (Z-A)",
        "Extension (A-Z)",  "Extension (Z-A)",
        "Size (small-big)", "Size (big-small)",
        "Date (old-new)",   "Date (new-old)",
        "Time (early-late)","Time (late-early)",
        "Default"
    };
    static const char *de[11] = {
        "Name (A-Z)",          "Name (Z-A)",
        "Endung (A-Z)",        "Endung (Z-A)",
        "Gr" oe ss "e (klein)","Gr" oe ss "e (gro" ss ")",
        "Datum (alt)",         "Datum (neu)",
        "Zeit (fr" ue "h)",    "Zeit (sp" ae "t)",
        "Standard"
    };
    const char *items[11];
    char keep[PANEL_NAME_MAX];
    PanelEntry *e;
    int i, init, r, isleft;

    if (g_active == 0) return;
    for (i = 0; i < 11; i++) items[i] = g_english ? en[i] : de[i];

    init = g_active->sort_key() * 2 + g_active->sort_desc();
    r = dlg_menu(L("Sort", "Sortieren"), items, 11, init);
    if (r >= 0) {
        e = g_active->selected();
        if (e) { strncpy(keep, e->name, sizeof(keep) - 1); keep[sizeof(keep) - 1] = '\0'; }
        else   { keep[0] = '\0'; }

        /* r 0..9 = key*2+dir; r == 10 = "Default" (Name ascending, forget the
         * saved choice for this pane). */
        if (r == 10) g_active->set_sort(Panel::SORT_NAME, 0);
        else         g_active->set_sort(r / 2, r & 1);
        g_active->resort();
        if (keep[0]) g_active->select_by_name(keep);

        /* Remember (or clear) the choice for THIS pane, keyed by object
         * identity so it is stable regardless of g_swapped. */
        isleft = (g_active == (Panel *)&g_left);
        if (isleft) {
            g_ui.lsort_key   = g_active->sort_key();
            g_ui.lsort_desc  = g_active->sort_desc();
            g_ui.lsort_saved = (r == 10) ? 0 : 1;
        } else {
            g_ui.rsort_key   = g_active->sort_key();
            g_ui.rsort_desc  = g_active->sort_desc();
            g_ui.rsort_saved = (r == 10) ? 0 : 1;
        }
        if (g_saveconn)
            connsave_store(g_host, g_portStr, g_user, g_pass, g_savepw, g_swapped, &g_ui);
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
    printf("Usage: FTP4DOS [/L:EN|DE] [/H:HOST] [/P:PORT] [/U:USER] [/W:PASS] [/D:DIR] [/S:ALL|NOPASS|OFF] [/EXMEM[:XMS|EMS]] [/Q] [/MONO|/COLOR]\n");
    printf("       ('-' may be used instead of '/'; flags are case-insensitive)\n\n");
    printf("  /L:EN|DE        force English or German user interface\n");
    printf("  /H:HOST         connect to HOST automatically on startup\n");
    printf("  /P:PORT         port (default 21)\n");
    printf("  /U:USER         user name (default anonymous)\n");
    printf("  /W:PASS         password  (WARNING: stored in cleartext in the batch file)\n");
    printf("  /D:DIR          FTP start directory after connect (empty = root)\n");
    printf("  /S:ALL          save connection incl. password to FTP4DOS.SAV (default)\n");
    printf("  /S:NOPASS       save connection but not the password\n");
    printf("  /S:OFF          do not save this connection\n");
    printf("  /EXMEM          list very large remote dirs via extended/expanded\n");
    printf("                  memory (auto: XMS then EMS; force with /EXMEM:XMS or :EMS)\n");
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
    sites_init(argv[0]);
    connsave_load(g_host, (int)sizeof(g_host), g_portStr, (int)sizeof(g_portStr),
                  g_user, (int)sizeof(g_user), g_pass, (int)sizeof(g_pass),
                  &g_savepw, &g_swapped, &g_ui);

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
            case 'd':       /* /D:DIR : FTP start directory (empty = root) */
                strncpy(g_ui.startdir, val, sizeof(g_ui.startdir) - 1);
                g_ui.startdir[sizeof(g_ui.startdir) - 1] = 0;
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
            case 'e':       /* /EXMEM[:XMS|:EMS] : large remote lists via XMS/EMS */
                if (strnicmp(o, "exmem", 5) == 0 &&
                    (o[5] == '\0' || o[5] == ':')) {
                    g_exmem = 1;
                    if (o[5] == ':') {
                        char b = (char)toupper((unsigned char)o[6]);
                        if      (b == 'X') g_exmem_pref = 1;
                        else if (b == 'E') g_exmem_pref = 2;
                    }
                }
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

    /* /EXMEM: back the remote panel with XMS/EMS so huge listings fit. The
     * local panel keeps the standard conventional buffer. */
    if (g_exmem) {
        ExtMem *mem = extmem_create(g_exmem_pref);
        if (mem) {
            g_extStore = new ExtStore(mem);
            if (g_extStore && g_extStore->ok()) {
                g_right.use_store(g_extStore);
            } else {
                if (g_extStore) { delete g_extStore; g_extStore = 0; }
                else            { delete mem; }
                dlg_message(L("Extended memory", "Erweiterter Speicher"),
                    L("Could not reserve extended memory.\nUsing the standard list.",
                      "Erweiterter Speicher nicht reservierbar.\nStandardliste wird verwendet."), 0);
            }
        } else {
            dlg_message(L("Extended memory", "Erweiterter Speicher"),
                L("XMS/EMS not available.\nUsing the standard list.",
                  "XMS/EMS nicht verf" ue "gbar.\nStandardliste wird verwendet."), 0);
        }
    }

    apply_panel_regions();          /* honors g_swapped loaded from FTP4DOS.SAV */
    /* Apply the remembered per-pane sort (if any) before the first refresh;
     * set_sort only sets the mode, the refresh() below sorts via sort_entries(). */
    if (g_ui.lsort_saved) g_left.set_sort(g_ui.lsort_key, g_ui.lsort_desc);
    if (g_ui.rsort_saved) g_right.set_sort(g_ui.rsort_key, g_ui.rsort_desc);
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
            if (g_startdir_warn)
                flash_status(L(" Start dir not found - connected at root.",
                               " Startverzeichnis nicht gefunden - Wurzel."));
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
            if (g_fullscreen) {                 /* the newly active panel takes over */
                apply_panel_regions();
                redraw_all();
            } else {
                g_left.draw();
                g_right.draw();
                draw_statusbar();
            }
            break;

        case KEY_CTRL_U:        /* swap panels left<->right (remembered) */
            do_swap_panels();
            break;

        case KEY_CTRL_A: do_detail();   break;  /* alias for Alt+F2 */
        case KEY_CTRL_F: do_search();   break;  /* alias for Alt+F7 */
        case KEY_CTRL_R: do_refresh();  break;  /* alias for F9     */

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
                warn_truncated(g_active);
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
            warn_truncated(g_active);
            break;

        /* Function keys. */
        case KEY_F1:
            /* Only keys NOT already shown on the function-key bar (the F-keys
             * and their Alt variants are visible there). Leading "\n" leaves a
             * blank line between the frame and the first content row. */
            dlg_message(L("Help - FTP4DOS v" APP_VERSION, "Hilfe - FTP4DOS v" APP_VERSION),
                L("\n"
                  "Tab        Switch active panel\n"
                  "Ctrl+U     Swap left/right panels\n"
                  "Ctrl+A     File details (= Alt+F2)\n"
                  "Ctrl+F     Search / jump to name (= Alt+F7)\n"
                  "Ctrl+R     Refresh active panel (= F9)\n"
                  "Insert     Mark item (copy/move/delete several)\n"
                  "*          Invert selection\n"
                  "+          Mark files missing or different in other panel\n"
                  "Enter      Enter directory / view file\n"
                  "Backspace  Parent directory\n"
                  "Arrows/PgUp/PgDn/Home/End  Navigate\n"
                  "\n"
                  "F1-F10     Commands on the key bar (hold Alt for alternatives)",
                  "\n"
                  "Tab        Aktives Panel wechseln\n"
                  "Strg+U     Panels tauschen\n"
                  "Strg+A     Dateidetails (= Alt+F2)\n"
                  "Strg+F     Suchen / zu Name springen (= Alt+F7)\n"
                  "Strg+R     Aktives Panel aktualisieren (= F9)\n"
                  "Einfg      Eintrag markieren (mehrere kop./versch./l" oe "schen)\n"
                  "*          Markierung invertieren\n"
                  "+          Fehlende/abweichende Dateien gg. anderem Panel markieren\n"
                  "Enter      Verzeichnis betreten / Datei anzeigen\n"
                  "Backspace  " Ue "bergeordnetes Verzeichnis\n"
                  "Pfeile, Bild-auf/ab, Pos1, Ende  Navigieren\n"
                  "\n"
                  "F1-F10     Befehle in der Tastenleiste (Alt halten f" ue "r Alternativen)"), 0);
            break;
        case KEY_F2:  do_connect(); break;
        case KEY_F3:  do_view(); break;
        case KEY_F4:  do_edit(); break;
        case KEY_F5:  do_copy(); break;
        case KEY_F6:  do_move(); break;
        case KEY_F7:  do_mkdir(); break;
        case KEY_F8:  do_delete(); break;
        case KEY_F9:  do_refresh(); break;     /* re-read the active panel            */
        case KEY_ALT_F1: do_drives(); break;   /* change drive (also shown on the bar) */
        case KEY_ALT_F2: do_detail(); break;   /* full name + size of selected entry  */
        case KEY_ALT_F3: do_sort(); break;     /* sort dialog for the active panel    */
        case KEY_ALT_F6: do_rename(); break;   /* F6 is Move; rename moved to Alt+F6  */
        case KEY_ALT_F7: do_search(); break;   /* jump to next name with a prefix     */
        case KEY_ALT_F8: do_fullscreen(); break; /* full-screen the active panel      */
        case KEY_ALT_F9: do_checksum(); break; /* CRC32 + MD5 of the selected file    */

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
