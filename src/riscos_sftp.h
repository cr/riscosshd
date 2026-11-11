/* RISC OS filetypes for SFTP, the HostFS/LanManFS convention:
 *   - a client name ending in ",xxx" (three hex digits) names the file
 *     without the suffix and asks for filetype xxx;
 *   - otherwise the extension is looked up with MimeMap (".txt" -> Text);
 *   - listings add ",xxx" to a file whose type its extension does not
 *     already imply, so types survive a round trip.
 * Called from libssh's SFTP server through deps/patches/libssh-riscos.patch.
 * Paths are UnixLib-style ("/HostFS::HostFS.$/dir/file.txt").
 */
#ifndef RISCOS_SFTP_H
#define RISCOS_SFTP_H
#include <stddef.h>
#include <sys/stat.h>

/* If name ends in ",xxx", remove it and return xxx; else -1. */
int riscos_sftp_strip_suffix(char *name);
/* Filetype implied by the extension of a name through MimeMap, or -1. */
int riscos_sftp_type_from_extension(const char *name);
/* Filetype of an existing file (-1 if untyped, absent or not a file). */
int riscos_sftp_type_of(const char *unixpath);
/* Set a file's type; errors are logged, not fatal. */
void riscos_sftp_set_type(const char *unixpath, int type);
/* The name to show a client for leaf (a directory entry of dir): the leaf
 * itself, or leaf,xxx when its type is not what MimeMap would infer. */
const char *riscos_sftp_display_name(const char *unixpath, const char *leaf,
                                     const struct stat *st, char *out, size_t outlen);
#endif
