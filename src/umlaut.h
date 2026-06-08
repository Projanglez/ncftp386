/* =============================================================================
 * umlaut.h - Deutsche Umlaute als CP437/850-Bytes fuer String-Literale
 * -----------------------------------------------------------------------------
 * Die Edit/Build-Werkzeuge speichern Quelltext als UTF-8; ein direkt getipptes
 * "ue" -> "ü" wuerde dort als 2 Bytes (0xC3 0xBC) landen und auf dem DOS-Schirm
 * Müll ergeben. Daher die Umlaute als Einzel-Byte-Escapes ueber Makros, die in
 * String-Literale einkonkateniert werden:  "L" oe "schen"  ->  "Löschen".
 *
 * Die Bytewerte sind in CP437 UND CP850 identisch (deutscher DOS-Standard):
 *   ä=0x84  ö=0x94  ü=0x81  Ä=0x8E  Ö=0x99  Ü=0x9A  ß=0xE1
 *
 * WICHTIG: Diesen Header IMMER als LETZTEN Include einbinden, damit die kurzen
 * Makronamen keine System- oder mTCP-Header beeinflussen.
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
