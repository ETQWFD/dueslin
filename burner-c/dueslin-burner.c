/*
 * DUESLIN 镜像刻录器 2.0 (Windows) — C 语言 + Win32 API
 *
 * 修复：
 *  - 全部宽字符（UTF-16），彻底解决中文乱码
 *  - 内置视觉样式 Manifest + 微软雅黑，界面美观
 *  - VERSIONINFO：作者 etc、版本、描述（右键属性可见）
 *  - 内置 DUESLIN 官方 Logo 图标
 *  - 仅 3 个镜像：桌面版 / 服务器版 / 精简版（x86-64）
 *  - 下载带 3 次重试与进度显示，连接 GitHub Release 更稳定
 *
 * 编译（MinGW-w64，单文件含图标/版本信息/视觉样式）：
 *   x86_64-w64-mingw32-windres logo.rc -O coff -o logo.res
 *   x86_64-w64-mingw32-gcc -O2 dueslin-burner.c logo.res \
 *       -lcomctl32 -lwininet -lole32 -lcomdlg32 -lurlmon -lgdi32 \
 *       -o DueslinBurner.exe \
 *       -static-libgcc -static-libstdc++ -Wl,-subsystem,windows
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <wininet.h>
#include <shellapi.h>
#include <winioctl.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

/* 控件 ID */
#define IDC_IMG_LIST    201
#define IDC_DIR_EDIT    202
#define IDC_DIR_BTN     203
#define IDC_MODE_ISO    204
#define IDC_MODE_DVD    205
#define IDC_DEV_LIST    206
#define IDC_DEV_REFRESH 207
#define IDC_YES_EDIT    208
#define IDC_NEXT        209
#define IDC_BACK        210
#define IDC_STATUS      211
#define IDC_PROGRESS    212
#define IDC_STEP        213

#define NUM_IMG 3
typedef struct { const wchar_t *name; const wchar_t *url; const wchar_t *file; } IMG;
static IMG g_imgs[NUM_IMG] = {
    { L"桌面版  (DUESLIN-1.0.iso)",          L"https://github.com/ETQWFD/dueslin/releases/download/v1.0.0/DUESLIN-1.0.iso",        L"DUESLIN-1.0.iso" },
    { L"服务器版  (DUESLIN-Server-1.0.iso)", L"https://github.com/ETQWFD/dueslin/releases/download/v1.0.0/DUESLIN-Server-1.0.iso", L"DUESLIN-Server-1.0.iso" },
    { L"精简版  (DUESLIN-Mini-1.0.iso)",     L"https://github.com/ETQWFD/dueslin/releases/download/v1.0.0/DUESLIN-Mini-1.0.iso", L"DUESLIN-Mini-1.0.iso" },
};

static HWND g_hwnd;
static int  g_page = 0;
static wchar_t g_dir[MAX_PATH] = L"";
static wchar_t g_dev[160] = L"";
static int  g_mode_iso = 1;
static volatile LONG g_busy = 0;

static HFONT g_fontTitle, g_fontText, g_fontBtn;

/* ---------- 主题 ---------- */
#define CLR_BG      RGB(0xF3, 0xF6, 0xFB)
#define CLR_ACCENT  RGB(0x1A, 0x73, 0xE8)
#define CLR_TEXT    RGB(0x1F, 0x29, 0x37)
#define CLR_SUB     RGB(0x6B, 0x72, 0x80)


static HWND CW(const wchar_t *cls, const wchar_t *name, DWORD style,
               int x, int y, int w, int h, HWND p, HMENU m, LPVOID pr, ...) {
    return CreateWindowW(cls, name, style, x, y, w, h, p, m, GetModuleHandleW(NULL), pr);
}

static void SetStatus(const wchar_t *s) { SetDlgItemTextW(g_hwnd, IDC_STATUS, s); }

static void MakeFonts(void) {
    g_fontTitle = CreateFontW(22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, 0, 0, L"Microsoft YaHei UI");
    g_fontText  = CreateFontW(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, 0, 0, L"Microsoft YaHei UI");
    g_fontBtn   = CreateFontW(14, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, 0, 0, L"Microsoft YaHei UI");
}

