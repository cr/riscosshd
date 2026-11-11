/* Wimp front end for sshd: icon-bar icon with a menu (Log window, Keys...,
 * Quit), a tail-style log window drawn with the desktop font, and a Keys
 * window where public keys can be typed, pasted or dropped as a file.
 * Windows and the menu are built in code, so there is no Templates file.
 * All Wimp access goes through _swix; no OSLib dependency.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>

#include "wimp.h"
#include "server.h"
#include "auth.h"
#include "hostkey.h"
#include "config.h"
#include "riscos.h"
#include "log.h"

#ifndef Wimp_TextOp
#define Wimp_TextOp 0x400F9
#endif

#define TASK_MAGIC 0x4B534154 /* "TASK" */

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
    struct wimp_icon icon[8];
};

struct wimp_menu_item {
    unsigned flags;
    int submenu;
    unsigned icon_flags;
    char text[12];
};

struct wimp_menu {
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int width, height, gap;
    struct wimp_menu_item item[3];
};

#define ICON_TEXT       (1u << 0)
#define ICON_SPRITE     (1u << 1)
#define ICON_BORDER     (1u << 2)
#define ICON_HCENTRE    (1u << 3)
#define ICON_VCENTRE    (1u << 4)
#define ICON_FILLED     (1u << 5)
#define ICON_INDIRECTED (1u << 8)
#define ICON_BUTTON(t)  ((unsigned)(t) << 12)
#define ICON_FG(c)      ((unsigned)(c) << 24)
#define ICON_BG(c)      ((unsigned)(c) << 28)

#define WIN_MOVEABLE    (1u << 1)
#define WIN_BACK        (1u << 24)
#define WIN_CLOSE       (1u << 25)
#define WIN_TITLE       (1u << 26)
#define WIN_TOGGLE      (1u << 27)
#define WIN_VSCROLL     (1u << 28)
#define WIN_ADJUST      (1u << 29)
#define WIN_NEWFORMAT   (1u << 31)

enum { MENU_LOG = 0, MENU_KEYS = 1, MENU_QUIT = 2 };
enum { KI_LABEL = 0, KI_LIST = 1, KI_PROMPT = 2, KI_INPUT = 3, KI_ADD = 4, KI_CLOSE = 5, KI_HOST = 6 };

/* --- state ------------------------------------------------------------- */

static int task_handle;
static int running = 1;
static int log_win = -1, keys_win = -1;
static int menu_x, menu_y;
static int poll_block[64];

#define LOG_LINES 400
#define LOG_LINE_H 40
static char log_lines[LOG_LINES][160];
static int log_count, log_first;   /* ring: oldest line is log_first */

static char keys_list[300];
static char key_input[1200];
static char host_line[560];
static char log_title[40] = "SSHd log";
static char keys_title[40] = "SSHd keys";

static struct wimp_menu menu = {
    "SSHd", 7, 2, 7, 0, 220, 44, 0,
    {
        { 0, -1, ICON_TEXT | ICON_FILLED | ICON_FG(7) | ICON_BG(0), "Log window" },
        { 2, -1, ICON_TEXT | ICON_FILLED | ICON_FG(7) | ICON_BG(0), "Keys..." },
        { 0x80, -1, ICON_TEXT | ICON_FILLED | ICON_FG(7) | ICON_BG(0), "Quit" },
    }
};

/* --- helpers ------------------------------------------------------------ */

void wimp_report_error(const char *msg)
{
    struct { int num; char text[252]; } err;
    err.num = 0;
    snprintf(err.text, sizeof err.text, "%s", msg);
    if (_swix(Wimp_ReportError, _INR(0, 2), &err, 1, "SSHd") != NULL)
        fprintf(stderr, "SSHd: %s\n", msg);
}

static void report_oserror(const char *what, _kernel_oserror *e)
{
    char buf[300];
    snprintf(buf, sizeof buf, "%s: %s", what, e ? e->errmess : "unknown error");
    wimp_report_error(buf);
}

