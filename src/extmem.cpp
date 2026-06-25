/* =============================================================================
 * extmem.cpp - XMS (HIMEM) and EMS (EMM386) backends for /EXMEM
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * XMS: detected via INT 2Fh AX=4300h; the driver entry point (AX=4310h) is
 *      called FAR. Records are copied with the "Move Extended Memory Block"
 *      function (AH=0Bh), whose move length must be EVEN - odd record sizes go
 *      through a small even-sized bounce buffer.
 * EMS: detected via the INT 67h vector + the "EMMXXXX0" device signature. A
 *      logical 16 KB page is mapped into the 64 KB page frame and accessed
 *      directly through a far pointer; records that straddle a page boundary
 *      map two consecutive logical pages into the contiguous frame.
 * ===========================================================================*/
#include <dos.h>
#include <i86.h>      /* int86, int86x, union REGS, struct SREGS, MK_FP        */
#include <string.h>   /* _fmemcpy, _fmemcmp                                    */

#include "extmem.h"

ExtMem::~ExtMem() {}

/* ===================================================================== */
/* XMS backend                                                           */
/* ===================================================================== */

/* XMS "Move Extended Memory Block" descriptor (packed, -zp1). */
struct XmsMove {
    unsigned long len;
    unsigned      src_handle;   /* 0 = conventional (offset is a far pointer) */
    unsigned long src_off;
    unsigned      dst_handle;
    unsigned long dst_off;
};

static unsigned long g_xmsEntry = 0;   /* seg:off of the XMS driver entry point */
static XmsMove       g_xmsmv;
static unsigned char g_bounce[64];     /* even-sized bounce for odd-length moves */

static unsigned long far_to_lin(void far *p)
{
    return ((unsigned long)FP_SEG(p) << 16) | (unsigned long)FP_OFF(p);
}

/* Every XMS entry call goes through a STACK-LOCAL copy of the entry pointer and
 * is called indirectly via that local (BP/SS-relative). This is essential: the
 * globals may live in a FAR segment, so a DS-relative "call [g_xmsEntry]" from
 * asm would read the wrong memory. The driver may also clobber BX/CX/SI/DI/ES
 * and BP - they are saved/restored, BP first so BP-relative stores stay valid. */

/* Allocate an EMB of 'kb' kilobytes. Returns the handle, or 0 on failure. */
static unsigned xms_alloc_kb(unsigned kb)
{
    unsigned long ent = g_xmsEntry;
    unsigned okv = 0, hnd = 0;
    _asm {
        push bx
        push cx
        push si
        push di
        push es
        push bp
        mov ah, 09h
        mov dx, kb
        call dword ptr [ent]
        pop bp
        mov okv, ax
        mov hnd, dx
        pop es
        pop di
        pop si
        pop cx
        pop bx
    }
    return okv ? hnd : 0;
}

static void xms_free(unsigned handle)
{
    unsigned long ent = g_xmsEntry;
    _asm {
        push bx
        push cx
        push si
        push di
        push es
        push bp
        mov ah, 0Ah
        mov dx, handle
        call dword ptr [ent]
        pop bp
        pop es
        pop di
        pop si
        pop cx
        pop bx
    }
}

/* Execute the move described by g_xmsmv. The descriptor is passed in DS:SI, so
 * DS is changed for the call; the entry local and the result are BP/SS-relative
 * (independent of DS). */
static unsigned xms_move(void)
{
    unsigned long ent = g_xmsEntry;
    void far *p = (void far *)&g_xmsmv;
    unsigned res = 0;
    _asm {
        push ds
        push bx
        push cx
        push si
        push di
        push es
        push bp
        mov  ah, 0Bh
        lds  si, p
        call dword ptr [ent]
        pop  bp
        pop  es
        pop  di
        pop  si
        pop  cx
        pop  bx
        pop  ds
        mov  res, ax
    }
    return res;
}

class XmsMem : public ExtMem {
public:
    XmsMem() : handle(0), granted(0) {}
    ~XmsMem() { if (handle) xms_free(handle); }
    long alloc(long bytes);
    void read (long off, void *dst, int len);
    void write(long off, const void *src, int len);
    const char *kind() const { return "XMS"; }
private:
    unsigned  handle;
    long      granted;
};

long XmsMem::alloc(long bytes)
{
    unsigned kb = (unsigned)((bytes + 1023L) / 1024L);
    while (kb >= 16) {
        handle = xms_alloc_kb(kb);
        if (handle) { granted = (long)kb * 1024L; return granted; }
        kb = (unsigned)((unsigned long)kb * 7U / 8U);   /* back off ~12% */
    }
    return 0;
}

void XmsMem::read(long off, void *dst, int len)
{
    int mlen = (len + 1) & ~1;                 /* XMS move length must be even */
    g_xmsmv.len        = (unsigned long)mlen;
    g_xmsmv.src_handle = handle;
    g_xmsmv.src_off    = (unsigned long)off;
    g_xmsmv.dst_handle = 0;
    g_xmsmv.dst_off    = far_to_lin((void far *)g_bounce);
    xms_move();
    _fmemcpy((void far *)dst, (void far *)g_bounce, len);
}

