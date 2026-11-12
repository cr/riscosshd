/* SPDX-License-Identifier: MIT */
/* Wimp front end: icon-bar icon, menu, Info, log and Keys windows.
 * The server itself is driven from null events via server_poll(0). */
#ifndef SSHD_WIMP_H
#define SSHD_WIMP_H
/* Register with the Wimp and create the icon and windows.  Returns -1 on
 * failure (errors reported with Wimp_ReportError where possible). */
int wimp_init(void);
/* Run the desktop event loop until Quit.  Calls server_poll() on idle. */
void wimp_run(void);
void wimp_shutdown(void);
/* Show an error box with OK (works before wimp_init too). */
void wimp_report_error(const char *msg);
void wimp_open_keys_window(void);
void wimp_open_log_window(void);
/* Show the greyed icon-bar sprite: the server did not start. */
void wimp_show_failed(void);
#endif
