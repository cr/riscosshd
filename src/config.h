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
};
extern struct sshd_config cfg;
#endif