/* ---------- 自绘圆角按钮 ---------- */
static WNDPROC g_origBtnProc = NULL;
static void DrawRoundBtn(HWND hwnd, HDC hdc, COLORREF bg, COLORREF fg) {
    RECT rc; GetClientRect(hwnd, &rc);
    HBRUSH br = CreateSolidBrush(bg);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
    RoundRect(hdc, 0, 0, rc.right, rc.bottom, 10, 10);
    SelectObject(hdc, ob); DeleteObject(br);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    wchar_t t[64]; GetWindowTextW(hwnd, t, 64);
    HFONT of = (HFONT)SelectObject(hdc, g_fontBtn);
    RECT tr = rc;
    DrawTextW(hdc, t, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, of);
}
static LRESULT CALLBACK BtnProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        COLORREF bg = CLR_ACCENT, fg = RGB(255,255,255);
        int id = (int)GetWindowLongPtrW(hwnd, GWLP_ID);
        if (id == IDC_BACK) { bg = RGB(0xE5,0xE7,0xEB); fg = CLR_TEXT; }
        if (id == IDC_DEV_REFRESH || id == IDC_DIR_BTN) { bg = RGB(0xEE,0xF2,0xFF); fg = CLR_ACCENT; }
        if (id == IDC_BACK && g_page == 0) { bg = RGB(0xF1,0xF3,0xF6); fg = RGB(0xB0,0xB6,0xC0); }
        if (!IsWindowEnabled(hwnd)) { bg = RGB(0xE9,0xEC,0xF1); fg = RGB(0xA0,0xA8,0xB4); }
        DrawRoundBtn(hwnd, hdc, bg, fg);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ENABLE) InvalidateRect(hwnd, NULL, TRUE);
    return CallWindowProcW(g_origBtnProc, hwnd, msg, w, l);
}
static HWND MkBtn(HWND parent, const wchar_t *text, int id, int x, int y, int w, int h) {
    HWND b = CW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    if (!g_origBtnProc) g_origBtnProc = (WNDPROC)SetWindowLongPtrW(b, GWLP_WNDPROC, (LONG_PTR)BtnProc);
    else SetWindowLongPtrW(b, GWLP_WNDPROC, (LONG_PTR)BtnProc);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_fontBtn, TRUE);
    return b;
}
static HWND MkLbl(HWND parent, const wchar_t *text, int x, int y, int w, int h, HFONT f, COLORREF col) {
    HWND l = CW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, NULL, NULL, NULL);
    SendMessageW(l, WM_SETFONT, (WPARAM)(f ? f : g_fontText), TRUE);
    return l;
}

/* ---------- 设备枚举 ---------- */
static void FillDevices(HWND combo) {
    ComboBox_ResetContent(combo);
    for (int i = 0; i < 8; i++) {
        wchar_t path[64], line[200];
        swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            wchar_t name[160] = L"磁盘";
            DWORD got = 0;
            STORAGE_PROPERTY_QUERY spq;
            memset(&spq, 0, sizeof(spq));
            spq.PropertyId = StorageDeviceProperty;
            spq.QueryType = PropertyStandardQuery;
            BYTE buf[200] = {0};
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &spq, sizeof(spq),
                                buf, sizeof(buf), &got, NULL) && got > 0) {
                for (DWORD k = 0; k < got && k < 150; k++)
                    name[k] = (wchar_t)buf[k];
                name[got > 150 ? 150 : got] = 0;
                for (int k = 0; k < 150; k++) if (name[k] == 0) name[k] = L' ';
            }
            swprintf(line, 200, L"物理磁盘 %d  -  %s", i, name);
            int idx = ComboBox_AddString(combo, line);
            ComboBox_SetItemData(combo, idx, i);
            CloseHandle(h);
        }
    }
    if (ComboBox_GetCount(combo) > 0) ComboBox_SetCurSel(combo, 0);
}

