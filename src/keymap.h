/* =============================================================================
 * keymap.h - Tastatur-Codes, readkey() und Aktions-Enum fuer NCFTP386
 * -----------------------------------------------------------------------------
 * DOS-Tastatur via getch(): normale Tasten liefern ihren ASCII-Code, erweiterte
 * Tasten (Pfeile, Funktionstasten) liefern zuerst 0, dann den Scancode. readkey()
 * fasst das zu einem 9-bit-Code zusammen: erweiterte Tasten => 0x100 | scancode.
 * ===========================================================================*/
#ifndef KEYMAP_H
#define KEYMAP_H

#include <conio.h>

/* ---- Normale Tasten (ASCII) ---- */
#define KEY_ENTER   0x0D
#define KEY_ESC     0x1B
#define KEY_TAB     0x09
#define KEY_BACKSP  0x08
#define KEY_SPACE   0x20

/* ---- Erweiterte Tasten (0x100 | Scancode) ---- */
#define KEY_UP      0x148
#define KEY_DOWN    0x150
#define KEY_LEFT    0x14B
#define KEY_RIGHT   0x14D
#define KEY_PGUP    0x149
#define KEY_PGDN    0x151
#define KEY_HOME    0x147
#define KEY_END     0x14F
#define KEY_INS     0x152
#define KEY_DEL     0x153
#define KEY_F1      0x13B
#define KEY_F2      0x13C
#define KEY_F3      0x13D
#define KEY_F4      0x13E
#define KEY_F5      0x13F
#define KEY_F6      0x140
#define KEY_F7      0x141
#define KEY_F8      0x142
#define KEY_F9      0x143
#define KEY_F10     0x144

/* Alt+Funktionstasten (geheime Shortcuts). Alt+F1 liefert Scancode 0x68. */
#define KEY_ALT_F1  0x168

/* Numpad * (grauer Stern) - wie normales '*' nicht unterscheidbar, aber im
 * Panel-Kontext unbelegt: Markierung invertieren (Norton-Commander-Stil). */
#define KEY_STAR    0x2A

/* Eine Taste lesen (blockierend). Erweiterte Tasten => 0x100 | Scancode. */
inline int readkey(void)
{
    int k = getch();
    if (k == 0)
        k = 0x100 | getch();
    return k;
}

/* Steht eine Taste bereit? (nicht-blockierend, fuer die Leerlaufschleife). */
inline int key_pending(void)
{
    return kbhit();
}

#endif /* KEYMAP_H */
