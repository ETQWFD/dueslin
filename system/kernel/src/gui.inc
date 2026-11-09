#include "font8x16.h"

/* WCT-OS windowed GUI: GOP, built-in font, native USB HID boot mouse,
   Simple/Absolute pointer fallback, draggable overlapping windows. */

typedef struct {
    UINT8 RequestType, Request;
    UINT16 Value, Index, Length;
} UsbDeviceRequest;

typedef struct __attribute__((packed)) {
    UINT8 Length, DescriptorType, InterfaceNumber, AlternateSetting, NumEndpoints;
    UINT8 InterfaceClass, InterfaceSubClass, InterfaceProtocol, Interface;
} UsbInterfaceDescriptor;

typedef struct __attribute__((packed)) {
    UINT8 Length, DescriptorType, EndpointAddress, Attributes;
    UINT16 MaxPacketSize;
    UINT8 Interval;
} UsbEndpointDescriptor;

typedef struct EFI_USB_IO_PROTOCOL EFI_USB_IO_PROTOCOL;
struct EFI_USB_IO_PROTOCOL {
    EFI_STATUS (EFIAPI *UsbControlTransfer)(EFI_USB_IO_PROTOCOL *, UsbDeviceRequest *, UINT32, UINT32, VOID *, UINTN, UINT32 *);
    EFI_STATUS (EFIAPI *UsbBulkTransfer)(EFI_USB_IO_PROTOCOL *, UINT8, VOID *, UINTN *, UINTN, UINT32 *);
    EFI_STATUS (EFIAPI *UsbAsyncInterruptTransfer)(EFI_USB_IO_PROTOCOL *, UINT8, BOOLEAN, UINTN, UINTN, VOID *, VOID *);
    EFI_STATUS (EFIAPI *UsbSyncInterruptTransfer)(EFI_USB_IO_PROTOCOL *, UINT8, VOID *, UINTN *, UINTN, UINT32 *);
    EFI_STATUS (EFIAPI *UsbIsochronousTransfer)(EFI_USB_IO_PROTOCOL *, UINT8, VOID *, UINTN, UINTN, UINT32 *);
    EFI_STATUS (EFIAPI *UsbAsyncIsochronousTransfer)(EFI_USB_IO_PROTOCOL *, UINT8, BOOLEAN, UINTN, UINTN, VOID *, VOID *);
    EFI_STATUS (EFIAPI *UsbGetDeviceDescriptor)(EFI_USB_IO_PROTOCOL *, VOID *);
    EFI_STATUS (EFIAPI *UsbGetConfigDescriptor)(EFI_USB_IO_PROTOCOL *, VOID *);
    EFI_STATUS (EFIAPI *UsbGetInterfaceDescriptor)(EFI_USB_IO_PROTOCOL *, UsbInterfaceDescriptor *);
    EFI_STATUS (EFIAPI *UsbGetEndpointDescriptor)(EFI_USB_IO_PROTOCOL *, UINT8, UsbEndpointDescriptor *);
    EFI_STATUS (EFIAPI *UsbGetStringDescriptor)(EFI_USB_IO_PROTOCOL *, UINT16, UINT16, VOID **);
    EFI_STATUS (EFIAPI *UsbGetSupportedLanguages)(EFI_USB_IO_PROTOCOL *, UINT16 **, UINT16 *);
    EFI_STATUS (EFIAPI *UsbPortReset)(EFI_USB_IO_PROTOCOL *);
};

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gfx;
static EFI_SIMPLE_POINTER_PROTOCOL *mouse;
static EFI_ABSOLUTE_POINTER_PROTOCOL *abs_ptr;
static EFI_USB_IO_PROTOCOL *hid_io;
static UINT8 hid_endpoint;
static int hid_ready;
static UINT32 *gfx_fb, *gfx_hw, *gfx_back;
static UINTN gfx_w, gfx_h, gfx_stride;
static UINT32 gfx_bg, gfx_top, gfx_panel, gfx_text_c, gfx_dim, gfx_accent;
static UINTN gfx_original_mode;
static int mouse_x, mouse_y, mouse_down, mouse_was_down, mouse_click, mouse_moved;
static int gfx_done;

#define GFX_DESKTOP 0
#define WIN_FILES 0
#define WIN_TERM 1
#define WIN_SETTINGS 2
#define WIN_SETTINGS 2
#define WIN_MAX 4
#define WIN_MAX_LINES 12
#define WIN_COLS 68
#define WIN_ENTRIES 32
typedef struct { CHAR16 name[64]; int dir; UINT64 size; } GuiEnt;

