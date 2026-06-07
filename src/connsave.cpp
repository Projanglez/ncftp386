/* =============================================================================
 * connsave.cpp - Letzte FTP-Verbindung persistent merken
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * Dateiformat (Text, editierbar), NCFTP.SAV neben der EXE:
 *     # NCFTP386 gespeicherte Verbindung / saved connection
 *     host=ftp.example.org
 *     port=21
 *     user=anonymous
 *     savepw=1
 *     pass=4A1F...        <- XOR(Schluessel) + Hex; leer wenn savepw=0
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>    /* _splitpath, _makepath, atoi, _MAX_*           */
#include <string.h>    /* strncpy, strchr, strpbrk, stricmp             */
#include <direct.h>    /* getcwd                                        */

#include "connsave.h"

/* Fester Schluessel fuer die XOR-Verschleierung. KEINE echte Verschluesselung -
 * nur damit das Passwort nicht im Klartext in der Datei steht. */
static const char XKEY[] = "NCFTP386";

static char g_path[160] = "";   /* voller Pfad der Speicherdatei */

/* ------------------------------------------------------------------ */
/* Pfad festlegen                                                      */
/* ------------------------------------------------------------------ */
void connsave_init(const char *argv0)
{
    char drive[_MAX_DRIVE], dir[_MAX_DIR];

    if (argv0 && argv0[0]) _splitpath(argv0, drive, dir, 0, 0);
    else                   { drive[0] = 0; dir[0] = 0; }

    if (drive[0] || dir[0]) {
        /* Verzeichnis der EXE; _splitpath liefert dir inkl. abschliessendem '\'. */
        _makepath(g_path, drive, dir, "NCFTP", "SAV");
        return;
    }

    /* Fallback: aktuelles (Start-)Verzeichnis. */
    {
        char cwd[128];
        int  n;
        if (getcwd(cwd, sizeof(cwd)) == 0) strcpy(cwd, ".");
        n = (int)strlen(cwd);
        if (n > 0 && (cwd[n - 1] == '\\' || cwd[n - 1] == '/'))
            sprintf(g_path, "%sNCFTP.SAV", cwd);
        else
            sprintf(g_path, "%s\\NCFTP.SAV", cwd);
    }
}

/* ------------------------------------------------------------------ */
/* XOR + Hex                                                           */
/* ------------------------------------------------------------------ */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* src -> hex(src[i] ^ XKEY[i%klen]). dst muss >= 2*strlen(src)+1 sein. */
static void obfus_hex(const char *src, char *dst)
{
    int i, klen = (int)sizeof(XKEY) - 1;
    for (i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i] ^ (unsigned char)XKEY[i % klen];
        sprintf(dst + i * 2, "%02X", (unsigned)c);
    }
    dst[i * 2] = '\0';
}

/* hex -> dst (entschluesselt). Bricht bei ungueltigem Zeichen sauber ab. */
static void deobfus_hex(const char *hex, char *dst, int dstsz)
{
    int i = 0, o = 0, klen = (int)sizeof(XKEY) - 1;
    while (hex[i] && hex[i + 1] && o < dstsz - 1) {
        int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
        unsigned char c;
        if (hi < 0 || lo < 0) break;
        c = (unsigned char)((hi << 4) | lo);
        dst[o] = (char)(c ^ (unsigned char)XKEY[o % klen]);
        o++; i += 2;
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Laden                                                               */
/* ------------------------------------------------------------------ */
int connsave_load(char *host, int hostsz, char *port, int portsz,
                  char *user, int usersz, char *pass, int passsz, int *savepw)
{
    FILE *f;
    char  line[256];

    if (g_path[0] == '\0') return 0;
    f = fopen(g_path, "r");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        char *nl, *eq, *key, *val;

        nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = line;
        val = eq + 1;

        if      (stricmp(key, "host") == 0) { strncpy(host, val, hostsz - 1); host[hostsz - 1] = 0; }
        else if (stricmp(key, "port") == 0) { strncpy(port, val, portsz - 1); port[portsz - 1] = 0; }
        else if (stricmp(key, "user") == 0) { strncpy(user, val, usersz - 1); user[usersz - 1] = 0; }
        else if (stricmp(key, "savepw") == 0) { if (savepw) *savepw = atoi(val) ? 1 : 0; }
        else if (stricmp(key, "pass") == 0) { if (val[0]) deobfus_hex(val, pass, passsz); }
    }

    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Speichern                                                           */
/* ------------------------------------------------------------------ */
void connsave_store(const char *host, const char *port,
                    const char *user, const char *pass, int savepw)
{
    FILE *f;

    if (g_path[0] == '\0') return;
    f = fopen(g_path, "w");
    if (!f) return;

    fprintf(f, "# NCFTP386 gespeicherte Verbindung / saved connection\n");
    fprintf(f, "host=%s\n", host ? host : "");
    fprintf(f, "port=%s\n", port ? port : "");
    fprintf(f, "user=%s\n", user ? user : "");
    fprintf(f, "savepw=%d\n", savepw ? 1 : 0);

    if (savepw && pass && pass[0]) {
        char hex[2 * 64 + 1];
        obfus_hex(pass, hex);
        fprintf(f, "pass=%s\n", hex);
    } else {
        fprintf(f, "pass=\n");
    }

    fclose(f);
}
