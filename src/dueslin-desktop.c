/*
 * dueslin-desktop.c — DUESLIN 极简桌面壳（纯 C + Xlib + libpng，无依赖 WM）
 * 功能：壁纸背景（PNG）+ 底部任务栏 + 开始菜单 + 程序启动 + 时钟
 *       内置设置窗口：更换壁纸 / 动态背景（幻灯片轮换）
 * 编译：gcc -O2 -o dueslin-desktop dueslin-desktop.c -lX11 -lpng
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <poll.h>

#define SCR_W 1024
#define SCR_H 768
#define BAR_H 48
#define MENU_W 240
#define MENU_H 330

/* 颜色 */
#define COL_BG_BOT   0x3D8BFF   /* 无壁纸时的海蓝底 */
#define COL_BAR      0x0F1E33   /* 任务栏深色 */
#define COL_BTN      0x1A73E8   /* 开始按钮 */
#define COL_MENU     0xFFFFFF
#define COL_MENU_TXT 0x111827
#define COL_MENU_HOV 0xE8F0FE
#define COL_WHITE    0xFFFFFF
#define COL_GRAY     0x9CA3AF

#define WP_SYSTEM  "/usr/share/dueslin/wallpaper.png"
#define WP_DIR     "/usr/share/dueslin/wallpapers"
#define WP_HOME    "Pictures"

typedef struct {
    unsigned char *rgb;   /* 3 字节/像素 */
    int w, h;
} Wp;

