/* =============================================================================
 * dircopy.cpp - Recursive copying of entire directory trees
 * -----------------------------------------------------------------------------
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 *
 * Memory/depth notes:
 *  - Upload uses _dos_findfirst/_dos_findnext in streaming fashion: each
 *    recursion level has its own find_t (Watcom resets the DTA before every
 *    findnext), so nested iteration is allowed - no intermediate buffer.
 *  - Download must read in an entire directory level completely (the LIST's
 *    data connection must be closed before the next entry's RETR/LIST can
 *    run). We therefore read into a temporary buffer, but free it BEFORE
 *    descending into subdirectories (only the compact list of subdirectory
 *    names is kept) -> low peak memory even for deep trees.
 *  - DC_MAXDEPTH limits infinite recursion (e.g. from cyclic symlinks).
 * ===========================================================================*/
#include <dos.h>      /* _dos_findfirst/_dos_findnext, _A_*, struct find_t */
#include <direct.h>   /* _mkdir                                            */
#include <string.h>
#include <stdlib.h>   /* malloc/free                                       */
#include <stdio.h>
#include <ctype.h>    /* toupper                                           */
#include <io.h>       /* access                                            */

#include "dircopy.h"
#include "panel.h"     /* PanelEntry, PANEL_NAME_MAX */
#include "rpanel.h"    /* ftp_parse_list_line        */

#define DC_MAXDEPTH    16
#define DC_PATHMAX     260
#define DC_LISTCAP     400     /* max. entries per remote directory level */

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

/* "dir\name" (local, backslash). Watch the "C:\" root, length-safe. */
static void join_local(char *out, int outsz, const char *dir, const char *name)
{
    int n;
    strncpy(out, dir, outsz - 1);
    out[outsz - 1] = '\0';
    n = (int)strlen(out);
    if (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/' && out[n - 1] != ':'
        && n < outsz - 1) {
        out[n++] = '\\';
        out[n] = '\0';
    }
    strncat(out, name, outsz - 1 - (int)strlen(out));
}

/* "dir/name" (remote, slash). Watch the "/" root, length-safe. */
static void join_remote(char *out, int outsz, const char *dir, const char *name)
{
    int n;
    strncpy(out, dir, outsz - 1);
    out[outsz - 1] = '\0';
    n = (int)strlen(out);
    if (n > 0 && out[n - 1] != '/' && n < outsz - 1) {
        out[n++] = '/';
        out[n] = '\0';
    }
    strncat(out, name, outsz - 1 - (int)strlen(out));
}

static int is_dot_dir(const char *name)
{
    if (name[0] == '.' && name[1] == '\0') return 1;
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') return 1;
    return 0;
}

