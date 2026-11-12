/* RISC OS helpers: file access through OS_File, GSTrans, and running a
 * command through OS_CLI with its VDU output captured to a buffer.
 * Everything here talks to the OS directly through SWIs so that no UnixLib
 * filename translation gets in the way of native RISC OS paths.
 */
#ifndef SSHD_RISCOS_H
#define SSHD_RISCOS_H

#include <stddef.h>

/* Expand <System$Variables> in src; returns malloc'd string or NULL. */
char *ro_gstrans(const char *src);

/* Load a whole file (native RISC OS path) into a malloc'd, NUL-terminated
 * buffer.  Returns NULL if the file does not exist or cannot be read. */
char *ro_load_file(const char *path, size_t *len_out);

/* Save a buffer to a file with the given filetype (e.g. 0xfff).  Returns
 * 0 on success, otherwise a pointer to a static error message. */
const char *ro_save_file(const char *path, int filetype, const void *data, size_t len);

/* Create a directory; succeeds if it already exists. */
const char *ro_mkdir(const char *path);

/* Append bytes to a file, created as Text if missing.  A file already
 * longer than limit bytes (0: no limit) is first renamed to <path>-old,
 * replacing the previous one, and a new file started.  Returns 0, or
 * -1 with the reason in ro_append_error. */
int ro_append_file(const char *path, const void *data, size_t len, size_t limit);
extern const char *ro_append_error;
/* Length of a file, or -1 if it is not a file. */
long ro_file_size(const char *path);

/* Delete a file; errors ignored. */
void ro_delete(const char *path);

/* Read a system variable as a string into buf.  Returns 0, or -1 if unset. */
int ro_read_var(const char *name, char *buf, size_t len);

/* Remove a system variable; errors ignored. */
void ro_unset_var(const char *name);

/* Text mode only: run a *command through OS_CLI with VDU output redirected
 * to capture_path, then read the captured output back.  *out receives a
 * malloc'd buffer (NUL-terminated, may contain \n\r pairs as RISC OS
 * writes them) and *out_len its length.  Returns the exit status to report
 * over SSH: 0 on success, 1 if the command raised a RISC OS error, 130 if
 * Escape interrupted it; *err is then set to a malloc'd message. */
int ro_run_command(const char *command, const char *capture_path,
                   char **out, size_t *out_len, char **err);

/* Set up Escape (SIGINT) handling: while a command runs an Escape only
 * aborts that command; otherwise it calls quit_fn so the server stops. */
void ro_init_signals(void (*quit_fn)(void));

/* The machine's first IPv4 address other than loopback, as text, into
 * buf; returns 0, or -1 if there is none (no network yet). */
int ro_local_address(char *buf, size_t len);

/* Centiseconds since the machine started (OS_ReadMonotonicTime), and the
 * time from one such reading to another.  The difference is taken in
 * unsigned arithmetic, so it stays right when the counter wraps. */
int ro_now_cs(void);
static inline int ro_cs_since(int then, int now)
{
    return (int)((unsigned)now - (unsigned)then);
}

/* Give the desktop a turn.  In a TaskWindow this is OS_UpCall 6, which the
 * TaskWindow module uses to pre-empt the child and to kill it when its
 * window is closed; outside a TaskWindow it returns at once. */
void ro_yield(void);

#endif
