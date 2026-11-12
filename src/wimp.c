/* Wimp front end for sshd: icon-bar icon with a menu (Info..., Log Window,
 * Verbose, Keys..., Options..., Quit), an Info window, a tail-style log window drawn with the
 * desktop font, and a Keys window where public keys can be typed, pasted,
 * dropped as a file, imported from another machine, and deleted.
 * Windows and the menu are built in code, so there is no Templates file.
 * All Wimp access goes through _swix; no OSLib dependency.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
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

#ifndef Wimp_TextOp
#define Wimp_TextOp 0x400F9
#endif
#ifndef Wimp_DeleteIcon
#define Wimp_DeleteIcon 0x400C4
#endif

#define TASK_MAGIC 0x4B534154 /* "TASK" */

/* Fixed icons of the Keys window; key rows are created dynamically below them. */
/* Group boxes come first so that the icons inside draw over them. */
enum { KI_BOX_HOST = 0, KI_BOX_ADD, KI_BOX_IMPORT,
       KI_HOSTLABEL, KI_HOST, KI_HOST2,
       KI_PROMPT, KI_INPUT, KI_ADD, KI_CLOSE,
       KI_FETCHLABEL, KI_FHOST, KI_FPASS, KI_FETCH,
       KI_LISTLABEL, KI_COUNT };
#define ROW_TOP   (-592)          /* work-area y of the first row's top edge */
#define ROW_H     44
#define KEYS_W    1200

enum { MENU_INFO = 0, MENU_LOG, MENU_VERBOSE, MENU_KEYS, MENU_OPTIONS, MENU_QUIT, MENU_ITEMS };
/* Info window icons: label/value pairs */
enum { II_NAME = 0, II_PURPOSE, II_WEBSITE, II_VERSION, II_BUILT, II_COMPILER, II_LIBSSH, II_CRYPTO,
       II_LICENCE, II_COUNT };
#define MAX_FIXED_ICONS 20        /* largest icon count of any window defined here */

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

#define LOG_LINES 400
#define LOG_LINE_H 40
#define LOG_WIDTH (160 * 18)      /* the longest line in the desktop font */
static char log_lines[LOG_LINES][160];
static int log_count, log_first;   /* ring: oldest line is log_first */

/* Dynamic key rows: one text icon and one Delete button per key. */
static int nrows;
static int *row_text_icon, *row_del_icon;
static char (*row_text)[160];
static char list_label[96];
static char key_input[1200];
static char fetch_host[256], fetch_pass[128];
static char host_line[560], host_line2[560];   /* base64 split over two lines */
static char host_label[80];
static char log_title[40] = "SSHd Server Log";
static char keys_title[40] = "SSHd Authorized Keys";

/* Menu entries are indirected, so they may be longer than 12 characters. */
static const char menu_info[] = "Info...";
static const char menu_log[] = "Log Window";
static const char menu_verbose[] = "Verbose";
static const char menu_keys[] = "Keys...";
static const char menu_options[] = "Options...";
static const char menu_quit[] = "Quit";
#define MENU_ITEM_FLAGS (ICON_TEXT | ICON_FILLED | ICON_INDIRECTED | ICON_FG(7) | ICON_BG(0))
static struct wimp_menu menu = {
    "SSHd", 7, 2, 7, 0, 220, 44, 0,
    {
        { 2,    -1, MENU_ITEM_FLAGS, { .ind = { menu_info, (const char *)-1, sizeof menu_info } } },
        { 0,    -1, MENU_ITEM_FLAGS, { .ind = { menu_log,  (const char *)-1, sizeof menu_log } } },
        { 2,    -1, MENU_ITEM_FLAGS, { .ind = { menu_verbose, (const char *)-1, sizeof menu_verbose } } },
        { 0,    -1, MENU_ITEM_FLAGS, { .ind = { menu_keys, (const char *)-1, sizeof menu_keys } } },
        { 2,    -1, MENU_ITEM_FLAGS, { .ind = { menu_options, (const char *)-1, sizeof menu_options } } },
        { 0x80, -1, MENU_ITEM_FLAGS, { .ind = { menu_quit, (const char *)-1, sizeof menu_quit } } },
    }
};

