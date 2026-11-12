/* SPDX-License-Identifier: MIT */
/* Wimp front end for sshd: icon-bar icon with a menu (Info, Help..., Log
 * window, Verbose, Keys..., Choices..., Quit), an Info window, a tail-style log
 * window drawn with the desktop font, and a Keys window where public keys
 * can be typed, pasted, dropped as a file, imported from another machine,
 * and deleted.  Windows and the menu are built in code, so there is no
 * Templates file.  All Wimp access goes through _swix; no OSLib.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <kernel.h>
#include <swis.h>
#include <libssh/libssh.h>
#include <mbedtls/version.h>

#include "wimp.h"
#include "server.h"
#include "auth.h"
#include "hostkey.h"
#include "keyfetch.h"
#include "backend_taskwindow.h"
#include "config.h"
#include "riscos.h"
#include "log.h"
#include "msgs.h"
#include "prof.h"

#ifndef Wimp_TextOp
#define Wimp_TextOp 0x400F9
#endif
#ifndef Wimp_DeleteIcon
#define Wimp_DeleteIcon 0x400C4
#endif
#ifndef Wimp_GetMenuState
#define Wimp_GetMenuState 0x400F4
#endif

#define TASK_MAGIC 0x4B534154 /* "TASK" */

/* Wimp message actions */
#define Message_Quit        0
#define Message_DataSave    1
#define Message_DataSaveAck 2
#define Message_DataLoad    3
#define Message_DataLoadAck 4
#define Message_HelpRequest 0x502
#define Message_HelpReply   0x503
/* Events never wanted: pointer leaving/entering (4, 5), caret lost/gained
 * (11, 12), message acknowledgements (19). */
#define POLL_MASK ((1 << 4) | (1 << 5) | (1 << 11) | (1 << 12) | (1 << 19))

/* Fixed icons of the Keys window; key rows are created dynamically below them. */
/* Group boxes come first so that the icons inside draw over them. */
enum { KI_BOX_HOST = 0, KI_BOX_ADD, KI_BOX_IMPORT,
       KI_HOSTLABEL, KI_HOST, KI_HOST2,
       KI_PROMPT, KI_INPUT, KI_ADD,
       KI_FETCHLABEL, KI_ACCLABEL, KI_PASSLABEL, KI_FHOST, KI_FPASS, KI_FETCH,
       KI_LISTLABEL, KI_COUNT };
#define ROW_TOP   (-640)          /* work-area y of the first row's top edge */
#define ROW_H     44
#define KEYS_W    1200
#define KEYS_MIN_EXTENT (-800)    /* keys window height with few rows */

enum { MENU_INFO = 0, MENU_HELP, MENU_LOG, MENU_VERBOSE, MENU_KEYS, MENU_OPTIONS, MENU_QUIT, MENU_ITEMS };
#define MENU_DOTTED 2             /* item flag: dotted line below */
/* Info window icons: label/value pairs */
enum { II_NAME = 0, II_PURPOSE, II_AUTHOR, II_VERSION, II_WEBSITE, II_COMPILER, II_LIBSSH, II_CRYPTO,
       II_LICENCE, II_COUNT };
#define MAX_FIXED_ICONS 20        /* largest icon count of any window defined here */

#define LOG_LINE_H 40
#define LOG_MIN_LINES 12          /* log window height before it fills */
#define LOG_WIDTH ((LOG_COLS - LOG_TIME_AT) * 16)   /* a full line in the desktop font */

/* --- Wimp data structures (as the SWIs expect them) --------------------- */

struct wimp_icon {
    int x0, y0, x1, y1;
    unsigned flags;
    union {
        char text[12];
        struct { char *text; const char *valid; int size; } ind;
    } d;
};

struct wimp_window {
    int x0, y0, x1, y1, scx, scy, behind;
    unsigned flags;
    unsigned char title_fg, title_bg, work_fg, work_bg, scroll_outer, scroll_inner, highlight, reserved;
    int ex0, ey0, ex1, ey1;
    unsigned title_flags, work_flags;
    void *sprite_area;
    short min_w, min_h;
    union {
        char text[12];
        struct { char *text; const char *valid; int size; } ind;
    } title;
    int nicons;
    struct wimp_icon icon[MAX_FIXED_ICONS];
};

struct wimp_menu_item {
    unsigned flags;
    int submenu;
    unsigned icon_flags;
    union {
        char text[12];
        struct { const char *text; const char *valid; int size; } ind;
    } d;
};

struct wimp_menu {
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int width, height, gap;
    struct wimp_menu_item item[MENU_ITEMS];
};

#define ICON_TEXT       (1u << 0)
#define ICON_SPRITE     (1u << 1)
#define ICON_BORDER     (1u << 2)
#define ICON_HCENTRE    (1u << 3)
#define ICON_VCENTRE    (1u << 4)
#define ICON_FILLED     (1u << 5)
#define ICON_INDIRECTED (1u << 8)
#define ICON_RJUST      (1u << 9)
#define ICON_BUTTON(t)  ((unsigned)(t) << 12)
#define ICON_FG(c)      ((unsigned)(c) << 24)
#define ICON_BG(c)      ((unsigned)(c) << 28)

#define WIN_MOVEABLE    (1u << 1)
#define WIN_AUTOREDRAW  (1u << 4)   /* icons only: the Wimp redraws it */
#define WIN_BACK        (1u << 24)
#define WIN_CLOSE       (1u << 25)
#define WIN_TITLE       (1u << 26)
#define WIN_TOGGLE      (1u << 27)
#define WIN_VSCROLL     (1u << 28)
#define WIN_ADJUST      (1u << 29)
#define WIN_HSCROLL     (1u << 30)
#define WIN_NEWFORMAT   (1u << 31)

/* --- state ------------------------------------------------------------- */

static int task_handle;
static int running = 1;
static int log_win = -1, keys_win = -1, info_win = -1;
static int menu_x, menu_y;
static int poll_block[64];
static int verbose_level = 1;     /* what ticking Verbose restores */
static int fetching;              /* a key import is in progress */

