/*
 * dueslin-desktop.c — DUESLIN 极简桌面壳（纯 C + Xlib，无依赖 WM）
 * 功能：海蓝渐变壁纸 + 底部任务栏 + 开始菜单 + 程序启动 + 时钟
 * 编译：gcc -O2 -o dueslin-desktop dueslin-desktop.c -lX11
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define SCR_W 1024
#define SCR_H 768
#define BAR_H 48
#define MENU_W 230
#define MENU_H 320

/* 颜色 */
#define COL_BG_TOP   0x9ECBFF
#define COL_BG_BOT   0x3D8BFF   /* 桌面背景浅海蓝 */
#define COL_BAR      0x0F1E33   /* 任务栏深色 */
#define COL_BTN      0x1A73E8   /* 开始按钮 */
#define COL_MENU     0xFFFFFF
#define COL_MENU_TXT 0x111827
#define COL_MENU_HOV 0xE8F0FE
#define COL_WHITE    0xFFFFFF
#define COL_GRAY     0x9CA3AF

typedef struct {
    Display *dpy;
    Window win;
    GC gc;
    int sw, sh;
    int menu_open;
    int hover_item;
    char clock[32];
    int clock_w;
    int start_btn[4];      /* x,y,w,h */
    int menu_rect[4];
    int items_rect[8][4];  /* 菜单项矩形 */
    const char *items[8];
    const char *cmds[8];
    int nitems;
} Desktop;

static unsigned long mkcol(Display *d, unsigned rgb) {
    XColor c;
    c.red   = ((rgb >> 16) & 0xFF) * 0x101;
    c.green = ((rgb >> 8)  & 0xFF) * 0x101;
    c.blue  = (rgb & 0xFF) * 0x101;
    c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(d, DefaultColormap(d, DefaultScreen(d)), &c);
    return c.pixel;
}

static Font loadfont(Display *d, const char *name) {
    Font f = XLoadFont(d, name);
    if (!f) f = XLoadFont(d, "fixed");
    return f;
}

static void fill(Desktop *dt, unsigned color, int x, int y, int w, int h) {
    XSetForeground(dt->dpy, dt->gc, color);
    XFillRectangle(dt->dpy, dt->win, dt->gc, x, y, w, h);
}

static void draw_text(Desktop *dt, unsigned color, int x, int y, const char *s) {
    XSetForeground(dt->dpy, dt->gc, color);
    XDrawString(dt->dpy, dt->win, dt->gc, x, y, s, (int)strlen(s));
}

static void draw_bg(Desktop *dt) {
    int h = dt->sh - BAR_H;
    fill(dt, COL_BG_BOT, 0, 0, dt->sw, h);
    /* 中央 Logo：蓝色方块 + 黑色大 D */
    int lx = dt->sw / 2 - 60, ly = h / 2 - 60;
    fill(dt, mkcol(dt->dpy, 0x1A73E8), lx, ly, 120, 120);
    XSetForeground(dt->dpy, dt->gc, mkcol(dt->dpy, 0x000000));
    XSetFont(dt->dpy, dt->gc, loadfont(dt->dpy, "-cronyx-helvetica-bold-o-normal--34-240-100-100-p-90-koi8-r"));
    XDrawString(dt->dpy, dt->win, dt->gc, lx + 34, ly + 84, "D", 1);
    XSetFont(dt->dpy, dt->gc, loadfont(dt->dpy, "fixed"));
}

static void draw_bar(Desktop *dt) {
    int y = dt->sh - BAR_H;
    fill(dt, COL_BAR, 0, y, dt->sw, BAR_H);
    /* 开始按钮 */
    fill(dt, COL_BTN, 6, y + 7, 118, BAR_H - 14);
    XSetForeground(dt->dpy, dt->gc, mkcol(dt->dpy, 0xFFFFFF));
    XSetFont(dt->dpy, dt->gc, loadfont(dt->dpy, "-cronyx-helvetica-bold-o-normal--20-140-100-100-p-77-koi8-r"));
    XDrawString(dt->dpy, dt->win, dt->gc, 24, y + 31, "DUESLIN", 7);
    XSetFont(dt->dpy, dt->gc, XLoadFont(dt->dpy, "fixed"));
    /* 时钟 */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(dt->clock, sizeof dt->clock, "%02d:%02d", tm->tm_hour, tm->tm_min);
    XSetForeground(dt->dpy, dt->gc, mkcol(dt->dpy, 0xFFFFFF));
    XDrawString(dt->dpy, dt->win, dt->gc, dt->sw - 70, y + 30, dt->clock, 5);
}

static void draw_menu(Desktop *dt) {
    if (!dt->menu_open) return;
    int y = dt->sh - BAR_H - MENU_H - 4;
    dt->menu_rect[0] = 8; dt->menu_rect[1] = y;
    dt->menu_rect[2] = MENU_W; dt->menu_rect[3] = MENU_H;
    fill(dt, COL_MENU, 8, y, MENU_W, MENU_H);
    /* 菜单标题 */
    fill(dt, mkcol(dt->dpy, 0x1A73E8), 8, y, MENU_W, 36);
    XSetForeground(dt->dpy, dt->gc, mkcol(dt->dpy, 0xFFFFFF));
    XSetFont(dt->dpy, dt->gc, loadfont(dt->dpy, "-cronyx-helvetica-bold-o-normal--14-100-100-100-p-56-koi8-r"));
    XDrawString(dt->dpy, dt->win, dt->gc, 20, y + 24, "DUESLIN Menu", 12);
    XSetFont(dt->dpy, dt->gc, XLoadFont(dt->dpy, "fixed"));
    /* 菜单项 */
    for (int i = 0; i < dt->nitems; i++) {
        int iy = y + 40 + i * 34;
        dt->items_rect[i][0] = 8; dt->items_rect[i][1] = iy;
        dt->items_rect[i][2] = MENU_W; dt->items_rect[i][3] = 32;
        if (i == dt->hover_item) fill(dt, COL_MENU_HOV, 8, iy, MENU_W - 2, 32);
        XSetForeground(dt->dpy, dt->gc, mkcol(dt->dpy, COL_MENU_TXT));
        XDrawString(dt->dpy, dt->win, dt->gc, 22, iy + 22, dt->items[i], (int)strlen(dt->items[i]));
    }
}