typedef struct {
    int type;
    int x, y, w, h, z;
    int alive;
    char input[96];
    int input_len;
    char text[WIN_MAX_LINES][WIN_COLS + 1];
    int trow, tcol;
    GuiEnt entries[WIN_ENTRIES];
    int entry_count;
    int loaded;
} GuiWin;

static GuiWin wins[WIN_MAX];
static GuiWin *gfx_term_target;
static int focus_win = -1;
static int z_top = 10;
static int dragging = -1;
static int drag_dx, drag_dy;

static UINT32 gfx_rgb(UINT8 r, UINT8 g, UINT8 b)
{
    /* PixelFormat 0 = RedGreenBlueReserved(字节序 R,G,B → 值 B<<16|G<<8|R)
       1 = BlueGreenRedReserved(字节序 B,G,R → 值 R<<16|G<<8|B) */
    UINT32 fmt = gfx->Mode->Info->PixelFormat;
    if (fmt == 0) return ((UINT32)b << 16) | ((UINT32)g << 8) | r;
    return ((UINT32)r << 16) | ((UINT32)g << 8) | b;
}

static void gfx_pixel(int x, int y, UINT32 c)
{
    if (x < 0 || y < 0 || (UINTN)x >= gfx_w || (UINTN)y >= gfx_h) return;
    gfx_fb[(UINTN)y * gfx_stride + (UINTN)x] = c;
}

static void gfx_fill(int x, int y, int w, int h, UINT32 c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)gfx_w) w = (int)gfx_w - x;
    if (y + h > (int)gfx_h) h = (int)gfx_h - y;
    for (int yy = 0; yy < h; ++yy) {
        UINT32 *p = gfx_fb + (UINTN)(y + yy) * gfx_stride + x;
        for (int xx = 0; xx < w; ++xx) p[xx] = c;
    }
}

static void gfx_border(int x, int y, int w, int h, UINT32 c)
{
    gfx_fill(x, y, w, 2, c);
    gfx_fill(x, y + h - 2, w, 2, c);
    gfx_fill(x, y, 2, h, c);
    gfx_fill(x + w - 2, y, 2, h, c);
}

static void gfx_char(int x, int y, unsigned char ch, UINT32 c)
{
    if (ch < 32 || ch > 126) ch = '?';
    for (int r = 0; r < 16; ++r) {
        unsigned char bits = font8x16[ch][r];
        for (int col = 0; col < 8; ++col)
            if (bits & (0x80 >> col)) gfx_pixel(x + col, y + r, c);
    }
}

static void gfx_text(int x, int y, const char *s, UINT32 c)
{
    while (*s) gfx_char(x, y, (unsigned char)*s++, c), x += 8;
}

static void gfx_text_n(int x, int y, const char *s, int max, UINT32 c)
{
    while (*s && max-- > 0) gfx_char(x, y, (unsigned char)*s++, c), x += 8;
}

static void gfx_text_w(int x, int y, const CHAR16 *s, UINT32 c)
{
    while (*s) gfx_char(x, y, (*s <= 126) ? (unsigned char)*s : '?', c), x += 8, ++s;
}

static void gfx_cursor(void)
{
    static const unsigned char arrow[16][2] = {
        {0x80,0},{0xC0,0},{0xE0,0},{0xF0,0},{0xF8,0},{0xFC,0},{0xFE,0},{0xFF,0},
        {0xFF,0x80},{0xF8,0},{0xD8,0},{0x8C,0},{0x0C,0},{0x06,0},{0x06,0},{0}
    };
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 10; ++x)
            if (arrow[y][x / 8] & (0x80 >> (x % 8)))
                gfx_pixel(mouse_x + x, mouse_y + y, gfx_rgb(255,255,255));
}

static void gfx_present(void)
{
    if (!gfx_back || !gfx_hw) return;
    for (UINTN y = 0; y < gfx_h; ++y)
        memcpy(gfx_hw + y * gfx_stride, gfx_back + y * gfx_stride, gfx_w * 4);
}

