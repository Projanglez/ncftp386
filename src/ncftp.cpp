/* =============================================================================
 * ncftp.cpp - NCFTP386: Main, Bildschirmaufbau und Event-Loop
 * -----------------------------------------------------------------------------
 * Norton-Commander-artiger Dual-Panel-Dateimanager: links lokales DOS-Datei-
 * system, rechts FTP-remote ueber mTCP. Die Hauptschleife arbeitet polymorph
 * ueber Panel*.
 *
 * Bildschirm (80x25) - ohne Menue-/Kommandozeile, maximaler Panelinhalt:
 *   Zeilen 0-22 : zwei Panels (je 40 Spalten breit)
 *   Zeile  23   : Statusleiste (Dateiinfo + Verbindungsstatus)
 *   Zeile  24   : Funktionstastenleiste
 *
 * Sprache (Deutsch/Englisch) wird beim Start aus der DOS-Laendereinstellung
 * abgeleitet; alle sichtbaren Texte laufen ueber L("de","en").
 * Kommandozeile: NCFTP EN  (oder /EN, -EN) -> erzwingt Englisch.
 * ===========================================================================*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <direct.h>
#include <dos.h>
#include <ctype.h>

#include "tui.h"
#include "panel.h"
#include "lpanel.h"
#include "rpanel.h"
#include "ftpcli.h"
#include "keymap.h"
#include "dialog.h"
#include "viewer.h"
#include "dircopy.h"
#include "connsave.h"
#include "i18n.h"

/* ---- Bildschirm-Layout ---- */
#define PANEL_TOP     0
#define PANEL_ROWS    23           /* Zeilen 0..22                     */
#define PANEL_COLS    40
#define ROW_STATUS    23
#define ROW_FKEYS     24

/* ---- Globale Panels ---- */
/* Grosse Objekte (~25 KB je Panel): beim Kompilieren von ncftp.cpp -zt256
 * setzen, damit Open Watcom sie in FAR-Datensegmente legt (DGROUP < 64 KB).
 * Links lokal, rechts FTP-remote. Die Hauptschleife arbeitet polymorph
 * ueber Panel*. */
static LocalPanel  g_left;
static RemotePanel g_right;
static Panel      *g_active = 0;

/* FTP-Client (Steuerung der Remote-Seite). g_ftp_ready=1, sobald der
 * mTCP-Stack erfolgreich initialisiert wurde. */
static FtpClient g_ftp;
static int       g_ftp_ready = 0;

/* Verbindungsdaten der zuletzt genutzten/gemerkten Verbindung. Auf Datei-Ebene,
 * damit main() (Laden aus NCFTP.SAV, Kommandozeile) und do_connect() (Dialog,
 * Speichern) gemeinsam darauf zugreifen. */
static char g_host[FTP_HOST_MAX] = "";
static char g_portStr[8]         = "21";
static char g_user[40]           = "anonymous";
static char g_pass[40]           = "";
static int  g_savepw      = 1;   /* Opt-out: Passwort standardmaessig merken   */
static int  g_nosave      = 0;   /* -n: diese Sitzung nicht in NCFTP.SAV ablegen*/
static int  g_autoconnect = 0;   /* per Kommandozeile (-h) automatisch verbinden*/

/* Kritischer-Fehler-Handler (INT 24h): verhindert die DOS-Abfrage
 * "Abort, Retry, Fail?" - z.B. bei einem leeren Diskettenlaufwerk. Statt den
 * Bildschirm zu zerstoeren, lassen wir die fehlgeschlagene DOS-Operation
 * einfach mit Fehler zurueckkehren ("Fail"). Wird einmal in main() registriert. */
static int ncftp_harderr(unsigned deverr, unsigned errcode, unsigned *devhdr)
{
    (void)deverr; (void)errcode; (void)devhdr;
    return _HARDERR_FAIL;           /* fehlgeschlagene DOS-Operation: einfach scheitern */
}

/* -------------------------------------------------------------------------
 * Bildschirm-Chrome
 * ---------------------------------------------------------------------- */

/* Funktionstasten-Beschriftung je Sprache. F7 = "MkDir" (in beiden gleich). */
static const char *fkey_label(int i)
{
    static const char *de[10] = {
        "Hilfe", "Verb", "Anzeig", "Edit", "Kopier",
        "Umben", "MkDir", "Loesch", "Laufw", "Ende"
    };
    static const char *en[10] = {
        "Help", "Conn", "View", "Edit", "Copy",
        "Ren",  "MkDir", "Del", "Drive", "Quit"
    };
    return g_english ? en[i] : de[i];
}

static void draw_fkeybar(void)
{
    int i;
    fill_rect(ROW_FKEYS, 0, 1, SCREEN_COLS, ' ', ATTR_FNKEY_LBL);
    for (i = 0; i < 10; i++) {
        int col = i * 8;            /* 10 Zellen a 8 Spalten = 80 */
        char num[4];
        int nlen;
        sprintf(num, "%d", i + 1);  /* 1..10 */
        nlen = (int)strlen(num);
        draw_text(ROW_FKEYS, col,        num,          ATTR_FNKEY_NUM, nlen);
        draw_text(ROW_FKEYS, col + nlen, fkey_label(i), ATTR_FNKEY_LBL, 8 - nlen);
    }
}