/* Dynamic key rows: one text icon and one Delete button per key. */
static int nrows;
static int *row_text_icon, *row_del_icon;
static char (*row_text)[160];
static char list_label[96];
static char key_input[1200];
static char fetch_host[256], fetch_pass[128];
static char host_line[128];
static char host_line2[96];
static char host_label[120];
static char log_title[40], keys_title[40];

/* Menu entries are indirected, so they may be longer than 12 characters. */
/* Menu entries are indirected, so they may be longer than 12 characters;
 * the texts come from the Messages file (load_texts). */
static char menu_texts[MENU_ITEMS][32];
static const char *const menu_tokens[MENU_ITEMS] = {
    "MInfo", "MHelp", "MLog", "MVerbose", "MKeys", "MChoices", "MQuit"
};
#define MENU_ITEM_FLAGS (ICON_TEXT | ICON_FILLED | ICON_INDIRECTED | ICON_FG(7) | ICON_BG(0))
#define MENU_ITEM(i, f) { f, -1, MENU_ITEM_FLAGS, { .ind = { menu_texts[i], (const char *)-1, sizeof menu_texts[i] } } }
enum { LMENU_OPEN = 0, LMENU_CLEAR, LMENU_ITEMS };
static char log_menu_texts[LMENU_ITEMS][32];
static const char *const log_menu_tokens[LMENU_ITEMS] = { "LMOpen", "LMClear" };
static struct {
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int width, height, gap;
    struct wimp_menu_item item[LMENU_ITEMS];
} log_menu = {
    "Log", 7, 2, 7, 0, 240, 44, 0,
    {
        { 0,    -1, MENU_ITEM_FLAGS, { .ind = { log_menu_texts[0], (const char *)-1, sizeof log_menu_texts[0] } } },
        { 0x80, -1, MENU_ITEM_FLAGS, { .ind = { log_menu_texts[1], (const char *)-1, sizeof log_menu_texts[1] } } },
    }
};
static void *current_menu;         /* the menu last opened: icon bar or log */

static struct wimp_menu menu = {
    "SSHd", 7, 2, 7, 0, 220, 44, 0,
    {
        MENU_ITEM(MENU_INFO, 0), MENU_ITEM(MENU_HELP, MENU_DOTTED), MENU_ITEM(MENU_LOG, 0),
        MENU_ITEM(MENU_VERBOSE, MENU_DOTTED), MENU_ITEM(MENU_KEYS, 0),
        MENU_ITEM(MENU_OPTIONS, MENU_DOTTED), MENU_ITEM(MENU_QUIT, 0x80),
    }
};

/* --- helpers ------------------------------------------------------------ */

/* Wimp_ReportError flags, RISC OS 3.5 style: bit 0 OK, bit 1 Cancel,
 * bit 8 new style, bits 9-11 category (the title reads "Error from" for
 * errors, "Message from" otherwise).  R3/R4 name the application sprite,
 * R5 lists extra buttons, which return 3, 4, ... */
#define BOX_OK        1
#define BOX_CANCEL    2
#define BOX_NEW       (1 << 8)
#define BOX_INFO      (1 << 9)
#define BOX_ERROR     (2 << 9)
#define BOX_QUESTION  (4 << 9)

static int boxf(unsigned flags, const char *buttons, const char *fmt, ...) PRINTF_LIKE(3, 4);
static int boxf(unsigned flags, const char *buttons, const char *fmt, ...)
{
    struct { int num; char text[252]; } b;
    int answer = 0;
    va_list ap;
    b.num = 0;
    va_start(ap, fmt);
    vsnprintf(b.text, sizeof b.text, fmt, ap);
    va_end(ap);
    if (_swix(Wimp_ReportError, _INR(0, 5) | _OUT(1), &b, flags | BOX_NEW, "SSHd", "!sshd", 1,
              buttons, &answer) != NULL) {
        fprintf(stderr, "SSHd: %s\n", b.text);
        return 0;
    }
    return answer;
}

/* An error box with OK, which works before wimp_init too; a message box
 * with OK for news that is not an error; and a question with an action
 * button and Cancel, true for the action. */
#define reportf(...)     boxf(BOX_OK | BOX_ERROR, NULL, __VA_ARGS__)
#define tellf(...)       boxf(BOX_OK | BOX_INFO, NULL, __VA_ARGS__)
#define ask(action, ...) (boxf(BOX_CANCEL | BOX_QUESTION, action, __VA_ARGS__) == 3)

void wimp_report_error(const char *msg)
{
    reportf("%s", msg);
}

static void icon(struct wimp_icon *ic, int x0, int y0, int x1, int y1, unsigned flags,
                 char *text, int size, const char *valid)
{
    ic->x0 = x0; ic->y0 = y0; ic->x1 = x1; ic->y1 = y1;
    ic->flags = flags | ICON_INDIRECTED;
    ic->d.ind.text = text;
    ic->d.ind.valid = valid ? valid : (const char *)-1;
    ic->d.ind.size = size;
}

/* Create an icon in win (-1: the icon bar); returns its handle, or -1 with
 * the error reported when what names it. */
static int create_icon(int win, struct wimp_icon *ic, const char *what)
{
    struct { int window; struct wimp_icon ic; } blk;
    int handle = -1;
    _kernel_oserror *e;
    blk.window = win;
    blk.ic = *ic;
    e = _swix(Wimp_CreateIcon, _INR(0, 1) | _OUT(0), 0, &blk, &handle);
    if (e) {
        if (what)
            reportf("%s: %s", what, e->errmess);
        return -1;
    }
    return handle;
}

/* Redraw an indirected icon after its text buffer changed. */
static void redraw_icon(int win, int ic)
{
    int block[4] = { win, ic, 0, 0 };
    _swix(Wimp_SetIconState, _IN(1), block);
}

static void delete_icon(int win, int icon_handle)
{
    int block[2] = { win, icon_handle };
    if (icon_handle >= 0)
        _swix(Wimp_DeleteIcon, _IN(1), block);
}

static void set_caret(int win, int ic, const char *text)
{
    _swix(Wimp_SetCaretPosition, _INR(0, 5), win, ic, 0, 0, -1, (int)strlen(text));
}