/* --- helpers ------------------------------------------------------------ */

static void icon(struct wimp_icon *ic, int x0, int y0, int x1, int y1, unsigned flags,
                 char *text, int size, const char *valid);

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

/* Redraw an indirected icon after its text buffer changed. */
static void redraw_icon(int win, int icon)
{
    int block[4] = { win, icon, 0, 0 };
    _swix(Wimp_SetIconState, _IN(1), block);
}

static void delete_icon(int win, int icon_handle)
{
    int block[2] = { win, icon_handle };
    if (icon_handle >= 0)
        _swix(Wimp_DeleteIcon, _IN(1), block);
}

static int create_icon(int win, struct wimp_icon *ic)
{
    struct { int window; struct wimp_icon ic; } blk;
    int handle = -1;
    blk.window = win;
    blk.ic = *ic;
    if (_swix(Wimp_CreateIcon, _INR(0, 1) | _OUT(0), 0, &blk, &handle))
        return -1;
    return handle;
}

static char delete_text[] = "Delete";

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
    nrows = n;
    for (int i = 0; i < n; i++) {
        struct wimp_icon ic;
        int top = ROW_TOP - i * ROW_H;
        auth_describe_one(i, row_text[i], sizeof row_text[i]);
        icon(&ic, 16, top - 40, KEYS_W - 200, top,
             ICON_TEXT | ICON_BORDER | ICON_VCENTRE | ICON_FILLED | ICON_FG(7) | ICON_BG(0),
             row_text[i], sizeof row_text[i], "R2");
        row_text_icon[i] = create_icon(keys_win, &ic);
        icon(&ic, KEYS_W - 184, top - 40, KEYS_W - 16, top,
             ICON_TEXT | ICON_BORDER | ICON_HCENTRE | ICON_VCENTRE | ICON_FILLED | ICON_BUTTON(3) |
             ICON_FG(7) | ICON_BG(1),
             delete_text, sizeof delete_text, "R5,3");
        row_del_icon[i] = create_icon(keys_win, &ic);
    }
    if (n == 0)
        snprintf(list_label, sizeof list_label, "Authorized keys: none yet, so nobody can log in.");
    else
        snprintf(list_label, sizeof list_label, "Authorized keys (%d):", n);
    redraw_icon(keys_win, KI_LISTLABEL);

    ext[0] = 0; ext[1] = ROW_TOP - n * ROW_H - 16; ext[2] = KEYS_W; ext[3] = 0;
    if (ext[1] > -800)
        ext[1] = -800;
    _swix(Wimp_SetExtent, _INR(0, 1), keys_win, ext);
    _swix(Wimp_ForceRedraw, _INR(0, 4), keys_win, 0, -4000, KEYS_W, ROW_TOP + 4);
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
    char err[128], msg[260], desc[160];
    struct { int num; char text[252]; } box;
    int answer = 0;
    if (i < 0 || i >= auth_count())
        return;
    auth_describe_one(i, desc, sizeof desc);
    box.num = 0;
    snprintf(box.text, sizeof box.text, "Delete key %s?", desc);
    /* flags: bit0 OK, bit1 Cancel; R1 on exit 1 = OK, 2 = Cancel */
    if (_swix(Wimp_ReportError, _INR(0, 2) | _OUT(1), &box, 3, "SSHd", &answer) || answer != 1)
        return;
    if (auth_remove_key(i, err, sizeof err) < 0) {
        snprintf(msg, sizeof msg, "Key not deleted: %s", err);
        wimp_report_error(msg);
    }
    refresh_keys_list();
}

/* --- log window ---------------------------------------------------------- */

