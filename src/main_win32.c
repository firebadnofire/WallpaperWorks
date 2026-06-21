#define err(...) do { \
  fprintf(stderr, "Error: "); \
  fprintf(stderr, __VA_ARGS__); \
  fprintf(stderr, "\n"); \
  *((int *) 0) = 0; \
} while (0)

#define warning(...) do { \
  fprintf(stderr, "Warning: "); \
  fprintf(stderr, __VA_ARGS__); \
  fprintf(stderr, "\n"); \
} while (0)

#define print(format, ...) do { \
    char buf[1024]; \
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, format, __VA_ARGS__); \
    MessageBox(NULL, buf, "Debug Message", MB_OK); \
} while (0)

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <shellapi.h>
#include <time.h>
#include <sys/time.h>
#include <Windows.h>

#define DS_IMPL
#include "ds.h"

typedef struct {
    HWND worker_w, def_view, progman;
    bool is_raised;
} DesktopStuff;

typedef enum {
    COLOR_B,
    COLOR_G,
    COLOR_R,
    COLOR_A,
} PlatformColorEnum;

typedef struct {
    uint8_t c[4];
} Color;

typedef struct {
    RECT rect;
} PlatformMonitor;

#define MAX_PLATFORM_MONITORS 100
typedef struct {
    PlatformMonitor buf[MAX_PLATFORM_MONITORS];
    int len;
} PlatformMonitors;

typedef struct {
    enum {
        NO_EVENT = 0,
        EVENT_TIMEOUT,
        EVENT_QUIT,
        EVENT_ERR,
        EVENT_SYS_TRAY,
        EVENT_SYS_TRAY_MENU,
        NUM_WIN_EVENTS,
    } type;
    enum {
        NO_CLICK,
        CLICK_L_UP,
        CLICK_L_DOWN,
        CLICK_M_UP,
        CLICK_M_DOWN,
        CLICK_R_UP,
        CLICK_R_DOWN,
        NUM_CLICKS,
    } click;
    unsigned int menu_item_id;
} WinEvent;

#define MAX_EVENT_QUEUE_LEN 30
#define SYS_TRAY_MSG (WM_USER + 1)

typedef struct {
    HWND win;
    BITMAPINFO bitmap_info;
    DesktopStuff desktop_stuff;
} PlatformWin;

typedef struct Win {
    int screen, w, h;
    Color *buf;
    bool resized, is_bg;
    WinEvent event_queue[MAX_EVENT_QUEUE_LEN];
    int event_queue_len;
    PlatformWin p;
    u64 hash;
    struct {
        char **buf;
        ssize len;
    } menu_items;
} Win;

#define IMAGE_IMPL
#include "image.h"

static void resize_win(Win *win) {
    if (win->buf) {
        free(win->buf);
        win->buf = 0;
    }

    win->resized = true;
    win->buf = calloc(win->w * win->h, sizeof(*win->buf));

    win->p.bitmap_info = (BITMAPINFO) {
        .bmiHeader = {
            .biSize = sizeof(win->p.bitmap_info.bmiHeader),
            .biWidth = win->w,
            .biHeight = -win->h,
            .biPlanes = 1,
            .biBitCount = 32,
        },
    };
}

static void add_window_style(HWND hwnd, int class, LONG_PTR style_to_add) {
    LONG_PTR old_style = GetWindowLongPtrA(hwnd, class);
    if ((old_style & style_to_add) == 0) {
        SetWindowLongPtrA(hwnd, class, old_style | style_to_add);
    }
}

static BOOL get_last_child_window_cb(HWND top, LPARAM vv) {
    *((HWND *) vv) = top;
    return true;
}

static HWND get_last_child_window(HWND parent) {
    HWND last_child = 0;

    EnumChildWindows(parent, get_last_child_window_cb, (LPARAM) &last_child);

    return last_child;
}

static BOOL collect_monitors_cb(HMONITOR h, HDC hdc, LPRECT rect, LPARAM vv) {
    (void) h;
    (void) hdc;

    PlatformMonitors *monitors = (PlatformMonitors *) vv;
    monitors->buf[monitors->len++] = (PlatformMonitor) {
        .rect = *rect,
    };
    return true;
}

