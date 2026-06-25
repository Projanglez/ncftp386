/* =============================================================================
 * rpanel.cpp - FTP remote panel
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * LIST parsing:
 *   Unix:  "drwxr-xr-x  2 user group  4096 Jan  1 12:00 name"
 *          anchor = month name; before it the size, after it day + time/year + name.
 *   DOS:   "12-25-2023  09:30AM       <DIR>          name"
 *          or "... 09:30AM   1234 file.txt"
 * ===========================================================================*/
#include <string.h>   /* strchr, strncpy, strnicmp, stricmp, memcpy   */
#include <stdlib.h>   /* strtoul, atoi, malloc                        */
#include <ctype.h>    /* isdigit, toupper                             */
#include <dos.h>      /* _dos_getdate, struct dosdate_t               */
#include <stdio.h>    /* sscanf, sprintf                              */

#include "rpanel.h"
#include "i18n.h"

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *MONTHS[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* 3-character token -> month 1..12, otherwise 0. */
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

/* Classify a token: 1 = "HH:MM" (time), 2 = "YYYY" (year), 0 = neither. */
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

/* Copy a name: up to PANEL_NAME_MAX, trims trailing CR/LF/space. */
/* Copy up to 'cap' characters of a name, trimming trailing whitespace. */
static void copy_name_cap(char *dst, const char *src, int cap)
{
    int n = 0;
    while (src[n] && n < cap) { dst[n] = src[n]; n++; }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\r' || dst[n - 1] == '\n' || dst[n - 1] == '\t'))
        n--;
    dst[n] = '\0';
}

static void copy_name(char *dst, const char *src)
{
    copy_name_cap(dst, src, PANEL_NAME_MAX - 1);
}

/* Copy a token into a small buffer (NUL-terminated). */
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

/* Last path component of a remote path ("/pub/games" -> "games"). */
static void path_leaf(const char *path, char *out, int outsz)
{
    const char *p, *leaf = path;
    int n = 0;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    while (leaf[n] && n < outsz - 1) { out[n] = leaf[n]; n++; }
    out[n] = '\0';
}

/* Split a line into tokens (offsets/lengths). Returns: number of tokens. */
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
/* Constructor                                                         */
/* ------------------------------------------------------------------ */
RemotePanel::RemotePanel()
{
    ftp = 0;
    cwd[0] = '\0';
    navFailed = 0;
    curYear = 1980;
    namePool = 0;
    poolUsed = 0;
    poolSize = 0;
}

/* Size of the per-listing full-name pool. 512 entries average well under this;
 * if a (pathological) listing overflows it, the extra names simply fall back to
 * their truncated form - no crash, same as the pre-pool behavior. */
#define REMOTE_NAME_POOL 32768U

char *RemotePanel::pool_store(const char *s)
{
    unsigned len = (unsigned)strlen(s) + 1;
    if (!namePool || poolUsed + len > poolSize) return 0;
    char *dst = namePool + poolUsed;
    memcpy(dst, s, len);
    poolUsed += len;
    return dst;
}

/* ------------------------------------------------------------------ */
/* Unix "ls -l" format                                                 */
/* ------------------------------------------------------------------ */
static int parse_unix(const char *line, int curYear, PanelEntry *e,
                      char *full, int fullcap)
{
    char c0 = line[0];
    if (!strchr("-dlbcps", c0)) return 0;       /* not a permission block */

    int isDir  = (c0 == 'd');
    int isLink = (c0 == 'l');

    int off[16], tlen[16];
    int nt = tokenize(line, off, tlen, 16);
    if (nt < 4) return 0;

    /* Find the month anchor: tok[k]=month, tok[k-1]=size(number),
     * tok[k+1]=day(number), tok[k+2]=time/year, tok[k+3..]=name. */
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

    /* Size */
    unsigned long size = 0;
    if (!isDir) {
        copy_tok(buf, line, off[m - 1], tlen[m - 1], 23);
        size = strtoul(buf, 0, 10);
    }

    /* Date: month / day / (time|year) */
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

    /* Name = from token m+3 onward (rest of the line, may contain spaces). */
    const char *nm = line + off[m + 3];
    copy_name(e->name, nm);
    if (full && fullcap > 0) copy_name_cap(full, nm, fullcap - 1);

    /* Symlink: "name -> target" -> keep only the name. */
    if (isLink) {
        char *arrow = strstr(e->name, " -> ");
        if (arrow) *arrow = '\0';
        if (full && fullcap > 0) {
            arrow = strstr(full, " -> ");
            if (arrow) *arrow = '\0';
        }
    }

    e->size      = size;
    e->date      = make_date(year, month, day);
    e->time      = make_time(hh, mm);
    e->is_dir    = (unsigned char)(isDir ? 1 : 0);
    e->is_parent = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* MS-DOS / IIS format                                                 */
/* ------------------------------------------------------------------ */
static int parse_dos(const char *line, PanelEntry *e, char *full, int fullcap)
{
    if (!isdigit((unsigned char)line[0])) return 0;
    if (line[2] != '-' && line[2] != '/') return 0;       /* date MM-DD-.. */

    int off[6], tlen[6];
    int nt = tokenize(line, off, tlen, 6);
    if (nt < 4) return 0;                                 /* date time DIR/size name */

    char d0[16], t1[16], sz[24];
    copy_tok(d0, line, off[0], tlen[0], 15);
    copy_tok(t1, line, off[1], tlen[1], 15);
    copy_tok(sz, line, off[2], tlen[2], 23);

    /* Date MM-DD-YY or MM/DD/YYYY */
    int mo = 0, da = 0, yr = 0;
    if (sscanf(d0, "%d%*c%d%*c%d", &mo, &da, &yr) != 3) return 0;
    if (yr < 100) yr += (yr < 70) ? 2000 : 1900;

    /* Time HH:MM with optional AM/PM */
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
    if (full && fullcap > 0) copy_name_cap(full, line + off[3], fullcap - 1);
    e->size      = size;
    e->date      = make_date(yr, mo, da);
    e->time      = make_time(hh, mm);
    e->is_dir    = (unsigned char)(isDir ? 1 : 0);
    e->is_parent = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public parser (Unix or DOS format)                                  */
/* ------------------------------------------------------------------ */
int ftp_parse_list_line(const char *line, int curYear, PanelEntry *e,
                        char *full, int fullcap)
{
    e->marked   = 0;
    e->fullname = 0;
    if (full && fullcap > 0) full[0] = '\0';
    if (parse_unix(line, curYear, e, full, fullcap)) return 1;
    if (parse_dos(line, e, full, fullcap))           return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* LIST callback                                                       */
/* ------------------------------------------------------------------ */
void RemotePanel::on_line(void *ctx, const char *line)
{
    ((RemotePanel *)ctx)->add_line(line);
}

void RemotePanel::add_line(const char *line)
{
    PanelEntry tmp;
    char full[FTP_LINE_MAX];
    if (!ftp_parse_list_line(line, curYear, &tmp, full, (int)sizeof(full)))
        return;                                                     /* not parseable */

    /* Discard "." and ".." from the listing (we add ".." ourselves). */
    if (tmp.name[0] == '\0') return;
    if (tmp.name[0] == '.' && tmp.name[1] == '\0') return;
    if (tmp.name[0] == '.' && tmp.name[1] == '.' && tmp.name[2] == '\0') return;

    /* Count every real entry; store only while the buffer has room. */
    total++;

    /* Keep the full name only when it was actually truncated into tmp.name;
     * otherwise tmp.name already is the complete name and fullname stays 0. */
    if (strcmp(full, tmp.name) != 0)
        tmp.fullname = pool_store(full);

    if (store->append(&tmp)) count++;
    else                     truncated = 1;
}

/* ------------------------------------------------------------------ */
/* refresh: list the current remote directory                         */
/* ------------------------------------------------------------------ */
int RemotePanel::refresh()
{
    count = 0;
    total = 0;
    truncated = 0;
    cursor = 0;
    topentry = 0;
    navFailed = 0;
    store->reset();

    if (!ftp || !ftp->is_connected()) {
        strcpy(header, L("(not connected)", "(nicht verbunden)"));
        cwd[0] = '\0';
        return 0;
    }

    /* Full-name pool for entries whose name exceeds PANEL_NAME_MAX. */
    if (namePool == 0) {
        namePool = (char *)malloc(REMOTE_NAME_POOL);
        poolSize = namePool ? REMOTE_NAME_POOL : 0;
    }
    poolUsed = 0;

    /* Determine the current path (for the header). */
    if (ftp->get_cwd(cwd, PANEL_HEADER_MAX) != FTP_OK) {
        strcpy(cwd, "/");
    }
    sprintf(header, "%.30s:%.46s", ftp->host_name(), cwd);

    curYear = current_year();

    /* Offer a ".." entry unless we are already at the root directory. */
    {
        int at_root = (cwd[0] == '\0') || (cwd[0] == '/' && cwd[1] == '\0');
        if (!at_root) {
            PanelEntry e;
            strcpy(e.name, "..");
            e.size = 0; e.date = 0; e.time = 0;
            e.is_dir = 1; e.is_parent = 1; e.marked = 0;
            e.fullname = 0;
            if (store->append(&e)) count++;
        }
    }

    int rc = ftp->list(0, on_line, this);
    if (rc != FTP_OK) navFailed = 1;

    sort_entries();
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
        /* Going up: afterwards put the cursor on the directory we left. */
        char leaf[PANEL_NAME_MAX];
        path_leaf(cwd, leaf, sizeof(leaf));
        rc = ftp->parent_dir();
        if (rc == FTP_OK) { refresh(); select_by_name(leaf); }
    } else {
        /* Use the full name (entry_name), not the truncated display name, so
         * directories with names longer than PANEL_NAME_MAX still enter. Only
         * re-list on success - a failed CWD must not yank the cursor to top. */
        rc = ftp->change_dir(entry_name(e));
        if (rc == FTP_OK) refresh();
    }
    if (rc != FTP_OK) navFailed = 1;
    return 1;
}

void RemotePanel::go_parent()
{
    char leaf[PANEL_NAME_MAX];
    if (!ftp || !ftp->is_connected()) return;
    path_leaf(cwd, leaf, sizeof(leaf));
    int rc = ftp->parent_dir();
    if (rc == FTP_OK) { refresh(); select_by_name(leaf); }
    else              navFailed = 1;
}