/* "1234567" -> "1.234.567" (de) bzw. "1,234,567" (en). out >= 20 Zeichen. */
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

/* Bytes -> "(123 KB)" bzw. "(1,2 MB)". MB ab > 1000 KB. out >= 24 Zeichen. */
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
        sprintf(conn, "%s %.20s", L("Verbunden:", "Connected:"), g_ftp.host_name());
    else
        strcpy(conn, L("Nicht verbunden", "Not connected"));
    clen = (int)strlen(conn);

    fill_rect(ROW_STATUS, 0, 1, SCREEN_COLS, ' ', ATTR_STATUSBAR);

    /* Markierungen haben Vorrang vor der Einzeldatei-Info. */
    if (g_active && g_active->marked_count() > 0) {
        int nm = g_active->marked_count();
        unsigned long ms = g_active->marked_size();
        if (ms > 0) {
            char num[20];
            format_thousands(ms, num);
            sprintf(info, L(" %d markiert   %s Bytes", " %d marked   %s bytes"), nm, num);
        } else {
            sprintf(info, L(" %d markiert", " %d marked"), nm);
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
        strcpy(info, L(" (leer)", " (empty)"));
    }
    draw_text(ROW_STATUS, 0, info, ATTR_STATUSBAR, SCREEN_COLS - 2 - clen);
    draw_text(ROW_STATUS, SCREEN_COLS - clen, conn, ATTR_STATUSBAR, clen);
}

/* Kurze Meldung in der Statusleiste (bis zur naechsten Aktualisierung). */
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

static void redraw_all(void)
{
    g_left.draw();
    g_right.draw();
    draw_statusbar();
    draw_fkeybar();
}

/* -------------------------------------------------------------------------
 * F2 - FTP-Verbindung herstellen / trennen
 * Verbindungsdaten stehen in g_host/g_portStr/g_user/g_pass (aus NCFTP.SAV oder
 * der Kommandozeile vorbelegt, im Dialog editierbar). Bei Erfolg werden sie -
 * sofern nicht per -n unterdrueckt - wieder gespeichert (Passwort nur, wenn
 * g_savepw gesetzt ist). Die eigentliche Verbindung ist blockierend.
 * ---------------------------------------------------------------------- */

/* Verbindung mit den aktuellen g_*-Daten aufbauen. Bei Erfolg: rechtes Panel
 * listen, Fokus dorthin, Daten speichern. Rueckgabe FTP_OK oder Fehlercode.
 * Zeichnet selbst NICHT neu (Aufrufer entscheidet ueber redraw/Fehlerdialog). */
static int perform_connect(void)
{
    unsigned port = (unsigned)atoi(g_portStr);
    int rc;

    if (port == 0) port = 21;
    flash_status(L(" Verbinde mit FTP-Server ...", " Connecting to FTP server ..."));

    rc = g_ftp.connect(g_host, port, g_user, g_pass);
    if (rc != FTP_OK) return rc;

    g_right.refresh();
    set_active((Panel *)&g_right);
    if (!g_nosave)
        connsave_store(g_host, g_portStr, g_user, g_pass, g_savepw);
    return FTP_OK;
}