typedef struct {
    Display *dpy;
    Window win;
    GC gc;
    int sw, sh;
    int menu_open;
    int hover_item;
    char clock[32];
    int start_btn[4];      /* x,y,w,h */
    int menu_rect[4];
    int items_rect[8][4];
    const char *items[8];
    const char *cmds[8];
    int nitems;

    /* 壁纸 */
    Wp *wp;
    char wp_path[512];
    char **wallpapers;     /* 壁纸候选列表 */
    int nwps, wp_index;
    int dynamic_on;        /* 动态背景开关 */
    int dyn_interval;      /* 轮换秒数 */

    /* 设置窗口 */
    Window setwin;
    int set_open;
    int set_rect[4];       /* 设置窗口矩形 */
    int wp_rows[12][4];    /* 壁纸候选行 */
    int nrows;
    int dyn_toggle[4];
    int dyn_minus[4], dyn_plus[4];
    int set_close[4];
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

static void fill_win(Display *d, GC gc, unsigned color, Window win, int x, int y, int w, int h) {
    XSetForeground(d, gc, color);
    XFillRectangle(d, win, gc, x, y, w, h);
}

static void draw_text(Desktop *dt, unsigned color, int x, int y, const char *s) {
    XSetForeground(dt->dpy, dt->gc, color);
    XDrawString(dt->dpy, dt->win, dt->gc, x, y, s, (int)strlen(s));
}

static void draw_text_win(Display *d, GC gc, unsigned color, Window win, Font f,
                          int x, int y, const char *s) {
    XSetForeground(d, gc, color);
    XSetFont(d, gc, f);
    XDrawString(d, win, gc, x, y, s, (int)strlen(s));
}

/* ---------- PNG 壁纸加载 ---------- */
static Wp *wp_load(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    if (!png || !info) {
        if (png) png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return NULL;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }
    png_init_io(png, fp);
    png_read_info(png, info);
    int w = png_get_image_width(png, info);
    int h = png_get_image_height(png, info);
    png_byte color = png_get_color_type(png, info);
    png_byte depth = png_get_bit_depth(png, info);
    if (depth == 16) png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_read_update_info(png, info);
    int channels = png_get_channels(png, info);

    Wp *wp = malloc(sizeof(Wp));
    wp->w = w; wp->h = h;
    wp->rgb = malloc((size_t)w * h * 3);
    png_bytep *rows = malloc(sizeof(png_bytep) * h);
    for (int i = 0; i < h; i++) rows[i] = malloc((size_t)w * channels);
    png_read_image(png, rows);
    for (int i = 0; i < h; i++) {
        unsigned char *src = rows[i], *dst = wp->rgb + (size_t)i * w * 3;
        for (int j = 0; j < w; j++) {
            dst[j*3]   = src[j*channels];
            dst[j*3+1] = src[j*channels+1];
            dst[j*3+2] = src[j*channels+2];
        }
        free(rows[i]);
    }
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return wp;
}

static void wp_draw(Desktop *dt, Wp *wp) {
    if (!wp || !wp->rgb) return;
    int scr = DefaultScreen(dt->dpy);
    Visual *vis = DefaultVisual(dt->dpy, scr);
    /* 让 X 计算行宽，再分配匹配的 data */
    XImage *img = XCreateImage(dt->dpy, vis, 24, ZPixmap, 0, NULL,
                               wp->w, wp->h, 32, 0);
    if (!img) return;
    img->data = malloc((size_t)img->bytes_per_line * wp->h);
    for (int i = 0; i < wp->h; i++)
        memcpy(img->data + (size_t)i * img->bytes_per_line,
               wp->rgb + (size_t)i * wp->w * 3, (size_t)wp->w * 3);
    int x = (dt->sw - wp->w) / 2; if (x < 0) x = 0;
    int y = (dt->sh - BAR_H - wp->h) / 2; if (y < 0) y = 0;
    XPutImage(dt->dpy, dt->win, dt->gc, img, 0, 0, x, y, wp->w, wp->h);
    XDestroyImage(img);   /* free data */
}

static void wp_free(Wp *wp) {
    if (wp) { free(wp->rgb); free(wp); }
}

static void draw_bg_full(Desktop *dt);   /* 前向声明 */

/* 收集壁纸候选：系统目录 + 用户 Pictures */
static void collect_wallpapers(Desktop *dt) {
    if (dt->wallpapers) {
        for (int i = 0; i < dt->nwps; i++) free(dt->wallpapers[i]);
        free(dt->wallpapers);
    }
    dt->wallpapers = NULL; dt->nwps = 0;
    char **list = malloc(sizeof(char *) * 64);
    int n = 0;
    /* 系统壁纸目录 */
    DIR *d = opendir(WP_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < 60) {
            if (strstr(e->d_name, ".png") || strstr(e->d_name, ".jpg") ||
                strstr(e->d_name, ".jpeg") || strstr(e->d_name, ".PNG")) {
                char p[512];
                snprintf(p, sizeof p, "%s/%s", WP_DIR, e->d_name);
                list[n++] = strdup(p);
            }
        }
        closedir(d);
    }
    if (access(WP_SYSTEM, F_OK) == 0)
        list[n++] = strdup(WP_SYSTEM);
    /* 用户 Pictures */
    char home[256] = "/root";
    const char *env = getenv("HOME");
    if (env) snprintf(home, sizeof home, "%s", env);
    char pics[512];
    snprintf(pics, sizeof pics, "%s/%s", home, WP_HOME);
    d = opendir(pics);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < 60) {
            if (strstr(e->d_name, ".png") || strstr(e->d_name, ".jpg") ||
                strstr(e->d_name, ".jpeg") || strstr(e->d_name, ".PNG")) {
                char p[600];
                snprintf(p, sizeof p, "%s/%s", pics, e->d_name);
                list[n++] = strdup(p);
            }
        }
        closedir(d);
    }
    dt->wallpapers = list;
    dt->nwps = n;
    if (n > 0) {
        /* 当前壁纸定位 */
        for (int i = 0; i < n; i++) {
            if (strcmp(dt->wallpapers[i], dt->wp_path) == 0) { dt->wp_index = i; break; }
        }
    }
}

static void apply_wallpaper(Desktop *dt, const char *path) {
    Wp *nw = wp_load(path);
    if (!nw) return;
    wp_free(dt->wp);
    dt->wp = nw;
    snprintf(dt->wp_path, sizeof dt->wp_path, "%s", path);
    draw_bg_full(dt);
    XFlush(dt->dpy);
}