/* Screen size in OS units, from the current mode. */
static void screen_size(int *w, int *h)
{
    int xl = 1279, yl = 1023, xe = 1, ye = 1;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 11, &xl);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 12, &yl);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 4, &xe);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 5, &ye);
    *w = (xl + 1) << xe;
    *h = (yl + 1) << ye;
}

/* Open a window on top of the stack: centred on the screen the first time
 * (a window that is already open stays where it is), where the user left
 * it afterwards. */
static void open_on_top(int win)
{
    static int centred[3];
    static int nc;
    int state[9], i;
    if (win < 0)
        return;
    state[0] = win;
    if (_swix(Wimp_GetWindowState, _IN(1), state))
        return;
    for (i = 0; i < nc && centred[i] != win; i++)
        ;
    if (i == nc && nc < 3 && !(state[8] & (1 << 16))) {
        int sw, sh, w = state[3] - state[1], h = state[4] - state[2];
        screen_size(&sw, &sh);
        if (w > sw) w = sw;
        if (h > sh - 160) h = sh - 160;   /* clear of the icon bar */
        state[1] = (sw - w) / 2;
        state[3] = state[1] + w;
        state[2] = 132 + (sh - 132 - h) / 2;
        state[4] = state[2] + h;
        centred[nc++] = win;
    }
    state[7] = -1;
    _swix(Wimp_OpenWindow, _IN(1), state);
}

/* --- keys window rows ---------------------------------------------------- */

static char delete_text[24];

/* Rebuild the key rows as icons and size the window to fit them. */
static void refresh_keys_list(void)
{
    int n = auth_count();
    int ext[4];

    if (keys_win < 0)
        return;
    for (int i = 0; i < nrows; i++) {
        delete_icon(keys_win, row_text_icon[i]);
        delete_icon(keys_win, row_del_icon[i]);
    }
    if (n > 0) {
        int *a = realloc(row_text_icon, sizeof *a * (size_t)n);
        int *b = realloc(row_del_icon, sizeof *b * (size_t)n);
        char (*t)[160] = realloc(row_text, sizeof *t * (size_t)n);
        if (a) row_text_icon = a;
        if (b) row_del_icon = b;
        if (t) row_text = t;
        if (!a || !b || !t)
            n = 0;
    }
    for (int i = 0; i < n; i++) {
        struct wimp_icon ic;
        int top = ROW_TOP - i * ROW_H;
        auth_describe_one(i, row_text[i], sizeof row_text[i]);
        icon(&ic, 16, top - 40, KEYS_W - 200, top,
             ICON_TEXT | ICON_BORDER | ICON_VCENTRE | ICON_FILLED | ICON_FG(7) | ICON_BG(0),
             row_text[i], sizeof row_text[i], "R2");
        row_text_icon[i] = create_icon(keys_win, &ic, NULL);
        icon(&ic, KEYS_W - 184, top - 40, KEYS_W - 16, top,
             ICON_TEXT | ICON_BORDER | ICON_HCENTRE | ICON_VCENTRE | ICON_FILLED | ICON_BUTTON(3) |
             ICON_FG(7) | ICON_BG(1),
             delete_text, sizeof delete_text, "R5,3");
        row_del_icon[i] = create_icon(keys_win, &ic, NULL);
    }
    /* the area to redraw covers the old rows as well as the new ones */
    ext[1] = ROW_TOP - (n > nrows ? n : nrows) * ROW_H - 16;
    nrows = n;
    if (n == 0)
        msgs_get("LNone", list_label, sizeof list_label, NULL);
    else {
        char num[12];
        snprintf(num, sizeof num, "%d", n);
        msgs_get("LCount", list_label, sizeof list_label, num);
    }
    redraw_icon(keys_win, KI_LISTLABEL);
    _swix(Wimp_ForceRedraw, _INR(0, 4), keys_win, 0, ext[1], KEYS_W, ROW_TOP + 4);

    ext[0] = 0; ext[1] = ROW_TOP - n * ROW_H - 16; ext[2] = KEYS_W; ext[3] = 0;
    if (ext[1] > KEYS_MIN_EXTENT)
        ext[1] = KEYS_MIN_EXTENT;
    _swix(Wimp_SetExtent, _INR(0, 1), keys_win, ext);
}

static int row_of_delete_icon(int icon_handle)
{
    for (int i = 0; i < nrows; i++)
        if (row_del_icon[i] == icon_handle)
            return i;
    return -1;
}

static void keys_delete_row(int i)
{
    char err[128], desc[160];
    if (i < 0 || i >= auth_count())
        return;
    auth_describe_one(i, desc, sizeof desc);
    if (!ask("Delete", "Remove the key %s? It will no longer be able to log in; "
                       "sessions already open stay open.", desc))
        return;
    if (auth_remove_key(i, err, sizeof err) < 0)
        reportf("Key not deleted: %s", err);
    refresh_keys_list();
}

/* --- log window ---------------------------------------------------------- */

static void log_redraw_rows(int from, int to)
{
    _swix(Wimp_ForceRedraw, _INR(0, 4), log_win, 0, -((to + 1) * LOG_LINE_H), LOG_WIDTH,
          -(from * LOG_LINE_H));
}

/* A new line was logged: grow the window while the ring fills, scroll to
 * the end if the window was showing the end (the "tail -f" behaviour;
 * someone reading older lines is not dragged down), and redraw what
 * changed.  With force the window scrolls to the end regardless. */
static void log_update(int force)
{
    static int extent_rows;               /* rows the extent was last set for */
    int state[9], n = log_count(), rows = n < LOG_MIN_LINES ? LOG_MIN_LINES : n;
    if (log_win < 0)
        return;
    if (rows != extent_rows) {             /* changes only while the ring fills */
        int ext[4] = { 0, -(rows * LOG_LINE_H), LOG_WIDTH, 0 };
        _swix(Wimp_SetExtent, _INR(0, 1), log_win, ext);
        extent_rows = rows;
    }
    state[0] = log_win;
    if (_swix(Wimp_GetWindowState, _IN(1), state) || !(state[8] & (1 << 16)))
        return;                            /* closed: drawn when opened */
    {
        int vis_h = state[4] - state[2];
        int bottom = rows * LOG_LINE_H > vis_h ? -(rows * LOG_LINE_H - vis_h) : 0;
        /* before this line was added the end was one line higher */
        if (force || state[6] <= bottom + LOG_LINE_H) {
            state[6] = bottom;
            _swix(Wimp_OpenWindow, _IN(1), state);
        }
    }
    if (n < LOG_LINES)
        log_redraw_rows(n - 1, n - 1);     /* only the new row is new */
    else
        log_redraw_rows(0, LOG_LINES - 1); /* every row moved up by one */
}

