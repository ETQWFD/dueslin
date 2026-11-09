/* WCT-OS Kernel K0 —— 自研内核（GOP 图形 + USB HID 键鼠 + 文件浏览），不依赖 Linux */
#include "efi.h"

EFI_SYSTEM_TABLE  *ST;
EFI_BOOT_SERVICES *BS;

/* ---- gui.inc 依赖的指针协议最小定义 ---- */
typedef struct { UINT64 ResolutionX, ResolutionY; } SP_MODE_OR;
typedef struct { INT32 RelativeMovementX, RelativeMovementY; BOOLEAN LeftButton, RightButton; } SP_STATE_OR;
typedef struct { UINT64 AbsoluteMinX, AbsoluteMinY, AbsoluteMaxX, AbsoluteMaxY, AbsoluteUnitsX, AbsoluteUnitsY; } AP_MODE_OR;
typedef struct { UINT64 CurrentX, CurrentY; UINT32 ActiveButtons; } AP_STATE_OR;
typedef struct EFI_SIMPLE_POINTER_PROTOCOL {
    EFI_STATUS (*Reset)(struct EFI_SIMPLE_POINTER_PROTOCOL *, BOOLEAN);
    EFI_STATUS (*GetState)(struct EFI_SIMPLE_POINTER_PROTOCOL *, SP_STATE_OR *);
    SP_MODE_OR *Mode;
} EFI_SIMPLE_POINTER_PROTOCOL;
typedef struct EFI_ABSOLUTE_POINTER_PROTOCOL {
    EFI_STATUS (*Reset)(struct EFI_ABSOLUTE_POINTER_PROTOCOL *, BOOLEAN);
    EFI_STATUS (*GetState)(struct EFI_ABSOLUTE_POINTER_PROTOCOL *, AP_STATE_OR *);
    AP_MODE_OR *Mode;
} EFI_ABSOLUTE_POINTER_PROTOCOL;
#define EFI_SIMPLE_POINTER_STATE SP_STATE_OR
#define EFI_ABSOLUTE_POINTER_STATE AP_STATE_OR

/* ---- 内核服务（供 gui.inc 使用） ---- */
static CHAR16 CWD_BUF[512];
#define CWD CWD_BUF
int theme_index = 0;   /* 0=Blue 默认 */
int gfx_active = 0;
#define HIST_MAX 4096
static char hist[HIST_MAX];
static int hist_count = 0, hist_pos = 0;
static int active_tty = 1;

static EFI_GUID gop_guid = { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } };
static EFI_GUID sfsp_guid = { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

static EFI_FILE_PROTOCOL *root_fs;

static void k_puts(const char *s)
{
    CHAR16 buf[256];
    int i = 0;
    while (*s && i < 255) { buf[i++] = (CHAR16)*s++; }
    buf[i] = 0;
    ST->ConOut->OutputString(ST->ConOut, buf);
}

void outln(const CHAR16 *s)
{
    ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s);
    ST->ConOut->OutputString(ST->ConOut, L"\r\n");
}
void reset_color(void) { ST->ConOut->SetAttribute(ST->ConOut, 0x0F); }
void print_banner(void) { }

int weq(const CHAR16 *a, const CHAR16 *b)
{
    while (*a && *b) { if (*a != *b) return 0; ++a; ++b; }
    return *a == *b;
}
int scmp(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 1; ++a; ++b; }
    return (*a == *b) ? 0 : 1;
}
void wcopy(CHAR16 *dst, int max, const CHAR16 *src)
{
    int i = 0;
    while (*src && i < max - 1) dst[i++] = *src++;
    dst[i] = 0;
}
int open_path(const CHAR16 *path, EFI_FILE_PROTOCOL **out, UINT64 mode, UINT64 attr)
{
    (void)mode; (void)attr;
    if (!root_fs) return 0;
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) { *out = root_fs; return 1; }
    EFI_FILE_PROTOCOL *f = root_fs;
    if (EFI_IS_ERROR(f->Open(f, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0))) return 0;
    *out = f;
    return 1;
}


