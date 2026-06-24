/* =============================================================================
 * i18n.h - Bilingual support (English / German) for NCFTP386
 * -----------------------------------------------------------------------------
 * The language is determined at startup from the DOS country setting
 * (INT 21h/38h, set via COUNTRY= in CONFIG.SYS): Germany/Austria/
 * Switzerland -> German, everything else -> English.
 *
 * Usage in code:  dlg_error(L("Error", "Fehler"), L("...", "..."));
 * ===========================================================================*/
#ifndef I18N_H
#define I18N_H

extern int g_english;          /* 0 = German, 1 = English */

/* Classic binary size units (1 KB = 1024 bytes). Used by every on-screen size
 * formatter so they cannot drift apart. Labels stay KB/MB/GB (never KiB/MiB). */
#define SZ_KB  1024UL
#define SZ_MB  1048576UL          /* 1024 * 1024            */
#define SZ_GB  1073741824UL       /* 1024 * 1024 * 1024     */

/* Regional number/date formatting, read from the DOS country block
 * (INT 21h/38h) at startup. Used for every on-screen size, date and time so
 * the display follows COUNTRY= in CONFIG.SYS (e.g. NL: "1.000.000", dd-mm-yy).
 * Independent of g_english, which only selects the UI text language. */
struct Locale {
    char thousands_sep;        /* digit grouping char, e.g. ',' (US) / '.' (NL) */
    char decimal_sep;          /* decimal point char,  e.g. '.' (US) / ',' (NL) */
    char date_sep;             /* date field separator, e.g. '/' or '-'         */
    char time_sep;             /* time field separator, usually ':'             */
    int  date_order;           /* 0 = MDY, 1 = DMY, 2 = YMD                      */
    int  time_24h;             /* 1 = 24-hour clock, 0 = 12-hour                 */
};
extern Locale g_locale;

/* Determine the language and regional formatting from the DOS country
 * setting (once at startup). */
void i18n_init(void);

/* Returns the English (en) or German (de) text depending on the detected
 * language. Both strings are static literals, so this is always safe. */
const char *L(const char *en, const char *de);

#endif /* I18N_H */
