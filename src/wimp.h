/* Wimp front end: icon-bar icon, menu, log window, keys window.
 * The server itself is driven from null events via server_poll(0). */
#ifndef WIMP_H
#define WIMP_H
/* Register with the Wimp and create the icon and windows.  Returns -1 on
 * failure (errors reported with Wimp_ReportError where possible). */
int wimp_init(void);
/* Run the desktop event loop until Quit.  Calls server_poll() on idle. */
void wimp_run(void);
void wimp_shutdown(void);
/* Show a fatal error box (works before wimp_init too). */
void wimp_report_error(const char *msg);
void wimp_open_keys_window(void);
void wimp_open_log_window(void);
#endif