static void hid_init(void)
{
    EFI_GUID guid = { 0x2B2F68D6, 0x0CD2, 0x44CF, { 0x8E, 0x8B, 0xBB, 0xA2, 0x0B, 0x1B, 0x5B, 0x75 } };
    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_IS_ERROR(ST->BootServices->LocateHandleBuffer(2, &guid, NULL, &count, &handles))) return;
    for (UINTN i = 0; i < count && !hid_ready; ++i) {
        EFI_USB_IO_PROTOCOL *io = NULL;
        if (EFI_IS_ERROR(ST->BootServices->HandleProtocol(handles[i], &guid, (VOID **)&io)) || !io) continue;
        UsbInterfaceDescriptor d;
        if (EFI_IS_ERROR(io->UsbGetInterfaceDescriptor(io, &d))) continue;
        if (d.InterfaceClass != 3 || d.InterfaceSubClass != 1 || d.InterfaceProtocol != 2) continue;
        for (UINT8 e = 0; e < d.NumEndpoints; ++e) {
            UsbEndpointDescriptor ep;
            if (EFI_IS_ERROR(io->UsbGetEndpointDescriptor(io, e, &ep))) continue;
            if ((ep.EndpointAddress & 0x80) == 0 || (ep.Attributes & 3) != 3) continue;
            UsbDeviceRequest req = { 0x21, 0x0B, 0, d.InterfaceNumber, 0 };
            UINT32 st = 0;
            io->UsbControlTransfer(io, &req, 2, 100, NULL, 0, &st);
            hid_io = io;
            hid_endpoint = ep.EndpointAddress;
            hid_ready = 1;
            break;
        }
    }
    if (handles) ST->BootServices->FreePool(handles);
}

static void hid_poll(void)
{
    UINT8 rep[8];
    UINTN len = sizeof(rep);
    UINT32 st = 0;
    if (EFI_IS_ERROR(hid_io->UsbSyncInterruptTransfer(hid_io, hid_endpoint, rep, &len, 10, &st))) return;
    if (len < 3) return;
    signed char dx = (signed char)rep[1], dy = (signed char)rep[2];
    mouse_x += dx; mouse_y += dy;
    mouse_down = (rep[0] & 1) != 0;
    if (dx || dy) mouse_moved = 1;
}

static void mouse_poll(void)
{
    mouse_click = 0;
    mouse_moved = 0;
    if (hid_ready) {
        hid_poll();
    } else if (abs_ptr && abs_ptr->Mode) {
        EFI_ABSOLUTE_POINTER_STATE st;
        if (!EFI_IS_ERROR(abs_ptr->GetState(abs_ptr, &st))) {
            UINT64 mx = abs_ptr->Mode->AbsoluteMaxX, my = abs_ptr->Mode->AbsoluteMaxY;
            if (mx && my) {
                mouse_x = (int)((st.CurrentX * (gfx_w - 1)) / mx);
                mouse_y = (int)((st.CurrentY * (gfx_h - 1)) / my);
            }
            mouse_down = (st.ActiveButtons & 1) != 0;
        }
    } else if (mouse) {
        EFI_SIMPLE_POINTER_STATE st;
        if (!EFI_IS_ERROR(mouse->GetState(mouse, &st))) {
            mouse_x += st.RelativeMovementX;
            mouse_y += st.RelativeMovementY;
            mouse_down = st.LeftButton;
        }
    }
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= (int)gfx_w) mouse_x = (int)gfx_w - 1;
    if (mouse_y >= (int)gfx_h) mouse_y = (int)gfx_h - 1;
    if (mouse_down && !mouse_was_down) mouse_click = 1;
    mouse_was_down = mouse_down;
}

static GuiWin *win_from_index(int i) { return (i >= 0 && i < WIN_MAX && wins[i].alive) ? &wins[i] : NULL; }

static void win_focus(int i)
{
    if (!win_from_index(i)) return;
    focus_win = i;
    wins[i].z = ++z_top;
}

static int win_at(int x, int y)
{
    int best = -1, bz = -1;
    for (int i = 0; i < WIN_MAX; ++i) {
        if (!wins[i].alive) continue;
        if (x >= wins[i].x && x < wins[i].x + wins[i].w && y >= wins[i].y && y < wins[i].y + wins[i].h) {
            if (wins[i].z > bz) { bz = wins[i].z; best = i; }
        }
    }
    return best;
}

static void win_close(int i)
{
    if (!win_from_index(i)) return;
    wins[i].alive = 0;
    if (focus_win == i) {
        focus_win = -1;
        for (int k = 0; k < WIN_MAX; ++k) if (wins[k].alive) { focus_win = k; break; }
    }
}