static void log_notify(void)
{
    log_update(0);
}

static void redraw_log(int *block)
{
    int more = 0, n = log_count();
    _swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), block, &more);
    _swix(Wimp_TextOp, _INR(0, 2), 0, 0x00000000, 0xFFFFFF00);
    while (more) {
        /* block: 1..4 visible area, 5,6 scroll, 7..10 clip (screen coords).
         * Work-area (x, y) maps to screen (x - scx + vis_x0, y - scy + vis_y1). */
        int scx = block[5], scy = block[6], vis_x0 = block[1], vis_y1 = block[4];
        int clip_y0 = block[8] + scy - vis_y1;   /* work-area coords */
        int clip_y1 = block[10] + scy - vis_y1;
        int first = (-clip_y1) / LOG_LINE_H - 1;
        int last = (-clip_y0) / LOG_LINE_H + 1;
        if (first < 0)
            first = 0;
        if (last > n - 1)
            last = n - 1;
        for (int i = first; i <= last; i++) {
            /* reason 2 | bit 30: baseline at Y, screen coordinates.  Row i
             * occupies work-area y in (-(i+1)*LH, -i*LH]; baseline 10 up from
             * the bottom of the row. */
            int wy = -((i + 1) * LOG_LINE_H) + 10;
            _swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 30), log_line(i) + LOG_TIME_AT, -1, -1,
                  8 - scx + vis_x0, wy - scy + vis_y1);
        }
        _swix(Wimp_GetRectangle, _IN(1) | _OUT(0), block, &more);
    }
}

void wimp_open_log_window(void)
{
    open_on_top(log_win);
    log_update(1);
}

/* --- keys window --------------------------------------------------------- */

void wimp_open_keys_window(void)
{
    if (keys_win < 0)
        return;
    refresh_keys_list();
    {
        char hint[96];
        connect_hint(hint, sizeof hint);      /* now: the network may have come up late */
        if (hint[0])
            msgs_get("LConnect", host_label, sizeof host_label, hint);
        else
            msgs_get("LHost", host_label, sizeof host_label, NULL);
    }
    snprintf(host_line, sizeof host_line, "%s",
             hostkey_fingerprint()[0] ? hostkey_fingerprint() : "(not loaded: the server did not start)");
    redraw_icon(keys_win, KI_HOSTLABEL);
    redraw_icon(keys_win, KI_HOST);
    open_on_top(keys_win);
    set_caret(keys_win, KI_INPUT, key_input);
}

static void keys_add_from_input(void)
{
    char err[128];
    int added, dup, bad;
    if (key_input[0] == '\0')
        return;
    if (auth_add_keys(key_input, &added, &dup, &bad, err, sizeof err) < 0 || added == 0) {
        reportf("Key not added: %s", err);
        refresh_keys_list();               /* it may apply for this session */
        return;
    }
    key_input[0] = '\0';
    redraw_icon(keys_win, KI_INPUT);
    set_caret(keys_win, KI_INPUT, key_input);
    refresh_keys_list();
}

/* A file dropped on the keys window: add every key it contains. */
static void keys_add_from_file(const char *path)
{
    char err[128];
    int added, dup, bad, rc;
    char *data = ro_load_file(path, NULL);
    if (!data) {
        reportf("Cannot read %s", path);
        return;
    }
    if (strstr(data, "PRIVATE KEY-----")) {
        free(data);
        reportf("That file is a private key. Drop the matching .pub file instead, "
                "and keep the private key secret.");
        return;
    }
    rc = auth_add_keys(data, &added, &dup, &bad, err, sizeof err);
    free(data);
    refresh_keys_list();
    if (rc < 0)
        reportf("%d key%s %s", added, added == 1 ? "" : "s", err);
    else if (added == 0 || dup || bad)     /* an error only if nothing was a key */
        boxf(BOX_OK | (added == 0 && bad ? BOX_ERROR : BOX_INFO), NULL,
             "%d key%s added, %d already authorised, %d line%s not a key",
             added, added == 1 ? "" : "s", dup, bad, bad == 1 ? "" : "s");
}

/* --- fetch keys from another machine ------------------------------------- */

static void dispatch(int reason, int *block);

/* Handle pending desktop events (redraws after a dialogue box went away,
 * TaskWindow messages for live sessions) before blocking again in the SSH
 * client.  Returns at the first null event or after ~half a second. */
static void wimp_pump(void)
{
    int start = ro_now_cs();
    for (;;) {
        int reason = 0;
        if (_swix(Wimp_Poll, _INR(0, 1) | _OUT(0), POLL_MASK, poll_block, &reason))
            return;
        if (reason == 0)
            return;
        dispatch(reason, poll_block);
        if (ro_cs_since(start, ro_now_cs()) > 50)
            return;
    }
}

static int fetch_confirm(const char *fingerprint, const char *host, unsigned port)
{
    int ok;
    _swix(Hourglass_Off, 0);
    /* the fingerprint first, so that a long host name cannot cut it off */
    ok = ask("Continue", "Host key %s from %s port %u. Continue only if it matches "
                         "'ssh-keygen -lf' of /etc/ssh/ssh_host_*_key.pub on that machine.",
             fingerprint, host, port);
    wimp_pump();
    _swix(Hourglass_On, 0);
    return ok;
}

static void keys_fetch(void)
{
    char msg[300];
    int rc;
    if (fetching)                          /* a click queued during the import */
        return;
    fetching = 1;
    _swix(Hourglass_On, 0);
    rc = keyfetch_run(fetch_host, fetch_pass, fetch_confirm, msg, sizeof msg);
    _swix(Hourglass_Off, 0);
    memset(fetch_pass, 0, sizeof fetch_pass);          /* used once, never kept */
    redraw_icon(keys_win, KI_FPASS);
    refresh_keys_list();
    boxf(BOX_OK | (rc > 0 ? BOX_INFO : BOX_ERROR), NULL, "%s", msg);
    fetching = 0;
}

