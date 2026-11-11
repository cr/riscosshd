/* RISC OS helpers: file access through OS_File, GSTrans, and running a
 * command through OS_CLI with its VDU output captured to a buffer.
 * Everything here talks to the OS directly through SWIs so that no UnixLib
 * filename translation gets in the way of native RISC OS paths.
 */
#ifndef RISCOS_H
#define RISCOS_H

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

/* Run a *command through OS_CLI with VDU output redirected to capture_path,
 * then read the captured output back.  *out receives a malloc'd buffer
 * (NUL-terminated, may contain \n\r pairs as RISC OS writes them) and
 * *out_len its length.  Returns the exit status to report over SSH:
 * 0 on success, 1 if the command raised a RISC OS error (the error text is
 * appended to *err as a malloc'd string). */
int ro_run_command(const char *command, const char *capture_path,
                   char **out, size_t *out_len, char **err);

/* Set up Escape (SIGINT) handling: while a command runs an Escape only
 * aborts that command; otherwise it calls quit_fn so the server stops. */
void ro_init_signals(void (*quit_fn)(void));

/* Give the desktop a turn.  In a TaskWindow this is OS_UpCall 6, which the
 * TaskWindow module uses to pre-empt the child and to kill it when its
 * window is closed; outside a TaskWindow it returns at once. */
void ro_yield(void);

#endif