static int win_open(int type)
{
    for (int i = 0; i < WIN_MAX; ++i) {
        if (wins[i].alive && wins[i].type == type) { win_focus(i); return i; }
    }
    for (int i = 0; i < WIN_MAX; ++i) {
        if (wins[i].alive) continue;
        for (UINTN k = 0; k < sizeof(wins[i]); ++k) ((UINT8 *)&wins[i])[k] = 0;
        wins[i].type = type;
        wins[i].alive = 1;
        int n = 0;
        for (int k = 0; k < WIN_MAX; ++k) if (wins[k].alive) ++n;
        wins[i].x = 70 + n * 45;
        wins[i].y = 90 + n * 35;
        wins[i].w = (int)gfx_w - 160;
        wins[i].h = (int)gfx_h - 220;
        if (type == WIN_SETTINGS) { wins[i].w = 560; wins[i].h = 430; }
        if (type == WIN_SETTINGS) { wins[i].w = 560; wins[i].h = 430; }
        if (wins[i].w > 760) wins[i].w = 760;
        if (wins[i].h > 460) wins[i].h = 460;
        wins[i].z = ++z_top;
        focus_win = i;
        return i;
    }
    return -1;
}

static void term_putc(GuiWin *w, char c)
{
    if (c == '\r') { w->tcol = 0; return; }
    if (c == '\n') {
        w->text[w->trow][w->tcol] = 0;
        ++w->trow; w->tcol = 0;
        if (w->trow >= WIN_MAX_LINES) {
            for (int r = 1; r < WIN_MAX_LINES; ++r) memcpy(w->text[r - 1], w->text[r], WIN_COLS + 1);
            w->trow = WIN_MAX_LINES - 1;
            w->text[w->trow][0] = 0;
        }
        return;
    }
    if (c == 8) { if (w->tcol) w->text[w->trow][--w->tcol] = 0; return; }
    if (c < 32 || c > 126) c = '?';
    if (w->tcol >= WIN_COLS) {
        w->text[w->trow][WIN_COLS] = 0;
        ++w->trow; w->tcol = 0;
        if (w->trow >= WIN_MAX_LINES) { w->trow = WIN_MAX_LINES - 1; w->text[w->trow][0] = 0; }
    }
    w->text[w->trow][w->tcol++] = c;
    w->text[w->trow][w->tcol] = 0;
}

static void gfx_out_wide(const CHAR16 *s)
{
    GuiWin *w = gfx_term_target ? gfx_term_target : win_from_index(focus_win);
    if (!w || w->type != WIN_TERM) return;
    while (*s) {
        CHAR16 c = *s++;
        if (c == '\r' || c == '\n' || c == 8 || (c >= 32 && c <= 126)) term_putc(w, (char)c);
        else term_putc(w, '?');
    }
}

static void term_execute(GuiWin *w)
{
    w->input[w->input_len] = 0;
    for (int i = 0; i < w->input_len; ++i) term_putc(w, w->input[i]);
    term_putc(w, '\n');
    if (!scmp(w->input, "exit")) { win_close(focus_win); return; }
    if (!scmp(w->input, "exitgui")) { gfx_done = 1; return; }
    if (w->input_len) {
        CHAR16 wide[96];
        for (int i = 0; i <= w->input_len; ++i) wide[i] = (CHAR16)(unsigned char)w->input[i];
        int duplicate = hist_count > 0 && weq(wide, hist[hist_count - 1]);
        if (!duplicate) {
            if (hist_count < HIST_MAX) wcopy(hist[hist_count++], 192, wide);
            else {
                for (int i = 1; i < HIST_MAX; ++i) wcopy(hist[i - 1], 192, hist[i]);
                wcopy(hist[HIST_MAX - 1], 192, wide);
            }
        }
        hist_pos = hist_count;
        active_tty = 2;
        gfx_term_target = w;
        process_command(wide);
        gfx_term_target = NULL;
        active_tty = 1;
        term_putc(w, '\n');
    }
    w->input_len = 0;
    w->input[0] = 0;
}