/* --- creation ------------------------------------------------------------ */

static void window_def(struct wimp_window *w, int x0, int y0, int x1, int y1, unsigned flags,
                       int work_bg, int ey0, int ex1, char *title, int title_size)
{
    memset(w, 0, sizeof *w);
    w->x0 = x0; w->y0 = y0; w->x1 = x1; w->y1 = y1; w->behind = -1;
    w->flags = WIN_NEWFORMAT | WIN_MOVEABLE | WIN_BACK | WIN_CLOSE | WIN_TITLE | flags;
    w->title_fg = 7; w->title_bg = 2; w->work_fg = 7; w->work_bg = (unsigned char)work_bg;
    w->scroll_outer = 3; w->scroll_inner = 1; w->highlight = 12;
    w->ex0 = 0; w->ey0 = ey0; w->ex1 = ex1; w->ey1 = 0;
    w->title_flags = ICON_TEXT | ICON_HCENTRE | ICON_VCENTRE | ICON_INDIRECTED;
    w->sprite_area = (void *)1;
    w->title.ind.text = title; w->title.ind.valid = (const char *)-1; w->title.ind.size = title_size;
}

static int create_window(struct wimp_window *w)
{
    int handle = -1;
    _kernel_oserror *e = _swix(Wimp_CreateWindow, _IN(1) | _OUT(0), w, &handle);
    if (e) {
        reportf("Cannot create window: %s", e->errmess);
        return -1;
    }
    return handle;
}

static char info_title[40];
static char info_labels[II_COUNT][24];
static const char *const info_tokens[II_COUNT] = {
    "IName", "IPurpose", "IAuthor", "IVersion", "IWebsite", "ICompiler", "ILibssh", "ICrypto", "ILicences"
};
static char info_values[II_COUNT][96] = {
    "SSHd", "", "\xa9 2026 Christiane Ruetten", "", "https://github.com/cr/riscosshd",
    "GCC " __VERSION__ " (GCCSDK, UnixLib)", "",
    "mbedTLS " MBEDTLS_VERSION_STRING ", CryptRandom entropy", "MIT, LGPL 2.1, Apache 2.0: see !Help"
};

/* "0.1.0-3 (12 Nov 2026)": the Style Guide's version line.  The date is
 * the build's SOURCE_DATE_EPOCH (CMakeLists.txt), not the time of
 * compiling, so that builds are reproducible. */
#ifndef SSHD_BUILD_DATE
#define SSHD_BUILD_DATE "unknown date"
#endif
static void info_version(void)
{
    snprintf(info_values[II_VERSION], sizeof info_values[II_VERSION], "%s (%s)",
             SSHD_VERSION, SSHD_BUILD_DATE);
    /* a build that is not the release itself says which commit it is */
    if (strcmp(SSHD_GIT, "v" SSHD_VERSION) != 0)
        snprintf(info_values[II_VERSION] + strlen(info_values[II_VERSION]),
                 sizeof info_values[II_VERSION] - strlen(info_values[II_VERSION]), ", %s", SSHD_GIT);
}
static char prompt_text[128];
static char group_dummy[] = "";
static char add_text[16], fetch_btn_text[16];
static char fetch_text[96];
static char acc_text[48], pass_text[48];

/* Every translatable text into its buffer, before the windows are made. */
static void load_texts(void)
{
    size_t longest = 0;
    for (int i = 0; i < MENU_ITEMS; i++) {
        msgs_get(menu_tokens[i], menu_texts[i], sizeof menu_texts[i], NULL);
        if (strlen(menu_texts[i]) > longest)
            longest = strlen(menu_texts[i]);
    }
    if ((int)longest * 16 + 32 > menu.width)
        menu.width = (int)longest * 16 + 32;
    msgs_get("TLog", log_title, sizeof log_title, NULL);
    for (int i = 0; i < LMENU_ITEMS; i++)
        msgs_get(log_menu_tokens[i], log_menu_texts[i], sizeof log_menu_texts[i], NULL);
    msgs_get("LMTitle", log_menu.title, sizeof log_menu.title, NULL);
    msgs_get("TKeys", keys_title, sizeof keys_title, NULL);
    msgs_get("TInfo", info_title, sizeof info_title, NULL);
    msgs_get("LHost2", host_line2, sizeof host_line2, NULL);
    msgs_get("LPrompt", prompt_text, sizeof prompt_text, NULL);
    msgs_get("LAdd", add_text, sizeof add_text, NULL);
    msgs_get("LFetch", fetch_text, sizeof fetch_text, NULL);
    msgs_get("LAccount", acc_text, sizeof acc_text, NULL);
    msgs_get("LPassword", pass_text, sizeof pass_text, NULL);
    msgs_get("LImport", fetch_btn_text, sizeof fetch_btn_text, NULL);
    msgs_get("LDelete", delete_text, sizeof delete_text, NULL);
    for (int i = 0; i < II_COUNT; i++)
        msgs_get(info_tokens[i], info_labels[i], sizeof info_labels[i], NULL);
    msgs_get("IPurposeV", info_values[II_PURPOSE], sizeof info_values[II_PURPOSE], NULL);
    info_version();
}

