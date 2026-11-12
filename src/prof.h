/* Optional timing of the server loop, for finding where transfer time
 * goes.  Built only with -DSSHD_PROFILE (make PROFILE=1); otherwise every
 * macro is empty.  The clock is OS_ReadMonotonicTime, in centiseconds:
 * a single interval is coarse, but summed over thousands of packets the
 * totals are sound.  Totals are logged every 5 s while there is traffic. */
#ifndef PROF_H
#define PROF_H

enum prof_slot {
    PROF_IDLE,      /* in Wimp_PollIdle / Wimp_Poll, i.e. given to the desktop */
    PROF_POLL,      /* all of server_poll */
    PROF_DOPOLL,    /* ssh_event_dopoll: socket reads, decryption, MAC, callbacks */
    PROF_CBDATA,    /* our channel data callback, inside DOPOLL */
    PROF_SFTP,      /* SFTP request handling: decode, file I/O, reply */
    PROF_PUMP,      /* sending queued channel output */
    PROF_SLOTS
};

#ifdef SSHD_PROFILE
int prof_now(void);
void prof_add(enum prof_slot slot, int start);
void prof_bytes(long in);          /* channel bytes received */
void prof_round(int had_data);     /* one ssh_event_dopoll round */
void prof_report(void);            /* logs and resets every 5 s */
#define PROF_START(v)       int v = prof_now()
#define PROF_STOP(slot, v)  prof_add(slot, v)
#define PROF_BYTES(n)       prof_bytes(n)
#define PROF_ROUND(d)       prof_round(d)
#define PROF_REPORT()       prof_report()
#else
#define PROF_START(v)       ((void)0)
#define PROF_STOP(slot, v)  ((void)0)
#define PROF_BYTES(n)       ((void)0)
#define PROF_ROUND(d)       ((void)0)
#define PROF_REPORT()       ((void)0)
#endif

#endif
