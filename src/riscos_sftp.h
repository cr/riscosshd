/* RISC OS filetypes for SFTP follow the HostFS/LanManFS convention, and
 * UnixLib implements it once __riscosify_control has __RISCOSIFY_FILETYPE_EXT
 * (set in main.c): "name,xxx" names the file without the suffix and types a
 * newly created file xxx; otherwise the extension is looked up in MimeMap;
 * directory listings add ",xxx" where the extension does not imply the type.
 * The one gap is rename(): UnixLib strips the suffix but does not retype,
 * which the helper below does for the libssh patch.
 */
#ifndef RISCOS_SFTP_H
#define RISCOS_SFTP_H
/* If name ends in ",xxx", remove it and return xxx; else -1. */
int riscos_sftp_strip_suffix(char *name);
/* Set a file's type (UnixLib-style path); errors are logged, not fatal. */
void riscos_sftp_set_type(const char *unixpath, int type);
#endif