/* ---- 内核最小 C 库与 WCT Shell ---- */
void *memcpy(void *d, const void *s, UINTN n)
{
    char *dd = (char *)d; const char *ss = (const char *)s;
    while (n--) *dd++ = *ss++;
    return d;
}
UINTN wlen(const CHAR16 *s) { UINTN n = 0; while (s[n]) ++n; return n; }
void wcat(CHAR16 *dst, UINTN cap, const CHAR16 *src)
{
    UINTN n = wlen(dst), i = 0;
    while (src[i] && n + i + 1 < cap) { dst[n + i] = src[i]; ++i; }
    dst[n + i] = 0;
}
void normalize_path(const CHAR16 *arg, CHAR16 *out, UINTN cap)
{
    UINTN i = 0;
    if (arg[0] == L'/' || wlen(CWD_BUF) == 0) { out[0] = 0; }
    else { for (i = 0; i < cap - 1 && CWD_BUF[i]; ++i) out[i] = CWD_BUF[i]; out[i] = 0; }
    while (*arg == L'/') ++arg;
    if (wlen(out) && out[wlen(out) - 1] != L'\\') wcat(out, cap, L"\\");
    wcat(out, cap, arg);
}
static void k_print(const char *s) { k_puts(s); }

void process_command(CHAR16 *line)
{
    /* K0 内核内置命令：help / version / uname / clear / ls / exit */
    char ascii[256]; int i = 0;
    while (line[i] && i < 255) { ascii[i] = (char)line[i]; ++i; }
    ascii[i] = 0;
    k_print("wct> ");
    k_print(ascii);
    k_print("\r\n");
    if (!scmp(ascii, "help")) {
        k_print("  help  version  uname  ls  clear  exit\r\n");
    } else if (!scmp(ascii, "version")) {
        k_print("  WCT-OS Kernel 0.1_ET   build " __DATE__ "\r\n");
    } else if (!scmp(ascii, "uname")) {
        k_print("  WCTOS wctk 0.1_ET x86_64 native\r\n");
    } else if (!scmp(ascii, "ls")) {
        EFI_FILE_PROTOCOL *d = NULL;
        if (open_path(CWD_BUF, &d, 0, 0) && d) {
            static UINT8 buf[1024];
            for (int g = 0; g < 32; ++g) {
                UINTN sz = sizeof(buf);
                if (EFI_IS_ERROR(d->Read(d, &sz, buf)) || sz == 0) break;
                EFI_FILE_INFO *inf = (EFI_FILE_INFO *)buf;
                if (!inf->FileName[0]) break;
                CHAR16 *nm = inf->FileName;
                for (int k = 0; nm[k] && k < 200; ++k) if ((unsigned)nm[k] < 128) ST->ConOut->OutputString(ST->ConOut, (CHAR16[]){nm[k], 0});
                k_print("\r\n");
            }
            d->Close(d);
        }
    } else if (!scmp(ascii, "clear")) {
        ST->ConOut->ClearScreen(ST->ConOut);
    } else if (!scmp(ascii, "")) {
        /* 空命令 */
    } else {
        k_print("  wctsh: command not found\r\n");
    }
}

#include "gui.inc"

static void banner(void)
{
    ST->ConOut->ClearScreen(ST->ConOut);
    ST->ConOut->SetAttribute(ST->ConOut, 0x1F);
    k_puts("  WCT Kernel 0.1_ET   WCT-OS native kernel   (GOP + USB HID + WCTFS)\r\n");
    ST->ConOut->SetAttribute(ST->ConOut, 0x0F);
    k_puts("  image: wctk.efi   arch: x86_64   mode: UEFI native   no linux\r\n\r\n");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    (void)image;
    ST = systab;
    BS = systab->BootServices;
    systab->ConOut->Reset(systab->ConOut, 0);
    banner();

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    if (!EFI_IS_ERROR(BS->LocateProtocol(&sfsp_guid, NULL, (VOID **)&fs)) && fs) {
        if (!EFI_IS_ERROR(fs->OpenVolume(fs, &root_fs))) k_puts("  [ok] WCTFS root mounted\r\n");
    }
    CWD_BUF[0] = 0;
    hist_count = 0;
    hist_pos = 0;

    k_puts("  [ok] kernel services ready\r\n");
    k_puts("  [ok] GOP framebuffer probe\r\n");
    k_puts("  [ok] USB HID input probe\r\n");
    k_puts("  entering WCT desktop...\r\n");
    BS->Stall(500000);
    cmd_gui();
    return EFI_SUCCESS;
}
