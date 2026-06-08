/* =============================================================================
 * rpanel.cpp - FTP-Remote-Panel
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * LIST-Parsing:
 *   Unix:  "drwxr-xr-x  2 user group  4096 Jan  1 12:00 name"
 *          Anker = Monatsname; davor die Groesse, danach Tag + Zeit/Jahr + Name.
 *   DOS:   "12-25-2023  09:30AM       <DIR>          name"
 *          bzw. "... 09:30AM   1234 datei.txt"
 * ===========================================================================*/
#include <string.h>   /* strchr, strncpy, strnicmp, stricmp, memcpy   */
#include <stdlib.h>   /* qsort, strtoul, atoi                         */
#include <ctype.h>    /* isdigit, toupper                             */
#include <dos.h>      /* _dos_getdate, struct dosdate_t               */
#include <stdio.h>    /* sscanf, sprintf                              */

#include "rpanel.h"
#include "i18n.h"

/* ------------------------------------------------------------------ */
/* Kleine Helfer                                                       */
/* ------------------------------------------------------------------ */

static const char *MONTHS[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* 3-Zeichen-Token -> Monat 1..12, sonst 0. */
static int month_num(const char *tok, int len)
{
    if (len != 3) return 0;
    for (int i = 0; i < 12; i++)
        if (strnicmp(tok, MONTHS[i], 3) == 0) return i + 1;
    return 0;
}

static int all_digits(const char *s, int len)
{
    if (len <= 0) return 0;
    for (int i = 0; i < len; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* Token klassifizieren: 1 = "HH:MM" (Zeit), 2 = "JJJJ" (Jahr), 0 = keins. */
static int time_or_year(const char *s, int len)
{
    int i, hasColon = 0;
    if (len == 4 && all_digits(s, 4)) return 2;
    for (i = 0; i < len; i++) if (s[i] == ':') hasColon = 1;
    if (hasColon) return 1;
    return 0;
}

static unsigned make_date(int year, int month, int day)
{
    if (year  < 1980) year  = 1980;
    if (year  > 2107) year  = 2107;
    if (month < 1)    month = 1;
    if (month > 12)   month = 12;
    if (day   < 1)    day   = 1;
    if (day   > 31)   day   = 31;
    return (unsigned)((((year - 1980) & 0x7F) << 9) | ((month & 0x0F) << 5) | (day & 0x1F));
}

static unsigned make_time(int hh, int mm)
{
    if (hh < 0) hh = 0; if (hh > 23) hh = 23;
    if (mm < 0) mm = 0; if (mm > 59) mm = 59;
    return (unsigned)(((hh & 0x1F) << 11) | ((mm & 0x3F) << 5));
}

/* Name kopieren: fuehrt bis PANEL_NAME_MAX, schneidet trailing CR/LF/Space ab. */
static void copy_name(char *dst, const char *src)
{
    int n = 0;
    while (src[n] && n < PANEL_NAME_MAX - 1) { dst[n] = src[n]; n++; }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\r' || dst[n - 1] == '\n' || dst[n - 1] == '\t'))
        n--;
    dst[n] = '\0';
}

/* Token in einen kleinen Puffer kopieren (null-terminiert). */
static void copy_tok(char *dst, const char *line, int off, int len, int cap)
{
    if (len > cap) len = cap;
    memcpy(dst, line + off, len);
    dst[len] = '\0';
}

static int current_year(void)
{
    struct dosdate_t d;
    _dos_getdate(&d);
    return (int)d.year;
}

/* Letzter Pfadbestandteil eines Remote-Pfads ("/pub/games" -> "games"). */
static void path_leaf(const char *path, char *out, int outsz)
{
    const char *p, *leaf = path;
    int n = 0;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    while (leaf[n] && n < outsz - 1) { out[n] = leaf[n]; n++; }
    out[n] = '\0';
}

/* Zeile in Tokens zerlegen (Offsets/Laengen). Rueckgabe: Anzahl Tokens. */
static int tokenize(const char *line, int *off, int *tlen, int maxtok)
{
    int i = 0, nt = 0;
    while (line[i] && nt < maxtok) {
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (!line[i]) break;
        off[nt] = i;
        int s = i;
        while (line[i] && line[i] != ' ' && line[i] != '\t') i++;
        tlen[nt] = i - s;
        nt++;
    }
    return nt;
}

/* ------------------------------------------------------------------ */
/* Konstruktor                                                         */
/* ------------------------------------------------------------------ */
RemotePanel::RemotePanel()
{
    ftp = 0;
    cwd[0] = '\0';
    navFailed = 0;
    curYear = 1980;
}

/* ------------------------------------------------------------------ */
/* Sortierung: ".." zuerst, dann Verzeichnisse, dann Dateien (alphab.) */
/* ------------------------------------------------------------------ */
int RemotePanel::compare(const void *a, const void *b)
{
    const PanelEntry *ea = (const PanelEntry *)a;
    const PanelEntry *eb = (const PanelEntry *)b;

    if (ea->is_parent != eb->is_parent) return ea->is_parent ? -1 : 1;
    if (ea->is_dir    != eb->is_dir)    return ea->is_dir    ? -1 : 1;
    return stricmp(ea->name, eb->name);
}

/* ------------------------------------------------------------------ */
/* Unix-"ls -l"-Format                                                 */
/* ------------------------------------------------------------------ */
static int parse_unix(const char *line, int curYear, PanelEntry *e)
{
    char c0 = line[0];
    if (!strchr("-dlbcps", c0)) return 0;       /* kein Permission-Block */

    int isDir  = (c0 == 'd');
    int isLink = (c0 == 'l');

    int off[16], tlen[16];
    int nt = tokenize(line, off, tlen, 16);
    if (nt < 4) return 0;

    /* Monats-Anker suchen: tok[k]=Monat, tok[k-1]=Groesse(Zahl),
     * tok[k+1]=Tag(Zahl), tok[k+2]=Zeit/Jahr, tok[k+3..]=Name. */
    int k, m = -1, tyType = 0;
    for (k = 1; k + 3 < nt; k++) {
        if (!month_num(line + off[k], tlen[k])) continue;
        if (!all_digits(line + off[k - 1], tlen[k - 1])) continue;
        if (!all_digits(line + off[k + 1], tlen[k + 1])) continue;
        tyType = time_or_year(line + off[k + 2], tlen[k + 2]);
        if (!tyType) continue;
        m = k; break;
    }
    if (m < 0) return 0;

    char buf[24];

    /* Groesse */
    unsigned long size = 0;
    if (!isDir) {
        copy_tok(buf, line, off[m - 1], tlen[m - 1], 23);
        size = strtoul(buf, 0, 10);
    }

    /* Datum: Monat / Tag / (Zeit|Jahr) */
    int month = month_num(line + off[m], tlen[m]);
    copy_tok(buf, line, off[m + 1], tlen[m + 1], 23);
    int day = atoi(buf);

    int year = curYear, hh = 0, mm = 0;
    copy_tok(buf, line, off[m + 2], tlen[m + 2], 23);
    if (tyType == 2) {
        year = atoi(buf);
    } else {
        sscanf(buf, "%d:%d", &hh, &mm);
    }

    /* Name = ab Token m+3 (Rest der Zeile, kann Leerzeichen enthalten). */
    const char *nm = line + off[m + 3];
    copy_name(e->name, nm);

    /* Symlink: "name -> ziel" -> nur den Namen behalten. */
    if (isLink) {
        char *arrow = strstr(e->name, " -> ");
        if (arrow) *arrow = '\0';
    }

    e->size      = size;
    e->date      = make_date(year, month, day);
    e->time      = make_time(hh, mm);
    e->is_dir    = (unsigned char)(isDir ? 1 : 0);
    e->is_parent = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* MS-DOS / IIS-Format                                                 */
/* ------------------------------------------------------------------ */
static int parse_dos(const char *line, PanelEntry *e)
{
    if (!isdigit((unsigned char)line[0])) return 0;
    if (line[2] != '-' && line[2] != '/') return 0;       /* Datum MM-TT-.. */

    int off[6], tlen[6];
    int nt = tokenize(line, off, tlen, 6);
    if (nt < 4) return 0;                                 /* Datum Zeit DIR/Size Name */

    char d0[16], t1[16], sz[24];
    copy_tok(d0, line, off[0], tlen[0], 15);
    copy_tok(t1, line, off[1], tlen[1], 15);
    copy_tok(sz, line, off[2], tlen[2], 23);

    /* Datum MM-TT-JJ oder MM/TT/JJJJ */
    int mo = 0, da = 0, yr = 0;
    if (sscanf(d0, "%d%*c%d%*c%d", &mo, &da, &yr) != 3) return 0;
    if (yr < 100) yr += (yr < 70) ? 2000 : 1900;

    /* Zeit HH:MM mit optionalem AM/PM */
    int hh = 0, mm = 0;
    sscanf(t1, "%d:%d", &hh, &mm);
    {
        int L = (int)strlen(t1);
        if (L >= 2) {
            char x = (char)toupper((unsigned char)t1[L - 2]);
            char y = (char)toupper((unsigned char)t1[L - 1]);
            if (y == 'M' && x == 'P' && hh < 12) hh += 12;
            if (y == 'M' && x == 'A' && hh == 12) hh = 0;
        }
    }

    int isDir = (stricmp(sz, "<DIR>") == 0);
    unsigned long size = isDir ? 0UL : strtoul(sz, 0, 10);

    copy_name(e->name, line + off[3]);
    e->size      = size;
    e->date      = make_date(yr, mo, da);
    e->time      = make_time(hh, mm);
    e->is_dir    = (unsigned char)(isDir ? 1 : 0);
    e->is_parent = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Oeffentlicher Parser (Unix- oder DOS-Format)                        */
/* ------------------------------------------------------------------ */
int ftp_parse_list_line(const char *line, int curYear, PanelEntry *e)
{
    e->marked = 0;
    if (parse_unix(line, curYear, e)) return 1;
    if (parse_dos(line, e))           return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* LIST-Callback                                                       */
/* ------------------------------------------------------------------ */
void RemotePanel::on_line(void *ctx, const char *line)
{
    ((RemotePanel *)ctx)->add_line(line);
}

void RemotePanel::add_line(const char *line)
{
    if (count >= PANEL_MAX_ENTRIES) return;

    PanelEntry tmp;
    if (!ftp_parse_list_line(line, curYear, &tmp)) return;          /* nicht lesbar */

    /* "." und ".." aus dem Listing verwerfen (".." fuegen wir selbst hinzu). */
    if (tmp.name[0] == '\0') return;
    if (tmp.name[0] == '.' && tmp.name[1] == '\0') return;
    if (tmp.name[0] == '.' && tmp.name[1] == '.' && tmp.name[2] == '\0') return;

    entries[count++] = tmp;
}

/* ------------------------------------------------------------------ */
/* refresh: aktuelles Remote-Verzeichnis listen                        */
/* ------------------------------------------------------------------ */
int RemotePanel::refresh()
{
    count = 0;
    cursor = 0;
    topentry = 0;
    navFailed = 0;

    if (!ftp || !ftp->is_connected()) {
        strcpy(header, L("(nicht verbunden)", "(not connected)"));
        cwd[0] = '\0';
        return 0;
    }

    /* Aktuellen Pfad ermitteln (fuer Header). */
    if (ftp->get_cwd(cwd, PANEL_HEADER_MAX) != FTP_OK) {
        strcpy(cwd, "/");
    }
    sprintf(header, "%.30s:%.46s", ftp->host_name(), cwd);

    curYear = current_year();

    /* ".."-Eintrag immer anbieten. */
    {
        PanelEntry *e = &entries[count++];
        strcpy(e->name, "..");
        e->size = 0; e->date = 0; e->time = 0;
        e->is_dir = 1; e->is_parent = 1; e->marked = 0;
    }

    int rc = ftp->list(0, on_line, this);
    if (rc != FTP_OK) navFailed = 1;

    qsort(entries, count, sizeof(PanelEntry), compare);
    cursor = 0;
    topentry = 0;
    return count;
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */
int RemotePanel::enter_selected()
{
    PanelEntry *e = selected();
    if (e == 0)     return 0;
    if (!e->is_dir) return 0;
    if (!ftp || !ftp->is_connected()) return 0;

    int rc;
    if (e->is_parent) {
        /* Hochwechseln: danach Cursor auf das verlassene Verzeichnis. */
        char leaf[PANEL_NAME_MAX];
        path_leaf(cwd, leaf, sizeof(leaf));
        rc = ftp->parent_dir();
        refresh();
        select_by_name(leaf);
    } else {
        rc = ftp->change_dir(e->name);
        refresh();                   /* listet den neuen Stand */
    }
    if (rc != FTP_OK) navFailed = 1; /* refresh() hat navFailed zurueckgesetzt */
    return 1;
}

void RemotePanel::go_parent()
{
    char leaf[PANEL_NAME_MAX];
    if (!ftp || !ftp->is_connected()) return;
    path_leaf(cwd, leaf, sizeof(leaf));
    int rc = ftp->parent_dir();
    refresh();
    select_by_name(leaf);
    if (rc != FTP_OK) navFailed = 1;
}
