#ifndef CONFIG_H
#define CONFIG_H
/* Runtime configuration, filled in by main() from the command line. */
struct sshd_config {
    int port;                   /* TCP port to listen on */
    const char *bind_addr;      /* "0.0.0.0" */
    char *config_dir;           /* RISC OS path, e.g. <Choices$Write>.SSHd */
    char *hostkey_path;         /* config_dir.hostkey */
    char *authorized_keys_path; /* config_dir.authkeys */
    char *capture_path;         /* scratch file for command output */
    char *sftp_root;            /* RISC OS directory SFTP clients start in */
    int auth_timeout;           /* seconds to log in, 0 = unlimited */
    int idle_timeout;           /* seconds logged in without a channel, 0 = unlimited */
    int max_sessions;           /* connections held at once */
    int allow_shell, allow_exec, allow_sftp;
    const char *user;           /* the one user name accepted; "" = any */
    char *banner;               /* text sent before authentication, or NULL */
    char *motd;                 /* text printed when a shell starts, or NULL */
    int default_filetype;       /* for SFTP uploads with no ",xxx" and no MimeMap match */
    int log_size_kb;            /* restart the log file past this size */
};
extern struct sshd_config cfg;
/* <config dir>.Options and a writer of its commented template (main.c) */
const char *options_path(void);
void options_ensure_file(void);
#endif