static void log_window_extent(void)
{
    int n = log_count < 12 ? 12 : log_count;
    int ext[4] = { 0, -(n * LOG_LINE_H), LOG_WIDTH, 0 };
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

/* Scroll to the bottom and redraw: the "tail -f" behaviour.  With
 * force 0 the window follows only if it was already showing the end, so
 * someone reading older lines is not dragged down by each new one. */
static void log_window_follow(int force)
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
        int bottom = ext_h > vis_h ? -(ext_h - vis_h) : 0;
        /* before this line was added the end was one line higher */
        if (!force && state[6] > bottom + LOG_LINE_H) {
            _swix(Wimp_ForceRedraw, _INR(0, 4), log_win, 0, -(LOG_LINES * LOG_LINE_H), LOG_WIDTH, 0);
            return;
        }
        state[6] = bottom;
    }
    _swix(Wimp_OpenWindow, _IN(1), state);
    _swix(Wimp_ForceRedraw, _INR(0, 4), log_win, 0, -(LOG_LINES * LOG_LINE_H), LOG_WIDTH, 0);
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
    log_window_follow(0);
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
        /* block: 1..4 visible area, 5,6 scroll, 7..10 clip (screen coords).
         * Work-area (x, y) maps to screen (x - scx + vis_x0, y - scy + vis_y1). */
        int scx = block[5], scy = block[6], vis_x0 = block[1], vis_y1 = block[4];
        int clip_y0 = block[8] + scy - vis_y1;   /* work-area coords */
        int clip_y1 = block[10] + scy - vis_y1;
        int first = (-clip_y1) / LOG_LINE_H - 1;
        int last = (-clip_y0) / LOG_LINE_H + 1;
        if (first < 0)
            first = 0;
        if (last > log_count - 1)
            last = log_count - 1;
        for (int i = first; i <= last; i++) {
            /* reason 2 | bit 30: baseline at Y, screen coordinates.  Row i
             * occupies work-area y in (-(i+1)*LH, -i*LH]; baseline 10 up from
             * the bottom of the row. */
            int wy = -((i + 1) * LOG_LINE_H) + 10;
            _swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 30), log_line(i), -1, -1,
                  8 - scx + vis_x0, wy - scy + vis_y1);
        }
        _swix(Wimp_GetRectangle, _IN(1) | _OUT(0), block, &more);
    }
}

/* Service a redraw request for a window we draw nothing in. */
static void redraw_empty(int *block)
{
    int more = 0;
    _swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), block, &more);
    while (more)
        _swix(Wimp_GetRectangle, _IN(1) | _OUT(0), block, &more);
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
    log_window_follow(1);
}

/* --- keys window --------------------------------------------------------- */

void wimp_open_keys_window(void)
{
    int state[9];
    if (keys_win < 0)
        return;
    refresh_keys_list();
    {
        const char *pub = hostkey_public_line(), *sp = strchr(pub, ' ');
        snprintf(host_label, sizeof host_label, "Server host key (%.*s):", sp ? (int)(sp - pub) : 0, pub);
        /* the desktop font needs ~18 units per character; 40 fit comfortably */
        snprintf(host_line, sizeof host_line, "%.40s", sp ? sp + 1 : pub);
        snprintf(host_line2, sizeof host_line2, "%s", (sp && strlen(sp + 1) > 40) ? sp + 41 : "");
    }
    redraw_icon(keys_win, KI_HOSTLABEL);
    redraw_icon(keys_win, KI_HOST);
    redraw_icon(keys_win, KI_HOST2);
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
    redraw_icon(keys_win, KI_INPUT);
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
        snprintf(msg, sizeof msg, "%d key%s added, %d line%s with invalid format", added,
                 added == 1 ? "" : "s", failed, failed == 1 ? "" : "s");
        wimp_report_error(msg);
    }
}

/* --- fetch keys from another machine ------------------------------------- */

static void dispatch(int reason, int *block);

/* Handle pending desktop events (redraws after a dialogue box went away,
 * TaskWindow messages for live sessions) before blocking again in the SSH
 * client.  Returns at the first null event or after ~half a second. */
static void wimp_pump(void)
{
    int start = 0, now = 0;
    _swix(OS_ReadMonotonicTime, _OUT(0), &start);
    for (;;) {
        int reason = 0;
        if (_swix(Wimp_Poll, _INR(0, 1) | _OUT(0), (1 << 4) | (1 << 5) | (1 << 11) | (1 << 12) | (1 << 19), poll_block, &reason))
            return;
        if (reason == 0)
            return;
        dispatch(reason, poll_block);
        _swix(OS_ReadMonotonicTime, _OUT(0), &now);
        if (now - start > 50)
            return;
    }
}