static BOOL get_desktop_stuff_cb(HWND top, LPARAM vv) {
    HWND def_view = FindWindowExA(top, 0, "SHELLDLL_DefView", 0);
    if (def_view) {
        ((DesktopStuff *) vv)->worker_w = FindWindowExA(0, top, "WorkerW", 0);
        ((DesktopStuff *) vv)->def_view = def_view;
    }
    return true;
}

static DesktopStuff get_desktop_stuff() {
    DesktopStuff ret = {0};

    ret.progman = FindWindowA("Progman", 0);

    {
        LONG_PTR ex = GetWindowLongPtrA(ret.progman, GWL_EXSTYLE);
        if (ex) ret.is_raised = (ex & WS_EX_NOREDIRECTIONBITMAP) != 0;
    }

    SendMessageTimeoutA(ret.progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, 0);
    EnumWindows(get_desktop_stuff_cb, (LPARAM) &ret);

    if (ret.is_raised) {
        ret.worker_w = FindWindowExA(ret.progman, 0, "WorkerW", 0);
    }

    return ret;
}

static void fill_working_area(Win *win) {
    assert(win->buf);

    RECT old = {0};
    GetClientRect(win->p.win, &old);

    MONITORINFO info = { .cbSize = sizeof(MONITORINFO) };

    RECT old_abs = {0};
    GetWindowRect(win->p.win, &old_abs);
    POINT point = { old_abs.left, old_abs.top };
    HMONITOR monitor = MonitorFromPoint(point, 0);
    assert(monitor);

    bool yes = GetMonitorInfoA(monitor, &info);
    assert(yes);

    RECT work = info.rcWork;
    int w = work.right - work.left;
    int h = work.bottom - work.top;

    bool resized = work.left != old.left ||
                   work.right != old.right ||
                   work.bottom != old.bottom ||
                   work.top != old.top;

    if (!resized) return;

    SetWindowPos(
        win->p.win,
        HWND_BOTTOM,
        work.left,
        work.top,
        w,
        h,
        SWP_NOACTIVATE | SWP_NOZORDER
    );

    const int is_windows_7 = false;

    if (is_windows_7) {
        SetParent(win->p.win, win->p.desktop_stuff.progman);
    } else if (win->p.desktop_stuff.is_raised) {
        UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

        add_window_style(win->p.win, GWL_STYLE, WS_CHILD);
        add_window_style(win->p.win, GWL_EXSTYLE, WS_EX_LAYERED);
        SetLayeredWindowAttributes(win->p.win, 0, 255, LWA_ALPHA);
        SetParent(win->p.win, win->p.desktop_stuff.progman);
        SetWindowPos(win->p.win, win->p.desktop_stuff.def_view, 0, 0, 0, 0, flags);

        if (get_last_child_window(win->p.desktop_stuff.progman) != win->p.desktop_stuff.worker_w) {
            SetWindowPos(win->p.desktop_stuff.worker_w, HWND_BOTTOM, 0, 0, 0, 0, flags);
        }
    } else {
        SetParent(win->p.win, win->p.desktop_stuff.worker_w);
    }

    SetWindowPos(
        win->p.win,
        HWND_BOTTOM,
        work.left,
        work.top,
        w,
        h,
        SWP_NOACTIVATE | SWP_NOZORDER
    );
}

static void draw_to_win(Win *win, PlatformMonitor *monitor) {
    (void) monitor;

    HDC ctx = GetDC(win->p.win);
    StretchDIBits(
        ctx,
        0, 0, win->w, win->h,
        0, 0, win->w, win->h,
        win->buf,
        &win->p.bitmap_info,
        DIB_RGB_COLORS,
        SRCCOPY
    );
    ReleaseDC(win->p.win, ctx);
}

