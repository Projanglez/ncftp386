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

/* Compact entry for a fetched remote directory level. */
struct DcEnt {
    char          name[PANEL_NAME_MAX];
    unsigned char is_dir;
};

struct DcCollect {
    DcEnt *arr;
    int    count;
    int    cap;
    int    curYear;
};

static int dc_current_year(void)
{
    struct dosdate_t d;
    _dos_getdate(&d);
    return (int)d.year;
}

static void dc_on_line(void *vctx, const char *line)
{
    DcCollect *c = (DcCollect *)vctx;
    PanelEntry e;

    if (c->count >= c->cap) return;
    if (!ftp_parse_list_line(line, c->curYear, &e)) return;
    if (e.name[0] == '\0' || is_dot_dir(e.name)) return;

    strncpy(c->arr[c->count].name, e.name, PANEL_NAME_MAX - 1);
    c->arr[c->count].name[PANEL_NAME_MAX - 1] = '\0';
    c->arr[c->count].is_dir = e.is_dir;
    c->count++;
}

/* Save the list of subdirectory names of a fetched level compactly.
 * Returns the count (0 = none / memory error) and sets up *out. */
static int extract_subdirs(DcCollect *col, char (**out)[PANEL_NAME_MAX])
{
    int i, nd = 0, j;
    *out = 0;
    for (i = 0; i < col->count; i++) if (col->arr[i].is_dir) nd++;
    if (nd <= 0) return 0;
    *out = (char (*)[PANEL_NAME_MAX])malloc((unsigned)nd * PANEL_NAME_MAX);
    if (!*out) return 0;
    j = 0;
    for (i = 0; i < col->count; i++)
        if (col->arr[i].is_dir) {
            strncpy((*out)[j], col->arr[i].name, PANEL_NAME_MAX - 1);
            (*out)[j][PANEL_NAME_MAX - 1] = '\0';
            j++;
        }
    return nd;
}

static int download_recurse(FtpClient *ftp, const char *remoteDir,
                            const char *localDir, const char *leaf, int depth,
                            DirCopyItemCb itemcb, FtpProgressCb progcb,
                            DirCopyConflictCb conflictcb, void *ctx)
{
    DcCollect col;
    int       i, nd, j, result = FTP_OK;
    char    (*dirs)[PANEL_NAME_MAX] = 0;

    if (depth > DC_MAXDEPTH) return FTP_ERR_GENERAL;

    /* Create the local target directory (ignore if it already exists). */
    if (itemcb) itemcb(ctx, leaf, 1);
    _mkdir(localDir);

    /* --- 1) read in the entire level --- */
    col.cap     = DC_LISTCAP;
    col.count   = 0;
    col.curYear = dc_current_year();
    col.arr     = (DcEnt *)malloc((unsigned)col.cap * sizeof(DcEnt));
    if (!col.arr) return FTP_ERR_LOCALIO;

    result = ftp->list(remoteDir, dc_on_line, &col);
    if (result != FTP_OK) { free(col.arr); return result; }

    /* --- 2) fetch all files first (no recursion, no extra memory) --- */
    for (i = 0; i < col.count; i++) {
        char childR[DC_PATHMAX], childL[DC_PATHMAX];
        if (col.arr[i].is_dir) continue;
        join_remote(childR, (int)sizeof(childR), remoteDir, col.arr[i].name);
        join_local (childL, (int)sizeof(childL), localDir,  col.arr[i].name);

        if (conflictcb && local_exists(childL)) {
            int d = conflictcb(ctx, col.arr[i].name);
            if (d == DC_ABORT) { free(col.arr); return FTP_ERR_ABORT; }
            if (d == DC_SKIP)  continue;
        }
        if (itemcb) itemcb(ctx, col.arr[i].name, 0);
        result = ftp->retr(childR, childL, progcb, ctx);
        if (result != FTP_OK) { free(col.arr); return result; }
    }

    /* --- 3) save subdirectory names compactly, free the large buffer --- */
    nd = extract_subdirs(&col, &dirs);
    free(col.arr);

    /* --- 4) descend into the subdirectories --- */
    for (j = 0; j < nd; j++) {
        char childR[DC_PATHMAX], childL[DC_PATHMAX];
        join_remote(childR, (int)sizeof(childR), remoteDir, dirs[j]);
        join_local (childL, (int)sizeof(childL), localDir,  dirs[j]);
        result = download_recurse(ftp, childR, childL, dirs[j], depth + 1,
                                  itemcb, progcb, conflictcb, ctx);
        if (result != FTP_OK) break;
    }
    if (dirs) free(dirs);
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
/* Tree count (for the delete warning)                                 */
/* ------------------------------------------------------------------ */
static int count_local_recurse(const char *dir, unsigned *nf, unsigned *nd, int depth)
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
                count_local_recurse(child, nf, nd, depth + 1);
            } else {
                (*nf)++;
            }
        }
        rc = _dos_findnext(&ff);
    }
    return FTP_OK;
}

