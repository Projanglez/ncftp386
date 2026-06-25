/* =============================================================================
 * sites.cpp - Site manager persistence (FTP4DOS.SIT)
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * File format (text, editable), one [site] block per profile:
 *     # FTP4DOS site manager
 *     [site]
 *     name=Home NAS
 *     host=ftp.lan
 *     port=21
 *     user=admin
 *     savepw=1
 *     pass=4A1F...        <- XOR(key)+hex; empty when savepw=0
 *     dir=/pub
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>    /* _splitpath, _makepath, atoi, _MAX_*           */
#include <string.h>    /* strncpy, strchr, strpbrk, stricmp             */
#include <direct.h>    /* getcwd                                        */

#include "sites.h"
#include "connsave.h"  /* connsave_obfus / connsave_deobfus             */

static char g_path[160] = "";   /* full path of the sites file */

/* ------------------------------------------------------------------ */
/* Determine the path (same scheme as connsave_init)                   */
/* ------------------------------------------------------------------ */
void sites_init(const char *argv0)
{
    char drive[_MAX_DRIVE], dir[_MAX_DIR];

    if (argv0 && argv0[0]) _splitpath(argv0, drive, dir, 0, 0);
    else                   { drive[0] = 0; dir[0] = 0; }

    if (drive[0] || dir[0]) {
        _makepath(g_path, drive, dir, "FTP4DOS", "SIT");
        return;
    }

    {
        char cwd[128];
        int  n;
        if (getcwd(cwd, sizeof(cwd)) == 0) strcpy(cwd, ".");
        n = (int)strlen(cwd);
        if (n > 0 && (cwd[n - 1] == '\\' || cwd[n - 1] == '/'))
            sprintf(g_path, "%sFTP4DOS.SIT", cwd);
        else
            sprintf(g_path, "%s\\FTP4DOS.SIT", cwd);
    }
}

/* ------------------------------------------------------------------ */
/* Copy a value into a fixed buffer, NUL-terminated                    */
/* ------------------------------------------------------------------ */
static void set_field(char *dst, int dstsz, const char *val)
{
    strncpy(dst, val, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* Load                                                                 */
/* ------------------------------------------------------------------ */
int sites_load(Site *arr, int maxn)
{
    FILE *f;
    char  line[256];
    int   count = 0;
    int   have  = 0;   /* 1 once we are inside a [site] block */

    if (g_path[0] == '\0' || maxn <= 0) return 0;
    f = fopen(g_path, "r");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        char *nl, *eq, *key, *val;

        nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        if (line[0] == '[') {
            /* Start a new record (the marker is "[site]"). */
            if (count >= maxn) break;
            have = 1;
            memset(&arr[count], 0, sizeof(Site));
            set_field(arr[count].port, SITE_PORT_MAX, "21");
            count++;
            continue;
        }

        if (!have) continue;          /* keys before the first [site] are ignored */

        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = line;
        val = eq + 1;

        {
            Site *s = &arr[count - 1];
            if      (stricmp(key, "name") == 0) set_field(s->name, SITE_NAME_MAX, val);
            else if (stricmp(key, "host") == 0) set_field(s->host, SITE_HOST_MAX, val);
            else if (stricmp(key, "port") == 0) set_field(s->port, SITE_PORT_MAX, val);
            else if (stricmp(key, "user") == 0) set_field(s->user, SITE_USER_MAX, val);
            else if (stricmp(key, "dir")  == 0) set_field(s->dir,  SITE_DIR_MAX,  val);
            else if (stricmp(key, "savepw") == 0) s->savepw = atoi(val) ? 1 : 0;
            else if (stricmp(key, "pass") == 0) { if (val[0]) connsave_deobfus(val, s->pass, SITE_PASS_MAX); }
        }
    }

    fclose(f);
    return count;
}

/* ------------------------------------------------------------------ */
/* Save                                                                 */
/* ------------------------------------------------------------------ */
int sites_store(const Site *arr, int n)
{
    FILE *f;
    int   i;

    if (g_path[0] == '\0') return 0;
    f = fopen(g_path, "w");
    if (!f) return 0;

    fprintf(f, "# FTP4DOS site manager\n");

    for (i = 0; i < n; i++) {
        const Site *s = &arr[i];
        fprintf(f, "\n[site]\n");
        fprintf(f, "name=%s\n", s->name);
        fprintf(f, "host=%s\n", s->host);
        fprintf(f, "port=%s\n", s->port[0] ? s->port : "21");
        fprintf(f, "user=%s\n", s->user);
        fprintf(f, "savepw=%d\n", s->savepw ? 1 : 0);
        if (s->savepw && s->pass[0]) {
            char hex[2 * SITE_PASS_MAX + 1];
            connsave_obfus(s->pass, hex);
            fprintf(f, "pass=%s\n", hex);
        } else {
            fprintf(f, "pass=\n");
        }
        fprintf(f, "dir=%s\n", s->dir);
    }

    fclose(f);
    return 1;
}