static LRESULT main_win_cb(HWND pwin, UINT msg, WPARAM hv, LPARAM vv) {
    LRESULT ret = 0;

    Win *win = (Win *) GetWindowLongPtr(pwin, GWLP_USERDATA);
    if (!win) return DefWindowProc(pwin, msg, hv, vv);

    RECT client_rect = {0};
    GetClientRect(pwin, &client_rect);

    win->p.win = pwin;
    win->w = client_rect.right - client_rect.left;
    win->h = client_rect.bottom - client_rect.top;

    WinEvent win_event = {0};

    switch (msg) {
    case WM_TIMER:
        win_event.type = EVENT_TIMEOUT;
        if (win->is_bg) fill_working_area(win);
        break;
    case WM_SIZE:
        resize_win(win);
        break;
    case WM_DESTROY:
    case WM_CLOSE:
        win_event.type = EVENT_QUIT;
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint = {0};
        BeginPaint(pwin, &paint);
        draw_to_win(win, NULL);
        EndPaint(pwin, &paint);
    } break;
    case SYS_TRAY_MSG: {
        win_event.type = EVENT_SYS_TRAY;

        int click = 0;
        switch (LOWORD(vv)) {
        case WM_LBUTTONUP:   click = CLICK_L_UP; break;
        case WM_LBUTTONDOWN: click = CLICK_L_DOWN; break;
        case WM_MBUTTONUP:   click = CLICK_M_UP; break;
        case WM_MBUTTONDOWN: click = CLICK_M_DOWN; break;
        case WM_RBUTTONUP:   click = CLICK_R_UP; break;
        case WM_RBUTTONDOWN: click = CLICK_R_DOWN; break;
        }

        win_event.click = click;
    } break;
    case WM_COMMAND:
        win_event.type = EVENT_SYS_TRAY_MENU;
        win_event.menu_item_id = hv;
        break;
    default:
        ret = DefWindowProc(pwin, msg, hv, vv);
        break;
    }

    if (win_event.type) win->event_queue[win->event_queue_len++] = win_event;
    assert(win->event_queue_len < MAX_EVENT_QUEUE_LEN);

    return ret;
}

static void new_win(Win *win, char *name, int w, int h) {
    *win = (Win) {0};

    WNDCLASS win_class = {
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = main_win_cb,
        .hInstance = GetModuleHandle(0),
        .lpszClassName = name,
        .hCursor = LoadCursor(0, IDC_ARROW),
    };
    RegisterClassA(&win_class);

    RECT work = {0};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);

    win->p.win = CreateWindowExA(
        0,
        win_class.lpszClassName,
        "Untitled Window",
        WS_OVERLAPPEDWINDOW,
        work.left,
        work.top,
        w,
        h,
        0,
        0,
        GetModuleHandle(0),
        0
    );

    SetWindowTextA(win->p.win, name);
    SetWindowLongPtr(win->p.win, GWLP_USERDATA, (LONG_PTR) win);

    win->buf = calloc(w * h, sizeof(*win->buf));
}

static void make_win_bg(Win *win, PlatformMonitor monitor, bool draw_to_root) {
    (void) monitor;
    (void) draw_to_root;

    win->is_bg = true;
    win->p.desktop_stuff = get_desktop_stuff();
    SetWindowLongPtrA(win->p.win, GWL_STYLE, WS_POPUP | WS_SYSMENU);
    fill_working_area(win);
}

static void show_win(Win *win) {
    ShowWindow(win->p.win, SW_NORMAL);
}

static void collect_monitors(PlatformMonitors *monitors) {
    *monitors = (PlatformMonitors) {0};
    EnumDisplayMonitors(NULL, NULL, collect_monitors_cb, (LPARAM) monitors);
}

static void get_events_timeout(Win *win, PlatformMonitor *monitor, int timeout_ms) {
    (void) win;
    (void) monitor;

    UINT_PTR timer_id = SetTimer(0, 1, timeout_ms, 0);

    WaitMessage();

    MSG msg = {0};
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    KillTimer(0, timer_id);
}

static void show_sys_tray_icon(Win *win, int icon_id, char *tooltip) {
    NOTIFYICONDATA nid = {
        .cbSize = sizeof(NOTIFYICONDATA),
        .hWnd = win->p.win,
        .uID = icon_id,
        .uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
        .uCallbackMessage = SYS_TRAY_MSG,
    };
    nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(icon_id));
    lstrcpy(nid.szTip, tooltip);
    if (!Shell_NotifyIcon(NIM_ADD, &nid)) warning("Could not add shell icon.");
}

static void show_sys_tray_menu(Win *win) {
    HMENU menu = CreatePopupMenu();

    for (ssize i = 0; i < win->menu_items.len; i++) {
        AppendMenu(menu, MF_STRING, i + 1, win->menu_items.buf[i]);
    }

    POINT point = {0};
    GetCursorPos(&point);
    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON,
        point.x,
        point.y,
        0,
        win->p.win,
        NULL
    );

    DestroyMenu(menu);
}