static void set_icon_text(int win, int icon, const char *text, char *buf, size_t size)
{
    int block[4] = { win, icon, 0, 0 };
    if (buf) {
        snprintf(buf, size, "%s", text);
    }
    _swix(Wimp_SetIconState, _IN(1), block);
}

static void refresh_keys_list(void)
{
    char tmp[300];
    auth_describe(tmp, sizeof tmp);
    snprintf(keys_list, sizeof keys_list, "%d: %s", auth_count(), tmp);
    if (keys_win >= 0)
        set_icon_text(keys_win, KI_LIST, keys_list, NULL, 0);
}

/* --- log window ---------------------------------------------------------- */

static void log_window_extent(void)
{
    int n = log_count < 12 ? 12 : log_count;
    int ext[4] = { 0, -(n * LOG_LINE_H), 1600, 0 };
    if (log_win >= 0)
        _swix(Wimp_SetExtent, _INR(0, 1), log_win, ext);
}

static int window_is_open(int win)
{
    int state[9];
    state[0] = win;
    if (_swix(Wimp_GetWindowState, _IN(1), state))
        return 0;
    return (state[8] & (1 << 16)) != 0;   /* window flags bit 16: open */
}

/* Scroll to the bottom and redraw: the "tail -f" behaviour. */
static void log_window_follow(void)
{
    int state[9];
    if (log_win < 0 || !window_is_open(log_win))
        return;
    state[0] = log_win;
    if (_swix(Wimp_GetWindowState, _IN(1), state))
        return;
    {
        int vis_h = state[4] - state[2];
        int ext_h = (log_count < 12 ? 12 : log_count) * LOG_LINE_H;
        state[6] = ext_h > vis_h ? -(ext_h - vis_h) : 0;
    }
    _swix(Wimp_OpenWindow, _IN(1), state);
    _swix(Wimp_ForceRedraw, _INR(0, 4), log_win, 0, -(LOG_LINES * LOG_LINE_H), 1600, 0);
}

static void log_sink(const char *line)
{
    int idx;
    if (log_count < LOG_LINES) {
        idx = log_count++;
    } else {
        idx = log_first;
        log_first = (log_first + 1) % LOG_LINES;
    }
    snprintf(log_lines[idx], sizeof log_lines[idx], "%s", line);
    log_window_extent();
    log_window_follow();
}

static const char *log_line(int i)
{
    return log_lines[(log_first + i) % LOG_LINES];
}

static void redraw_log(int *block)
{
    int more = 0;
    _swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), block, &more);
    _swix(Wimp_TextOp, _INR(0, 2), 0, 0x00000000, 0xFFFFFF00);
    while (more) {
        /* block: 1..4 visible area, 5,6 scroll, 7..10 clip (screen coords) */
        int scy = block[6], vis_y1 = block[4];
        int clip_y0 = block[8] + scy - vis_y1;   /* work-area coords */
        int clip_y1 = block[10] + scy - vis_y1;
        int first = (-clip_y1) / LOG_LINE_H - 1;
        int last = (-clip_y0) / LOG_LINE_H + 1;
        if (first < 0)
            first = 0;
        if (last > log_count - 1)
            last = log_count - 1;
        for (int i = first; i <= last; i++) {
            /* reason 2 | bit 29: window-relative coords | bit 30: baseline like system font */
            _swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 29) | (1u << 30), log_line(i), -1, -1,
                  8, -(i * LOG_LINE_H) - 4);
        }
        _swix(Wimp_GetRectangle, _IN(1) | _OUT(0), block, &more);
    }
}

void wimp_open_log_window(void)
{
    int state[9];
    if (log_win < 0)
        return;
    state[0] = log_win;
    if (_swix(Wimp_GetWindowState, _IN(1), state))
        return;
    state[7] = -1;   /* on top */
    _swix(Wimp_OpenWindow, _IN(1), state);
    log_window_follow();
}

