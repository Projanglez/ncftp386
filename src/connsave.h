/* =============================================================================
 * connsave.h - Remember the last FTP connection persistently
 * -----------------------------------------------------------------------------
 * Stores host/port/user/password of the last successfully established
 * connection in a small text file NCFTP.SAV next to the EXE and loads it
 * on the next start. Only ONE (the last) connection.
 *
 * Security (deliberately "reasonable", not maximal): FTP transmits the
 * password in plain text anyway, and the target hardware is single-user
 * retro DOS (physical access = full access). The password is therefore only
 * LIGHTLY obfuscated (XOR with a fixed key + hex) - this protects against
 * casual viewing of the file but is NOT real encryption.
 * ===========================================================================*/
#ifndef CONNSAVE_H
#define CONNSAVE_H

/* Set the storage path once: the EXE's directory (from argv[0]),
 * fallback to the current directory at startup (before any navigation). */
void connsave_init(const char *argv0);

/* Load the saved connection. Fields are only overwritten if present in the
 * file; missing ones are left unchanged (caller sets defaults).
 * *savepw receives the saved "remember password" setting (0/1).
 * *swap (may be null) receives the saved "panels swapped" UI setting (0/1).
 * Return value: 1 = file read, 0 = no file / not readable. */
int  connsave_load(char *host, int hostsz, char *port, int portsz,
                   char *user, int usersz, char *pass, int passsz,
                   int *savepw, int *swap);

/* Save the connection. Host/port/user are always stored; the password
 * only if savepw != 0 (otherwise the pass line stays empty). swap stores
 * the "panels swapped" UI setting (0/1). */
void connsave_store(const char *host, const char *port,
                    const char *user, const char *pass, int savepw, int swap);

#endif /* CONNSAVE_H */