/* 1 if the local file exists (overwrite prompt for download). */
static int local_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* 1 if a local file OR directory with this path exists (used for 8.3 name
 * uniqueness: fopen-based local_exists() can't see directories). */
static int path_exists(const char *path)
{
    return access(path, 0) == 0;
}

#define DC_AMASK (_A_SUBDIR | _A_HIDDEN | _A_SYSTEM | _A_RDONLY | _A_ARCH)

/* ------------------------------------------------------------------ */
/* Upload (local -> remote), recursive                                 */
/* ------------------------------------------------------------------ */
static int upload_recurse(FtpClient *ftp, const char *localDir,
                          const char *remoteDir, const char *leaf, int depth,
                          DirCopyItemCb itemcb, FtpProgressCb progcb,
                          DirCopyConflictCb conflictcb, void *ctx)
{
    struct find_t ff;
    char     pat[DC_PATHMAX];
    unsigned rc;
    int      result = FTP_OK;

    if (depth > DC_MAXDEPTH) return FTP_ERR_GENERAL;

    /* Create the target directory remotely. If it already exists, we
     * ignore the error and copy into the existing directory. */
    if (itemcb) itemcb(ctx, leaf, 1);
    ftp->make_dir(remoteDir);

    join_local(pat, (int)sizeof(pat), localDir, "*.*");
    rc = _dos_findfirst(pat, DC_AMASK, &ff);
    while (rc == 0) {
        if (!is_dot_dir(ff.name)) {
            char childL[DC_PATHMAX], childR[DC_PATHMAX];
            join_local(childL, (int)sizeof(childL), localDir, ff.name);
            join_remote(childR, (int)sizeof(childR), remoteDir, ff.name);

            if (ff.attrib & _A_SUBDIR) {
                result = upload_recurse(ftp, childL, childR, ff.name, depth + 1,
                                        itemcb, progcb, conflictcb, ctx);
            } else {
                int do_copy = 1;
                if (conflictcb && ftp->remote_file_exists(childR)) {
                    int d = conflictcb(ctx, ff.name);
                    if (d == DC_ABORT)     result = FTP_ERR_ABORT;
                    else if (d == DC_SKIP) do_copy = 0;
                }
                if (result == FTP_OK && do_copy) {
                    if (itemcb) itemcb(ctx, ff.name, 0);
                    result = ftp->stor(childL, childR, progcb, ctx);
                }
            }
            if (result != FTP_OK) break;
        }
        rc = _dos_findnext(&ff);
    }
    return result;
}

int dircopy_upload(FtpClient *ftp, const char *localDir, const char *remoteName,
                   DirCopyItemCb itemcb, FtpProgressCb progcb,
                   DirCopyConflictCb conflictcb, void *ctx)
{
    return upload_recurse(ftp, localDir, remoteName, remoteName, 0,
                          itemcb, progcb, conflictcb, ctx);
}

/* ------------------------------------------------------------------ */
/* Download (remote -> local), recursive                               */
/* ------------------------------------------------------------------ */

/* Compact entry for a fetched directory level. 'name' points into the owning
 * DcCollect's pool and holds the FULL (untruncated) name. */
struct DcEnt {
    char         *name;
    unsigned char is_dir;
    unsigned long size;        /* file size in bytes (0 for directories) */
};

struct DcCollect {
    DcEnt   *arr;
    int      count;
    int      cap;
    int      curYear;
    char    *pool;             /* full-name pool for this level           */
    unsigned poolUsed;
    unsigned poolSize;
};

#define DC_NAMEPOOL  49152U    /* per-level full-name pool (48 KB)         */

static int dc_current_year(void)
{
    struct dosdate_t d;
    _dos_getdate(&d);
    return (int)d.year;
}

/* Allocate a collection (entry array + name pool). 0 = out of memory. */
static int dc_init(DcCollect *c, int cap)
{
    c->cap = cap; c->count = 0; c->curYear = dc_current_year();
    c->poolSize = DC_NAMEPOOL; c->poolUsed = 0;
    c->arr  = (DcEnt *)malloc((unsigned)cap * sizeof(DcEnt));
    c->pool = (char  *)malloc(DC_NAMEPOOL);
    if (!c->arr || !c->pool) { free(c->arr); free(c->pool); c->arr = 0; c->pool = 0; return 0; }
    return 1;
}

static void dc_free(DcCollect *c)
{
    free(c->arr); free(c->pool); c->arr = 0; c->pool = 0;
}

/* Append one entry, copying 'name' into the pool. 0 = no room (entry dropped). */
static int dc_add(DcCollect *c, const char *name, unsigned char is_dir, unsigned long size)
{
    unsigned len = (unsigned)strlen(name) + 1;
    char    *dst;
    if (c->count >= c->cap) return 0;
    if (!c->pool || c->poolUsed + len > c->poolSize) return 0;
    dst = c->pool + c->poolUsed;
    memcpy(dst, name, len);
    c->poolUsed += len;
    c->arr[c->count].name   = dst;
    c->arr[c->count].is_dir = is_dir;
    c->arr[c->count].size   = size;
    c->count++;
    return 1;
}

static void dc_on_line(void *vctx, const char *line)
{
    DcCollect *c = (DcCollect *)vctx;
    PanelEntry e;
    char full[FTP_LINE_MAX];

    if (!ftp_parse_list_line(line, c->curYear, &e, full, (int)sizeof(full))) return;
    if (full[0] == '\0' || is_dot_dir(full)) return;
    dc_add(c, full, e.is_dir, e.is_dir ? 0UL : e.size);
}

/* Subdirectory names of a level, preserved compactly so the big level buffer
 * can be freed before descending (keeps peak memory low for deep trees). */
struct DcSubdirs {
    char  *pool;        /* names concatenated         */
    char **name;        /* 'count' pointers into pool */
    int    count;
};

static int extract_subdirs(DcCollect *col, DcSubdirs *out)
{
    int      i;
    unsigned bytes = 0, used = 0;
    out->pool = 0; out->name = 0; out->count = 0;
    for (i = 0; i < col->count; i++)
        if (col->arr[i].is_dir) { out->count++; bytes += (unsigned)strlen(col->arr[i].name) + 1; }
    if (out->count <= 0) { out->count = 0; return 0; }
    out->pool = (char  *)malloc(bytes);
    out->name = (char **)malloc((unsigned)out->count * sizeof(char *));
    if (!out->pool || !out->name) { free(out->pool); free(out->name); out->pool = 0; out->name = 0; out->count = 0; return 0; }
    out->count = 0;
    for (i = 0; i < col->count; i++)
        if (col->arr[i].is_dir) {
            unsigned len = (unsigned)strlen(col->arr[i].name) + 1;
            memcpy(out->pool + used, col->arr[i].name, len);
            out->name[out->count++] = out->pool + used;
            used += len;
        }
    return out->count;
}

static void free_subdirs(DcSubdirs *s)
{
    free(s->pool); free(s->name); s->pool = 0; s->name = 0; s->count = 0;
}

/* ------------------------------------------------------------------ */
/* 8.3 name generation (for long remote names on the local DOS side)   */
/* ------------------------------------------------------------------ */

/* 1 if 'name' already is a clean DOS 8.3 name (<=8 base, <=3 ext, one dot,
 * only FAT-safe characters). */
static int is_clean_83(const char *name)
{
    int base = 0, ext = 0, dot = 0;
    const char *p;
    if (!name[0]) return 0;
    for (p = name; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '.') { if (dot) return 0; dot = 1; continue; }
        if (ch <= ' ' || ch > 126) return 0;
        if (strchr("\"*+,/:;<=>?[\\]|", ch)) return 0;      /* FAT-illegal */
        if (dot) { if (++ext > 3) return 0; }
        else     { if (++base > 8) return 0; }
    }
    return (base >= 1);
}

