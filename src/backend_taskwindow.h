/* TaskWindow backend: interactive sessions in a TaskWindow child task,
 * relayed through Wimp messages.  Only usable when the server is a Wimp
 * task; wimp.c registers the task handle and forwards the messages. */
#ifndef BACKEND_TASKWINDOW_H
#define BACKEND_TASKWINDOW_H
#include "backend.h"

#define Message_TaskWindow_Input   0x808C0
#define Message_TaskWindow_Output  0x808C1
#define Message_TaskWindow_Ego     0x808C2
#define Message_TaskWindow_Morio   0x808C3
#define Message_TaskWindow_Morite  0x808C4
#define Message_TaskWindow_NewTask 0x808C5
#define Message_TaskWindow_Suspend 0x808C6
#define Message_TaskWindow_Resume  0x808C7
#define Message_TaskCloseDown      0x400C3

extern const struct backend_ops backend_taskwindow;

/* Called by the Wimp front end once registered; 0 = not a Wimp task. */
void tw_set_task_handle(int handle);
/* 1 when running as a Wimp task, i.e. TaskWindow children can be started. */
int tw_available(void);
/* Offer a Wimp message (reason 17/18, block) to the backend.  Returns 1
 * if it was a TaskWindow message and has been handled. */
int tw_message(int reason, int *block);
#endif