static void create_windows(void)
{
    struct wimp_window w;              /* Wimp_CreateWindow copies it */
    const unsigned text_flags = ICON_TEXT | ICON_VCENTRE | ICON_FG(7);
    const unsigned title_flags = ICON_TEXT | ICON_VCENTRE | ICON_FILLED | ICON_FG(7) | ICON_BG(1);
    const unsigned box_flags = ICON_TEXT | ICON_BORDER | ICON_VCENTRE | ICON_FILLED | ICON_FG(7) | ICON_BG(0);
    const unsigned button_flags = ICON_TEXT | ICON_BORDER | ICON_HCENTRE | ICON_VCENTRE | ICON_FILLED |
                                  ICON_BUTTON(3) | ICON_FG(7) | ICON_BG(1);

    /* Log window: scrolling text, drawn in redraw_log(). */
    window_def(&w, 100, 200, 1100, 680, WIN_TOGGLE | WIN_VSCROLL | WIN_ADJUST | WIN_HSCROLL,
               0, -(LOG_MIN_LINES * LOG_LINE_H), LOG_WIDTH, log_title, sizeof log_title);
    w.work_flags = ICON_BUTTON(3);     /* clicks reported: Menu opens the log menu */
    log_win = create_window(&w);

    /* Keys window: host key, add and import sections, then the key rows. */
    window_def(&w, 40, 100, 40 + KEYS_W, 920, WIN_AUTOREDRAW | WIN_TOGGLE | WIN_VSCROLL | WIN_ADJUST,
               1, KEYS_MIN_EXTENT, KEYS_W, keys_title, sizeof keys_title);
    w.work_flags = ICON_BUTTON(10);    /* click/drag so DataLoad hits the window */
    w.nicons = KI_COUNT;
    /* Section 1: server host key.  Group titles sit on the top border, with
     * a filled background in the window colour to mask the channel line. */
    icon(&w.icon[KI_BOX_HOST], 16, -168, KEYS_W - 16, -20, ICON_BORDER, group_dummy, sizeof group_dummy, "R4");
    icon(&w.icon[KI_HOSTLABEL], 40, -40, KEYS_W - 40, 0, title_flags, host_label, sizeof host_label, NULL);
    icon(&w.icon[KI_HOST], 40, -104, KEYS_W - 40, -64, text_flags, host_line, sizeof host_line, NULL);
    icon(&w.icon[KI_HOST2], 40, -148, KEYS_W - 40, -108, text_flags, host_line2, sizeof host_line2, NULL);
    /* Section 2: add a key by hand. */
    icon(&w.icon[KI_BOX_ADD], 16, -368, KEYS_W - 16, -212, ICON_BORDER, group_dummy, sizeof group_dummy, "R4");
    icon(&w.icon[KI_PROMPT], 40, -232, KEYS_W - 40, -192, title_flags, prompt_text, sizeof prompt_text, NULL);
    /* Validation K: t Tab and a arrows move between fields; r (account
     * field only) makes Return move on too, so Return in the key and
     * password fields reaches key_pressed and acts. */
    icon(&w.icon[KI_INPUT], 40, -308, KEYS_W - 40, -256, box_flags | ICON_BUTTON(15), key_input, sizeof key_input,
         "Kta;Pptr_write");
    /* R6: the default action button, as Return in the field does Add */
    icon(&w.icon[KI_ADD], 40, -360, 220, -312, button_flags, add_text, sizeof add_text, "R6,3");
    /* Section 3: import from another machine. */
    icon(&w.icon[KI_BOX_IMPORT], 16, -560, KEYS_W - 16, -412, ICON_BORDER, group_dummy, sizeof group_dummy, "R4");
    icon(&w.icon[KI_FETCHLABEL], 40, -432, KEYS_W - 40, -392, title_flags, fetch_text, sizeof fetch_text, NULL);
    icon(&w.icon[KI_ACCLABEL], 40, -488, 620, -448, text_flags, acc_text, sizeof acc_text, NULL);
    icon(&w.icon[KI_PASSLABEL], 640, -488, 980, -448, text_flags, pass_text, sizeof pass_text, NULL);
    icon(&w.icon[KI_FHOST], 40, -544, 620, -492, box_flags | ICON_BUTTON(15), fetch_host, sizeof fetch_host,
         "Ktar;Pptr_write");
    icon(&w.icon[KI_FPASS], 640, -544, 980, -492, box_flags | ICON_BUTTON(15), fetch_pass, sizeof fetch_pass,
         "Kta;D*;Pptr_write");
    icon(&w.icon[KI_FETCH], 1000, -544, KEYS_W - 40, -492, button_flags, fetch_btn_text, sizeof fetch_btn_text, "R5,3");
    /* Section 4: the list, growing downwards. */
    icon(&w.icon[KI_LISTLABEL], 16, -636, KEYS_W - 16, -596, text_flags, list_label, sizeof list_label, NULL);
    keys_win = create_window(&w);

    /* Info window: the usual label/value box, icons only; it is the Info
     * menu item's submenu, so the Wimp opens it on its own. */
    snprintf(info_values[II_LIBSSH], sizeof info_values[II_LIBSSH], "%s", ssh_version(0));
    window_def(&w, 300, 360, 1240, 780, WIN_AUTOREDRAW, 1, -(16 + II_COUNT * 44), 940,
               info_title, sizeof info_title);
    w.flags &= ~(WIN_BACK | WIN_CLOSE);  /* an Info box has a title bar only */
    w.nicons = II_COUNT * 2;
    for (int i = 0; i < II_COUNT; i++) {
        int top = -16 - i * 44;
        icon(&w.icon[2 * i], 16, top - 36, 200, top, ICON_TEXT | ICON_VCENTRE | ICON_RJUST | ICON_FG(7),
             info_labels[i], sizeof info_labels[i], NULL);
        icon(&w.icon[2 * i + 1], 216, top - 40, 924, top + 4, box_flags, info_values[i], sizeof info_values[i], "R2");
    }
    info_win = create_window(&w);
}

static char sprite_name[12] = "!sshd";
static int iconbar_icon = -1;

void wimp_show_failed(void)
{
    /* the greyed sprite, so that a failed start shows without opening the log */
    snprintf(sprite_name, sizeof sprite_name, "sshdfail");
    if (iconbar_icon >= 0)
        redraw_icon(-2, iconbar_icon);
}

static int create_iconbar_icon(void)
{
    struct wimp_icon ic;
    icon(&ic, 0, 0, 68, 68, ICON_SPRITE | ICON_HCENTRE | ICON_VCENTRE | ICON_BUTTON(3),
         sprite_name, sizeof sprite_name, NULL);
    ic.d.ind.valid = (const char *)1;      /* sprite area: Wimp pool */
    iconbar_icon = create_icon(-1, &ic, "Cannot create icon bar icon");
    return iconbar_icon < 0 ? -1 : 0;
}

