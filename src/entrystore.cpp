/* =============================================================================
 * entrystore.cpp - ConvStore (conventional) and ExtStore (XMS/EMS) backends
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#include <string.h>   /* stricmp, strrchr, memcmp     */
#include <stdlib.h>   /* qsort, malloc, free          */
#include <ctype.h>    /* tolower                      */

#include "panel.h"    /* PanelEntry, PANEL_MAX_ENTRIES, Panel::SORT_*, entrystore.h */
#include "extmem.h"

EntryStore::~EntryStore() {}

/* ------------------------------------------------------------------ */
/* Shared comparator                                                   */
/* ------------------------------------------------------------------ */
static const char *name_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == 0 || dot == name) return "";
    return dot + 1;
}

static int cmp_u(unsigned a, unsigned b)
{
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}

/* Order two entries by the given mode. ".." first, directories before files
 * regardless of key/direction; the key orders within a group, with a final
 * name tiebreak for a deterministic order. */
static int cmp_entries(const PanelEntry *ea, const PanelEntry *eb, int key, int desc)
{
    int r;

    if (ea->is_parent != eb->is_parent) return ea->is_parent ? -1 : 1;
    if (ea->is_dir    != eb->is_dir)    return ea->is_dir    ? -1 : 1;

    switch (key) {
    case Panel::SORT_EXT:
        r = stricmp(name_ext(ea->name), name_ext(eb->name));
        break;
    case Panel::SORT_SIZE:
        r = (ea->size < eb->size) ? -1 : (ea->size > eb->size) ? 1 : 0;
        break;
    case Panel::SORT_DATE:
        r = cmp_u(ea->date, eb->date);
        if (r == 0) r = cmp_u(ea->time, eb->time);
        break;
    case Panel::SORT_TIME:
        r = cmp_u(ea->time, eb->time);
        if (r == 0) r = cmp_u(ea->date, eb->date);
        break;
    case Panel::SORT_NAME:
    default:
        r = stricmp(ea->name, eb->name);
        break;
    }

    if (desc) r = -r;
    if (r == 0) r = stricmp(ea->name, eb->name);   /* stable tiebreak by name */
    return r;
}

/* ------------------------------------------------------------------ */
/* ConvStore                                                           */
/* ------------------------------------------------------------------ */
int ConvStore::append(const PanelEntry *e)
{
    if (cnt >= PANEL_MAX_ENTRIES) return 0;
    arr[cnt++] = *e;
    return 1;
}

void ConvStore::fetch(int i, PanelEntry *out)
{
    *out = arr[i];
}

static int g_convKey, g_convDesc;
static int conv_cmp(const void *a, const void *b)
{
    return cmp_entries((const PanelEntry *)a, (const PanelEntry *)b, g_convKey, g_convDesc);
}

void ConvStore::sort(int key, int desc)
{
    g_convKey = key; g_convDesc = desc;
    qsort(arr, cnt, sizeof(PanelEntry), conv_cmp);
}

/* ------------------------------------------------------------------ */
/* ExtStore                                                            */
/* ------------------------------------------------------------------ */
static const int  REC_SIZE   = (int)sizeof(PanelEntry);
static const long REC_STRIDE = ((long)sizeof(PanelEntry) + 1L) & ~1L;   /* even */

ExtStore::ExtStore(ExtMem *backend)
{
    mem      = backend;
    cnt      = 0;
    ringNext = 0;
    capCount = 0;
    order    = (int *)malloc((unsigned)EXT_CAP * sizeof(int));
    if (mem && order) {
        long granted = mem->alloc((long)EXT_CAP * REC_STRIDE);
        capCount = (int)(granted / REC_STRIDE);
        if (capCount > EXT_CAP) capCount = EXT_CAP;
    }
}

ExtStore::~ExtStore()
{
    if (order) free(order);
    if (mem)   delete mem;
}

int ExtStore::append(const PanelEntry *e)
{
    if (cnt >= capCount) return 0;
    mem->write((long)cnt * REC_STRIDE, e, REC_SIZE);
    order[cnt] = cnt;
    cnt++;
    return 1;
}

const PanelEntry *ExtStore::peek(int i)
{
    PanelEntry *slot = &ring[ringNext];
    mem->read((long)order[i] * REC_STRIDE, slot, REC_SIZE);
    ringNext = (ringNext + 1) % EXT_RING;
    return slot;
}

void ExtStore::fetch(int i, PanelEntry *out)
{
    mem->read((long)order[i] * REC_STRIDE, out, REC_SIZE);
}

