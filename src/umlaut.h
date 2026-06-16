/* =============================================================================
 * umlaut.h - German umlauts as CP437/850 bytes for string literals
 * -----------------------------------------------------------------------------
 * The edit/build tools store source code as UTF-8; a directly typed
 * "ue" -> "ü" would end up there as 2 bytes (0xC3 0xBC) and produce
 * garbage on the DOS screen. So the umlauts are single-byte escapes via
 * macros that are concatenated into string literals:  "L" oe "schen"  ->
 * "Löschen".
 *
 * The byte values are identical in CP437 AND CP850 (German DOS standard):
 *   ä=0x84  ö=0x94  ü=0x81  Ä=0x8E  Ö=0x99  Ü=0x9A  ß=0xE1
 *
 * IMPORTANT: Always include this header LAST so that the short macro
 * names don't affect system or mTCP headers.
 * ===========================================================================*/
#ifndef UMLAUT_H
#define UMLAUT_H

#define ae "\x84"
#define oe "\x94"
#define ue "\x81"
#define Ae "\x8E"
#define Oe "\x99"
#define Ue "\x9A"
#define ss "\xE1"

#endif /* UMLAUT_H */