int wimp_init(void)
{
    static int messages[] = { Message_DataSave, Message_DataLoad, Message_HelpRequest,
                              Message_TaskWindow_Output, Message_TaskWindow_Ego,
                              Message_TaskWindow_Morio, Message_TaskCloseDown, 0 };
    _kernel_oserror *e = _swix(Wimp_Initialise, _INR(0, 3) | _OUT(1), 350, TASK_MAGIC, "SSHd",
                               messages, &task_handle);
    if (e) {
        reportf("Wimp_Initialise: %s", e->errmess);
        return -1;
    }
    tw_set_task_handle(task_handle);
    msgs_open();
    load_texts();
    create_windows();
    if (create_iconbar_icon() < 0 || log_win < 0 || keys_win < 0 || info_win < 0) {
        wimp_shutdown();        /* leave no half-registered task behind */
        return -1;
    }
    log_set_notify(log_notify);
    log_update(0);              /* size the window for the lines logged so far */
    menu.item[MENU_INFO].submenu = info_win;   /* Style Guide: Info is a submenu box */
    refresh_keys_list();
    return 0;
}

/* --- event loop ----------------------------------------------------------- */

/* Open (or, after an Adjust click, reopen) the icon bar menu with the
 * Verbose tick up to date. */
static void show_menu(void)
{
    menu.item[MENU_VERBOSE].flags = (menu.item[MENU_VERBOSE].flags & ~1u) | (log_verbose ? 1u : 0u);
    current_menu = &menu;
    _swix(Wimp_CreateMenu, _INR(1, 3), &menu, menu_x, menu_y);
}

static void show_log_menu(void)
{
    current_menu = &log_menu;
    _swix(Wimp_CreateMenu, _INR(1, 3), &log_menu, menu_x, menu_y);
}

/* The log window's title: port and logged-in sessions, as a status line. */
static void log_title_update(void)
{
    static int shown = -1;
    int n = server_logged_in();
    char port[12], num[12];
    if (n == shown || log_win < 0)
        return;
    shown = n;
    snprintf(port, sizeof port, "%d", cfg.port);
    snprintf(num, sizeof num, "%d", n);
    msgs_get2("TLogStatus", log_title, sizeof log_title, port, num);
    _swix(Wimp_ForceRedraw, _INR(0, 2), log_win, TASK_MAGIC, 3);   /* title bar only */
}

static void open_menu(int x)
{
    int height = 96;                       /* above the icon bar */
    for (int i = 0; i < MENU_ITEMS; i++)
        height += 44 + ((menu.item[i].flags & MENU_DOTTED) ? 24 : 0);
    menu_x = x - 64;
    menu_y = height;
    show_menu();
}

static void open_options(void)
{
    /* The settings are a text file: hand it to the user's editor.  It is
     * written on first run, and again here if it has gone. */
    char cmd[320];
    _kernel_oserror *e;
    options_ensure_file();
    snprintf(cmd, sizeof cmd, "Filer_Run %s", options_path());
    e = _swix(OS_CLI, _IN(0), cmd);
    if (e)
        reportf("%s", e->errmess);
    else
        log_msg("editing %s: changes apply when SSHd is next started", options_path());
}

static void log_menu_selection(int *items)
{
    int ptr[5];
    if (items[0] == LMENU_OPEN) {
        char cmd[320];
        if (!log_file_name())
            reportf("No log file is written (the log setting is off)");
        else {
            snprintf(cmd, sizeof cmd, "Filer_Run %s", log_file_name());
            if (_swix(OS_CLI, _IN(0), cmd) != NULL)
                reportf("Cannot open the log file");
        }
    } else if (items[0] == LMENU_CLEAR) {
        log_clear();
        log_update(1);
        _swix(Wimp_ForceRedraw, _INR(0, 4), log_win, 0, -(LOG_LINES * LOG_LINE_H), LOG_WIDTH, 0);
    }
    if (_swix(Wimp_GetPointerInfo, _IN(1), ptr) == NULL && (ptr[2] & 1))
        show_log_menu();
}

static void menu_selection(int *items)
{
    int ptr[5];
    if (current_menu == &log_menu) {
        log_menu_selection(items);
        return;
    }
    switch (items[0]) {
    case MENU_INFO:    open_on_top(info_win); break;
    case MENU_LOG:     wimp_open_log_window(); break;
    case MENU_VERBOSE:
        /* untick remembers the level, so -vv comes back as 2 */
        if (log_verbose) {
            verbose_level = log_verbose;
            log_verbose = 0;
        } else {
            log_verbose = verbose_level;
        }
        log_msg("verbose logging %s", log_verbose ? "on" : "off");
        break;
    case MENU_KEYS:    wimp_open_keys_window(); break;
    case MENU_HELP:
        if (_swix(OS_CLI, _IN(0), "Filer_Run <SSHd$Dir>.!Help") != NULL)
            reportf("Cannot open the help file");
        break;
    case MENU_OPTIONS: open_options(); break;
    case MENU_QUIT: {
        int n = server_logged_in();
        if (n == 0 || ask("Quit", "%d SSH session%s open. Quitting SSHd ends %s.",
                          n, n == 1 ? " is" : "s are", n == 1 ? "it" : "them"))
            running = 0;
        break;
    }
    }
    /* Adjust keeps the menu open. */
    if (_swix(Wimp_GetPointerInfo, _IN(1), ptr) == NULL && (ptr[2] & 1))
        show_menu();
}

static void mouse_click(int *b)
{
    int x = b[0], buttons = b[2], win = b[3], ic = b[4];
    if (win == -2) {                       /* icon bar */
        if (buttons & 2)
            open_menu(x);
        else if (buttons & 4)
            wimp_open_log_window();        /* Select */
        else if (buttons & 1)
            wimp_open_keys_window();       /* Adjust */
    } else if (win == log_win && (buttons & 2)) {
        menu_x = x - 64;
        menu_y = b[1];
        show_log_menu();
    } else if (win == keys_win) {
        if (ic == KI_ADD)
            keys_add_from_input();
        else if (ic == KI_FETCH)
            keys_fetch();
        else if (row_of_delete_icon(ic) >= 0)
            keys_delete_row(row_of_delete_icon(ic));
    }
}

