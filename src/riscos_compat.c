/* POSIX calls libssh's SFTP server expects that UnixLib does not provide
 * or that make no sense on RISC OS.  See deps/riscos-compat. */
#include <errno.h>
#include <sys/statvfs.h>

int statvfs(const char *path, struct statvfs *buf)
{
    (void)path; (void)buf;
    errno = ENOSYS;
    return -1;
}

int fstatvfs(int fd, struct statvfs *buf)
{
    (void)fd; (void)buf;
    errno = ENOSYS;
    return -1;
}