/* Append up to 'max' FAT-safe, uppercased characters of 'src' into dst, starting
 * at 'pos' (capped at 'cap'). Dots and illegal characters are dropped. */
static int san_copy(char *dst, int pos, int cap, const char *src, int max)
{
    int k = 0;
    const char *p;
    for (p = src; *p && k < max && pos < cap - 1; p++) {
        unsigned char ch = (unsigned char)toupper((unsigned char)*p);
        if (ch <= ' ' || ch > 126) continue;
        if (strchr("\"*+,/:;<=>?[\\]|.", ch)) continue;
        dst[pos++] = (char)ch; k++;
    }
    return pos;
}

/* Produce a valid, unique 8.3 local name for (possibly long) 'name' in
 * 'localDir'. Clean 8.3 names are used verbatim; otherwise a unique
 * "PREFIX~N.EXT" is built (like VFAT short names). */
static void make_local_83(const char *name, const char *localDir, char *out, int outsz)
{
    char        ext[4];
    const char *dotp;
    long        n;

    if (is_clean_83(name)) {
        strncpy(out, name, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }

    ext[0] = '\0';
    dotp = strrchr(name, '.');
    if (dotp && dotp[1]) ext[san_copy(ext, 0, (int)sizeof(ext), dotp + 1, 3)] = '\0';

    for (n = 1; n < 100000L; n++) {
        char base[9], tilde[8], cand[16], probe[DC_PATHMAX];
        int  bcap, blen;

        sprintf(tilde, "~%ld", n);
        bcap = 8 - (int)strlen(tilde);
        if (bcap < 1) bcap = 1;
        blen = san_copy(base, 0, (int)sizeof(base), name, bcap);
        if (blen == 0) base[blen++] = 'X';
        base[blen] = '\0';

        if (ext[0]) sprintf(cand, "%s%s.%s", base, tilde, ext);
        else        sprintf(cand, "%s%s", base, tilde);

        join_local(probe, (int)sizeof(probe), localDir, cand);
        if (!path_exists(probe)) { strncpy(out, cand, outsz - 1); out[outsz - 1] = '\0'; return; }
    }
    strncpy(out, "DC______.TMP", outsz - 1);
    out[outsz - 1] = '\0';
}

static int download_recurse(FtpClient *ftp, const char *remoteDir,
                            const char *localDir, const char *leaf, int depth,
                            DirCopyItemCb itemcb, FtpProgressCb progcb,
                            DirCopyConflictCb conflictcb, void *ctx)
{
    DcCollect col;
    DcSubdirs subs;
    int       i, nd, j, result = FTP_OK;

    if (depth > DC_MAXDEPTH) return FTP_ERR_GENERAL;

    /* Create the local target directory (ignore if it already exists). */
    if (itemcb) itemcb(ctx, leaf, 1);
    _mkdir(localDir);

    /* --- 1) read in the entire level --- */
    if (!dc_init(&col, DC_LISTCAP)) return FTP_ERR_LOCALIO;

    result = ftp->list(remoteDir, dc_on_line, &col);
    if (result != FTP_OK) { dc_free(&col); return result; }

    /* --- 2) fetch all files first (no recursion, no extra memory). The remote
     *        RETR uses the full name; the local file gets a valid 8.3 name
     *        (long names are mangled to PREFIX~N.EXT). --- */
    for (i = 0; i < col.count; i++) {
        char childR[DC_PATHMAX], childL[DC_PATHMAX], shortn[PANEL_NAME_MAX];
        if (col.arr[i].is_dir) continue;
        join_remote(childR, (int)sizeof(childR), remoteDir, col.arr[i].name);
        make_local_83(col.arr[i].name, localDir, shortn, (int)sizeof(shortn));
        join_local (childL, (int)sizeof(childL), localDir,  shortn);

        if (conflictcb && local_exists(childL)) {
            int d = conflictcb(ctx, col.arr[i].name);
            if (d == DC_ABORT) { dc_free(&col); return FTP_ERR_ABORT; }
            if (d == DC_SKIP)  continue;
        }
        if (itemcb) itemcb(ctx, col.arr[i].name, 0);
        result = ftp->retr(childR, childL, progcb, ctx);
        if (result != FTP_OK) { dc_free(&col); return result; }
    }

    /* --- 3) save subdirectory names compactly, free the large buffer --- */
    nd = extract_subdirs(&col, &subs);
    dc_free(&col);

    /* --- 4) descend into the subdirectories --- */
    for (j = 0; j < nd; j++) {
        char childR[DC_PATHMAX], childL[DC_PATHMAX], shortn[PANEL_NAME_MAX];
        join_remote(childR, (int)sizeof(childR), remoteDir, subs.name[j]);
        make_local_83(subs.name[j], localDir, shortn, (int)sizeof(shortn));
        join_local (childL, (int)sizeof(childL), localDir,  shortn);
        result = download_recurse(ftp, childR, childL, subs.name[j], depth + 1,
                                  itemcb, progcb, conflictcb, ctx);
        if (result != FTP_OK) break;
    }
    free_subdirs(&subs);
    return result;
}

int dircopy_download(FtpClient *ftp, const char *remotePath, const char *localDir,
                     DirCopyItemCb itemcb, FtpProgressCb progcb,
                     DirCopyConflictCb conflictcb, void *ctx)
{
    /* Derive the leaf name (for display) from the remote path. */
    const char *leaf = remotePath;
    const char *p;
    for (p = remotePath; *p; p++)
        if (*p == '/' || *p == '\\') leaf = p + 1;

    return download_recurse(ftp, remotePath, localDir, leaf, 0,
                            itemcb, progcb, conflictcb, ctx);
}

/* ------------------------------------------------------------------ */
/* Tree measure (files + subdirs + total bytes)                        */
/* Used both for the confirm-dialog totals and the batch progress.     */
/* The root directory is NOT counted here (the caller adds it).         */
/* ------------------------------------------------------------------ */
static int measure_local_recurse(const char *dir, unsigned *nf, unsigned *nd,
                                 unsigned long *bytes, int depth)
{
    struct find_t ff;
    char     pat[DC_PATHMAX];
    unsigned rc;

    if (depth > DC_MAXDEPTH) return FTP_ERR_GENERAL;

    join_local(pat, (int)sizeof(pat), dir, "*.*");
    rc = _dos_findfirst(pat, DC_AMASK, &ff);
    while (rc == 0) {
        if (!is_dot_dir(ff.name)) {
            if (ff.attrib & _A_SUBDIR) {
                char child[DC_PATHMAX];
                (*nd)++;
                join_local(child, (int)sizeof(child), dir, ff.name);
                measure_local_recurse(child, nf, nd, bytes, depth + 1);
            } else {
                (*nf)++;
                *bytes += ff.size;
            }
        }
        rc = _dos_findnext(&ff);
    }
    return FTP_OK;
}

int dircopy_measure_local(const char *path, unsigned *nfiles, unsigned *ndirs,
                          unsigned long *bytes)
{
    return measure_local_recurse(path, nfiles, ndirs, bytes, 0);
}

static int measure_remote_recurse(FtpClient *ftp, const char *dir,
                                  unsigned *nf, unsigned *nd,
                                  unsigned long *bytes, int depth)
{
    DcCollect col;
    DcSubdirs subs;
    int       i, nsub, j, rc;

    if (depth > DC_MAXDEPTH) return FTP_ERR_GENERAL;

    if (!dc_init(&col, DC_LISTCAP)) return FTP_ERR_LOCALIO;

    rc = ftp->list(dir, dc_on_line, &col);
    if (rc != FTP_OK) { dc_free(&col); return rc; }

    for (i = 0; i < col.count; i++) {
        if (col.arr[i].is_dir) (*nd)++;
        else { (*nf)++; *bytes += col.arr[i].size; }
    }
    nsub = extract_subdirs(&col, &subs);
    dc_free(&col);

    for (j = 0; j < nsub; j++) {
        char child[DC_PATHMAX];
        join_remote(child, (int)sizeof(child), dir, subs.name[j]);
        rc = measure_remote_recurse(ftp, child, nf, nd, bytes, depth + 1);
        if (rc != FTP_OK) break;
    }
    free_subdirs(&subs);
    return rc;
}

int dircopy_measure_remote(FtpClient *ftp, const char *path,
                           unsigned *nfiles, unsigned *ndirs, unsigned long *bytes)
{
    return measure_remote_recurse(ftp, path, nfiles, ndirs, bytes, 0);
}

/* ------------------------------------------------------------------ */
/* Recursive delete                                                     */
/* ------------------------------------------------------------------ */
static int delete_local_recurse(const char *dir, const char *leaf, int depth,
                                DirCopyItemCb itemcb, void *ctx)
{
    struct find_t ff;
    char     pat[DC_PATHMAX];
    unsigned rc;
    DcCollect col;
    DcSubdirs subs;
    int      i, nsub, j, errors = 0;

    if (depth > DC_MAXDEPTH) return FTP_ERR_GENERAL;

    /* --- collect the level (collect-then-delete: safe against findnext) --- */
    if (!dc_init(&col, DC_LISTCAP)) return FTP_ERR_LOCALIO;

    join_local(pat, (int)sizeof(pat), dir, "*.*");
    rc = _dos_findfirst(pat, DC_AMASK, &ff);
    while (rc == 0) {
        if (!is_dot_dir(ff.name))
            dc_add(&col, ff.name, (unsigned char)((ff.attrib & _A_SUBDIR) ? 1 : 0), 0UL);
        rc = _dos_findnext(&ff);
    }

    /* --- 1) delete files --- */
    for (i = 0; i < col.count; i++) {
        char child[DC_PATHMAX];
        if (col.arr[i].is_dir) continue;
        join_local(child, (int)sizeof(child), dir, col.arr[i].name);
        if (itemcb) itemcb(ctx, col.arr[i].name, 0);
        if (remove(child) != 0) errors++;
    }

    /* --- 2) save subdirectories, free the large buffer, descend --- */
    nsub = extract_subdirs(&col, &subs);
    dc_free(&col);
    for (j = 0; j < nsub; j++) {
        char child[DC_PATHMAX];
        join_local(child, (int)sizeof(child), dir, subs.name[j]);
        if (delete_local_recurse(child, subs.name[j], depth + 1, itemcb, ctx) != FTP_OK)
            errors++;
    }
    free_subdirs(&subs);

    /* --- 3) remove the now-empty directory itself --- */
    if (itemcb) itemcb(ctx, leaf, 1);
    if (_rmdir(dir) != 0) errors++;

    return errors ? FTP_ERR_LOCALIO : FTP_OK;
}

int dircopy_delete_local(const char *path, DirCopyItemCb itemcb, void *ctx)
{
    const char *leaf = path, *p;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    return delete_local_recurse(path, leaf, 0, itemcb, ctx);
}

static int delete_remote_recurse(FtpClient *ftp, const char *dir, const char *leaf,
                                 int depth, DirCopyItemCb itemcb, void *ctx)
{
    DcCollect col;
    DcSubdirs subs;
    int       i, nsub, j, rc, errors = 0;

    if (depth > DC_MAXDEPTH) return FTP_ERR_GENERAL;

    if (!dc_init(&col, DC_LISTCAP)) return FTP_ERR_LOCALIO;

    rc = ftp->list(dir, dc_on_line, &col);
    if (rc != FTP_OK) { dc_free(&col); return rc; }

    /* --- 1) delete files (DELE with full path) --- */
    for (i = 0; i < col.count; i++) {
        char child[DC_PATHMAX];
        if (col.arr[i].is_dir) continue;
        join_remote(child, (int)sizeof(child), dir, col.arr[i].name);
        if (itemcb) itemcb(ctx, col.arr[i].name, 0);
        if (ftp->remove_file(child) != FTP_OK) errors++;
    }

    /* --- 2) save subdirectories, free buffer, descend --- */
    nsub = extract_subdirs(&col, &subs);
    dc_free(&col);
    for (j = 0; j < nsub; j++) {
        char child[DC_PATHMAX];
        join_remote(child, (int)sizeof(child), dir, subs.name[j]);
        if (delete_remote_recurse(ftp, child, subs.name[j], depth + 1, itemcb, ctx) != FTP_OK)
            errors++;
    }
    free_subdirs(&subs);

    /* --- 3) remove the now-empty directory itself (RMD) --- */
    if (itemcb) itemcb(ctx, leaf, 1);
    if (ftp->remove_dir(dir) != FTP_OK) errors++;

    return errors ? FTP_ERR_SERVER : FTP_OK;
}

int dircopy_delete_remote(FtpClient *ftp, const char *path,
                          DirCopyItemCb itemcb, void *ctx)
{
    const char *leaf = path, *p;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    return delete_remote_recurse(ftp, path, leaf, 0, itemcb, ctx);
}