/* --- keys window --------------------------------------------------------- */

void wimp_open_keys_window(void)
{
    int state[9];
    if (keys_win < 0)
        return;
    refresh_keys_list();
    snprintf(host_line, sizeof host_line, "Host key: %s", hostkey_public_line());
    set_icon_text(keys_win, KI_HOST, host_line, NULL, 0);
    state[0] = keys_win;
    if (_swix(Wimp_GetWindowState, _IN(1), state))
        return;
    state[7] = -1;
    _swix(Wimp_OpenWindow, _IN(1), state);
    _swix(Wimp_SetCaretPosition, _INR(0, 5), keys_win, KI_INPUT, 0, 0, -1, (int)strlen(key_input));
}

static void keys_add_from_input(void)
{
    char err[128];
    if (key_input[0] == '\0')
        return;
    if (auth_add_key_line(key_input, err, sizeof err) < 0) {
        char msg[200];
        snprintf(msg, sizeof msg, "Key not added: %s", err);
        wimp_report_error(msg);
        return;
    }
    key_input[0] = '\0';
    set_icon_text(keys_win, KI_INPUT, "", NULL, 0);
    _swix(Wimp_SetCaretPosition, _INR(0, 5), keys_win, KI_INPUT, 0, 0, -1, 0);
    refresh_keys_list();
}

/* A file dropped on the keys window: add every key line it contains. */
static void keys_add_from_file(const char *path)
{
    char err[128], msg[200];
    int added = 0, failed = 0;
    char *data = ro_load_file(path, NULL), *p, *nl;
    if (!data) {
        snprintf(msg, sizeof msg, "Cannot read %s", path);
        wimp_report_error(msg);
        return;
    }
    for (p = data; p && *p; p = nl) {
        nl = strchr(p, '\n');
        if (nl)
            *nl++ = '\0';
        if (p[0] == '\0' || p[0] == '#')
            continue;
        if (auth_add_key_line(p, err, sizeof err) == 0)
            added++;
        else
            failed++;
    }
    free(data);
    refresh_keys_list();
    if (added == 0 || failed) {
        snprintf(msg, sizeof msg, "%d key%s added, %d line%s not understood", added,
                 added == 1 ? "" : "s", failed, failed == 1 ? "" : "s");
        wimp_report_error(msg);
    }
}

/* --- creation ------------------------------------------------------------ */

static void icon(struct wimp_icon *ic, int x0, int y0, int x1, int y1, unsigned flags,
                 char *text, int size, const char *valid)
{
    ic->x0 = x0; ic->y0 = y0; ic->x1 = x1; ic->y1 = y1;
    ic->flags = flags | ICON_INDIRECTED;
    ic->d.ind.text = text;
    ic->d.ind.valid = valid ? valid : (const char *)-1;
    ic->d.ind.size = size;
}

static int create_window(struct wimp_window *w)
{
    int handle = -1;
    _kernel_oserror *e = _swix(Wimp_CreateWindow, _IN(1) | _OUT(0), w, &handle);
    if (e) {
        report_oserror("Cannot create window", e);
        return -1;
    }
    return handle;
}

static struct wimp_window log_wdef, keys_wdef;
static char label_text[] = "Keys that may log in (Choices:SSHd.authkeys):";
static char prompt_text[] = "Add a public key: type or paste it, or drop a file here, then Add:";
static char add_text[] = "Add", close_text[] = "Close";

