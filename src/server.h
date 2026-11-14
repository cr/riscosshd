/* SPDX-License-Identifier: MIT */
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

/* Pairing, for ssh-copy-id: for seconds, password login is offered and
 * accepts one login with a new random code (written to code, "abcd-efgh",
 * which must hold PAIR_CODE_SIZE bytes).  That login may only install
 * keys.  Pairing ends with that login, when the time runs out, after
 * PAIR_TRIES wrong codes, or with server_pair_stop().  Returns 0, or -1
 * if no random code could be had. */
#define PAIR_CODE_SIZE 10
#define PAIR_TRIES 3
int server_pair_start(char *code, int seconds);
void server_pair_stop(void);
/* Seconds of pairing left; 0 when it is off. */
int server_pair_left(void);
#endif