int dircopy_count_local(const char *path, unsigned *nfiles, unsigned *ndirs)
{
    (*ndirs)++;                          /* the root directory itself */
    return count_local_recurse(path, nfiles, ndirs, 0);
}

static int count_remote_recurse(FtpClient *ftp, const char *dir,
                                unsigned *nf, unsigned *nd, int depth)
{
    DcCollect col;
    char    (*subs)[PANEL_NAME_MAX] = 0;
    int       i, nsub, j, rc;

    if (depth > DC_MAXDEPTH) return FTP_ERR_GENERAL;

    col.cap     = DC_LISTCAP;
    col.count   = 0;
    col.curYear = dc_current_year();
    col.arr     = (DcEnt *)malloc((unsigned)col.cap * sizeof(DcEnt));
    if (!col.arr) return FTP_ERR_LOCALIO;

    rc = ftp->list(dir, dc_on_line, &col);
    if (rc != FTP_OK) { free(col.arr); return rc; }

    for (i = 0; i < col.count; i++) {
        if (col.arr[i].is_dir) (*nd)++; else (*nf)++;
    }
    nsub = extract_subdirs(&col, &subs);
    free(col.arr);

    for (j = 0; j < nsub; j++) {
        char child[DC_PATHMAX];
        join_remote(child, (int)sizeof(child), dir, subs[j]);
        rc = count_remote_recurse(ftp, child, nf, nd, depth + 1);
        if (rc != FTP_OK) break;
    }
    if (subs) free(subs);
    return rc;
}

int dircopy_count_remote(FtpClient *ftp, const char *path,
                         unsigned *nfiles, unsigned *ndirs)
{
    (*ndirs)++;                          /* the root directory itself */
    return count_remote_recurse(ftp, path, nfiles, ndirs, 0);
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
    DcEnt   *arr;
    char   (*subs)[PANEL_NAME_MAX] = 0;
    DcCollect col;
    int      i, nsub, j, errors = 0;

    if (depth > DC_MAXDEPTH) return FTP_ERR_GENERAL;

    /* --- collect the level (collect-then-delete: safe against findnext) --- */
    arr = (DcEnt *)malloc((unsigned)DC_LISTCAP * sizeof(DcEnt));
    if (!arr) return FTP_ERR_LOCALIO;
    col.arr = arr; col.cap = DC_LISTCAP; col.count = 0;

    join_local(pat, (int)sizeof(pat), dir, "*.*");
    rc = _dos_findfirst(pat, DC_AMASK, &ff);
    while (rc == 0 && col.count < col.cap) {
        if (!is_dot_dir(ff.name)) {
            strncpy(arr[col.count].name, ff.name, PANEL_NAME_MAX - 1);
            arr[col.count].name[PANEL_NAME_MAX - 1] = '\0';
            arr[col.count].is_dir = (unsigned char)((ff.attrib & _A_SUBDIR) ? 1 : 0);
            col.count++;
        }
        rc = _dos_findnext(&ff);
    }

    /* --- 1) delete files --- */
    for (i = 0; i < col.count; i++) {
        char child[DC_PATHMAX];
        if (arr[i].is_dir) continue;
        join_local(child, (int)sizeof(child), dir, arr[i].name);
        if (itemcb) itemcb(ctx, arr[i].name, 0);
        if (remove(child) != 0) errors++;
    }

    /* --- 2) save subdirectories, free the large buffer, descend --- */
    nsub = extract_subdirs(&col, &subs);
    free(arr);
    for (j = 0; j < nsub; j++) {
        char child[DC_PATHMAX];
        join_local(child, (int)sizeof(child), dir, subs[j]);
        if (delete_local_recurse(child, subs[j], depth + 1, itemcb, ctx) != FTP_OK)
            errors++;
    }
    if (subs) free(subs);

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
    char    (*subs)[PANEL_NAME_MAX] = 0;
    int       i, nsub, j, rc, errors = 0;

    if (depth > DC_MAXDEPTH) return FTP_ERR_GENERAL;

    col.cap     = DC_LISTCAP;
    col.count   = 0;
    col.curYear = dc_current_year();
    col.arr     = (DcEnt *)malloc((unsigned)col.cap * sizeof(DcEnt));
    if (!col.arr) return FTP_ERR_LOCALIO;

    rc = ftp->list(dir, dc_on_line, &col);
    if (rc != FTP_OK) { free(col.arr); return rc; }

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
    free(col.arr);
    for (j = 0; j < nsub; j++) {
        char child[DC_PATHMAX];
        join_remote(child, (int)sizeof(child), dir, subs[j]);
        if (delete_remote_recurse(ftp, child, subs[j], depth + 1, itemcb, ctx) != FTP_OK)
            errors++;
    }
    if (subs) free(subs);

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
