/* Minimal <sys/statvfs.h> for RISC OS (UnixLib has none).  Used only by
 * libssh's SFTP server for the statvfs@openssh.com extension, which is
 * answered with "not supported". */
#ifndef RISCOS_COMPAT_SYS_STATVFS_H
#define RISCOS_COMPAT_SYS_STATVFS_H
#include <errno.h>
#include <sys/types.h>
struct statvfs {
    unsigned long f_bsize, f_frsize;
    unsigned long f_blocks, f_bfree, f_bavail;
    unsigned long f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
};
#define ST_RDONLY 1
#define ST_NOSUID 2
static inline int statvfs(const char *path, struct statvfs *buf)
{
    (void)path; (void)buf;
    errno = ENOSYS;
    return -1;
}
#endif