void ExtStore::set_marked(int i, int v)
{
    PanelEntry t;
    long off = (long)order[i] * REC_STRIDE;
    mem->read(off, &t, REC_SIZE);
    t.marked = (unsigned char)v;
    mem->write(off, &t, REC_SIZE);
}

static ExtMem    *g_extMem;
static int        g_extKey, g_extDesc;
static PanelEntry g_extA, g_extB;

/* Slow path: read both records from extended memory for every comparison.
 * Used only when the conventional key cache below cannot be allocated. */
static int ext_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;     /* physical slots */
    g_extMem->read((long)ia * REC_STRIDE, &g_extA, REC_SIZE);
    g_extMem->read((long)ib * REC_STRIDE, &g_extB, REC_SIZE);
    return cmp_entries(&g_extA, &g_extB, g_extKey, g_extDesc);
}

/* Fast path: a compact conventional sort key per entry, indexed by physical
 * slot. The key holds the group (dirs/parent first) plus a memcmp-comparable
 * payload for the active sort column, so the bulk of qsort's comparisons never
 * touch extended memory. 8 bytes/entry keeps the whole table under one 64 KB
 * segment for up to EXT_CAP entries. Records are only read back on a key tie. */
#define EXT_KEYPAY 7
struct ExtKey { unsigned char grp; unsigned char pay[EXT_KEYPAY]; };
static ExtKey *g_keys = 0;

static void build_key(const PanelEntry *e, int key, ExtKey *k)
{
    int i;
    k->grp = e->is_parent ? 0 : (e->is_dir ? 1 : 2);
    for (i = 0; i < EXT_KEYPAY; i++) k->pay[i] = 0;
    switch (key) {
    case Panel::SORT_SIZE:                              /* big-endian -> numeric */
        k->pay[0] = (unsigned char)(e->size >> 24);
        k->pay[1] = (unsigned char)(e->size >> 16);
        k->pay[2] = (unsigned char)(e->size >> 8);
        k->pay[3] = (unsigned char)(e->size);
        break;
    case Panel::SORT_DATE:
        k->pay[0] = (unsigned char)(e->date >> 8);
        k->pay[1] = (unsigned char)(e->date);
        k->pay[2] = (unsigned char)(e->time >> 8);
        k->pay[3] = (unsigned char)(e->time);
        break;
    case Panel::SORT_TIME:
        k->pay[0] = (unsigned char)(e->time >> 8);
        k->pay[1] = (unsigned char)(e->time);
        k->pay[2] = (unsigned char)(e->date >> 8);
        k->pay[3] = (unsigned char)(e->date);
        break;
    case Panel::SORT_EXT: {
        const char *x = name_ext(e->name);
        for (i = 0; i < EXT_KEYPAY && x[i]; i++)
            k->pay[i] = (unsigned char)tolower((unsigned char)x[i]);
        break;
    }
    case Panel::SORT_NAME:
    default:                                            /* lowercased -> stricmp order */
        for (i = 0; i < EXT_KEYPAY && e->name[i]; i++)
            k->pay[i] = (unsigned char)tolower((unsigned char)e->name[i]);
        break;
    }
}

static int extk_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;     /* physical slots */
    const ExtKey *ka = &g_keys[ia], *kb = &g_keys[ib];
    int r;
    if (ka->grp != kb->grp) return ka->grp < kb->grp ? -1 : 1;   /* dirs first */
    r = memcmp(ka->pay, kb->pay, EXT_KEYPAY);
    if (g_extDesc) r = -r;
    if (r) return r;
    /* Key tie -> read the records for the exact comparison (name tiebreak). */
    g_extMem->read((long)ia * REC_STRIDE, &g_extA, REC_SIZE);
    g_extMem->read((long)ib * REC_STRIDE, &g_extB, REC_SIZE);
    return cmp_entries(&g_extA, &g_extB, g_extKey, g_extDesc);
}

void ExtStore::sort(int key, int desc)
{
    g_extMem = mem; g_extKey = key; g_extDesc = desc;
    if (cnt < 2) return;

    /* Build the conventional key table (one read per record); on success qsort
     * compares keys in conventional memory. Fall back to the slow path if the
     * table can't be allocated. */
    g_keys = (ExtKey *)malloc((unsigned)cnt * sizeof(ExtKey));
    if (g_keys) {
        PanelEntry t;
        int i;
        for (i = 0; i < cnt; i++) {          /* physical slot i */
            mem->read((long)i * REC_STRIDE, &t, REC_SIZE);
            build_key(&t, key, &g_keys[i]);
        }
        qsort(order, cnt, sizeof(int), extk_cmp);
        free(g_keys);
        g_keys = 0;
    } else {
        qsort(order, cnt, sizeof(int), ext_cmp);
    }
}
