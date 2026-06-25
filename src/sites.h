/* =============================================================================
 * sites.h - Site manager: multiple named FTP connection profiles
 * -----------------------------------------------------------------------------
 * Unlike connsave.{cpp,h} (which remembers only the single LAST connection plus
 * the UI state in FTP4DOS.SAV), this stores a library of named profiles in a
 * separate text file FTP4DOS.SIT next to the EXE. Each profile keeps host, port,
 * user, password and an optional FTP start directory.
 *
 * Passwords are only stored when the profile's savepw flag is set, and then only
 * LIGHTLY obfuscated (XOR + hex, shared with connsave.cpp). This is not real
 * encryption - see the note in connsave.h.
 * ===========================================================================*/
#ifndef SITES_H
#define SITES_H

#define SITE_NAME_MAX 32
#define SITE_HOST_MAX 64        /* matches FTP_HOST_MAX                         */
#define SITE_PORT_MAX 8
#define SITE_USER_MAX 40
#define SITE_PASS_MAX 40
#define SITE_DIR_MAX  80
#define SITE_MAX      48        /* upper bound on stored profiles               */

struct Site {
    char name[SITE_NAME_MAX];
    char host[SITE_HOST_MAX];
    char port[SITE_PORT_MAX];
    char user[SITE_USER_MAX];
    char pass[SITE_PASS_MAX];   /* plain in memory; obfuscated on disk          */
    char dir [SITE_DIR_MAX];
    int  savepw;                /* 1 = write pass to disk, 0 = leave it empty   */
};

/* Set the storage path once: the EXE's directory (from argv[0]), fallback to the
 * current directory at startup. Mirrors connsave_init(). */
void sites_init(const char *argv0);

/* Load all profiles into arr (up to maxn). Returns the count (>= 0). */
int  sites_load(Site *arr, int maxn);

/* Write all n profiles, replacing the file. Returns 1 on success, 0 on error. */
int  sites_store(const Site *arr, int n);

#endif /* SITES_H */