static void win_load_files(GuiWin *w)
{
    w->entry_count = 0;
    EFI_FILE_PROTOCOL *dir = NULL;
    if (!open_path(CWD, &dir, EFI_FILE_MODE_READ, 0)) { w->loaded = 1; return; }
    static UINT8 buf[2048];
    for (int guard = 0; guard < WIN_ENTRIES + 4; ++guard) {
        UINTN sz = sizeof(buf);
        if (EFI_IS_ERROR(dir->Read(dir, &sz, buf)) || sz == 0) break;
        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        if (!info->FileName[0]) break;
        if (weq(info->FileName, L".") || weq(info->FileName, L"..")) continue;
        GuiEnt *e = &w->entries[w->entry_count++];
        wcopy(e->name, 64, info->FileName);
        e->dir = (info->Attribute & EFI_FILE_DIRECTORY) ? 1 : 0;
        e->size = info->FileSize;
        if (w->entry_count >= WIN_ENTRIES) break;
    }
    dir->Close(dir);
    w->loaded = 1;
}

static void draw_files(GuiWin *w)
{
    if (!w->loaded) win_load_files(w);
    int x = w->x + 8, y = w->y + 36;
    gfx_text(x, y, "Path:", gfx_dim);
    gfx_text_w(x + 48, y, CWD, gfx_text_c);
    gfx_fill(x, y + 24, 64, 24, gfx_panel);
    gfx_border(x, y + 24, 64, 24, gfx_accent);
    gfx_text(x + 10, y + 30, "Back", gfx_text_c);
    for (int row = 0; row < w->entry_count; ++row) {
        int yy = y + 62 + row * 20;
        if (yy + 16 > w->y + w->h - 10) break;
        gfx_text_w(x + 18, yy, w->entries[row].name, w->entries[row].dir ? gfx_accent : gfx_text_c);
        if (w->entries[row].dir)
            gfx_text(x + 18 + (int)wlen(w->entries[row].name) * 8, yy, "/", gfx_accent);
    }
}

static void draw_term(GuiWin *w)
{
    for (int r = 0; r < WIN_MAX_LINES; ++r)
        gfx_text_n(w->x + 8, w->y + 38 + r * 16, w->text[r], WIN_COLS, gfx_text_c);
    int yy = w->y + w->h - 24;
    gfx_text(w->x + 8, yy, "tty2>", gfx_accent);
    gfx_text(w->x + 80, yy, w->input, gfx_text_c);
    gfx_fill(w->x + 80 + w->input_len * 8, yy + 14, 7, 2, gfx_text_c);
}

static const char *theme_names[5] = { "Blue", "Midnight", "Green", "Matrix", "Amber" };
static void apply_theme(void);

static void draw_settings(GuiWin *w)
{
    int x0 = w->x + 18, y0 = w->y + 44;
    gfx_text(x0, y0 - 8, "Appearance", gfx_dim);
    for (int i = 0; i < 5; ++i) {
        int ry = y0 + 14 + i * 40;
        int sel = (theme_index == i);
        gfx_fill(x0 - 6, ry - 6, w->w - 36, 32, sel ? gfx_panel : gfx_rgb(10, 12, 20));
        gfx_border(x0 - 6, ry - 6, w->w - 36, 32, sel ? gfx_accent : gfx_dim);
        gfx_text(x0, ry, theme_names[i], sel ? gfx_accent : gfx_text_c);
        gfx_text(x0 + 130, ry, sel ? "current" : "click to apply", gfx_dim);
    }
    int ry = y0 + 14 + 5 * 40 + 10;
    gfx_text(x0, ry, "Window manager", gfx_dim);
    gfx_text(x0, ry + 22, "drag title bar to move   /   X to close   /   taskbar to focus", gfx_text_c);
    gfx_text(x0, ry + 44, "kernel: WCT Kernel 0.1_ET   ui: wctdesk   input: usb hid / ps2", gfx_dim);
    gfx_text(x0, ry + 66, "display: GOP framebuffer   shell: wctsh", gfx_dim);
}
static void draw_window(GuiWin *w)
{
    UINT32 border = (win_from_index(focus_win) == w) ? gfx_accent : gfx_dim;
    gfx_fill(w->x, w->y, w->w, w->h, gfx_rgb(8, 8, 14));
    gfx_border(w->x, w->y, w->w, w->h, border);
    gfx_fill(w->x + 2, w->y + 2, w->w - 4, 28, gfx_top);
    gfx_text(w->x + 10, w->y + 8,
             w->type == WIN_FILES ? "Files" :
             (w->type == WIN_SETTINGS ? "Settings" : "Terminal"), border);
    gfx_fill(w->x + w->w - 34, w->y + 6, 24, 20, gfx_panel);
    gfx_border(w->x + w->w - 34, w->y + 6, 24, 20, border);
    gfx_text(w->x + w->w - 26, w->y + 9, "X", gfx_text_c);
    if (w->type == WIN_FILES) draw_files(w);
    else if (w->type == WIN_SETTINGS) draw_settings(w);
    else draw_term(w);
}