static void create_windows(void)
{
    struct wimp_window *w;
    const unsigned text_flags = ICON_TEXT | ICON_VCENTRE | ICON_FG(7);
    const unsigned box_flags = ICON_TEXT | ICON_BORDER | ICON_VCENTRE | ICON_FILLED | ICON_FG(7) | ICON_BG(0);
    const unsigned button_flags = ICON_TEXT | ICON_BORDER | ICON_HCENTRE | ICON_VCENTRE | ICON_FILLED |
                                  ICON_BUTTON(3) | ICON_FG(7) | ICON_BG(1);

    /* Log window: scrolling text, drawn in redraw_log(). */
    w = &log_wdef;
    memset(w, 0, sizeof *w);
    w->x0 = 100; w->y0 = 200; w->x1 = 1100; w->y1 = 680; w->behind = -1;
    w->flags = WIN_NEWFORMAT | WIN_MOVEABLE | WIN_BACK | WIN_CLOSE | WIN_TITLE | WIN_TOGGLE | WIN_VSCROLL | WIN_ADJUST;
    w->title_fg = 7; w->title_bg = 2; w->work_fg = 7; w->work_bg = 0;
    w->scroll_outer = 3; w->scroll_inner = 1; w->highlight = 12;
    w->ex0 = 0; w->ey0 = -(12 * LOG_LINE_H); w->ex1 = 1600; w->ey1 = 0;
    w->title_flags = ICON_TEXT | ICON_HCENTRE | ICON_VCENTRE | ICON_INDIRECTED;
    w->sprite_area = (void *)1;
    w->title.ind.text = log_title; w->title.ind.valid = (const char *)-1; w->title.ind.size = sizeof log_title;
    w->nicons = 0;
    log_win = create_window(w);

    /* Keys window. */
    w = &keys_wdef;
    memset(w, 0, sizeof *w);
    w->x0 = 160; w->y0 = 260; w->x1 = 1120; w->y1 = 700; w->behind = -1;
    w->flags = WIN_NEWFORMAT | WIN_MOVEABLE | WIN_BACK | WIN_CLOSE | WIN_TITLE;
    w->title_fg = 7; w->title_bg = 2; w->work_fg = 7; w->work_bg = 1;
    w->scroll_outer = 3; w->scroll_inner = 1; w->highlight = 12;
    w->ex0 = 0; w->ey0 = -440; w->ex1 = 960; w->ey1 = 0;
    w->title_flags = ICON_TEXT | ICON_HCENTRE | ICON_VCENTRE | ICON_INDIRECTED;
    w->work_flags = ICON_BUTTON(10);   /* click/drag so DataLoad hits the window */
    w->sprite_area = (void *)1;
    w->title.ind.text = keys_title; w->title.ind.valid = (const char *)-1; w->title.ind.size = sizeof keys_title;
    w->nicons = 7;
    icon(&w->icon[KI_LABEL], 16, -56, 944, -12, text_flags, label_text, sizeof label_text, NULL);
    icon(&w->icon[KI_LIST], 16, -116, 944, -64, box_flags, keys_list, sizeof keys_list, "R2");
    icon(&w->icon[KI_PROMPT], 16, -176, 944, -132, text_flags, prompt_text, sizeof prompt_text, NULL);
    icon(&w->icon[KI_INPUT], 16, -236, 944, -184, box_flags | ICON_BUTTON(15), key_input, sizeof key_input,
         "Ktar;Pptr_write");
    icon(&w->icon[KI_ADD], 16, -308, 196, -256, button_flags, add_text, sizeof add_text, "R5,3");
    icon(&w->icon[KI_CLOSE], 216, -308, 396, -256, button_flags, close_text, sizeof close_text, "R5,3");
    icon(&w->icon[KI_HOST], 16, -424, 944, -340, text_flags, host_line, sizeof host_line, NULL);
    keys_win = create_window(w);
}

static char sprite_name[] = "!sshd";

static int create_iconbar_icon(void)
{
    struct { int window; struct wimp_icon ic; } blk;
    int handle;
    _kernel_oserror *e;
    blk.window = -1;   /* right-hand side of the icon bar */
    icon(&blk.ic, 0, 0, 68, 68, ICON_SPRITE | ICON_HCENTRE | ICON_VCENTRE | ICON_BUTTON(3),
         sprite_name, sizeof sprite_name - 1, NULL);
    blk.ic.d.ind.valid = (const char *)1;   /* sprite area: Wimp pool */
    e = _swix(Wimp_CreateIcon, _INR(0, 1) | _OUT(0), 0, &blk, &handle);
    if (e) {
        report_oserror("Cannot create icon bar icon", e);
        return -1;
    }
    return 0;
}

