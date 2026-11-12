#ifndef SSHD_SERVER_H
#define SSHD_SERVER_H
#include <libssh/libssh.h>
/* Create the listening socket.  hostkey is adopted by the bind. */
int server_init(ssh_key hostkey);
/* One iteration of the cooperative main loop: accept new connections,
 * drive key exchange, dispatch packets and callbacks, pump channel output.
 * The first poll waits up to timeout_ms; while data keeps arriving it polls
 * again for a few centiseconds more.  Returns the number of live sessions. */
int server_poll(int timeout_ms);
void server_shutdown(void);
/* Connections that have logged in. */
int server_logged_in(void);
#endif