/* ---------- 页面 ---------- */
static void BuildPage(void) {
    HWND d = g_hwnd;
    HWND child = GetWindow(d, GW_CHILD);
    while (child) { HWND nx = GetWindow(child, GW_HWNDNEXT); DestroyWindow(child); child = nx; }
    SetStatus(L"");

    const wchar_t *names[5] = { L"选择镜像", L"下载目录", L"刻录方式", L"选择设备", L"确认" };
    wchar_t step[80];
    swprintf(step, 80, L"第 %d / 5 步  ·  %s", g_page + 1, names[g_page]);
    HWND stp = CW(L"STATIC", step, WS_CHILD | WS_VISIBLE, 320, 12, 170, 24, d, (HMENU)(INT_PTR)IDC_STEP, GetModuleHandleW(NULL), NULL);
    SendMessageW(stp, WM_SETFONT, (WPARAM)g_fontText, TRUE);

    int x = 30;
    if (g_page == 0) {
        MkLbl(d, L"请选择要下载并刻录的 DUESLIN 镜像：", x, 72, 400, 24, NULL, CLR_TEXT);
        HWND combo = CW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   x, 104, 420, 220, d, (HMENU)(INT_PTR)IDC_IMG_LIST, GetModuleHandleW(NULL), NULL);
        SendMessageW(combo, WM_SETFONT, (WPARAM)g_fontText, TRUE);
        for (int i = 0; i < NUM_IMG; i++) ComboBox_AddString(combo, g_imgs[i].name);
        ComboBox_SetCurSel(combo, 0);
        MkLbl(d, L"镜像来自 GitHub Releases，下载支持自动重试。", x, 140, 420, 20, NULL, CLR_SUB);
    } else if (g_page == 1) {
        MkLbl(d, L"选择下载目录", x, 72, 300, 26, g_fontTitle, CLR_TEXT);
        MkLbl(d, L"镜像将下载到该目录：", x, 112, 260, 22, NULL, CLR_TEXT);
        CW(L"EDIT", g_dir, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                      x, 142, 340, 30, d, (HMENU)(INT_PTR)IDC_DIR_EDIT, GetModuleHandleW(NULL), NULL);
        SendMessageW(GetDlgItem(d, IDC_DIR_EDIT), WM_SETFONT, (WPARAM)g_fontText, TRUE);
        MkBtn(d, L"浏览", IDC_DIR_BTN, 380, 142, 70, 30);
    } else if (g_page == 2) {
        MkLbl(d, L"选择刻录模式", x, 72, 300, 26, g_fontTitle, CLR_TEXT);
        HWND rb1 = CW(L"BUTTON", L"ISO   刻录到 U 盘", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                 x, 124, 220, 30, d, (HMENU)(INT_PTR)IDC_MODE_ISO, NULL, NULL);
        HWND rb2 = CW(L"BUTTON", L"DVD   刻录到光盘", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                 x, 164, 220, 30, d, (HMENU)(INT_PTR)IDC_MODE_DVD, NULL, NULL);
        SendMessageW(rb1, WM_SETFONT, (WPARAM)g_fontText, TRUE);
        SendMessageW(rb2, WM_SETFONT, (WPARAM)g_fontText, TRUE);
        Button_SetCheck(rb1, g_mode_iso ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(rb2, g_mode_iso ? BST_UNCHECKED : BST_CHECKED);
    } else if (g_page == 3) {
        MkLbl(d, L"选择目标设备", x, 72, 300, 26, g_fontTitle, CLR_TEXT);
        MkLbl(d, g_mode_iso ? L"请插入 U 盘并选择（刻录将格式化该设备）：" : L"请插入空白光盘：",
              x, 112, 420, 22, NULL, CLR_TEXT);
        HWND combo = CW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   x, 144, 340, 200, d, (HMENU)(INT_PTR)IDC_DEV_LIST, GetModuleHandleW(NULL), NULL);
        SendMessageW(combo, WM_SETFONT, (WPARAM)g_fontText, TRUE);
        FillDevices(combo);
        MkBtn(d, L"刷新", IDC_DEV_REFRESH, 380, 144, 70, 30);
        MkLbl(d, L"警告：设备上的所有数据将被清除！", x, 184, 360, 20, NULL, RGB(0xDC,0x26,0x26));
    } else if (g_page == 4) {
        int sel = ComboBox_GetCurSel(GetDlgItem(d, IDC_IMG_LIST));
        if (sel < 0) sel = 0;
        wchar_t msg[600];
        swprintf(msg, 600, L"镜像：%s\n目标：%s\n模式：%s\n\n设备上的所有数据将被清除！\n请输入大写 YES 确认：",
                 g_imgs[sel].name, g_dev, g_mode_iso ? L"U盘(ISO)" : L"光盘(DVD)");
        MkLbl(d, msg, x, 72, 430, 130, NULL, CLR_TEXT);
        CW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_UPPERCASE | ES_AUTOHSCROLL,
                      x, 220, 130, 30, d, (HMENU)(INT_PTR)IDC_YES_EDIT, GetModuleHandleW(NULL), NULL);
        SendMessageW(GetDlgItem(d, IDC_YES_EDIT), WM_SETFONT, (WPARAM)g_fontText, TRUE);
    }

    MkLbl(d, L"", x, 272, 440, 22, NULL, CLR_ACCENT);
    CW(L"PROGRESS_CLASS", L"", WS_CHILD | WS_VISIBLE,
                  x, 300, 440, 18, d, (HMENU)(INT_PTR)IDC_PROGRESS, GetModuleHandleW(NULL), NULL);

    MkBtn(d, L"<  上一步", IDC_BACK, 200, 340, 104, 34);
    MkBtn(d, g_page == 4 ? L"开始刻录" : L"下一步  >", IDC_NEXT, 320, 340, 114, 34);
    EnableWindow(GetDlgItem(d, IDC_BACK), g_page > 0 && !g_busy);
    EnableWindow(GetDlgItem(d, IDC_NEXT), !g_busy);
    SetFocus(GetDlgItem(d, IDC_NEXT));
    InvalidateRect(d, NULL, TRUE);
}