static int fetch_confirm(const char *fingerprint, const char *host, unsigned port)
{
    struct { int num; char text[252]; } box;
    int answer = 0;
    box.num = 0;
    snprintf(box.text, sizeof box.text,
             "%s port %u reports the host key fingerprint %s. Continue only if that matches what it shows (ssh-keygen -lf).",
             host, port, fingerprint);
    _swix(Hourglass_Off, 0);
    if (_swix(Wimp_ReportError, _INR(0, 2) | _OUT(1), &box, 3, "SSHd", &answer))
        answer = 0;
    wimp_pump();
    _swix(Hourglass_On, 0);
    return answer == 1;
}

static void keys_fetch(void)
{
    char msg[300];
    int rc;
    if (fetch_host[0] == '\0') {
        wimp_report_error("Expected format is user@host");
        _swix(Wimp_SetCaretPosition, _INR(0, 5), keys_win, KI_FHOST, 0, 0, -1, 0);
        return;
    }
    _swix(Hourglass_On, 0);
    rc = keyfetch_run(fetch_host, fetch_pass, fetch_confirm, msg, sizeof msg);
    _swix(Hourglass_Off, 0);
    memset(fetch_pass, 0, sizeof fetch_pass);          /* used once, never kept */
    redraw_icon(keys_win, KI_FPASS);
    refresh_keys_list();
    wimp_report_error(msg);
    (void)rc;
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

static struct wimp_window log_wdef, keys_wdef, info_wdef;
static char info_title[16] = "About SSHd";
#ifndef SSHD_VERSION
#define SSHD_VERSION "dev"
#endif
static char info_labels[II_COUNT][12] = {
    "Name", "Purpose", "Website", "Version", "Built", "Compiler", "libssh", "Crypto", "Licences"
};
static char info_values[II_COUNT][96] = {
    "SSHd", "SSH server for RISC OS", "https://github.com/cr/riscosshd", SSHD_VERSION,
    __DATE__ " " __TIME__, "", "", "", "MIT, LGPL 2.1, Apache 2.0: see !Help"
};

void wimp_open_info_window(void)
{
    int state[9];
    if (info_win < 0)
        return;
    snprintf(info_values[II_COMPILER], sizeof info_values[II_COMPILER], "GCC %s (GCCSDK, UnixLib)", __VERSION__);
    snprintf(info_values[II_LIBSSH], sizeof info_values[II_LIBSSH], "%s", ssh_version(0));
    snprintf(info_values[II_CRYPTO], sizeof info_values[II_CRYPTO], "mbedTLS %s, CryptRandom entropy",
             MBEDTLS_VERSION_STRING);
    state[0] = info_win;
    if (_swix(Wimp_GetWindowState, _IN(1), state))
        return;
    state[7] = -1;
    _swix(Wimp_OpenWindow, _IN(1), state);
    _swix(Wimp_ForceRedraw, _INR(0, 4), info_win, 0, -(16 + II_COUNT * 44), 1000, 0);
}
static char prompt_text[] = "Add a public key (type, paste, or drop file, then Add):";
static char group_dummy[] = "";
static char add_text[] = "Add", close_text[] = "Close", fetch_btn_text[] = "Import";
static char fetch_text[] = "Import from ssh server (user@host, password):";

static void create_windows(void)
{
    struct wimp_window *w;
    const unsigned text_flags = ICON_TEXT | ICON_VCENTRE | ICON_FG(7);
    const unsigned title_flags = ICON_TEXT | ICON_VCENTRE | ICON_FILLED | ICON_FG(7) | ICON_BG(1);
    const unsigned box_flags = ICON_TEXT | ICON_BORDER | ICON_VCENTRE | ICON_FILLED | ICON_FG(7) | ICON_BG(0);
    const unsigned button_flags = ICON_TEXT | ICON_BORDER | ICON_HCENTRE | ICON_VCENTRE | ICON_FILLED |
                                  ICON_BUTTON(3) | ICON_FG(7) | ICON_BG(1);

    /* Log window: scrolling text, drawn in redraw_log(). */
    w = &log_wdef;
    memset(w, 0, sizeof *w);
    w->x0 = 100; w->y0 = 200; w->x1 = 1100; w->y1 = 680; w->behind = -1;
    w->flags = WIN_NEWFORMAT | WIN_MOVEABLE | WIN_BACK | WIN_CLOSE | WIN_TITLE | WIN_TOGGLE | WIN_VSCROLL |
               WIN_ADJUST | WIN_HSCROLL;
    w->title_fg = 7; w->title_bg = 2; w->work_fg = 7; w->work_bg = 0;
    w->scroll_outer = 3; w->scroll_inner = 1; w->highlight = 12;
    w->ex0 = 0; w->ey0 = -(12 * LOG_LINE_H); w->ex1 = LOG_WIDTH; w->ey1 = 0;
    w->title_flags = ICON_TEXT | ICON_HCENTRE | ICON_VCENTRE | ICON_INDIRECTED;
    w->sprite_area = (void *)1;
    w->title.ind.text = log_title; w->title.ind.valid = (const char *)-1; w->title.ind.size = sizeof log_title;
    w->nicons = 0;
    log_win = create_window(w);

    /* Keys window: add section at the top, dynamic key rows below. */
    w = &keys_wdef;
    memset(w, 0, sizeof *w);
    w->x0 = 40; w->y0 = 100; w->x1 = 40 + KEYS_W; w->y1 = 920; w->behind = -1;
    w->flags = WIN_NEWFORMAT | WIN_MOVEABLE | WIN_AUTOREDRAW | WIN_BACK | WIN_CLOSE | WIN_TITLE |
               WIN_TOGGLE | WIN_VSCROLL | WIN_ADJUST;
    w->title_fg = 7; w->title_bg = 2; w->work_fg = 7; w->work_bg = 1;
    w->scroll_outer = 3; w->scroll_inner = 1; w->highlight = 12;
    w->ex0 = 0; w->ey0 = -800; w->ex1 = KEYS_W; w->ey1 = 0;
    w->title_flags = ICON_TEXT | ICON_HCENTRE | ICON_VCENTRE | ICON_INDIRECTED;
    w->work_flags = ICON_BUTTON(10);   /* click/drag so DataLoad hits the window */
    w->sprite_area = (void *)1;
    w->title.ind.text = keys_title; w->title.ind.valid = (const char *)-1; w->title.ind.size = sizeof keys_title;
    w->nicons = KI_COUNT;
    /* Section 1: server host key.  Group titles sit on the top border, with
     * a filled background in the window colour to mask the channel line. */
    icon(&w->icon[KI_BOX_HOST], 16, -168, KEYS_W - 16, -20, ICON_BORDER, group_dummy, sizeof group_dummy, "R4");
    icon(&w->icon[KI_HOSTLABEL], 40, -40, 900, 0, title_flags, host_label, sizeof host_label, NULL);
    icon(&w->icon[KI_HOST], 40, -104, KEYS_W - 40, -64, text_flags, host_line, sizeof host_line, NULL);
    icon(&w->icon[KI_HOST2], 40, -148, KEYS_W - 40, -108, text_flags, host_line2, sizeof host_line2, NULL);
    /* Section 2: add a key by hand. */
    icon(&w->icon[KI_BOX_ADD], 16, -368, KEYS_W - 16, -212, ICON_BORDER, group_dummy, sizeof group_dummy, "R4");
    icon(&w->icon[KI_PROMPT], 40, -232, KEYS_W - 40, -192, title_flags, prompt_text, sizeof prompt_text, NULL);
    icon(&w->icon[KI_INPUT], 40, -308, KEYS_W - 40, -256, box_flags | ICON_BUTTON(15), key_input, sizeof key_input,
         "Ktar;Pptr_write");
    icon(&w->icon[KI_ADD], 40, -356, 220, -316, button_flags, add_text, sizeof add_text, "R5,3");
    icon(&w->icon[KI_CLOSE], KEYS_W - 220, -356, KEYS_W - 40, -316, button_flags, close_text, sizeof close_text, "R5,3");
    /* Section 3: import from another machine. */
    icon(&w->icon[KI_BOX_IMPORT], 16, -512, KEYS_W - 16, -412, ICON_BORDER, group_dummy, sizeof group_dummy, "R4");
    icon(&w->icon[KI_FETCHLABEL], 40, -432, KEYS_W - 40, -392, title_flags, fetch_text, sizeof fetch_text, NULL);
    icon(&w->icon[KI_FHOST], 40, -500, 620, -448, box_flags | ICON_BUTTON(15), fetch_host, sizeof fetch_host,
         "Ktar;Pptr_write");
    icon(&w->icon[KI_FPASS], 640, -500, 980, -448, box_flags | ICON_BUTTON(15), fetch_pass, sizeof fetch_pass,
         "Ktar;D*;Pptr_write");
    icon(&w->icon[KI_FETCH], 1000, -500, KEYS_W - 40, -448, button_flags, fetch_btn_text, sizeof fetch_btn_text, "R5,3");
    /* Section 4: the list, growing downwards. */
    icon(&w->icon[KI_LISTLABEL], 16, -588, KEYS_W - 16, -548, text_flags, list_label, sizeof list_label, NULL);
    keys_win = create_window(w);

    /* Info window: the usual label/value box, icons only. */
    w = &info_wdef;
    memset(w, 0, sizeof *w);
    w->x0 = 300; w->y0 = 360; w->x1 = 1240; w->y1 = 780; w->behind = -1;
    w->flags = WIN_NEWFORMAT | WIN_MOVEABLE | WIN_AUTOREDRAW | WIN_BACK | WIN_CLOSE | WIN_TITLE;
    w->title_fg = 7; w->title_bg = 2; w->work_fg = 7; w->work_bg = 1;
    w->scroll_outer = 3; w->scroll_inner = 1; w->highlight = 12;
    w->ex0 = 0; w->ey0 = -(16 + II_COUNT * 44); w->ex1 = 940; w->ey1 = 0;
    w->title_flags = ICON_TEXT | ICON_HCENTRE | ICON_VCENTRE | ICON_INDIRECTED;
    w->sprite_area = (void *)1;
    w->title.ind.text = info_title; w->title.ind.valid = (const char *)-1; w->title.ind.size = sizeof info_title;
    w->nicons = II_COUNT * 2;
    for (int i = 0; i < II_COUNT; i++) {
        int top = -16 - i * 44;
        icon(&w->icon[2 * i], 16, top - 36, 200, top, ICON_TEXT | ICON_VCENTRE | ICON_RJUST | ICON_FG(7),
             info_labels[i], sizeof info_labels[i], NULL);
        icon(&w->icon[2 * i + 1], 216, top - 40, 924, top + 4, box_flags, info_values[i], sizeof info_values[i], "R2");
    }
    info_win = create_window(w);
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
    static int messages[] = { 1 /* DataSave */, 3 /* DataLoad */, 0x502 /* HelpRequest */, Message_TaskWindow_Output,
                              Message_TaskWindow_Ego, Message_TaskWindow_Morio, Message_TaskWindow_NewTask,
                              Message_TaskCloseDown, 0 };
    _kernel_oserror *e = _swix(Wimp_Initialise, _INR(0, 3) | _OUT(1), 350, TASK_MAGIC, "SSHd",
                               messages, &task_handle);
    if (e) {
        report_oserror("Wimp_Initialise", e);
        return -1;
    }
    log_set_sink(log_sink);
    tw_set_task_handle(task_handle);
    create_windows();
    if (create_iconbar_icon() < 0 || log_win < 0 || keys_win < 0 || info_win < 0) {
        wimp_shutdown();        /* leave no half-registered task behind */
        return -1;
    }
    log_window_extent();        /* the lines replayed into the sink above */
    menu.item[MENU_INFO].submenu = info_win;   /* Style Guide: Info is a submenu box */
    refresh_keys_list();
    return 0;
}

/* --- event loop ----------------------------------------------------------- */

static void open_menu(int x)
{
    menu.item[MENU_VERBOSE].flags = (menu.item[MENU_VERBOSE].flags & ~1u) | (log_verbose ? 1u : 0u);  /* tick */
    menu_x = x - 64;
    menu_y = 96 + MENU_ITEMS * 44;
    _swix(Wimp_CreateMenu, _INR(1, 3), &menu, menu_x, menu_y);
}

static void menu_selection(int *items)
{
    int ptr[5];
    switch (items[0]) {
    case MENU_INFO: wimp_open_info_window(); break;
    case MENU_LOG:  wimp_open_log_window(); break;
    case MENU_VERBOSE:
        log_verbose = log_verbose ? 0 : 1;
        log_msg("verbose logging %s", log_verbose ? "on" : "off");
        break;
    case MENU_KEYS: wimp_open_keys_window(); break;
    case MENU_OPTIONS: {
        /* The settings are a text file: hand it to the user's editor.  It
         * is written on first run, and again here if it has gone. */
        char cmd[320];
        _kernel_oserror *e;
        options_ensure_file();
        snprintf(cmd, sizeof cmd, "Filer_Run %s", options_path());
        e = _swix(OS_CLI, _IN(0), cmd);
        if (e)
            wimp_report_error(e->errmess);
        else
            log_msg("editing %s: changes apply when SSHd is next started", options_path());
        break;
    }
    case MENU_QUIT: running = 0; break;
    }
    /* Adjust keeps the menu open. */
    if (_swix(Wimp_GetPointerInfo, _IN(1), ptr) == NULL && (ptr[2] & 1)) {
        menu.item[MENU_VERBOSE].flags = (menu.item[MENU_VERBOSE].flags & ~1u) | (log_verbose ? 1u : 0u);
        _swix(Wimp_CreateMenu, _INR(1, 3), &menu, menu_x, menu_y);
    }
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
        else if (ic == KI_FETCH)
            keys_fetch();
        else if (row_of_delete_icon(ic) >= 0)
            keys_delete_row(row_of_delete_icon(ic));
    }
}

