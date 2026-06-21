/* =============================================================================
 * keymap.h - Key codes, readkey(), and the action enum for NCFTP386
 * -----------------------------------------------------------------------------
 * DOS keyboard via getch(): normal keys return their ASCII code, extended
 * keys (arrows, function keys) return 0 first, then the scan code. readkey()
 * combines that into a 9-bit code: extended keys => 0x100 | scancode.
 * ===========================================================================*/
#ifndef KEYMAP_H
#define KEYMAP_H

#include <conio.h>

/* ---- Normal keys (ASCII) ---- */
#define KEY_ENTER   0x0D
#define KEY_ESC     0x1B
#define KEY_TAB     0x09
#define KEY_BACKSP  0x08
#define KEY_SPACE   0x20
#define KEY_CTRL_U  0x15  /* swap panels left<->right (Norton Ctrl-U)            */

/* ---- Extended keys (0x100 | scancode) ---- */
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

/* Alt+function keys. Alt+F1..F10 return scan codes 0x68..0x71.
 * Alt+F1 = change drive, Alt+F6 = rename (F6 itself is Move). */
#define KEY_ALT_F1  0x168
#define KEY_ALT_F6  0x16D

/* Numpad keys: unused in the panel context, Norton Commander style.
 * Numpad * and + are ASCII and indistinguishable from the like-named keys. */
#define KEY_STAR    0x2A  /* invert marks                                          */
#define KEY_PLUS    0x2B  /* mark files missing/differing vs. the other panel      */

/* Read one key (blocking). Extended keys => 0x100 | scancode. */
inline int readkey(void)
{
    int k = getch();
    if (k == 0)
        k = 0x100 | getch();
    return k;
}

/* Is a key available? (non-blocking, for the idle loop). */
inline int key_pending(void)
{
    return kbhit();
}

#endif /* KEYMAP_H */