/* ---------- 绘制 ---------- */
static void draw_bg_full(Desktop *dt) {
    int h = dt->sh - BAR_H;
    fill(dt, COL_BG_BOT, 0, 0, dt->sw, h);
    if (dt->wp) {
        wp_draw(dt, dt->wp);
        /* 右下角小 Logo 标记 */
        int lx = dt->sw - 64, ly = h - 64;
        fill(dt, mkcol(dt->dpy, 0x1A73E8), lx, ly, 40, 40);
        XSetForeground(dt->dpy, dt->gc, mkcol(dt->dpy, 0x000000));
        XSetFont(dt->dpy, dt->gc, loadfont(dt->dpy, "-cronyx-helvetica-bold-o-normal--24-170-100-100-p-60-koi8-r"));
        XDrawString(dt->dpy, dt->win, dt->gc, lx + 10, ly + 29, "D", 1);
        XSetFont(dt->dpy, dt->gc, loadfont(dt->dpy, "fixed"));
    } else {
        /* 无壁纸：中央 Logo 方块 + 黑 D */
        int lx = dt->sw / 2 - 60, ly = h / 2 - 60;
        fill(dt, mkcol(dt->dpy, 0x1A73E8), lx, ly, 120, 120);
        XSetForeground(dt->dpy, dt->gc, mkcol(dt->dpy, 0x000000));
        XSetFont(dt->dpy, dt->gc, loadfont(dt->dpy, "-cronyx-helvetica-bold-o-normal--34-240-100-100-p-90-koi8-r"));
        XDrawString(dt->dpy, dt->win, dt->gc, lx + 34, ly + 84, "D", 1);
        XSetFont(dt->dpy, dt->gc, loadfont(dt->dpy, "fixed"));
    }
}

