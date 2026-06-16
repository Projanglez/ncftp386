/* =============================================================================
 * i18n.cpp - Language detection via the DOS country setting
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <dos.h>
#include <i86.h>

#include "i18n.h"

int g_english = 0;             /* Default: German (set in i18n_init) */

const char *L(const char *en, const char *de)
{
    return g_english ? en : de;
}

void i18n_init(void)
{
    union REGS   in, out;
    struct SREGS sr;
    static unsigned char ctybuf[34];     /* buffer for the country info (DS:DX) */
    void far *p = (void far *)ctybuf;

    segread(&sr);
    in.h.ah = 0x38;            /* DOS: Get Country Dependent Information */
    in.h.al = 0x00;            /* 0 = current country                   */
    in.x.dx = FP_OFF(p);
    sr.ds   = FP_SEG(p);
    intdosx(&in, &out, &sr);

    /* Error (carry) -> default to English to be safe. */
    if (out.x.cflag) { g_english = 1; return; }

    /* Country code in BX. German-speaking: DE=49, AT=43, CH=41. */
    g_english = (out.x.bx == 49 || out.x.bx == 43 || out.x.bx == 41) ? 0 : 1;
}