static void do_connect(void)
{
    if (!g_ftp_ready) {
        dlg_error(L("FTP nicht verfuegbar", "FTP unavailable"),
                  L("TCP/IP konnte nicht gestartet werden.\n"
                    "MTCPCFG pruefen und Programm neu starten.",
                    "TCP/IP could not be started.\n"
                    "Check MTCPCFG and restart the program."));
        redraw_all();
        return;
    }

    /* Bereits verbunden -> Trennen anbieten. */
    if (g_ftp.is_connected()) {
        if (dlg_confirm(L("Trennen", "Disconnect"),
                        L("Bestehende FTP-Verbindung trennen?",
                          "Close the current FTP connection?"))) {
            g_ftp.disconnect();
            g_right.refresh();
        }
        redraw_all();
        return;
    }

    if (!dlg_input(L("FTP-Verbindung", "FTP Connection"), "Host:", g_host, FTP_HOST_MAX - 1, 0)) { redraw_all(); return; }
    if (g_host[0] == '\0')                                                                        { redraw_all(); return; }
    if (!dlg_input(L("FTP-Verbindung", "FTP Connection"), "Port:", g_portStr, 6, 0))              { redraw_all(); return; }
    if (!dlg_input(L("FTP-Verbindung", "FTP Connection"), L("Benutzer:", "User:"), g_user, 38, 0)){ redraw_all(); return; }
    if (!dlg_input(L("FTP-Verbindung", "FTP Connection"), L("Passwort:", "Password:"), g_pass, 38, 1)) { redraw_all(); return; }

    /* Opt-out: Host/Port/Benutzer werden immer gemerkt; nur fuers Passwort
     * fragen wir nach. Vorgabe = zuletzt getroffene Wahl (frisch: Ja). */
    g_savepw = dlg_confirm_def(L("Speichern", "Save"),
                               L("Passwort mitspeichern?", "Save the password too?"),
                               g_savepw);

    redraw_all();
    if (perform_connect() != FTP_OK) {
        dlg_error(L("Verbindung fehlgeschlagen", "Connection failed"), g_ftp.last_error());
        redraw_all();
        return;
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F5 - Kopieren zwischen lokalem und Remote-Panel
 * Die Richtung ergibt sich aus dem aktiven Panel:
 *   aktiv = lokal  -> Upload   (STOR) lokal  -> Remote
 *   aktiv = remote -> Download (RETR) Remote -> lokal
 *
 * Es werden alle markierten Eintraege kopiert (Einfg-Taste); ohne Markierung
 * der Eintrag unter dem Cursor. Verzeichnisse werden rekursiv inkl. aller
 * Unterverzeichnisse kopiert (dircopy.cpp). Beim Kopieren einer einzelnen
 * Datei ohne Markierung laesst sich der Zielname vorher editieren.
 * ---------------------------------------------------------------------- */

/* Fortschritts-Callback fuer FtpClient::retr/stor (waehrend des Transfers). */
static void copy_progress(void *ctx, unsigned long sofar, unsigned long total)
{
    (void)ctx;
    dlg_progress_update(sofar, total);
}

/* Callback fuer dircopy: aktuell bearbeitete Datei/Verzeichnis anzeigen. */
static void copy_item(void *ctx, const char *name, int is_dir)
{
    (void)ctx; (void)is_dir;
    dlg_progress_setfile(name);
}

/* Zustand eines (rekursiven) Kopiervorgangs - wird je do_copy() neu angelegt,
 * d.h. "Alle ueberschreiben" gilt nur fuer den aktuellen Vorgang. */
struct CopyCtx {
    int overwrite_all;
};

/* 4-Optionen-Abfrage bei Dateikonflikt. Rueckgabe wie dlg_choice (0..3 / -1). */
static int dlg_overwrite(const char *name)
{
    char        msg[120];
    const char *items[4];
    sprintf(msg, L("Datei existiert bereits:\n%.40s",
                   "File already exists:\n%.40s"), name);
    items[0] = L("Ueberschreiben",      "Overwrite");
    items[1] = L("Datei ueberspringen", "Skip file");
    items[2] = L("Alle ueberschreiben", "Overwrite all");
    items[3] = L("Vorgang abbrechen",   "Cancel operation");
    return dlg_choice(L("Ueberschreiben?", "Overwrite?"), msg, items, 4);
}

/* Konflikt-Callback fuer dircopy + die Einzeldatei-Stapelfaelle. */
static int copy_conflict(void *vctx, const char *name)
{
    CopyCtx *c = (CopyCtx *)vctx;
    int r;
    if (c && c->overwrite_all) return DC_OVERWRITE;

    r = dlg_overwrite(name);
    if (r == 0) return DC_OVERWRITE;                         /* Ueberschreiben */
    if (r == 1) return DC_SKIP;                              /* Ueberspringen  */
    if (r == 2) { if (c) c->overwrite_all = 1; return DC_OVERWRITE; }  /* Alle */
    return DC_ABORT;                                         /* Abbrechen/Esc  */
}

/* 1, falls die lokale Datei existiert (fuer die Ueberschreiben-Abfrage). */
static int local_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Lokalen Pfad "dir\name" zusammensetzen (Wurzel "C:\" beachten), laengensicher. */
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

/* Einzelne Datei interaktiv kopieren (Zielname editierbar, Ueberschreib-Abfrage).
 * to_remote != 0 => Upload (lokal -> Remote), sonst Download. */
static void copy_single_file_interactive(int to_remote, PanelEntry *e)
{
    char target[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
    char prompt[64];
    int  rc;

    if (!to_remote) {
        /* --- Download: Remote -> lokal --- */
        join_local(target, (int)sizeof(target), g_left.path(), e->name);
        sprintf(prompt, L("\"%.20s\" laden nach:", "Download \"%.20s\" to:"), e->name);
        if (!dlg_input(L("Download", "Download"), prompt, target, (int)sizeof(target) - 1, 0)) { redraw_all(); return; }
        if (target[0] == '\0') { redraw_all(); return; }

        if (local_exists(target)) {
            char q[120];
            sprintf(q, L("Lokale Datei existiert bereits:\n%.40s\nUeberschreiben?",
                         "Local file already exists:\n%.40s\nOverwrite?"), target);
            if (!dlg_confirm(L("Download", "Download"), q)) { redraw_all(); return; }
        }

        redraw_all();
        dlg_progress_begin(L("Download", "Download"), e->name);
        rc = g_ftp.retr(e->name, target, copy_progress, 0);
        dlg_progress_end();

        if (rc != FTP_OK) dlg_error(L("Download fehlgeschlagen", "Download failed"), g_ftp.last_error());
        g_left.refresh();
    } else {
        /* --- Upload: lokal -> Remote --- */
        char localpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(localpath, (int)sizeof(localpath), g_left.path(), e->name);
        strncpy(target, e->name, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
        sprintf(prompt, L("\"%.20s\" senden als:", "Upload \"%.20s\" as:"), e->name);
        if (!dlg_input(L("Upload", "Upload"), prompt, target, (int)sizeof(target) - 1, 0)) { redraw_all(); return; }
        if (target[0] == '\0') { redraw_all(); return; }

        if (g_right.has_entry(target)) {
            char q[120];
            sprintf(q, L("Remote-Datei existiert bereits:\n%.40s\nUeberschreiben?",
                         "Remote file already exists:\n%.40s\nOverwrite?"), target);
            if (!dlg_confirm(L("Upload", "Upload"), q)) { redraw_all(); return; }
        }

        redraw_all();
        dlg_progress_begin(L("Upload", "Upload"), e->name);
        rc = g_ftp.stor(localpath, target, copy_progress, 0);
        dlg_progress_end();

        if (rc != FTP_OK) dlg_error(L("Upload fehlgeschlagen", "Upload failed"), g_ftp.last_error());
        g_right.refresh();
    }
    redraw_all();
}

/* Einen Eintrag (Datei oder Verzeichnis) im Stapelbetrieb kopieren. Zielname =
 * Quellname im jeweils anderen Panel-Verzeichnis. Verzeichnisse rekursiv.
 * Bei existierender Zieldatei fragt copy_conflict; Rueckgabe FTP_OK,
 * FTP_ERR_ABORT (Benutzerabbruch) oder ein anderer FTP_ERR_* Code. */
static int copy_one_entry(int to_remote, PanelEntry *e, CopyCtx *cc)
{
    char localpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];

    join_local(localpath, (int)sizeof(localpath), g_left.path(), e->name);

    if (to_remote) {
        if (e->is_dir)
            return dircopy_upload(&g_ftp, localpath, e->name,
                                  copy_item, copy_progress, copy_conflict, cc);
        /* Einzelne Datei: existiert sie remote schon? */
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
        /* Einzelne Datei: existiert sie lokal schon? */
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
        dlg_error(L("Kopieren", "Copy"),
                  L("Keine FTP-Verbindung.\nMit F2 zuerst verbinden.",
                    "No FTP connection.\nConnect with F2 first."));
        redraw_all();
        return;
    }

    to_remote = (g_active == (Panel *)&g_left);   /* lokal aktiv -> Upload */
    nmarked   = g_active->marked_count();
    cur       = g_active->selected();

    /* --- Einzeldatei ohne Markierung: interaktiver Komfortpfad --- */
    if (nmarked == 0) {
        if (cur == 0 || cur->is_parent) { redraw_all(); return; }
        if (!cur->is_dir) { copy_single_file_interactive(to_remote, cur); return; }
    }

    /* --- Stapel-/Verzeichnis-Kopie --- */
    total   = (nmarked > 0) ? nmarked : 1;
    destdir = to_remote ? g_right.path() : g_left.path();

    sprintf(q, L("%d Eintrag/Eintraege kopieren nach:\n%.40s",
                 "Copy %d item(s) to:\n%.40s"), total, destdir);
    if (!dlg_confirm(L("Kopieren", "Copy"), q)) { redraw_all(); return; }

    cc.overwrite_all = 0;              /* je Vorgang frisch (kein "Alle" uebernommen) */

    redraw_all();
    dlg_progress_begin(L("Kopieren", "Copy"), "");

    rc = FTP_OK;
    if (nmarked > 0) {
        for (i = 0; i < g_active->entry_count(); i++) {
            PanelEntry *e = g_active->entry_at(i);
            if (!e || !e->marked || e->is_parent) continue;
            rc = copy_one_entry(to_remote, e, &cc);
            if (rc != FTP_OK) break;
        }
    } else {
        rc = copy_one_entry(to_remote, cur, &cc);   /* einzelnes Verzeichnis */
    }

    dlg_progress_end();

    if (rc != FTP_OK && rc != FTP_ERR_ABORT)
        dlg_error(L("Kopieren fehlgeschlagen", "Copy failed"), g_ftp.last_error());

    /* Markierungen aufheben und beide Seiten neu einlesen. */
    g_active->clear_marks();
    g_left.refresh();
    g_right.refresh();
    redraw_all();
    if (rc == FTP_ERR_ABORT)
        flash_status(L(" Kopieren abgebrochen.", " Copy aborted."));
}

/* -------------------------------------------------------------------------
 * F3 - Datei anzeigen (lokal direkt, remote ueber temporaeren Download)
 * ---------------------------------------------------------------------- */
static void do_view(void)
{
    char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
    PanelEntry *e;

    if (g_active == 0) return;
    e = g_active->selected();
    if (e == 0 || e->is_parent || e->is_dir) { redraw_all(); return; }

    if (g_active == (Panel *)&g_left) {
        /* Lokale Datei direkt anzeigen. */
        join_local(path, (int)sizeof(path), g_left.path(), e->name);
        view_file(path, e->name);
    } else {
        /* Remote: in temporaere lokale Datei laden, anzeigen, dann loeschen. */
        int rc;
        if (!g_ftp.is_connected()) {
            dlg_error(L("Anzeigen", "View"),
                      L("Keine FTP-Verbindung.\nMit F2 zuerst verbinden.",
                        "No FTP connection.\nConnect with F2 first."));
            redraw_all();
            return;
        }
        join_local(path, (int)sizeof(path), g_left.path(), "$NCVIEW$.TMP");
        redraw_all();
        dlg_progress_begin(L("Anzeigen", "View"), e->name);
        rc = g_ftp.retr(e->name, path, copy_progress, 0);
        dlg_progress_end();
        if (rc != FTP_OK) {
            remove(path);   /* evtl. angefangene Temp-Datei aufraeumen */
            dlg_error(L("Anzeigen fehlgeschlagen", "View failed"), g_ftp.last_error());
            redraw_all();
            return;
        }
        view_file(path, e->name);
        remove(path);
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F7 - Verzeichnis erstellen (lokal oder remote)
 * ---------------------------------------------------------------------- */
static void do_mkdir(void)
{
    char name[PANEL_NAME_MAX];
    int rc;

    if (g_active == 0) return;
    name[0] = '\0';
    if (!dlg_input(L("Verzeichnis erstellen", "Make Directory"),
                   L("Name:", "Name:"), name, PANEL_NAME_MAX - 1, 0)) {
        redraw_all(); return;
    }
    if (name[0] == '\0') { redraw_all(); return; }

    if (g_active == (Panel *)&g_right) {
        if (!g_ftp.is_connected()) {
            dlg_error(L("Verzeichnis erstellen", "Make Directory"),
                      L("Keine FTP-Verbindung.\nMit F2 zuerst verbinden.",
                        "No FTP connection.\nConnect with F2 first."));
            redraw_all(); return;
        }
        rc = g_ftp.make_dir(name);
        if (rc != FTP_OK)
            dlg_error(L("Verzeichnis erstellen", "Make Directory"), g_ftp.last_error());
        else
            g_right.refresh();
    } else {
        char path[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(path, (int)sizeof(path), g_left.path(), name);
        if (_mkdir(path) != 0)
            dlg_error(L("Verzeichnis erstellen", "Make Directory"),
                      L("Konnte Verzeichnis nicht anlegen.", "Could not create directory."));
        else
            g_left.refresh();
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F8 - Loeschen mit Bestaetigung (Datei oder leeres Verzeichnis)
 * ---------------------------------------------------------------------- */
/* Einen einzelnen Eintrag loeschen (Datei oder LEERES Verzeichnis).
 * Rueckgabe 0 = Erfolg, sonst Fehler (Fehlertext liegt dann in g_ftp bzw.
 * wird vom Aufrufer generisch gemeldet). on_remote != 0 => Remote-Seite. */
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

/* Einen Eintrag rekursiv loeschen (Datei oder ganzer Verzeichnisbaum).
 * Rueckgabe 0 = Erfolg, sonst Fehler. */
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

/* Zaehlt den Baum eines Eintrags zu *nf/*nd hinzu (Datei = 1 Datei). */
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
    int         on_remote, nmarked, i, errors;
    unsigned    nfiles = 0, ndirs = 0;

    if (g_active == 0) return;
    on_remote = (g_active == (Panel *)&g_right);

    if (on_remote && !g_ftp.is_connected()) {
        dlg_error(L("Loeschen", "Delete"),
                  L("Keine FTP-Verbindung.\nMit F2 zuerst verbinden.",
                    "No FTP connection.\nConnect with F2 first."));
        redraw_all(); return;
    }

    nmarked = g_active->marked_count();
    cur     = g_active->selected();

    /* --- Komfortpfad: einzelne Datei (kein Baum) --- */
    if (nmarked == 0) {
        if (cur == 0 || cur->is_parent) { redraw_all(); return; }
        if (!cur->is_dir) {
            sprintf(prompt, L("Datei \"%.32s\"\nloeschen?",
                              "Delete file\n\"%.32s\"?"), cur->name);
            if (!dlg_confirm(L("Loeschen", "Delete"), prompt)) { redraw_all(); return; }
            if (delete_one_entry(on_remote, cur) != 0) {
                if (on_remote)
                    dlg_error(L("Loeschen fehlgeschlagen", "Delete failed"), g_ftp.last_error());
                else
                    dlg_error(L("Loeschen fehlgeschlagen", "Delete failed"),
                              L("Datei konnte nicht\ngeloescht werden.",
                                "Could not delete\nthe file."));
            } else {
                if (on_remote) g_right.refresh(); else g_left.refresh();
            }
            redraw_all();
            return;
        }
    }

    /* --- Baum-/Stapel-Loeschung: erst zaehlen, dann warnen --- */
    flash_status(L(" Ermittle Anzahl ...", " Counting ..."));
    if (nmarked > 0) {
        for (i = 0; i < g_active->entry_count(); i++) {
            PanelEntry *e = g_active->entry_at(i);
            if (!e || !e->marked || e->is_parent) continue;
            count_one(on_remote, e, &nfiles, &ndirs);
        }
    } else {
        count_one(on_remote, cur, &nfiles, &ndirs);   /* einzelnes Verzeichnis */
    }

    sprintf(prompt, L("%u Datei(en) und %u Verzeichnis(se)\nunwiderruflich loeschen?",
                      "Permanently delete\n%u file(s) and %u director(y/ies)?"),
            nfiles, ndirs);
    if (!dlg_confirm(L("Loeschen", "Delete"), prompt)) { redraw_all(); return; }

    redraw_all();
    dlg_progress_begin(L("Loeschen", "Delete"), "");

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
    if (errors)
        dlg_error(L("Loeschen", "Delete"),
                  L("Einige Eintraege konnten nicht\nvollstaendig geloescht werden.",
                    "Some items could not be\nfully deleted."));
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F6 - Umbenennen (Datei oder Verzeichnis, lokal oder remote)
 * Reines Umbenennen im selben Verzeichnis (kein Verschieben zwischen Panels).
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
    sprintf(prompt, L("\"%.20s\" umbenennen in:", "Rename \"%.20s\" to:"), e->name);
    if (!dlg_input(L("Umbenennen", "Rename"), prompt, newname, PANEL_NAME_MAX - 1, 0)) { redraw_all(); return; }
    if (newname[0] == '\0')            { redraw_all(); return; }
    if (strcmp(newname, e->name) == 0) { redraw_all(); return; }   /* unveraendert */

    if (g_active == (Panel *)&g_right) {
        if (!g_ftp.is_connected()) {
            dlg_error(L("Umbenennen", "Rename"),
                      L("Keine FTP-Verbindung.\nMit F2 zuerst verbinden.",
                        "No FTP connection.\nConnect with F2 first."));
            redraw_all(); return;
        }
        rc = g_ftp.rename(e->name, newname);
        if (rc != FTP_OK)
            dlg_error(L("Umbenennen fehlgeschlagen", "Rename failed"), g_ftp.last_error());
        else
            g_right.refresh();
    } else {
        char oldpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        char newpath[PANEL_HEADER_MAX + PANEL_NAME_MAX + 4];
        join_local(oldpath, (int)sizeof(oldpath), g_left.path(), e->name);
        join_local(newpath, (int)sizeof(newpath), g_left.path(), newname);
        if (rename(oldpath, newpath) != 0)
            dlg_error(L("Umbenennen fehlgeschlagen", "Rename failed"),
                      L("Konnte nicht umbenennen.\nName ungueltig oder existiert bereits.",
                        "Could not rename.\nName invalid or already exists."));
        else
            g_left.refresh();
    }
    redraw_all();
}

/* -------------------------------------------------------------------------
 * F9 - Lokales Laufwerk wechseln
 * Nur fuer das lokale Panel sinnvoll (FTP-Seite hat keine Laufwerke). Es
 * werden ausschliesslich vorhandene Laufwerke angeboten.
 * ---------------------------------------------------------------------- */

/* 1, falls Laufwerk 'd' (1=A, 2=B, ...) existiert. INT 21h/AX=4409h
 * ("ist Block-Geraet remote?") liefert bei ungueltigem Laufwerk Carry. */
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
        dlg_error(L("Laufwerk", "Drive"),
                  L("Keine Laufwerke gefunden.", "No drives found."));
        redraw_all(); return;
    }

    cur = _getdrive();                 /* aktuelles Laufwerk (1=A) */
    initial = 0;
    { int i; for (i = 0; i < n; i++) if (letters[i] == cur) initial = i; }

    sel = dlg_menu(L("Laufwerk waehlen", "Select Drive"), items, n, initial);
    if (sel < 0) { redraw_all(); return; }

    newd = letters[sel];
    if (newd != cur) {
        char test[PANEL_HEADER_MAX];
        _chdrive(newd);
        /* getcwd schlaegt fehl, wenn das Laufwerk nicht bereit ist (leere
         * Diskette). Der Harderr-Handler verhindert die DOS-Abfrage. */
        if (getcwd(test, sizeof(test)) == 0) {
            _chdrive(cur);             /* zurueck auf das alte Laufwerk */
            dlg_error(L("Laufwerk", "Drive"),
                      L("Laufwerk nicht bereit.", "Drive not ready."));
            redraw_all(); return;
        }
        g_left.refresh();
    }
    set_active((Panel *)&g_left);      /* Laufwerkswechsel betrifft das lokale Panel */
    redraw_all();
}

/* Kurzhilfe (/?) auf stdout - laeuft vor tui_init, daher normale Ausgabe. */
static void print_usage(void)
{
    if (g_english) {
        printf("NCFTP386 - Norton Commander style FTP client for DOS\n\n");
        printf("Usage: NCFTP [EN] [-h HOST] [-p PORT] [-u USER] [-w PASS] [-n]\n\n");
        printf("  EN        force English user interface\n");
        printf("  -h HOST   connect to HOST automatically on startup\n");
        printf("  -p PORT   port (default 21)\n");
        printf("  -u USER   user name (default anonymous)\n");
        printf("  -w PASS   password  (WARNING: stored in cleartext in the batch file)\n");
        printf("  -n        do not save this connection to NCFTP.SAV\n");
        printf("  /?        this help\n");
    } else {
        printf("NCFTP386 - FTP-Client im Norton-Commander-Stil fuer DOS\n\n");
        printf("Aufruf: NCFTP [EN] [-h HOST] [-p PORT] [-u USER] [-w PASS] [-n]\n\n");
        printf("  EN        englische Oberflaeche erzwingen\n");
        printf("  -h HOST   beim Start automatisch mit HOST verbinden\n");
        printf("  -p PORT   Port (Vorgabe 21)\n");
        printf("  -u USER   Benutzername (Vorgabe anonymous)\n");
        printf("  -w PASS   Passwort  (ACHTUNG: steht im Klartext in der Batchdatei)\n");
        printf("  -n        diese Verbindung nicht in NCFTP.SAV speichern\n");
        printf("  /?        diese Hilfe\n");
    }
}

/* -------------------------------------------------------------------------
 * Main / Event-Loop
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int running = 1;
    int i;

    /* Sprache aus der DOS-Laendereinstellung bestimmen (vor jeder Ausgabe). */
    i18n_init();

    /* Gemerkte Verbindung laden (NCFTP.SAV neben der EXE). Fuellt g_host etc.;
     * fehlt die Datei, bleiben die Vorgaben stehen. */
    connsave_init(argv[0]);
    connsave_load(g_host, (int)sizeof(g_host), g_portStr, (int)sizeof(g_portStr),
                  g_user, (int)sizeof(g_user), g_pass, (int)sizeof(g_pass), &g_savepw);

    /* Kommandozeile parsen (ueberschreibt die geladenen Werte):
     *   EN  -> Englisch    -h/-p/-u/-w -> Verbindungsdaten + Auto-Connect (bei -h)
     *   -n  -> nicht speichern         /? -> Hilfe und beenden
     * Prefix '-' und '/' beide erlaubt; Wert folgt als naechstes Argument. */
    {
        int want_help = 0;
        for (i = 1; i < argc; i++) {
            const char *o = argv[i];
            char f;
            if (*o == '/' || *o == '-') o++;

            if ((o[0] == 'e' || o[0] == 'E') && (o[1] == 'n' || o[1] == 'N') && o[2] == '\0') {
                g_english = 1; continue;
            }
            if (o[0] == '?' && o[1] == '\0') { want_help = 1; continue; }

            f = (char)tolower((unsigned char)o[0]);
            if (o[1] == '\0' && (f == 'h' || f == 'p' || f == 'u' || f == 'w')) {
                const char *val = (i + 1 < argc) ? argv[++i] : "";
                if      (f == 'h') { strncpy(g_host,    val, sizeof(g_host)    - 1); g_host[sizeof(g_host) - 1]       = 0; g_autoconnect = 1; }
                else if (f == 'p') { strncpy(g_portStr, val, sizeof(g_portStr) - 1); g_portStr[sizeof(g_portStr) - 1] = 0; }
                else if (f == 'u') { strncpy(g_user,    val, sizeof(g_user)    - 1); g_user[sizeof(g_user) - 1]       = 0; }
                else               { strncpy(g_pass,    val, sizeof(g_pass)    - 1); g_pass[sizeof(g_pass) - 1]       = 0; }
                continue;
            }
            if (f == 'n' && o[1] == '\0') { g_nosave = 1; continue; }
        }
        if (want_help) { print_usage(); return 0; }
    }

    /* Kritische DOS-Fehler (leeres Laufwerk usw.) automatisch fehlschlagen
     * lassen, statt die TUI mit "Abort, Retry, Fail?" zu zerstoeren. */
    _harderr(ncftp_harderr);

    /* mTCP-Stack VOR tui_init starten: parseEnv/initStack koennen bei Fehler
     * auf stderr schreiben - tui_init loescht den Schirm anschliessend. Schlaegt
     * es fehl (z.B. MTCPCFG fehlt / kein Packet-Driver), laeuft das Programm
     * als reiner lokaler Dateimanager weiter; F2 meldet dann den Fehler. */
    g_ftp_ready = (FtpClient::init_stack() == FTP_OK) ? 1 : 0;
    g_right.attach(&g_ftp);

    tui_init();

    g_left.set_region(PANEL_TOP, 0,          PANEL_ROWS, PANEL_COLS);
    g_right.set_region(PANEL_TOP, PANEL_COLS, PANEL_ROWS, PANEL_COLS);
    g_left.refresh();
    g_right.refresh();
    set_active((Panel *)&g_left);

    redraw_all();

    /* Auto-Connect, wenn ein Host auf der Kommandozeile stand (-h).
     * Die allererste Verbindung direkt nach dem Start schlaegt auf echter
     * Hardware manchmal fehl: ARP-/DNS-Cache sind kalt und der Packet-Treiber
     * wurde gerade erst eingehaengt. Daher Stack kurz warmlaufen lassen und bei
     * einem transienten Fehler einmal automatisch wiederholen (entspricht dem
     * manuellen F2-Retry, der ja zuverlaessig klappt). Bei echten Fehlern
     * (z.B. Login abgelehnt) wird NICHT wiederholt. */
    if (g_autoconnect) {
        if (g_ftp_ready) {
            int attempt, rc = FTP_ERR_GENERAL;

            FtpClient::stack_poll(750);            /* Treiber/Link setteln lassen */
            for (attempt = 0; attempt < 2; attempt++) {
                rc = perform_connect();
                if (rc == FTP_OK) break;
                if (rc != FTP_ERR_TIMEOUT && rc != FTP_ERR_DNS &&
                    rc != FTP_ERR_CONNECT && rc != FTP_ERR_DATACONN) break;
                if (attempt == 0) {
                    flash_status(L(" Erneuter Verbindungsversuch ...",
                                   " Retrying connection ..."));
                    FtpClient::stack_poll(500);    /* ARP/DNS sind jetzt warm */
                }
            }
            if (rc != FTP_OK)
                dlg_error(L("Verbindung fehlgeschlagen", "Connection failed"), g_ftp.last_error());
            redraw_all();
        } else {
            flash_status(L(" FTP nicht verfuegbar (MTCPCFG?).",
                           " FTP unavailable (MTCPCFG?)."));
        }
    }

    while (running) {
        int key = readkey();

        switch (key) {
        case KEY_TAB:
            set_active(g_active == (Panel *)&g_left
                       ? (Panel *)&g_right : (Panel *)&g_left);
            g_left.draw();
            g_right.draw();
            draw_statusbar();
            break;

        case KEY_UP:   g_active->move_step(-1); draw_statusbar(); break;
        case KEY_DOWN: g_active->move_step(+1); draw_statusbar(); break;
        case KEY_PGUP: g_active->page_up();   g_active->draw(); draw_statusbar(); break;
        case KEY_PGDN: g_active->page_down(); g_active->draw(); draw_statusbar(); break;
        case KEY_HOME: g_active->move_home(); g_active->draw(); draw_statusbar(); break;
        case KEY_END:  g_active->move_end();  g_active->draw(); draw_statusbar(); break;

        case KEY_INS:  /* Markierung umschalten + Cursor nach unten (Norton-Stil) */
            g_active->toggle_mark(); draw_statusbar(); break;

        case KEY_ENTER:
            if (g_active->enter_selected()) {
                g_active->draw();
                draw_statusbar();
                if (g_active == (Panel *)&g_right && g_right.nav_failed())
                    flash_status(g_right.last_error());
            }
            /* sonst: Datei -> Anzeigen (wie F3). */
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

        /* Funktionstasten. */
        case KEY_F1:
            dlg_message(L("Hilfe", "Help"),
                L("Tab        Panel wechseln\n"
                  "Pfeile     Auswahl bewegen\n"
                  "Einfg      Eintrag markieren (mehrere kopieren/loeschen)\n"
                  "Enter      Verzeichnis betreten / Datei anzeigen\n"
                  "Backspace  Uebergeordnetes Verzeichnis\n"
                  "F2 Verbinden  F3 Anzeigen  F5 Kopieren (rekursiv)\n"
                  "F6 Umbenennen  F7 MkDir  F8 Loeschen  F9 Laufwerk\n"
                  "F10        Beenden",
                  "Tab        Switch panel\n"
                  "Arrows     Move selection\n"
                  "Insert     Mark item (copy/delete several)\n"
                  "Enter      Enter directory / view file\n"
                  "Backspace  Parent directory\n"
                  "F2 Connect  F3 View  F5 Copy (recursive)\n"
                  "F6 Rename  F7 MkDir  F8 Delete  F9 Drive\n"
                  "F10        Quit"), 0);
            break;
        case KEY_F2:  do_connect(); break;
        case KEY_F3:  do_view(); break;
        case KEY_F4:  flash_status(L(" F4  Bearbeiten - folgt spaeter", " F4  Edit - coming later")); break;
        case KEY_F5:  do_copy(); break;
        case KEY_F6:  do_rename(); break;
        case KEY_F7:  do_mkdir(); break;
        case KEY_F8:  do_delete(); break;
        case KEY_F9:  do_drives(); break;

        case KEY_F10:
            if (dlg_confirm(L("Beenden", "Quit"),
                            L("NCFTP386 wirklich beenden?", "Really quit NCFTP386?")))
                running = 0;
            break;

        default:
            break;
        }
    }

    /* Sauber trennen und mTCP-Stack zurueckgeben. */
    if (g_ftp.is_connected())
        g_ftp.disconnect();
    if (g_ftp_ready)
        FtpClient::shutdown_stack();

    tui_shutdown();
    return 0;
}
