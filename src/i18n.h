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

/* Determine the language from the DOS country setting (once at startup). */
void i18n_init(void);

/* Returns the English (en) or German (de) text depending on the detected
 * language. Both strings are static literals, so this is always safe. */
const char *L(const char *en, const char *de);

#endif /* I18N_H */