static void key_pressed(int *b)
{
    int win = b[0], ic = b[1], key = b[6];
    if (win == keys_win && key == 13) {
        if (ic == KI_INPUT) {
            keys_add_from_input();
            return;
        }
        if (ic == KI_FHOST) {
            _swix(Wimp_SetCaretPosition, _INR(0, 5), keys_win, KI_FPASS, 0, 0, -1, (int)strlen(fetch_pass));
            return;
        }
        if (ic == KI_FPASS) {
            keys_fetch();
            return;
        }
    }
    _swix(Wimp_ProcessKey, _IN(0), key);
}

/* Interactive help: Message_HelpRequest (0x502) names a window and icon,
 * Message_HelpReply (0x503) carries the text. */
static void help_request(int *b)
{
    int win = b[8], ic = b[9];
    const char *text = NULL;
    if (win == -1) {                       /* a menu item: which one from Wimp_GetMenuState */
        int sel[4] = { -1, -1, -1, -1 };
        if (_swix(0x400F4 /* Wimp_GetMenuState */, _INR(0, 1), 0, sel) == NULL) {
            switch (sel[0]) {
            case MENU_INFO:    text = "Shows this program's version and how it was built."; break;
            case MENU_LOG:     text = "Opens the log window, which follows the server's activity."; break;
            case MENU_VERBOSE: text = "Ticked, the log carries more detail about each connection."; break;
            case MENU_KEYS:    text = "Opens the window of keys that may log in."; break;
            case MENU_OPTIONS: text = "Opens the settings file in your text editor; changes apply at the next start."; break;
            case MENU_QUIT:    text = "Stops the SSH server and removes it from the icon bar."; break;
            default: break;
            }
        }
    } else if (win == -2) {
        text = "This is the SSH server. Click SELECT to open its log, ADJUST for the authorized keys, MENU for the settings and more.";
    } else if (win == log_win) {
        text = "The server's log, newest line at the bottom. The same lines go to Choices:SSHd.log.";
    } else if (win == info_win) {
        text = "Information about this copy of SSHd.";
    } else if (win == keys_win) {
        switch (ic) {
        case KI_INPUT: text = "Type or paste a public key here (the content of an id_*.pub file), then press RETURN or click Add."; break;
        case KI_ADD:   text = "Click SELECT to add the key in the field to the authorized keys."; break;
        case KI_CLOSE: text = "Click SELECT to close this window."; break;
        case KI_FHOST: text = "The account on another machine whose public keys you want to allow, as user@host."; break;
        case KI_FPASS: text = "That account's password. It is used once to log in and read ~/.ssh/id_*.pub, then forgotten."; break;
        case KI_FETCH: text = "Click SELECT to log in to the machine named on the left and import its public keys."; break;
        case KI_HOST: case KI_HOST2: case KI_HOSTLABEL:
                       text = "This server's own public host key, which clients see on their first connection."; break;
        default:
            text = row_of_delete_icon(ic) >= 0
                 ? "Click SELECT to remove this key; it will no longer be able to log in."
                 : "Drop a file containing public keys anywhere on this window to add them.";
        }
    }
    if (!text)
        return;
    snprintf((char *)&b[5], 236, "%s", text);
    b[0] = (int)((20 + strlen((char *)&b[5]) + 1 + 3) & ~3u);
    b[3] = b[2];
    b[4] = 0x503;
    _swix(Wimp_SendMessage, _INR(0, 2), 17, b, b[1]);
}