static void close_win(Win *win) {
    free(win->buf);
    win->buf = 0;

    if (win->is_bg) {
        SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, 0, SPIF_UPDATEINIFILE);
    }
    DestroyWindow(win->p.win);
}

#include "main.c"

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    if (argc == 3 && !strcmp(argv[1], "--path")) {
        chdir(argv[2]);
    } else {
        char cmd[3 * KiB] = {0};
        ssize cmd_len = 0;

        cmd[cmd_len++] = '"';
        GetModuleFileNameA(NULL, &cmd[cmd_len], arrlen(cmd) - cmd_len);
        cmd_len = strlen(cmd);
        cmd[cmd_len++] = '"';

        cmd[cmd_len++] = ' ';
        cmd[cmd_len++] = '-';
        cmd[cmd_len++] = '-';
        cmd[cmd_len++] = 'p';
        cmd[cmd_len++] = 'a';
        cmd[cmd_len++] = 't';
        cmd[cmd_len++] = 'h';
        cmd[cmd_len++] = ' ';

        cmd[cmd_len++] = '"';
        getcwd(&cmd[cmd_len], arrlen(cmd) - cmd_len);
        cmd_len = strlen(cmd);
        cmd[cmd_len++] = '"';

        HKEY key = NULL;
        RegCreateKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", &key);
        RegSetValueExA(key, APP_NAME, 0, REG_SZ, cmd, strlen(cmd) + 1);
        RegCloseKey(key);
    }

    PlatformMonitors monitors = {0};
    collect_monitors(&monitors);

    Win wins[arrlen(monitors.buf)] = {0};

    for (int i = 0; i < monitors.len; i++) {
        new_win(&wins[i], APP_NAME, 500, 500);
        make_win_bg(&wins[i], monitors.buf[i], true);
        show_win(&wins[i]);
        ctx.monitors[i].screen = (Image) {
            .buf = wins[i].buf,
            .alloc_w = wins[i].w,
            .w = wins[i].w,
            .h = wins[i].h,
        };
    }
    ctx.monitors_len = monitors.len;

    show_sys_tray_icon(&wins[0], ICON_ID, APP_NAME);
    char *menu_items[] = { "Skip image", "Quit", };
    wins[0].menu_items.buf = menu_items;
    wins[0].menu_items.len = arrlen(menu_items);

    start();

    while (true) for (int monitor_i = 0; monitor_i < ctx.monitors_len; monitor_i++) {
        Monitor *monitor = &ctx.monitors[monitor_i];
        Win *win = &wins[monitor_i];

        monitor->screen = (Image) {
            .buf = win->buf,
            .alloc_w = win->w,
            .w = win->w,
            .h = win->h,
        };

        app_loop(monitor_i, true, time(0));

        usleep(100000 * 0.25);

        struct timeval time_val = {0};
        gettimeofday(&time_val, NULL);

        get_events_timeout(
            win,
            &monitors.buf[monitor_i],
            1000 - (time_val.tv_usec / 1000)
        );

        for (int i = 0; i < win->event_queue_len; i++) {
            WinEvent event = win->event_queue[i];
            switch (event.type) {
            case EVENT_QUIT:
                goto end;
            case EVENT_SYS_TRAY: {
                int click = event.click;
                if (click == CLICK_L_UP ||
                    click == CLICK_M_UP ||
                    click == CLICK_R_UP) {
                    show_sys_tray_menu(win);
                }
            } break;
            case EVENT_SYS_TRAY_MENU: {
                unsigned int id = event.menu_item_id - 1;
                if (id < win->menu_items.len) {
                    char *item = win->menu_items.buf[id];
                    if (!strcmp(item, "Skip image")) ctx.skip_image = true;
                    else if (!strcmp(item, "Quit")) goto end;
                }
            } break;
            default:
                break;
            }
        }

        win->event_queue_len = 0;

        draw_to_win(win, &monitors.buf[monitor_i]);
    }

end:
    for (int win_i = 0; win_i < ctx.monitors_len; win_i++) {
        close_win(&wins[win_i]);
    }
}
