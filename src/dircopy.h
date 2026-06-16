/* =============================================================================
 * dircopy.h - Recursive copying of entire directory trees
 * -----------------------------------------------------------------------------
 * Copies directories including all subdirectories between the local
 * filesystem and the FTP server. Both directions work path-based (no
 * CWD state changes on the server), so the remote working directory
 * stays unchanged after copying.
 *
 *   Upload   : local directory -> remote (MKD + STOR, recursive)
 *   Download : remote directory -> local (mkdir + RETR, recursive)
 *
 * Compiler: Open Watcom (wpp), Large Memory Model, 16-bit Real-Mode DOS.
 * ===========================================================================*/
#ifndef DIRCOPY_H
#define DIRCOPY_H

#include "ftpcli.h"

/* Called before each file / directory (UI: display the current name).
 * is_dir != 0 => this is a directory. */
typedef void (*DirCopyItemCb)(void *ctx, const char *name, int is_dir);

/* Decision of the conflict callback when the target file already exists. */
#define DC_OVERWRITE 0   /* overwrite this file       */
#define DC_SKIP      1   /* skip this file             */
#define DC_ABORT     2   /* abort the whole operation  */

/* Called when a target file already exists. 'name' is the file name.
 * Return value: DC_OVERWRITE / DC_SKIP / DC_ABORT. May be 0
 * (in which case it always overwrites). */
typedef int (*DirCopyConflictCb)(void *ctx, const char *name);

/* Recursive upload: create the local directory 'localDir' (full path)
 * as 'remoteName' in the current remote working directory and upload its
 * entire contents. Returns FTP_OK, FTP_ERR_ABORT (user cancelled),
 * or another FTP_ERR_* code. */
int dircopy_upload(FtpClient *ftp, const char *localDir, const char *remoteName,
                   DirCopyItemCb itemcb, FtpProgressCb progcb,
                   DirCopyConflictCb conflictcb, void *ctx);

/* Recursive download: fetch the remote directory 'remotePath' (relative to
 * the current remote directory) into 'localDir' (full local path). */
int dircopy_download(FtpClient *ftp, const char *remotePath, const char *localDir,
                     DirCopyItemCb itemcb, FtpProgressCb progcb,
                     DirCopyConflictCb conflictcb, void *ctx);

/* Tree count for the delete warning. Recursively counts ALL files and
 * directories below 'path' AND the root directory itself; the values
 * are ADDED to *nfiles / *ndirs (caller must pre-initialize to 0).
 * Returns FTP_OK or an FTP_ERR_* code. */
int dircopy_count_local (const char *path, unsigned *nfiles, unsigned *ndirs);
int dircopy_count_remote(FtpClient *ftp, const char *path,
                         unsigned *nfiles, unsigned *ndirs);

/* Recursive delete (including all subdirectories and 'path' itself). */
int dircopy_delete_local (const char *path, DirCopyItemCb itemcb, void *ctx);
int dircopy_delete_remote(FtpClient *ftp, const char *path,
                          DirCopyItemCb itemcb, void *ctx);

#endif /* DIRCOPY_H */