static void key_pressed(int *b)
{
    int win = b[0], ic = b[1], key = b[6];
    if (win == keys_win && key == 0x1b) {  /* Escape closes the window */
        _swix(Wimp_CloseWindow, _IN(1), &keys_win);
        return;
    }
    if (win == keys_win && key == 13) {
        if (ic == KI_INPUT) {
            keys_add_from_input();
            return;
        }
        if (ic == KI_FPASS) {
            keys_fetch();
            return;
        }
    }
    _swix(Wimp_ProcessKey, _IN(0), key);
}

static const char *keys_help(int ic)
{
    switch (ic) {
    case KI_INPUT: return "HInput";
    case KI_ADD:   return "HAdd";
    case KI_FHOST: return "HAccount";
    case KI_FPASS: return "HPassword";
    case KI_FETCH: return "HImport";
    case KI_HOST: case KI_HOST2: case KI_HOSTLABEL:
                   return "HHost";
    default:
        return row_of_delete_icon(ic) >= 0 ? "HDelete" : "HDrop";
    }
}

/* Interactive help: Message_HelpRequest names a window and icon,
 * Message_HelpReply carries the text. */
static void help_request(int *b)
{
    int win = b[8], ic = b[9];
    const char *text = NULL;
    char token[16];
    if (win == -2)
        text = "HIcon";
    else if (win == log_win)
        text = "HLog";
    else if (win == info_win)
        text = "HInfo";
    else if (win == keys_win)
        text = keys_help(ic);
    else {
        /* Any other window sent to us is our open menu: ask the Wimp which
         * item is under the pointer.  Every item's help token is its text
         * token with an H in front, so a new item needs no code here. */
        int sel[4] = { -1, -1, -1, -1 };
        int is_log = current_menu == &log_menu;
        int count = is_log ? LMENU_ITEMS : MENU_ITEMS;
        if (_swix(Wimp_GetMenuState, _INR(0, 3), 1, sel, win, ic) == NULL &&
            sel[0] >= 0 && sel[0] < count) {
            snprintf(token, sizeof token, "H%s", (is_log ? log_menu_tokens : menu_tokens)[sel[0]]);
            text = token;
        }
    }
    if (!text)
        return;
    msgs_get(text, (char *)&b[5], 236, NULL);    /* text is a Messages token */
    b[0] = (int)((20 + strlen((char *)&b[5]) + 1 + 3) & ~3u);
    b[3] = b[2];
    b[4] = Message_HelpReply;
    _swix(Wimp_SendMessage, _INR(0, 2), WIMP_USER_MESSAGE, b, b[1]);
}

/* my_ref of our last DataSaveAck: a DataLoad answering it is our scrap
 * transfer, and only that file is ours to delete. */
static int scrap_ack_ref;

static void user_message(int reason, int *b)
{
    int action = b[4];
    if (tw_message(reason, b))
        return;
    if (action == Message_HelpRequest) {
        help_request(b);
    } else if (action == Message_Quit) {
        running = 0;
    } else if (action == Message_DataSave && b[5] == keys_win) {   /* a drag from an application */
        static const char scrap[] = "<Wimp$Scrap>";
        b[3] = b[2];                       /* your_ref = my_ref */
        b[4] = Message_DataSaveAck;
        b[9] = -1;                         /* "not a safe destination" */
        memcpy(&b[11], scrap, sizeof scrap);
        b[0] = (int)((44 + sizeof scrap + 3) & ~3u);
        if (_swix(Wimp_SendMessage, _INR(0, 2), WIMP_USER_MESSAGE, b, b[1]) == NULL)
            scrap_ack_ref = b[2];          /* the Wimp filled in my_ref */
    } else if (action == Message_DataLoad && b[5] == keys_win) {
        char path[212];
        const char *name = (const char *)&b[11];
        size_t n = 0;
        while (n < sizeof path && (unsigned char)name[n] >= 0x20)   /* any control char ends it */
            n++;
        if (n == sizeof path) {            /* too long to hold: never use a shortened name */
            reportf("The file name is too long");
            return;
        }
        memcpy(path, name, n);
        path[n] = '\0';
        keys_add_from_file(path);
        if (b[3] != 0 && b[3] == scrap_ack_ref) {
            ro_delete(path);               /* our scrap transfer */
            scrap_ack_ref = 0;
        }
        b[3] = b[2];
        b[4] = Message_DataLoadAck;
        _swix(Wimp_SendMessage, _INR(0, 2), WIMP_USER_MESSAGE, b, b[1]);
    }
}

static int active_sessions;

/* One Wimp_Poll event.  Shared by the main loop and wimp_pump().  Only
 * the log window is drawn by us; the others redraw themselves. */
static void dispatch(int reason, int *block)
{
    switch (reason) {
    case 0:  active_sessions = server_poll(0); log_title_update(); break;
    case 1:  redraw_log(block); break;
    case 2:  _swix(Wimp_OpenWindow, _IN(1), block); break;
    case 3:  _swix(Wimp_CloseWindow, _IN(1), block); break;
    case 6:  mouse_click(block); break;
    case 8:  key_pressed(block); break;
    case 9:  menu_selection(block); break;
    case WIMP_USER_MESSAGE: case WIMP_USER_MESSAGE_RECORDED: user_message(reason, block); break;
    }
}

void wimp_run(void)
{
    while (running) {
        int reason = 0, now = 0;
        PROF_START(t);
        _swix(OS_ReadMonotonicTime, _OUT(0), &now);
        /* Poll the server 50 times a second while sessions exist, 4 times
         * a second otherwise. */
        if (_swix(Wimp_PollIdle, _INR(0, 2) | _OUT(0), POLL_MASK,
                  poll_block, now + (active_sessions ? 2 : 25), &reason))
            continue;
        PROF_STOP(PROF_IDLE, t);
        dispatch(reason, poll_block);
    }
}

void wimp_shutdown(void)
{
    msgs_close();
    if (task_handle)
        _swix(Wimp_CloseDown, _INR(0, 1), task_handle, TASK_MAGIC);
    task_handle = 0;
}