int wimp_init(void)
{
    static int messages[] = { 3 /* DataLoad */, 0 };
    _kernel_oserror *e = _swix(Wimp_Initialise, _INR(0, 3) | _OUT(1), 310, TASK_MAGIC, "SSHd",
                               messages, &task_handle);
    if (e) {
        report_oserror("Wimp_Initialise", e);
        return -1;
    }
    log_set_sink(log_sink);
    create_windows();
    if (create_iconbar_icon() < 0 || log_win < 0 || keys_win < 0)
        return -1;
    refresh_keys_list();
    return 0;
}

/* --- event loop ----------------------------------------------------------- */

static void open_menu(int x)
{
    menu_x = x - 64;
    menu_y = 96 + 3 * 44;
    _swix(Wimp_CreateMenu, _INR(1, 3), &menu, menu_x, menu_y);
}

static void menu_selection(int *items)
{
    int ptr[5];
    switch (items[0]) {
    case MENU_LOG:  wimp_open_log_window(); break;
    case MENU_KEYS: wimp_open_keys_window(); break;
    case MENU_QUIT: running = 0; break;
    }
    /* Adjust keeps the menu open. */
    if (_swix(Wimp_GetPointerInfo, _IN(1), ptr) == NULL && (ptr[2] & 1))
        _swix(Wimp_CreateMenu, _INR(1, 3), &menu, menu_x, menu_y);
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
    } else if (win == keys_win) {
        if (ic == KI_ADD)
            keys_add_from_input();
        else if (ic == KI_CLOSE)
            _swix(Wimp_CloseWindow, _IN(1), &keys_win);
    }
}

static void key_pressed(int *b)
{
    int win = b[0], ic = b[1], key = b[6];
    if (win == keys_win && ic == KI_INPUT && key == 13) {
        keys_add_from_input();
        return;
    }
    _swix(Wimp_ProcessKey, _IN(0), key);
}

static void user_message(int reason, int *b)
{
    int action = b[4];
    (void)reason;
    if (action == 0) {                     /* Message_Quit */
        running = 0;
    } else if (action == 3 && b[5] == keys_win) {   /* Message_DataLoad */
        keys_add_from_file((const char *)&b[11]);
        b[3] = b[2];                       /* your_ref = my_ref */
        b[4] = 4;                          /* Message_DataLoadAck */
        _swix(Wimp_SendMessage, _INR(0, 2), 17, b, b[1]);
    }
}

void wimp_run(void)
{
    while (running) {
        int reason = 0, now = 0;
        _swix(OS_ReadMonotonicTime, _OUT(0), &now);
        /* mask: pointer leaving/entering (bits 4,5), lose/gain caret (11,12) */
        if (_swix(Wimp_PollIdle, _INR(0, 2) | _OUT(0), (1 << 4) | (1 << 5) | (1 << 11) | (1 << 12),
                  poll_block, now + 2, &reason))
            continue;
        switch (reason) {
        case 0:  server_poll(0); break;
        case 1:  if (poll_block[0] == log_win) redraw_log(poll_block); break;
        case 2:  _swix(Wimp_OpenWindow, _IN(1), poll_block); break;
        case 3:  _swix(Wimp_CloseWindow, _IN(1), poll_block); break;
        case 6:  mouse_click(poll_block); break;
        case 8:  key_pressed(poll_block); break;
        case 9:  menu_selection(poll_block); break;
        case 17: case 18: user_message(reason, poll_block); break;
        }
    }
}

void wimp_shutdown(void)
{
    if (task_handle)
        _swix(Wimp_CloseDown, _INR(0, 1), task_handle, TASK_MAGIC);
    task_handle = 0;
}