static void launch(Desktop *dt, const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(1);
    }
}

static void toggle_menu(Desktop *dt, int open) {
    dt->menu_open = open;
    dt->hover_item = -1;
    if (open) draw_menu(dt);
    else {
        int y = dt->sh - BAR_H - MENU_H - 4;
        draw_bg(dt);
        draw_bar(dt);
        XClearArea(dt->dpy, dt->win, 8, y, MENU_W, MENU_H, 0);
        XFlush(dt->dpy);
    }
}

int main(int argc, char **argv) {
    Desktop dt;
    memset(&dt, 0, sizeof dt);
    dt.dpy = XOpenDisplay(NULL);
    if (!dt.dpy) {
        fprintf(stderr, "dueslin-desktop: cannot open display\n");
        return 1;
    }
    int scr = DefaultScreen(dt.dpy);
    dt.sw = DisplayWidth(dt.dpy, scr);
    dt.sh = DisplayHeight(dt.dpy, scr);
    dt.sw = dt.sw < 640 ? 640 : dt.sw;
    dt.sh = dt.sh < 480 ? 480 : dt.sh;

    dt.items[0] = "Terminal";      dt.cmds[0] = "xterm";
    dt.items[1] = "Firefox";       dt.cmds[1] = "firefox";
    dt.items[2] = "File Manager";  dt.cmds[2] = "thunar";
    dt.items[3] = "Settings";      dt.cmds[3] = "dueslin-setup-gui";
    dt.items[4] = "About";         dt.cmds[4] = "xterm -e sh -c 'duiver; sleep 5'";
    dt.items[5] = "Lock";          dt.cmds[5] = "xsetroot -name lock";
    dt.items[6] = "Reboot";        dt.cmds[6] = "reboot";
    dt.items[7] = "Shutdown";      dt.cmds[7] = "poweroff";
    dt.nitems = 8;

    XSetWindowAttributes att;
    att.override_redirect = True;
    att.background_pixel = mkcol(dt.dpy, COL_BG_BOT);
    att.event_mask = ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | ExposureMask;
    dt.win = XCreateWindow(dt.dpy, DefaultRootWindow(dt.dpy),
                           0, 0, dt.sw, dt.sh, 0, CopyFromParent,
                           InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel | CWEventMask, &att);
    XStoreName(dt.dpy, dt.win, "DUESLIN Desktop");
    XMapRaised(dt.dpy, dt.win);
    dt.gc = XCreateGC(dt.dpy, dt.win, 0, NULL);
    XSetForeground(dt.dpy, dt.gc, mkcol(dt.dpy, COL_WHITE));

    dt.start_btn[0] = 6; dt.start_btn[1] = dt.sh - BAR_H + 7;
    dt.start_btn[2] = 118; dt.start_btn[3] = BAR_H - 14;

    draw_bg(&dt);
    draw_bar(&dt);
    XFlush(dt.dpy);

    int running = 1;
    while (running) {
        XEvent ev;
        XNextEvent(dt.dpy, &ev);
        switch (ev.type) {
        case Expose:
            draw_bg(&dt);
            draw_bar(&dt);
            if (dt.menu_open) draw_menu(&dt);
            XFlush(dt.dpy);
            break;
        case ButtonPress: {
            int x = ev.xbutton.x, y = ev.xbutton.y;
            if (dt.menu_open) {
                int m = -1;
                for (int i = 0; i < dt.nitems; i++) {
                    if (x >= dt.items_rect[i][0] && x < dt.items_rect[i][0] + dt.items_rect[i][2] &&
                        y >= dt.items_rect[i][1] && y < dt.items_rect[i][1] + dt.items_rect[i][3]) {
                        m = i; break;
                    }
                }
                if (m >= 0) {
                    toggle_menu(&dt, 0);
                    launch(&dt, dt.cmds[m]);
                } else {
                    toggle_menu(&dt, 0);
                }
            } else if (x >= dt.start_btn[0] && x < dt.start_btn[0] + dt.start_btn[2] &&
                       y >= dt.start_btn[1] && y < dt.start_btn[1] + dt.start_btn[3]) {
                toggle_menu(&dt, 1);
            }
            XFlush(dt.dpy);
            break;
        }
        case MotionNotify:
            if (dt.menu_open) {
                int m = -1;
                for (int i = 0; i < dt.nitems; i++) {
                    if (ev.xmotion.x >= dt.items_rect[i][0] &&
                        ev.xmotion.x < dt.items_rect[i][0] + dt.items_rect[i][2] &&
                        ev.xmotion.y >= dt.items_rect[i][1] &&
                        ev.xmotion.y < dt.items_rect[i][1] + dt.items_rect[i][3]) {
                        m = i; break;
                    }
                }
                if (m != dt.hover_item) {
                    dt.hover_item = m;
                    draw_menu(&dt);
                    XFlush(dt.dpy);
                }
            }
            break;
        default:
            break;
        }
    }
    return 0;
}
