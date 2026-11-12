/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <kernel.h>
#include <swis.h>
#include "msgs.h"

#define MSGS_FILE "<SSHd$Dir>.Messages"

static int desc[4];               /* MessageTrans file descriptor */
static int is_open;

void msgs_open(void)
{
    /* R2 = 0: MessageTrans keeps the file in the RMA until closed */
    if (_swix(MessageTrans_OpenFile, _INR(0, 2), desc, MSGS_FILE, 0) == NULL)
        is_open = 1;
}

void msgs_close(void)
{
    if (is_open)
        _swix(MessageTrans_CloseFile, _IN(0), desc);
    is_open = 0;
}

void msgs_get2(const char *token, char *buf, size_t len, const char *arg0, const char *arg1)
{
    if (!is_open ||
        _swix(MessageTrans_Lookup, _INR(0, 7), desc, token, buf, (int)len, arg0, arg1, 0, 0) != NULL)
        snprintf(buf, len, "%s", token);
}
