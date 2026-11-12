/* RISC OS filetypes for SFTP follow the HostFS/LanManFS convention, and
 * UnixLib implements it once __riscosify_control has __RISCOSIFY_FILETYPE_EXT
 * (set in main.c): "name,xxx" names the file without the suffix and types a
 * newly created file xxx; otherwise the extension is looked up in MimeMap;
 * directory listings add ",xxx" where the extension does not imply the type.
 * Two gaps are filled here for the libssh patch: rename() strips the suffix
 * but does not retype, and a new file MimeMap knows nothing about gets the
 * configured default type.
 */
#ifndef SSHD_RISCOS_SFTP_H
#define SSHD_RISCOS_SFTP_H
/* If name ends in ",xxx", remove it and return xxx; else -1. */
int riscos_sftp_strip_suffix(char *name);
/* Set a file's type (UnixLib-style path); errors are logged, not fatal. */
void riscos_sftp_set_type(const char *unixpath, int type);
/* Called by the libssh patch after SFTP creates a file: when the name
 * carries no ",xxx" suffix and MimeMap knows nothing of its extension,
 * give it riscos_sftp_default_type instead of UnixLib's choice. */
extern int riscos_sftp_default_type;
void riscos_sftp_type_new_file(const char *unixpath);
#endif