static void draw_desktop(void)
{
    gfx_fill(0, 0, (int)gfx_w, (int)gfx_h, gfx_bg);
    gfx_fill(0, 0, (int)gfx_w, 38, gfx_top);
    gfx_text(14, 11, "WCT-OS Desktop", gfx_text_c);
    gfx_text((int)gfx_w - 220, 11, "aqtwWCT95  |  4.0.441", gfx_dim);
    const char *names[5] = { "Files", "Terminal", "Settings", "Theme", "Exit" };
    const char *subs[5]  = { "open", "open", "open", "cycle", "leave" };
    for (int i = 0; i < 5; ++i) {
        int x = 40 + i * 176, y = 75;
        gfx_fill(x, y, 160, 74, gfx_panel);
        gfx_border(x, y, 160, 74, gfx_accent);
        gfx_text(x + 14, y + 14, names[i], gfx_text_c);
        gfx_text(x + 14, y + 42, subs[i], gfx_dim);
    }
    gfx_text(40, 180, "Drag window title bars. Click X to close a window.", gfx_dim);
    gfx_text(40, 202, "Settings > Appearance switches the desktop theme.", gfx_dim);
    /* taskbar */
    gfx_fill(0, (int)gfx_h - 34, (int)gfx_w, 34, gfx_top);
    int bx = 8;
    for (int i = 0; i < WIN_MAX; ++i) {
        if (!wins[i].alive) continue;
        gfx_fill(bx, (int)gfx_h - 30, 150, 24, i == focus_win ? gfx_accent : gfx_panel);
        gfx_text(bx + 8, (int)gfx_h - 26,
                 wins[i].type == WIN_FILES ? "Files" :
                 (wins[i].type == WIN_SETTINGS ? "Settings" : "Terminal"), gfx_text_c);
        bx += 160;
    }
}

static void draw_all(void)
{
    draw_desktop();
    for (int z = 1; z <= z_top; ++z)
        for (int i = 0; i < WIN_MAX; ++i)
            if (wins[i].alive && wins[i].z == z) draw_window(&wins[i]);
    gfx_cursor();
}

static void handle_term_key(GuiWin *w, EFI_INPUT_KEY key)
{
    if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n' || key.ScanCode == 0x0D) { term_execute(w); return; }
    if (key.UnicodeChar == 8) { if (w->input_len) w->input[--w->input_len] = 0; return; }
    if (key.UnicodeChar >= 32 && key.UnicodeChar < 127 && w->input_len < 95) {
        w->input[w->input_len++] = (char)key.UnicodeChar;
        w->input[w->input_len] = 0;
    }
}

