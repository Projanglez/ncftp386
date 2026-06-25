/* =============================================================================
 * entrystore.h - Pluggable storage for a panel's directory entries
 * -----------------------------------------------------------------------------
 * Included by panel.h AFTER PanelEntry and PANEL_MAX_ENTRIES are defined.
 *
 * Two backends:
 *   ConvStore - a fixed inline array in conventional memory (default; the
 *               proven, always-available path, capped at PANEL_MAX_ENTRIES).
 *   ExtStore  - records in extended/expanded memory (XMS/EMS) via an ExtMem
 *               backend, with a conventional display-order index. Enabled by
 *               /EXMEM for very large remote listings.
 *
 * The panel sees a sorted sequence 0..count-1 and never deals with the backend.
 * ===========================================================================*/
#ifndef ENTRYSTORE_H
#define ENTRYSTORE_H

class EntryStore {
public:
    virtual ~EntryStore();
    virtual int  capacity() const = 0;
    virtual int  count() const = 0;
    virtual void reset() = 0;                          /* count -> 0            */
    virtual int  append(const PanelEntry *e) = 0;      /* 1 ok, 0 = full        */
    /* Transient read pointer to entry i (display order). Valid until the next
     * peek() on THIS store (backed by a small ring in the external store). */
    virtual const PanelEntry *peek(int i) = 0;
    /* Copy entry i (display order) into a caller-owned, stable buffer. */
    virtual void fetch(int i, PanelEntry *out) = 0;
    virtual void set_marked(int i, int v) = 0;         /* mutate just .marked   */
    virtual void sort(int key, int desc) = 0;          /* reorder 0..count-1    */
};

/* ---- Default: fixed conventional buffer ---------------------------------- */
class ConvStore : public EntryStore {
public:
    ConvStore() : cnt(0) {}
    int  capacity() const { return PANEL_MAX_ENTRIES; }
    int  count() const    { return cnt; }
    void reset()          { cnt = 0; }
    int  append(const PanelEntry *e);
    const PanelEntry *peek(int i) { return &arr[i]; }
    void fetch(int i, PanelEntry *out);
    void set_marked(int i, int v) { arr[i].marked = (unsigned char)v; }
    void sort(int key, int desc);
private:
    PanelEntry arr[PANEL_MAX_ENTRIES];
    int        cnt;
};

/* ---- Optional: XMS/EMS-backed buffer for huge listings ------------------- */
class ExtMem;                       /* extmem.h */

#define EXT_CAP   8000              /* max entries in /EXMEM mode (order[]=16KB)*/
#define EXT_RING  4                 /* peek() scratch slots                     */

class ExtStore : public EntryStore {
public:
    ExtStore(ExtMem *backend);      /* takes ownership of backend               */
    ~ExtStore();
    int  ok() const { return capCount > 0 && order != 0; }
    int  capacity() const { return capCount; }
    int  count() const    { return cnt; }
    void reset()          { cnt = 0; }
    int  append(const PanelEntry *e);
    const PanelEntry *peek(int i);
    void fetch(int i, PanelEntry *out);
    void set_marked(int i, int v);
    void sort(int key, int desc);
private:
    ExtMem    *mem;
    int       *order;               /* display-order -> physical slot           */
    int        cnt;
    int        capCount;
    PanelEntry ring[EXT_RING];
    int        ringNext;
};

#endif /* ENTRYSTORE_H */
