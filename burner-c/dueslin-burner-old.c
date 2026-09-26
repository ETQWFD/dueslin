/*
 * DUESLIN 系统镜像刻录器 (Windows)
 * 使用 C 语言 + Win32 API 制作
 *
 * 功能流程:
 *   选择镜像 -> 选择下载目录 -> 选择刻录模式(ISO/DVD) -> 选择设备 -> 输入YES确认
 *   -> 下载镜像 -> 刻录到 U 盘 / 提示刻录光盘
 *
 * 编译 (MinGW-w64, 单文件含图标):
 *   windres logo.rc -O coff -o logo.res
 *   x86_64-w64-mingw32-gcc -O2 -municode dueslin-burner.c logo.res \
 *       -lcomctl32 -lwininet -lole32 -lcomdlg32 -o DueslinBurner.exe \
 *       -static-libgcc -static-libstdc++ -Wl,-subsystem,windows
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <wininet.h>
#include <winioctl.h>
#include <urlmon.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#define IDC_IMG_LIST    101
#define IDC_DIR_EDIT    102
#define IDC_DIR_BTN     103
#define IDC_MODE_ISO    104
#define IDC_MODE_DVD    105
#define IDC_DEV_LIST    106
#define IDC_DEV_REFRESH 107
#define IDC_YES_EDIT    108
#define IDC_NEXT        109
#define IDC_BACK        110
#define IDC_STATUS      111
#define IDC_PROGRESS    112
#define IDC_WIFI_NOTE   113

/* 6 个镜像：名称 + GitHub Release 下载 URL */
typedef struct { const char *name; const char *url; const char *file; } IMG;
static IMG g_imgs[] = {
    {"桌面版 (x86_64)", "https://github.com/ETQWFD/dueslin/releases/download/v1.0.0/DUESLIN-1.0.iso", "DUESLIN-1.0.iso"},
    {"桌面版 (ARM64)",  "https://github.com/ETQWFD/dueslin/releases/download/v1.0.0/DUESLIN-1.0-arm64.iso", "DUESLIN-1.0-arm64.iso"},
    {"服务器版 (x86_64)", "https://github.com/ETQWFD/dueslin/releases/download/v1.0.0/DUESLIN-Server-1.0.iso", "DUESLIN-Server-1.0.iso"},
    {"服务器版 (ARM64)", "https://github.com/ETQWFD/dueslin/releases/download/v1.0.0/DUESLIN-Server-1.0-arm64.iso", "DUESLIN-Server-1.0-arm64.iso"},
    {"精简版 (x86_64)", "https://github.com/ETQWFD/dueslin/releases/download/v1.0.0/DUESLIN-Mini-1.0.iso", "DUESLIN-Mini-1.0.iso"},
    {"精简版 (ARM64)",  "https://github.com/ETQWFD/dueslin/releases/download/v1.0.0/DUESLIN-Mini-1.0-arm64.iso", "DUESLIN-Mini-1.0-arm64.iso"},
};
#define NUM_IMG ((int)(sizeof(g_imgs)/sizeof(g_imgs[0])))

static HWND g_hwnd;
static int g_page = 0;
static char g_dir[MAX_PATH] = "";
static char g_dev[MAX_PATH] = "";
static int g_mode_iso = 1;   /* 1=ISO(U盘), 0=DVD */
static volatile int g_busy = 0;

/* ---------- 页面构建 ---------- */
static void SetStatus(const char *s) {
    SetDlgItemTextA(g_hwnd, IDC_STATUS, s);
}