static void handle_click(void)
{
    int wi = win_at(mouse_x, mouse_y);
    if (wi >= 0) {
        GuiWin *w = &wins[wi];
        int old = focus_win;
        win_focus(wi);
        int lx = mouse_x - w->x, ly = mouse_y - w->y;
        if (lx >= w->w - 36 && ly >= 4 && ly <= 28) { win_close(wi); return; }
        if (ly < 30) {
            dragging = wi;
            drag_dx = mouse_x - w->x;
            drag_dy = mouse_y - w->y;
            return;
        }
        if (w->type == WIN_SETTINGS) {
            int y0 = 44;
            for (int i = 0; i < 5; ++i) {
                int ry = y0 + 14 + i * 40;
                if (ly >= ry - 8 && ly < ry + 28) { theme_index = i; apply_theme(); return; }
            }
            return;
        }
        if (w->type == WIN_SETTINGS) {
            int y0 = 44;
            for (int i = 0; i < 5; ++i) {
                int ry = y0 + 14 + i * 40;
                if (ly >= ry - 8 && ly < ry + 28) { theme_index = i; apply_theme(); return; }
            }
            return;
        }
        if (w->type == WIN_FILES) {
            if (lx >= 8 && lx < 72 && ly >= 60 && ly < 84) {
                CHAR16 parent[256];
                normalize_path(L"..", parent, sizeof(parent) / sizeof(parent[0]));
                wcopy(CWD, sizeof(CWD) / sizeof(CWD[0]), parent);
                w->loaded = 0;
                return;
            }
            if (ly >= 98) {
                int row = (ly - 98) / 20;
                if (row < w->entry_count) {
                    CHAR16 next[256];
                    wcopy(next, sizeof(next) / sizeof(next[0]), CWD);
                    if (wlen(next) && next[wlen(next)-1] != L'\\')
                        wcat(next, sizeof(next)/sizeof(next[0]), L"\\");
                    wcat(next, sizeof(next)/sizeof(next[0]), w->entries[row].name);
                    if (w->entries[row].dir) {
                        EFI_FILE_PROTOCOL *chk = NULL;
                        if (open_path(next, &chk, EFI_FILE_MODE_READ, 0)) {
                            chk->Close(chk);
                            if (wlen(next) < 255) {
                                wcopy(CWD, sizeof(CWD) / sizeof(CWD[0]), next);
                                w->loaded = 0;
                            }
                        }
                    } else {
                        int t = win_open(WIN_TERM);
                        if (t >= 0) {
                            term_putc(&wins[t], 'F'); term_putc(&wins[t], ':');
                            term_putc(&wins[t], ' ');
                            for (int k = 0; next[k] && k < 60; ++k) term_putc(&wins[t], (char)next[k]);
                            term_putc(&wins[t], '\n');
                        }
                    }
                }
            }
        }
        (void)old;
        return;
    }
    /* taskbar */
    if (mouse_y >= (int)gfx_h - 34) {
        int bx = 8;
        for (int i = 0; i < WIN_MAX; ++i) {
            if (!wins[i].alive) continue;
            if (mouse_x >= bx && mouse_x < bx + 150) { win_focus(i); return; }
            bx += 160;
        }
    }
    /* desktop icons */
    for (int i = 0; i < 5; ++i) {
        int x = 40 + i * 176, y = 75;
        if (mouse_x >= x && mouse_x < x + 160 && mouse_y >= y && mouse_y < y + 74) {
            if (i == 0) win_open(WIN_FILES);
            else if (i == 1) win_open(WIN_TERM);
            else if (i == 2) win_open(WIN_SETTINGS);
            else if (i == 3) theme_index = (theme_index + 1) % 5;
            else gfx_done = 1;
            return;
        }
    }
}


static void apply_theme(void)
{
    switch (theme_index) {
    case 0: gfx_bg=gfx_rgb(6,12,26);  gfx_top=gfx_rgb(18,44,86);  gfx_panel=gfx_rgb(16,30,52);  gfx_accent=gfx_rgb(84,170,255); break;
    case 1: gfx_bg=gfx_rgb(10,10,16); gfx_top=gfx_rgb(30,32,48);  gfx_panel=gfx_rgb(22,24,36);  gfx_accent=gfx_rgb(140,160,220); break;
    case 2: gfx_bg=gfx_rgb(5,16,12);   gfx_top=gfx_rgb(10,48,32);  gfx_panel=gfx_rgb(14,34,24);  gfx_accent=gfx_rgb(60,210,140); break;
    case 3: gfx_bg=gfx_rgb(0,10,0);    gfx_top=gfx_rgb(0,40,0);    gfx_panel=gfx_rgb(0,26,0);    gfx_accent=gfx_rgb(0,255,90);  break;
    default: gfx_bg=gfx_rgb(16,12,4);  gfx_top=gfx_rgb(62,38,8);   gfx_panel=gfx_rgb(38,28,10);  gfx_accent=gfx_rgb(255,190,70); break;
    }
    gfx_text_c = gfx_rgb(235,240,250);
    gfx_dim = gfx_rgb(150,160,180);
}