/* ---------- 下载（重试 3 次） ---------- */
static BOOL DownloadFile(const wchar_t *url, const wchar_t *dest) {
    for (int attempt = 1; attempt <= 3; attempt++) {
        wchar_t s[128];
        swprintf(s, 128, L"正在下载镜像（第 %d/3 次尝试）...", attempt);
        SetStatus(s);
        HRESULT hr = URLDownloadToFileW(NULL, url, dest, 0, NULL);
        if (SUCCEEDED(hr)) return TRUE;
        SetStatus(L"连接中断，正在自动重试...");
        Sleep(1500);
    }
    return FALSE;
}

static DWORD WINAPI Worker(LPVOID param) {
    int sel = (int)(INT_PTR)param;
    HWND d = g_hwnd;
    InterlockedExchange(&g_busy, 1);
    PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDC_NEXT, BN_CLICKED), 0);

    wchar_t yes[32] = L"";
    GetDlgItemTextW(d, IDC_YES_EDIT, yes, 32);
    if (wcscmp(yes, L"YES") != 0) {
        SetStatus(L"请先输入大写 YES 确认。");
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    wchar_t dest[MAX_PATH];
    if (!g_dir[0]) GetCurrentDirectoryW(MAX_PATH, g_dir);
    swprintf(dest, MAX_PATH, L"%s\\%s", g_dir, g_imgs[sel].file);

    if (!DownloadFile(g_imgs[sel].url, dest)) {
        SetStatus(L"下载失败：请检查网络后重试。");
        InterlockedExchange(&g_busy, 0);
        return 0;
    }
    SetStatus(L"下载完成，正在刻录...");
    SendMessageW(GetDlgItem(d, IDC_PROGRESS), PBM_SETPOS, 10, 0);

    if (g_mode_iso) {
        HANDLE src = CreateFileW(dest, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        int drive = -1;
        HWND cb = GetDlgItem(d, IDC_DEV_LIST);
        int i = ComboBox_GetCurSel(cb);
        if (i >= 0) drive = (int)ComboBox_GetItemData(cb, i);
        wchar_t devpath[64];
        swprintf(devpath, 64, L"\\\\.\\PhysicalDrive%d", drive < 0 ? 0 : drive);
        HANDLE dst = CreateFileW(devpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);
        if (src == INVALID_HANDLE_VALUE || dst == INVALID_HANDLE_VALUE) {
            SetStatus(L"刻录失败：无法打开镜像或设备（请以管理员身份运行）。");
            if (src != INVALID_HANDLE_VALUE) CloseHandle(src);
            if (dst != INVALID_HANDLE_VALUE) CloseHandle(dst);
            InterlockedExchange(&g_busy, 0);
            return 0;
        }
        BYTE buf[1 << 20];
        DWORD rd, wr, total = 0;
        LARGE_INTEGER sz = {0};
        GetFileSizeEx(src, &sz);
        while (ReadFile(src, buf, sizeof(buf), &rd, NULL) && rd > 0) {
            WriteFile(dst, buf, rd, &wr, NULL);
            total += rd;
            if (sz.QuadPart > 0) {
                int pct = (int)(total * 100 / sz.QuadPart);
                SendMessageW(GetDlgItem(d, IDC_PROGRESS), PBM_SETPOS, pct, 0);
            }
        }
        CloseHandle(src); CloseHandle(dst);
        SendMessageW(GetDlgItem(d, IDC_PROGRESS), PBM_SETPOS, 100, 0);
        SetStatus(L"刻录完成！可以拔出 U 盘用于启动安装。");
    } else {
        SetStatus(L"DVD 模式：请在资源管理器中右键该镜像文件 → 刻录光盘映像。");
    }
    InterlockedExchange(&g_busy, 0);
    return 0;
}

/* ---------- 主窗口 ---------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        MakeFonts();
        BuildPage();
        return 0;
    case WM_ERASEBKGND:
        {
            RECT rc; GetClientRect(hwnd, &rc);
            HDC hdc = (HDC)w;
            HBRUSH br = CreateSolidBrush(CLR_BG);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            RECT tb = { 0, 0, rc.right, 46 };
            HBRUSH bb = CreateSolidBrush(CLR_ACCENT);
            FillRect(hdc, &tb, bb);
            DeleteObject(bb);
            return 1;
        }
    case WM_CTLCOLORSTATIC:
        {
            int id = (int)GetDlgCtrlID((HWND)l);
            if (id == IDC_STEP) {
                SetTextColor((HDC)w, RGB(255,255,255));
                SetBkMode((HDC)w, TRANSPARENT);
                return (LRESULT)GetStockObject(NULL_BRUSH);
            }
            SetTextColor((HDC)w, CLR_TEXT);
            SetBkMode((HDC)w, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (HIWORD(w) == BN_CLICKED) {
            if (id == IDC_NEXT) {
                if (g_page == 0) {
                    g_page = 1; BuildPage();
                } else if (g_page == 1) {
                    GetDlgItemTextW(hwnd, IDC_DIR_EDIT, g_dir, MAX_PATH);
                    if (!g_dir[0]) GetCurrentDirectoryW(MAX_PATH, g_dir);
                    g_page = 2; BuildPage();
                } else if (g_page == 2) {
                    g_mode_iso = Button_GetCheck(GetDlgItem(hwnd, IDC_MODE_ISO)) == BST_CHECKED;
                    g_page = 3; BuildPage();
                } else if (g_page == 3) {
                    HWND cb = GetDlgItem(hwnd, IDC_DEV_LIST);
                    int i = ComboBox_GetCurSel(cb);
                    wchar_t line[200] = L"";
                    if (i >= 0) ComboBox_GetLBText(cb, i, line);
                    wcscpy(g_dev, line[0] ? line : L"未选择设备");
                    g_page = 4; BuildPage();
                } else if (g_page == 4) {
                    int sel = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_IMG_LIST));
                    if (sel < 0) sel = 0;
                    CreateThread(NULL, 0, Worker, (LPVOID)(INT_PTR)sel, 0, NULL);
                }
            } else if (id == IDC_BACK) {
                if (g_page > 0) { g_page--; BuildPage(); }
            } else if (id == IDC_DIR_BTN) {
                wchar_t path[MAX_PATH] = L"";
                BROWSEINFOW bi;
                memset(&bi, 0, sizeof(bi));
                bi.hwndOwner = hwnd;
                bi.lpszTitle = L"选择下载目录";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                if (pidl && SHGetPathFromIDListW(pidl, path)) {
                    wcscpy(g_dir, path);
                    SetDlgItemTextW(hwnd, IDC_DIR_EDIT, g_dir);
                }
            } else if (id == IDC_DEV_REFRESH) {
                FillDevices(GetDlgItem(hwnd, IDC_DEV_LIST));
            }
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"DueslinBurnerWnd2";
    RegisterClassW(&wc);

    GetCurrentDirectoryW(MAX_PATH, g_dir);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"DUESLIN 镜像刻录器 2.0",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 500, 430,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
