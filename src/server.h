#ifndef SERVER_H
#define SERVER_H
#include <libssh/libssh.h>
/* Create the listening socket.  hostkey is adopted by the bind. */
int server_init(ssh_key hostkey);
/* One iteration of the cooperative main loop: accept new connections,
 * drive key exchange, dispatch packets and callbacks, pump channel output.
 * timeout_ms bounds how long it blocks.  Returns the number of live sessions. */
int server_poll(int timeout_ms);
void server_shutdown(void);
#endif
