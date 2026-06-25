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

/* Extra persisted UI state that travels with the saved connection: the FTP
 * start directory (empty = root) and the per-pane sort mode. Each pane's
 * *sort_saved flag tells whether a sort was explicitly remembered (0 = use
 * the program default). Bundled so the save/load API stays compact. */
struct UiState {
    char startdir[80];
    int  lsort_key, lsort_desc, lsort_saved;   /* left/local pane   */
    int  rsort_key, rsort_desc, rsort_saved;   /* right/remote pane */
};

/* Set the storage path once: the EXE's directory (from argv[0]),
 * fallback to the current directory at startup (before any navigation). */
void connsave_init(const char *argv0);

/* Load the saved connection. Fields are only overwritten if present in the
 * file; missing ones are left unchanged (caller sets defaults).
 * *savepw receives the saved "remember password" setting (0/1).
 * *swap (may be null) receives the saved "panels swapped" UI setting (0/1).
 * *ui  (may be null) receives the saved start dir + per-pane sort state.
 * Return value: 1 = file read, 0 = no file / not readable. */
int  connsave_load(char *host, int hostsz, char *port, int portsz,
                   char *user, int usersz, char *pass, int passsz,
                   int *savepw, int *swap, UiState *ui);

/* Save the connection. Host/port/user are always stored; the password
 * only if savepw != 0 (otherwise the pass line stays empty). swap stores
 * the "panels swapped" UI setting (0/1). ui (may be null) stores the start
 * dir + per-pane sort state. */
void connsave_store(const char *host, const char *port,
                    const char *user, const char *pass, int savepw, int swap,
                    const UiState *ui);

/* Light XOR(+hex) password obfuscation, shared with the site manager.
 * Not real encryption - only hides the plain text in the save files.
 *   connsave_obfus:   src -> hex; dst must hold >= 2*strlen(src)+1 chars.
 *   connsave_deobfus: hex -> dst (<= dstsz-1 chars); stops on invalid hex. */
void connsave_obfus(const char *src, char *dst);
void connsave_deobfus(const char *hex, char *dst, int dstsz);

#endif /* CONNSAVE_H */