static void user_message(int reason, int *b)
{
    int action = b[4];
    if (tw_message(reason, b))
        return;
    if (action == 0x502) {                 /* Message_HelpRequest */
        help_request(b);
    } else if (action == 0) {              /* Message_Quit */
        running = 0;
    } else if (action == 1 && b[5] == keys_win) {   /* Message_DataSave: drag from an app */
        static const char scrap[] = "<Wimp$Scrap>";
        b[3] = b[2];                       /* your_ref = my_ref */
        b[4] = 2;                          /* Message_DataSaveAck */
        b[9] = -1;                         /* "not a safe destination" */
        memcpy(&b[11], scrap, sizeof scrap);
        b[0] = (int)((44 + sizeof scrap + 3) & ~3u);
        _swix(Wimp_SendMessage, _INR(0, 2), 17, b, b[1]);
    } else if (action == 3 && b[5] == keys_win) {   /* Message_DataLoad */
        char path[212];
        const char *name = (const char *)&b[11];
        size_t n = 0;
        while (n < sizeof path - 1 && (unsigned char)name[n] >= 0x20)   /* any control char ends it */
            n++;
        memcpy(path, name, n);
        path[n] = '\0';
        keys_add_from_file(path);
        if (b[3] != 0)                     /* our scrap transfer: the file is ours to delete */
            _swix(OS_File, _INR(0, 1), 6, path);
        b[3] = b[2];
        b[4] = 4;                          /* Message_DataLoadAck */
        _swix(Wimp_SendMessage, _INR(0, 2), 17, b, b[1]);
    }
}