void XmsMem::write(long off, const void *src, int len)
{
    int mlen = (len + 1) & ~1;
    _fmemcpy((void far *)g_bounce, (void far *)src, len);
    if (mlen > len) g_bounce[len] = 0;
    g_xmsmv.len        = (unsigned long)mlen;
    g_xmsmv.src_handle = 0;
    g_xmsmv.src_off    = far_to_lin((void far *)g_bounce);
    g_xmsmv.dst_handle = handle;
    g_xmsmv.dst_off    = (unsigned long)off;
    xms_move();
}

/* Detect the XMS driver and capture its entry point. ES:BX from INT 2Fh/4310h
 * is read directly in asm (not via int86x, whose returned ES is unreliable). */
static int xms_present(void)
{
    unsigned char inst = 0;
    unsigned eseg = 0, eoff = 0;

    _asm {
        push bx
        push es
        mov  ax, 4300h
        int  2Fh
        mov  inst, al
        pop  es
        pop  bx
    }
    if (inst != 0x80) return 0;

    _asm {
        push bx
        push es
        mov  ax, 4310h
        int  2Fh
        mov  eoff, bx
        mov  eseg, es
        pop  es
        pop  bx
    }
    g_xmsEntry = ((unsigned long)eseg << 16) | (unsigned long)eoff;
    return eseg != 0;
}

/* ===================================================================== */
/* EMS backend                                                           */
/* ===================================================================== */

#define EMS_PAGE 16384L

class EmsMem : public ExtMem {
public:
    EmsMem() : handle(0), frame(0), granted(0) {}
    ~EmsMem();
    long alloc(long bytes);
    void read (long off, void *dst, int len);
    void write(long off, const void *src, int len);
    const char *kind() const { return "EMS"; }
private:
    void map(long off, int len);
    unsigned handle;
    unsigned frame;        /* page-frame segment */
    long     granted;
};

EmsMem::~EmsMem()
{
    if (handle) {
        union REGS r;
        r.h.ah = 0x45; r.x.dx = handle;     /* deallocate pages */
        int86(0x67, &r, &r);
    }
}

long EmsMem::alloc(long bytes)
{
    union REGS r;
    unsigned pages = (unsigned)((bytes + EMS_PAGE - 1) / EMS_PAGE);

    /* Page-frame segment. */
    r.h.ah = 0x41;
    int86(0x67, &r, &r);
    if (r.h.ah != 0) return 0;
    frame = r.x.bx;

    while (pages >= 1) {
        r.h.ah = 0x43; r.x.bx = pages;      /* allocate 'pages' 16 KB pages */
        int86(0x67, &r, &r);
        if (r.h.ah == 0) { handle = r.x.dx; granted = (long)pages * EMS_PAGE; return granted; }
        pages = (unsigned)((unsigned long)pages * 7U / 8U);
    }
    return 0;
}

/* Map the logical page(s) covering [off, off+len) into the frame's physical
 * pages 0 (and 1 when the record straddles a 16 KB boundary). */
void EmsMem::map(long off, int len)
{
    union REGS r;
    unsigned logical = (unsigned)(off / EMS_PAGE);
    int      poff    = (int)(off % EMS_PAGE);

    r.h.ah = 0x44; r.h.al = 0; r.x.bx = logical; r.x.dx = handle;
    int86(0x67, &r, &r);
    if (poff + len > (int)EMS_PAGE) {
        r.h.ah = 0x44; r.h.al = 1; r.x.bx = logical + 1; r.x.dx = handle;
        int86(0x67, &r, &r);
    }
}

void EmsMem::read(long off, void *dst, int len)
{
    int poff = (int)(off % EMS_PAGE);
    map(off, len);
    _fmemcpy((void far *)dst, (void far *)MK_FP(frame, poff), len);
}

void EmsMem::write(long off, const void *src, int len)
{
    int poff = (int)(off % EMS_PAGE);
    map(off, len);
    _fmemcpy((void far *)MK_FP(frame, poff), (void far *)src, len);
}

/* Detect EMM via the INT 67h vector's "EMMXXXX0" device signature, then
 * confirm the manager reports OK. */
static int ems_present(void)
{
    union REGS r;
    struct SREGS s;
    static const char sig[8] = { 'E','M','M','X','X','X','X','0' };
    unsigned char far *name;

    /* Get the INT 67h handler segment:offset. */
    r.h.ah = 0x35; r.h.al = 0x67;
    int86x(0x21, &r, &r, &s);
    name = (unsigned char far *)MK_FP(s.es, 10);   /* device name at handler+10 */
    if (_fmemcmp(name, sig, 8) != 0) return 0;

    r.h.ah = 0x40;                                  /* manager status */
    int86(0x67, &r, &r);
    return r.h.ah == 0;
}

/* ===================================================================== */
/* Factory                                                               */
/* ===================================================================== */
ExtMem *extmem_create(int prefer)
{
    /* prefer: 0 auto, 1 XMS, 2 EMS */
    if (prefer != 2 && xms_present()) return new XmsMem();
    if (prefer != 1 && ems_present()) return new EmsMem();
    return 0;
}
