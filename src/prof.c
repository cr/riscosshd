/* SPDX-License-Identifier: MIT */
/* See prof.h.  Compiles to nothing without SSHD_PROFILE. */
#ifdef SSHD_PROFILE
#include "prof.h"
#include "riscos.h"
#include "log.h"

static long cs[PROF_SLOTS], calls[PROF_SLOTS];
static long bytes_in, rounds, rounds_data;
static int window_start;

void prof_add(enum prof_slot slot, int start)
{
    cs[slot] += ro_cs_since(start, ro_now_cs());
    calls[slot]++;
}

void prof_bytes(long in)
{
    bytes_in += in;
}

void prof_round(int had_data)
{
    rounds++;
    if (had_data)
        rounds_data++;
}

void prof_report(void)
{
    int now = ro_now_cs(), wall;
    if (!window_start)
        window_start = now;
    wall = ro_cs_since(window_start, now);
    if (wall < 500)
        return;
    if (bytes_in || calls[PROF_SFTP])
        log_msg("prof %dcs: idle %ld (%ld polls) server %ld (%ld): dopoll %ld (cb %ld), "
                "sftp %ld (%ld req), pump %ld; rounds %ld (%ld with data); in %ld KB/s",
                wall, cs[PROF_IDLE], calls[PROF_IDLE], cs[PROF_POLL], calls[PROF_POLL],
                cs[PROF_DOPOLL], cs[PROF_CBDATA], cs[PROF_SFTP], calls[PROF_SFTP],
                cs[PROF_PUMP], rounds, rounds_data, bytes_in * 100 / 1024 / wall);
    for (int i = 0; i < PROF_SLOTS; i++)
        cs[i] = calls[i] = 0;
    bytes_in = rounds = rounds_data = 0;
    window_start = now;
}
#endif