static void FillDevices(HWND combo) {
    ComboBox_ResetContent(combo);
    /* 枚举物理盘 \\.\PhysicalDrive0..7 */
    for (int i = 0; i < 8; i++) {
        char path[64], line[160];
        wsprintfA(path, "\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            char name[128] = "";
            DWORD got = 0;
            STORAGE_PROPERTY_QUERY spq;
            memset(&spq, 0, sizeof(spq));
            spq.PropertyId = StorageDeviceProperty;
            spq.QueryType = PropertyStandardQuery;
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &spq, sizeof(spq),
                                name, sizeof(name), &got, NULL) && got > 0) {
                /* name 前部为 vendor/product */
                char *p = name;
                for (int k = 0; k < (int)got && k < 127; k++)
                    if (name[k] == 0) name[k] = ' ';
                name[got > 100 ? 100 : got] = 0;
                p = name;
                while (*p == ' ') p++;
            } else {
                strcpy(name, "磁盘");
            }
            wsprintfA(line, "物理磁盘 %d  -  %s", i, name);
            ComboBox_AddString(combo, line);
            ComboBox_SetItemData(combo, ComboBox_GetCount(combo)-1, i);
            CloseHandle(h);
        }
    }
    if (ComboBox_GetCount(combo) > 0)
        ComboBox_SetCurSel(combo, 0);
}