static int active_sessions;

/* One Wimp_Poll event.  Shared by the main loop and wimp_pump(). */
static void dispatch(int reason, int *block)
{
    switch (reason) {
    case 0:  active_sessions = server_poll(0); break;
    case 1:  if (block[0] == log_win) redraw_log(block); else redraw_empty(block); break;
    case 2:  _swix(Wimp_OpenWindow, _IN(1), block); break;
    case 3:  _swix(Wimp_CloseWindow, _IN(1), block); break;
    case 6:  mouse_click(block); break;
    case 8:  key_pressed(block); break;
    case 9:  menu_selection(block); break;
    case 17: case 18: user_message(reason, block); break;
    }
}

void wimp_run(void)
{
    while (running) {
        int reason = 0, now = 0;
        _swix(OS_ReadMonotonicTime, _OUT(0), &now);
        /* mask: pointer leaving/entering (bits 4,5), lose/gain caret (11,12).
         * Poll the server 50 times a second while sessions exist, 4 times
         * a second otherwise. */
        if (_swix(Wimp_PollIdle, _INR(0, 2) | _OUT(0), (1 << 4) | (1 << 5) | (1 << 11) | (1 << 12) | (1 << 19),
                  poll_block, now + (active_sessions ? 2 : 25), &reason))
            continue;
        dispatch(reason, poll_block);
    }
}

void wimp_shutdown(void)
{
    if (task_handle)
        _swix(Wimp_CloseDown, _INR(0, 1), task_handle, TASK_MAGIC);
    task_handle = 0;
}