static void draw_bar(Desktop *dt) {
    int y = dt->sh - BAR_H;
    fill(dt, COL_BAR, 0, y, dt->sw, BAR_H);
    fill(dt, COL_BTN, 6, y + 7, 124, BAR_H - 14);
    XSetForeground(dt->dpy, dt->gc, mkcol(dt->dpy, 0xFFFFFF));
    XSetFont(dt->dpy, dt->gc, loadfont(dt->dpy, "-cronyx-helvetica-bold-o-normal--20-140-100-100-p-77-koi8-r"));
    XDrawString(dt->dpy, dt->win, dt->gc, 24, y + 31, "DUESLIN", 7);
    XSetFont(dt->dpy, dt->gc, XLoadFont(dt->dpy, "fixed"));
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
    fill(dt, mkcol(dt->dpy, 0x1A73E8), 8, y, MENU_W, 36);
    XSetForeground(dt->dpy, dt->gc, mkcol(dt->dpy, 0xFFFFFF));
    XSetFont(dt->dpy, dt->gc, loadfont(dt->dpy, "-cronyx-helvetica-bold-o-normal--14-100-100-100-p-56-koi8-r"));
    XDrawString(dt->dpy, dt->win, dt->gc, 20, y + 24, "DUESLIN Menu", 12);
    XSetFont(dt->dpy, dt->gc, XLoadFont(dt->dpy, "fixed"));
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

/* ---------- 设置窗口 ---------- */
static void settings_draw(Desktop *dt) {
    if (!dt->set_open) return;
    Display *d = dt->dpy;
    GC gc = dt->gc;
    Window w = dt->setwin;
    int W = 460, H = 380;
    dt->set_rect[0] = dt->sw / 2 - W / 2;
    dt->set_rect[1] = 60;
    dt->set_rect[2] = W; dt->set_rect[3] = H;
    fill_win(d, gc, 0xF3F6FA, w, 0, 0, W, H);
    fill_win(d, gc, 0x1A73E8, w, 0, 0, W, 40);
    draw_text_win(d, gc, 0xFFFFFF, w, loadfont(d, "-cronyx-helvetica-bold-o-normal--16-110-100-100-p-60-koi8-r"), 16, 27, "DUESLIN 设置");
    /* 关闭 */
    dt->set_close[0] = W - 44; dt->set_close[1] = 4;
    dt->set_close[2] = 38; dt->set_close[3] = 32;
    fill_win(d, gc, 0xC33, w, dt->set_close[0], dt->set_close[1], 38, 32);
    draw_text_win(d, gc, 0xFFFFFF, w, loadfont(d, "fixed"), dt->set_close[0] + 12, dt->set_close[1] + 22, "X");

    /* 壁纸区 */
    int y = 56;
    draw_text_win(d, gc, 0x111827, w, loadfont(d, "-cronyx-helvetica-bold-o-normal--12-90-100-100-p-56-koi8-r"), 16, y + 14, "壁纸 (点击应用)");
    y += 24;
    dt->nrows = dt->nwps < 8 ? dt->nwps : 8;
    for (int i = 0; i < dt->nrows; i++) {
        int ry = y + i * 24;
        dt->wp_rows[i][0] = 12; dt->wp_rows[i][1] = ry;
        dt->wp_rows[i][2] = W - 24; dt->wp_rows[i][3] = 22;
        fill_win(d, gc, (strcmp(dt->wallpapers[i], dt->wp_path) == 0) ? 0xD6E4FF : 0xFFFFFF,
                 w, 12, ry, W - 24, 22);
        draw_text_win(d, gc, 0x333, w, loadfont(d, "fixed"), 20, ry + 16,
                      strrchr(dt->wallpapers[i], '/') ? strrchr(dt->wallpapers[i], '/') + 1
                                                       : dt->wallpapers[i]);
    }
    y += dt->nrows * 24 + 8;

    /* 动态背景 */
    draw_text_win(d, gc, 0x111827, w, loadfont(d, "-cronyx-helvetica-bold-o-normal--12-90-100-100-p-56-koi8-r"), 16, y + 14, "动态背景 (自动轮换)");
    dt->dyn_toggle[0] = 16; dt->dyn_toggle[1] = y + 22;
    dt->dyn_toggle[2] = 70; dt->dyn_toggle[3] = 28;
    fill_win(d, gc, dt->dynamic_on ? 0x34A853 : 0xCCCCCC, w, 16, y + 22, 70, 28);
    draw_text_win(d, gc, 0xFFFFFF, w, loadfont(d, "fixed"), 26, y + 41, dt->dynamic_on ? "开" : "关");
    /* 间隔 */
    dt->dyn_minus[0] = 100; dt->dyn_minus[1] = y + 22;
    dt->dyn_minus[2] = 30; dt->dyn_minus[3] = 28;
    dt->dyn_plus[0] = 200; dt->dyn_plus[1] = y + 22;
    dt->dyn_plus[2] = 30; dt->dyn_plus[3] = 28;
    fill_win(d, gc, 0x1A73E8, w, 100, y + 22, 30, 28);
    fill_win(d, gc, 0x1A73E8, w, 200, y + 22, 30, 28);
    draw_text_win(d, gc, 0xFFFFFF, w, loadfont(d, "fixed"), 110, y + 41, "-");
    draw_text_win(d, gc, 0xFFFFFF, w, loadfont(d, "fixed"), 210, y + 41, "+");
    char iv[32];
    snprintf(iv, sizeof iv, "间隔 %d 秒", dt->dyn_interval);
    draw_text_win(d, gc, 0x333, w, loadfont(d, "fixed"), 240, y + 41, iv);
    XFlush(d);
}

static void open_settings(Desktop *dt) {
    if (dt->set_open) { XRaiseWindow(dt->dpy, dt->setwin); return; }
    dt->set_open = 1;
    int W = 460, H = 380;
    XSetWindowAttributes att;
    att.override_redirect = True;
    att.background_pixel = mkcol(dt->dpy, 0xF3F6FA);
    att.event_mask = ButtonPressMask | ExposureMask;
    dt->setwin = XCreateWindow(dt->dpy, DefaultRootWindow(dt->dpy),
                               dt->sw / 2 - W / 2, 60, W, H, 1, CopyFromParent,
                               InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask, &att);
    XStoreName(dt->dpy, dt->setwin, "DUESLIN Settings");
    XMapRaised(dt->dpy, dt->setwin);
    settings_draw(dt);
}

static void close_settings(Desktop *dt) {
    if (!dt->set_open) return;
    XUnmapWindow(dt->dpy, dt->setwin);
    XDestroyWindow(dt->dpy, dt->setwin);
    dt->set_open = 0;
}

static void settings_click(Desktop *dt, int x, int y) {
    /* 关闭 */
    if (x >= dt->set_close[0] && x < dt->set_close[0] + dt->set_close[2] &&
        y >= dt->set_close[1] && y < dt->set_close[1] + dt->set_close[3]) {
        close_settings(dt);
        return;
    }
    /* 壁纸行 */
    for (int i = 0; i < dt->nrows; i++) {
        if (x >= dt->wp_rows[i][0] && x < dt->wp_rows[i][0] + dt->wp_rows[i][2] &&
            y >= dt->wp_rows[i][1] && y < dt->wp_rows[i][1] + dt->wp_rows[i][3]) {
            apply_wallpaper(dt, dt->wallpapers[i]);
            settings_draw(dt);
            draw_bg_full(dt);
            draw_bar(dt);
            XFlush(dt->dpy);
            return;
        }
    }
    /* 动态开关 */
    if (x >= dt->dyn_toggle[0] && x < dt->dyn_toggle[0] + dt->dyn_toggle[2] &&
        y >= dt->dyn_toggle[1] && y < dt->dyn_toggle[1] + dt->dyn_toggle[3]) {
        dt->dynamic_on = !dt->dynamic_on;
        if (dt->dynamic_on && dt->nwps < 2) dt->dynamic_on = 0;
        settings_draw(dt);
        return;
    }
    if (x >= dt->dyn_minus[0] && x < dt->dyn_minus[0] + dt->dyn_minus[2] &&
        y >= dt->dyn_minus[1] && y < dt->dyn_minus[1] + dt->dyn_minus[3]) {
        if (dt->dyn_interval > 3) dt->dyn_interval -= 1;
        settings_draw(dt);
        return;
    }
    if (x >= dt->dyn_plus[0] && x < dt->dyn_plus[0] + dt->dyn_plus[2] &&
        y >= dt->dyn_plus[1] && y < dt->dyn_plus[1] + dt->dyn_plus[3]) {
        if (dt->dyn_interval < 300) dt->dyn_interval += 1;
        settings_draw(dt);
        return;
    }
}

/* 动态轮换下一张 */
static void next_wallpaper(Desktop *dt) {
    if (!dt->dynamic_on || dt->nwps < 2) return;
    dt->wp_index = (dt->wp_index + 1) % dt->nwps;
    Wp *nw = wp_load(dt->wallpapers[dt->wp_index]);
    if (!nw) return;
    wp_free(dt->wp);
    dt->wp = nw;
    snprintf(dt->wp_path, sizeof dt->wp_path, "%s", dt->wallpapers[dt->wp_index]);
    draw_bg_full(dt);
    draw_bar(dt);
    if (dt->set_open) settings_draw(dt);
    XFlush(dt->dpy);
}

static void toggle_menu(Desktop *dt, int open) {
    dt->menu_open = open;
    dt->hover_item = -1;
    if (open) draw_menu(dt);
    else {
        int y = dt->sh - BAR_H - MENU_H - 4;
        draw_bg_full(dt);
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

    dt.items[0] = "Terminal";       dt.cmds[0] = "xterm";
    dt.items[1] = "Browser (Edge)"; dt.cmds[1] = "microsoft-edge";
    dt.items[2] = "File Manager";   dt.cmds[2] = "thunar";
    dt.items[3] = "Settings";       dt.cmds[3] = "__settings__";
    dt.items[4] = "About";          dt.cmds[4] = "xterm -e sh -c 'duiver; sleep 5'";
    dt.items[5] = "Lock";           dt.cmds[5] = "xsetroot -name lock";
    dt.items[6] = "Reboot";         dt.cmds[6] = "reboot";
    dt.items[7] = "Shutdown";       dt.cmds[7] = "poweroff";
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
    dt.start_btn[2] = 124; dt.start_btn[3] = BAR_H - 14;

    dt.dyn_interval = 10;
    snprintf(dt.wp_path, sizeof dt.wp_path, "%s", WP_SYSTEM);
    collect_wallpapers(&dt);
    /* 默认壁纸：系统壁纸（若有） */
    dt.wp = wp_load(WP_SYSTEM);
    if (dt.wp) snprintf(dt.wp_path, sizeof dt.wp_path, "%s", WP_SYSTEM);

    draw_bg_full(&dt);
    draw_bar(&dt);
    XFlush(dt.dpy);

    int running = 1;
    time_t last_swap = time(NULL);
    while (running) {
        /* 动态背景定时检查 */
        if (dt.dynamic_on) {
            time_t now = time(NULL);
            if (now - last_swap >= dt.dyn_interval) {
                last_swap = now;
                next_wallpaper(&dt);
            }
        }
        while (XPending(dt.dpy) > 0) {
            XEvent ev;
            XNextEvent(dt.dpy, &ev);
            if (ev.xany.window == dt.setwin && dt.set_open) {
                if (ev.type == ButtonPress) {
                    settings_click(&dt, ev.xbutton.x, ev.xbutton.y);
                } else if (ev.type == Expose) {
                    settings_draw(&dt);
                }
                continue;
            }
            switch (ev.type) {
            case Expose:
                draw_bg_full(&dt);
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
                    toggle_menu(&dt, 0);
                    if (m >= 0) {
                        if (strcmp(dt.cmds[m], "__settings__") == 0) open_settings(&dt);
                        else launch(&dt, dt.cmds[m]);
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
        usleep(200000);
    }
    return 0;
}