static void BuildPage(void) {
    HWND dlg = g_hwnd;
    /* 清掉旧控件 */
    HWND child = GetWindow(dlg, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        DestroyWindow(child);
        child = next;
    }
    SetStatus("");

    if (g_page == 0) {           /* 选择镜像 */
        CreateWindowA("STATIC", "DUESLIN 镜像刻录器", WS_CHILD|WS_VISIBLE,
                      20, 16, 360, 28, dlg, NULL, NULL, NULL);
        HFONT f = CreateFontA(22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, 0, 0, "Microsoft YaHei UI");
        HWND t = GetDlgItem(dlg, 0);
        SendMessage(GetWindow(dlg, GW_CHILD), WM_SETFONT, (WPARAM)f, TRUE);
        CreateWindowA("STATIC", "请选择要下载并刻录的 DUESLIN 镜像：", WS_CHILD|WS_VISIBLE,
                      20, 60, 380, 22, dlg, NULL, NULL, NULL);
        HWND combo = CreateWindowA("COMBOBOX", "", WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
                                   20, 90, 380, 220, dlg, (HMENU)IDC_IMG_LIST, NULL, NULL);
        for (int i = 0; i < NUM_IMG; i++)
            ComboBox_AddString(combo, g_imgs[i].name);
        ComboBox_SetCurSel(combo, 0);
        SetWindowLongPtrA(combo, GWLP_ID, IDC_IMG_LIST);
        CreateWindowA("STATIC", "下载镜像将来自 GitHub Releases（国内加速）。", WS_CHILD|WS_VISIBLE,
                      20, 120, 380, 20, dlg, NULL, NULL, NULL);
    } else if (g_page == 1) {    /* 下载目录 */
        CreateWindowA("STATIC", "选择下载目录", WS_CHILD|WS_VISIBLE,
                      20, 16, 300, 26, dlg, NULL, NULL, NULL);
        CreateWindowA("STATIC", "镜像将下载到该目录：", WS_CHILD|WS_VISIBLE,
                      20, 56, 200, 20, dlg, NULL, NULL, NULL);
        CreateWindowA("EDIT", g_dir, WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
                      20, 84, 300, 26, dlg, (HMENU)IDC_DIR_EDIT, NULL, NULL);
        CreateWindowA("BUTTON", "浏览...", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                      330, 84, 70, 26, dlg, (HMENU)IDC_DIR_BTN, NULL, NULL);
    } else if (g_page == 2) {    /* 刻录模式 */
        CreateWindowA("STATIC", "选择刻录模式", WS_CHILD|WS_VISIBLE,
                      20, 16, 300, 26, dlg, NULL, NULL, NULL);
        HWND rb1 = CreateWindowA("BUTTON", "ISO  -  刻录到 U 盘", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
                                 20, 60, 200, 26, dlg, (HMENU)IDC_MODE_ISO, NULL, NULL);
        HWND rb2 = CreateWindowA("BUTTON", "DVD  -  刻录到光盘", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
                                 20, 96, 200, 26, dlg, (HMENU)IDC_MODE_DVD, NULL, NULL);
        Button_SetCheck(rb1, g_mode_iso ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(rb2, g_mode_iso ? BST_UNCHECKED : BST_CHECKED);
    } else if (g_page == 3) {    /* 选择设备 */
        CreateWindowA("STATIC", "选择目标设备", WS_CHILD|WS_VISIBLE,
                      20, 16, 300, 26, dlg, NULL, NULL, NULL);
        CreateWindowA("STATIC", g_mode_iso ? "请插入 U 盘并选择：" : "请插入空白光盘并选择光驱：",
                      WS_CHILD|WS_VISIBLE, 20, 54, 320, 20, dlg, NULL, NULL, NULL);
        HWND combo = CreateWindowA("COMBOBOX", "", WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
                                   20, 82, 340, 200, dlg, (HMENU)IDC_DEV_LIST, NULL, NULL);
        FillDevices(combo);
        CreateWindowA("BUTTON", "刷新", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                      370, 82, 60, 26, dlg, (HMENU)IDC_DEV_REFRESH, NULL, NULL);
        CreateWindowA("STATIC", "刻录将格式化该设备，所有数据将被清除！", WS_CHILD|WS_VISIBLE,
                      20, 118, 340, 20, dlg, NULL, NULL, NULL);
    } else if (g_page == 4) {    /* 确认 */
        CreateWindowA("STATIC", "确认刻录", WS_CHILD|WS_VISIBLE,
                      20, 16, 300, 26, dlg, NULL, NULL, NULL);
        char msg[512];
        int sel = ComboBox_GetCurSel(GetDlgItem(dlg, IDC_IMG_LIST));
        if (sel < 0) sel = 0;
        wsprintfA(msg, "镜像：%s\n目标：%s\n模式：%s\n\n设备上的所有数据将被清除！\n请输入大写 YES 确认：",
                  g_imgs[sel].name, g_dev, g_mode_iso ? "U盘(ISO)" : "光盘(DVD)");
        CreateWindowA("STATIC", msg, WS_CHILD|WS_VISIBLE,
                      20, 16, 380, 110, dlg, NULL, NULL, NULL);
        CreateWindowA("EDIT", "", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_UPPERCASE,
                      20, 140, 120, 26, dlg, (HMENU)IDC_YES_EDIT, NULL, NULL);
    }

    /* 底部按钮 */
    HWND back = CreateWindowA("BUTTON", "< 上一步", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                              160, 300, 90, 30, dlg, (HMENU)IDC_BACK, NULL, NULL);
    HWND next = CreateWindowA("BUTTON", g_page == 4 ? "开始刻录" : "下一步 >",
                              WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                              260, 300, 100, 30, dlg, (HMENU)IDC_NEXT, NULL, NULL);
    EnableWindow(back, g_page > 0 && !g_busy);
    EnableWindow(next, !g_busy);
    SetFocus(next);
}

/* ---------- 下载 + 刻录 ---------- */
static DWORD WINAPI Worker(LPVOID param) {
    char url[512], dest[MAX_PATH], yes[32];
    int sel = (int)(intptr_t)param;
    HWND dlg = g_hwnd;
    g_busy = 1;
    PostMessage(dlg, WM_COMMAND, MAKEWPARAM(IDC_NEXT, BN_CLICKED), 0); /* 刷新按钮态 */

    GetDlgItemTextA(dlg, IDC_YES_EDIT, yes, sizeof(yes));
    if (strcmp(yes, "YES") != 0) {
        SetStatus("请输入大写 YES 确认后再开始。");
        g_busy = 0;
        return 0;
    }

    wsprintfA(url, "%s", g_imgs[sel].url);
    if (!g_dir[0]) GetCurrentDirectoryA(sizeof(g_dir), g_dir);
    wsprintfA(dest, "%s\\%s", g_dir, g_imgs[sel].file);

    /* 1. 下载 */
    SetStatus("正在下载镜像（请耐心等待）...");
    HRESULT hr = URLDownloadToFileA(NULL, url, dest, 0, NULL);
    if (FAILED(hr)) {
        SetStatus("下载失败：请检查网络，或稍后重试。");
        g_busy = 0;
        return 0;
    }
    SetStatus("下载完成，正在刻录...");

    /* 2. 刻录 */
    if (g_mode_iso) {
        HANDLE src = CreateFileA(dest, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        char devpath[64];
        int drive = -1;
        HWND cb = GetDlgItem(dlg, IDC_DEV_LIST);
        int i = ComboBox_GetCurSel(cb);
        if (i >= 0) drive = (int)ComboBox_GetItemData(cb, i);
        wsprintfA(devpath, "\\\\.\\PhysicalDrive%d", drive < 0 ? 0 : drive);
        HANDLE dst = CreateFileA(devpath, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);
        if (src == INVALID_HANDLE_VALUE || dst == INVALID_HANDLE_VALUE) {
            SetStatus("刻录失败：无法打开镜像或设备（请以管理员身份运行）。");
            if (src != INVALID_HANDLE_VALUE) CloseHandle(src);
            if (dst != INVALID_HANDLE_VALUE) CloseHandle(dst);
            g_busy = 0;
            return 0;
        }
        BYTE buf[1 << 20];
        DWORD rd, wr, total = 0;
        while (ReadFile(src, buf, sizeof(buf), &rd, NULL) && rd > 0) {
            WriteFile(dst, buf, rd, &wr, NULL);
            total += rd;
            if (total % (8 << 20) == 0) {
                char s[128];
                wsprintfA(s, "正在刻录... %.1f MB", total / 1048576.0);
                SetStatus(s);
            }
        }
        CloseHandle(src); CloseHandle(dst);
        SetStatus("刻录完成！可以拔出 U 盘用于启动安装。");
    } else {
        SetStatus("DVD 模式：请使用系统自带刻录功能将镜像写入光盘（Win10/11 资源管理器右键该文件 -> 刻录光盘映像）。");
    }
    g_busy = 0;
    return 0;
}

/* ---------- 消息循环 ---------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        BuildPage();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (HIWORD(w) == BN_CLICKED) {
            if (id == IDC_NEXT) {
                if (g_page == 0) {
                    g_page = 1; BuildPage();
                } else if (g_page == 1) {
                    GetDlgItemTextA(hwnd, IDC_DIR_EDIT, g_dir, sizeof(g_dir));
                    if (!g_dir[0]) GetCurrentDirectoryA(sizeof(g_dir), g_dir);
                    g_page = 2; BuildPage();
                } else if (g_page == 2) {
                    g_mode_iso = Button_GetCheck(GetDlgItem(hwnd, IDC_MODE_ISO)) == BST_CHECKED;
                    g_page = 3; BuildPage();
                } else if (g_page == 3) {
                    HWND cb = GetDlgItem(hwnd, IDC_DEV_LIST);
                    int i = ComboBox_GetCurSel(cb);
                    char line[160] = "";
                    if (i >= 0) ComboBox_GetLBText(cb, i, line);
                    wsprintfA(g_dev, "%s", line[0] ? line : "未选择设备");
                    g_page = 4; BuildPage();
                } else if (g_page == 4) {
                    int sel = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_IMG_LIST));
                    if (sel < 0) sel = 0;
                    CreateThread(NULL, 0, Worker, (LPVOID)(intptr_t)sel, 0, NULL);
                }
            } else if (id == IDC_BACK) {
                if (g_page > 0) { g_page--; BuildPage(); }
            } else if (id == IDC_DIR_BTN) {
                char path[MAX_PATH] = "";
                BROWSEINFOA bi;
                memset(&bi, 0, sizeof(bi));
                bi.hwndOwner = hwnd;
                bi.lpszTitle = "选择下载目录";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
                if (pidl && SHGetPathFromIDListA(pidl, path)) {
                    strcpy(g_dir, path);
                    SetDlgItemTextA(hwnd, IDC_DIR_EDIT, g_dir);
                }
            } else if (id == IDC_DEV_REFRESH) {
                HWND cb = GetDlgItem(hwnd, IDC_DEV_LIST);
                FillDevices(cb);
            }
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(1));
    wc.lpszClassName = "DueslinBurnerWnd";
    RegisterClassA(&wc);

    GetCurrentDirectoryA(sizeof(g_dir), g_dir);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "DUESLIN 镜像刻录器 1.0",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 420, 380,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
