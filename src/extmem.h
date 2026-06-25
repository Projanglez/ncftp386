/* =============================================================================
 * extmem.h - Extended/expanded memory backend for the /EXMEM entry store
 * -----------------------------------------------------------------------------
 * A flat, byte-addressed block living OUTSIDE conventional memory: either an
 * XMS Extended Memory Block (HIMEM) or EMS expanded-memory pages (EMM386).
 * The entry store (entrystore.cpp) reads/writes fixed-size records into it.
 *
 * Only the small read/write of one record at a time crosses into conventional
 * memory, so this never competes with the conventional heap that mTCP uses.
 * ===========================================================================*/
#ifndef EXTMEM_H
#define EXTMEM_H

class ExtMem {
public:
    virtual ~ExtMem();
    /* Reserve 'bytes'. Returns the number of bytes actually granted (may be
     * less than requested; 0 = failure). Call once after construction. */
    virtual long alloc(long bytes) = 0;
    /* Copy 'len' bytes between the external block (at byte offset 'off') and a
     * conventional buffer. */
    virtual void read (long off, void *dst, int len) = 0;
    virtual void write(long off, const void *src, int len) = 0;
    /* "XMS" / "EMS" - for status messages. */
    virtual const char *kind() const = 0;
};

/* Detect and create a backend. prefer: 0 = auto (XMS first, then EMS),
 * 1 = XMS only, 2 = EMS only. Returns 0 if the requested memory isn't
 * available. The caller owns the object and deletes it when done. */
ExtMem *extmem_create(int prefer);

#endif /* EXTMEM_H */