static void gui_loop(void)
{
    gfx_done = 0;
    focus_win = -1;
    z_top = 10;
    mouse_x = (int)gfx_w / 2;
    mouse_y = (int)gfx_h / 2;
    mouse_down = mouse_was_down = 0;
    for (int i = 0; i < WIN_MAX; ++i) wins[i].alive = 0;

    while (!gfx_done) {
        mouse_poll();
        if (mouse_click) handle_click();
        if (dragging >= 0 && wins[dragging].alive) {
            if (mouse_down) {
                wins[dragging].x = mouse_x - drag_dx;
                wins[dragging].y = mouse_y - drag_dy;
                if (wins[dragging].y < 0) wins[dragging].y = 0;
                if (wins[dragging].x < 0) wins[dragging].x = 0;
            } else dragging = -1;
        }
        EFI_INPUT_KEY key;
        if (!EFI_IS_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key))) {
            if (key.UnicodeChar == L's' && focus_win < 0) { win_open(WIN_SETTINGS); }
            else if (key.ScanCode == 0x14) gfx_done = 1;
            else if (key.ScanCode == 0x17) {
                if (focus_win >= 0) win_close(focus_win);
                else gfx_done = 1;
            } else if (focus_win >= 0 && wins[focus_win].alive && wins[focus_win].type == WIN_SETTINGS) {
                if (key.UnicodeChar >= L'1' && key.UnicodeChar <= L'5') {
                    theme_index = (int)(key.UnicodeChar - L'1');
                    apply_theme();
                }
            } else if (focus_win >= 0 && wins[focus_win].alive && wins[focus_win].type == WIN_SETTINGS) {
                if (key.UnicodeChar >= L'1' && key.UnicodeChar <= L'5') {
                    theme_index = (int)(key.UnicodeChar - L'1');
                    apply_theme();
                }
            } else if (focus_win >= 0 && wins[focus_win].alive && wins[focus_win].type == WIN_TERM) {
                handle_term_key(&wins[focus_win], key);
            }
        }
        apply_theme();
        draw_all();
        gfx_present();
        ST->BootServices->Stall(16000);
    }
}

static void cmd_gui(void)
{
    if (EFI_IS_ERROR(ST->BootServices->LocateProtocol(&gop_guid, NULL, (VOID **)&gfx)) ||
        !gfx || !gfx->Mode || !gfx->Mode->FrameBufferBase) {
        outln(L"gui: GOP framebuffer not available");
        return;
    }
    gfx_w = gfx->Mode->Info->HorizontalResolution;
    gfx_h = gfx->Mode->Info->VerticalResolution;
    gfx_stride = gfx->Mode->Info->PixelsPerScanLine;
    gfx_hw = (UINT32 *)(UINTN)gfx->Mode->FrameBufferBase;
    gfx_fb = gfx_hw;
    gfx_back = NULL;
    UINTN bytes = gfx_stride * gfx_h * 4;
    if (!EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, bytes, (VOID **)&gfx_back)) && gfx_back)
        gfx_fb = gfx_back;

    /* 输入设备在启动时可能尚未注册（OVMF 驱动异步加载）→ 轮询等待，最多 3 秒 */
    EFI_GUID spg = { 0x31878C87, 0x0B75, 0x11D5, { 0x9A, 0x4F, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };
    EFI_GUID apg = { 0x8D59D32B, 0xC655, 0x4AE9, { 0x9B, 0x15, 0xF2, 0x59, 0x04, 0x99, 0x2A, 0x43 } };
    EFI_SIMPLE_POINTER_PROTOCOL *sp = NULL;
    EFI_ABSOLUTE_POINTER_PROTOCOL *ap = NULL;
    for (int i = 0; i < 15; ++i) {
        if (!sp && !EFI_IS_ERROR(ST->BootServices->LocateProtocol(&spg, NULL, (VOID **)&sp)) && sp) mouse = sp;
        if (!ap && !EFI_IS_ERROR(ST->BootServices->LocateProtocol(&apg, NULL, (VOID **)&ap)) && ap) abs_ptr = ap;
        if (mouse || abs_ptr) break;
        hid_init();
        if (hid_ready) break;
        ST->BootServices->Stall(200000);
    }
    mouse = sp; abs_ptr = ap;
    if (!mouse && !abs_ptr) hid_init();

    gfx_original_mode = ST->ConOut->Mode->Mode;
    gfx_active = 1;
    ST->ConOut->EnableCursor(ST->ConOut, FALSE);
    gui_loop();
    gfx_active = 0;
    if (gfx_back) { ST->BootServices->FreePool(gfx_back); gfx_back = NULL; }
    ST->ConOut->SetMode(ST->ConOut, gfx_original_mode);
    ST->ConOut->EnableCursor(ST->ConOut, TRUE);
    ST->ConOut->ClearScreen(ST->ConOut);
    reset_color();
    print_banner();
}

static void cmd_exitgui(void)
{
    outln(L"exitgui: not in GUI (run gui first)");
}
