#include "efi.h"

static EFI_SYSTEM_TABLE *ST;
static EFI_HANDLE g_image;
static EFI_LOADED_IMAGE_PROTOCOL *g_loaded;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FS;
static EFI_FILE_PROTOCOL *ROOT;
static EFI_FILE_PROTOCOL *BOOT_ROOT;
#define MAX_VOLUMES 8
static EFI_FILE_PROTOCOL *vol_roots[MAX_VOLUMES];
static CHAR16 vol_labels[MAX_VOLUMES][32];
static UINT64 vol_sizes[MAX_VOLUMES];
static int vol_count;
static int vol_boot = -1;
static int vol_current;
static CHAR16 CWD[128] = L"\\";
static UINT8 boot_device_path[1024] __attribute__((aligned(8)));

typedef struct {
    const char *name;
    UINTN text;
    UINTN bright;
    UINTN dim;
    UINTN accent;
    UINTN prompt;
    UINTN dir;
    UINTN error;
} Theme;

static const Theme themes[] = {
    { "ubuntu", 0x07, 0x0F, 0x08, 0x0D, 0x0D, 0x0D, 0x0C },
    { "wct",    0x07, 0x0F, 0x08, 0x0A, 0x0A, 0x09, 0x0C },
    { "matrix", 0x0A, 0x0F, 0x02, 0x0A, 0x0A, 0x0A, 0x0C },
    { "amber",  0x0E, 0x0F, 0x06, 0x06, 0x0E, 0x06, 0x0C },
    { "ice",    0x0B, 0x0F, 0x08, 0x09, 0x0F, 0x0B, 0x0C }
};
static const CHAR16 *wct_logo[] = {
    L"█     █  █████  ███████   █████  ███████  ____ _____",
    L"█     █ █          █     █     █ █         |  _|_   _|",
    L"█  █  █ █          █     █     █ ███████   | |   | |",
    L"█ █ █ █ █          █     █     █       █   | |_  | |",
    L"██   ██  █████     █      █████  ███████  |___| |_|"
};

static int theme_index = 1;
static UINTN current_attr = 0x07;
static UINTN output_color_override;
#define LOG_MAX 128
#define LOG_INFO 0
#define LOG_WARN 1
#define LOG_ERROR 2
typedef struct {
    char time[9];
    int level;
    char tag[8];
    char text[160];
} LogEntry;
static LogEntry log_entries[LOG_MAX];
static int log_count;
static int log_file_number;
static char log_file_date[11];

static void log_add_level(int level, const char *tag, const char *text)
{
    if (log_count >= LOG_MAX) {
        for (int i = 1; i < LOG_MAX; ++i) log_entries[i - 1] = log_entries[i];
        --log_count;
    }
    LogEntry *e = &log_entries[log_count++];
    e->level = level;
    e->time[0] = '0'; e->time[1] = '0'; e->time[2] = ':';
    e->time[3] = '0'; e->time[4] = '0'; e->time[5] = ':';
    e->time[6] = '0'; e->time[7] = '0'; e->time[8] = 0;
    EFI_TIME now;
    if (ST->RuntimeServices && ST->RuntimeServices->GetTime &&
        !EFI_IS_ERROR(ST->RuntimeServices->GetTime(&now, NULL))) {
        e->time[0] = (char)('0' + now.Hour / 10);
        e->time[1] = (char)('0' + now.Hour % 10);
        e->time[3] = (char)('0' + now.Minute / 10);
        e->time[4] = (char)('0' + now.Minute % 10);
        e->time[6] = (char)('0' + now.Second / 10);
        e->time[7] = (char)('0' + now.Second % 10);
    }
    int n = 0;
    while (tag && tag[n] && n < 7) { e->tag[n] = tag[n]; ++n; }
    e->tag[n] = 0;
    n = 0;
    while (text && text[n] && n < 159) { e->text[n] = text[n]; ++n; }
    e->text[n] = 0;
}

static void log_add(const char *tag, const char *text)
{
    log_add_level(LOG_INFO, tag, text);
}

static void log_add_int(const char *tag, const char *label, UINT64 value)
{
    char buf[160];
    int n = 0;
    while (label[n] && n < 100) { buf[n] = label[n]; ++n; }
    buf[n++] = ':';
    buf[n++] = ' ';
    char digits[24];
    int d = 0;
    if (value == 0) digits[d++] = '0';
    while (value > 0 && d < 23) { digits[d++] = (char)('0' + value % 10); value /= 10; }
    while (d > 0 && n < 158) buf[n++] = digits[--d];
    buf[n] = 0;
    log_add(tag, buf);
}
#define SCRIPT_VARS 8
static char script_var_names[SCRIPT_VARS][24];
static char script_var_values[SCRIPT_VARS][96];
static int script_var_count;
static void process_command(CHAR16 *line);
static int log_save(int quiet);
static void print_banner(void);
static void cmd_clear(void);
static void scopy(char *dst, UINTN cap, const char *src);
static char *strim(char *text);
static int scmp(const char *a, const char *b);
static int resolve_mount(const CHAR16 *path, EFI_FILE_PROTOCOL **root, CHAR16 *relative, UINTN cap);
static int completion_suffix(const CHAR16 *line, CHAR16 *suffix, UINTN cap);
static UINTN input_start_col, input_start_row, drawn_total;
#define HIST_MAX 16
static CHAR16 hist[HIST_MAX][192];
static int hist_count;
static int hist_pos;
static int last_control;
typedef struct { UINT32 pid; char name[20]; char state[10]; } Proc;
static Proc procs[8] = {
    { 1, "wct-init",  "running" },
    { 2, "wct-shell", "running" }
};
static int proc_count = 2;

static EFI_PHYSICAL_ADDRESS hog_address;
static UINTN hog_pages;
static int hog_active;
static UINT64 boot_total_mb;
static UINT64 boot_main_mb;
static UINT64 boot_reserve_mb;
static int linux_available;
static int boot_no_linux;

static EFI_GUID loaded_image_guid =
    { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static EFI_GUID simple_fs_guid =
    { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static EFI_GUID device_path_guid =
    { 0x09576E91, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static EFI_GUID fs_info_guid =
    { 0x09576E93, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static EFI_GUID gop_guid =
    { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } };

static void clear_graphics(void)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (ST->BootServices->LocateProtocol(&gop_guid, NULL, (VOID **)&gop) != EFI_SUCCESS || !gop)
        return;
    if (!gop->Mode || !gop->Mode->FrameBufferBase || !gop->Mode->FrameBufferSize)
        return;
    volatile UINT8 *fb = (volatile UINT8 *)(UINTN)gop->Mode->FrameBufferBase;
    for (UINTN i = 0; i < gop->Mode->FrameBufferSize; ++i)
        fb[i] = 0;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

static UINTN wlen(const CHAR16 *s)
{
    UINTN n = 0;
    while (s && s[n])
        ++n;
    return n;
}

static int weq(const CHAR16 *a, const CHAR16 *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static int wprefix(const CHAR16 *s, const CHAR16 *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static void wcopy(CHAR16 *dst, UINTN cap, const CHAR16 *src)
{
    UINTN i = 0;
    if (!cap)
        return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static void wcat(CHAR16 *dst, UINTN cap, const CHAR16 *src)
{
    UINTN n = wlen(dst);
    wcopy(dst + n, cap - n, src);
}

static void out(const CHAR16 *s)
{
    if (output_color_override)
        ST->ConOut->SetAttribute(ST->ConOut, output_color_override);
    ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s);
    if (output_color_override)
        ST->ConOut->SetAttribute(ST->ConOut, current_attr);
}

static void outln(const CHAR16 *s)
{
    out(s);
    out(L"\r\n");
}

static const Theme *theme(void) { return &themes[theme_index]; }

static void set_color(UINTN attr)
{
    current_attr = attr;
    ST->ConOut->SetAttribute(ST->ConOut, attr);
}

static void reset_color(void)
{
    set_color(theme()->text);
}

static void outc(UINTN attr, const CHAR16 *s)
{
    set_color(attr);
    out(s);
    reset_color();
}

static void out_ascii(const char *s)
{
    CHAR16 buf[2];
    buf[1] = 0;
    while (s && *s) {
        char c = *s++;
        if (c == '\n') {
            out(L"\r\n");
        } else if (c == '\r') {
            continue;
        } else {
            buf[0] = (c >= 32 && c < 127) ? (CHAR16)c : (CHAR16)'.';
            out(buf);
        }
    }
}

static void out_int(UINT64 value)
{
    CHAR16 digits[32];
    int n = 0;
    if (value == 0) {
        out(L"0");
        return;
    }
    while (value && n < 31) {
        digits[n++] = (CHAR16)(L'0' + (value % 10));
        value /= 10;
    }
    CHAR16 outbuf[33];
    for (int i = 0; i < n; ++i)
        outbuf[i] = digits[n - 1 - i];
    outbuf[n] = 0;
    out(outbuf);
}

static void redraw_line(CHAR16 *line, UINTN len, int suggest)
{
    ST->ConOut->SetCursorPosition(ST->ConOut, input_start_col, input_start_row);
    set_color(theme()->text);
    CHAR16 suffix[192];
    suffix[0] = 0;
    UINTN slen = 0;
    if (suggest && len > 0 && completion_suffix(line, suffix, sizeof(suffix) / sizeof(suffix[0]))) {
        slen = wlen(suffix);
    }
    out(line);
    if (slen) {
        set_color(0x09);
        out(suffix);
        set_color(theme()->text);
    }
    UINTN total = len + slen;
    if (drawn_total > total) {
        for (UINTN i = 0; i < drawn_total - total; ++i)
            out(L" ");
    }
    drawn_total = total;
    ST->ConOut->SetCursorPosition(ST->ConOut, input_start_col + len, input_start_row);
}

static void cpuid(UINT32 leaf, UINT32 sub, UINT32 *a, UINT32 *b, UINT32 *c, UINT32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static void print_memory_summary(void)
{
    UINTN size = 0, key = 0, desc_size = 0;
    UINT32 version = 0;
    ST->BootServices->GetMemoryMap(&size, NULL, &key, &desc_size, &version);
    size += 4096;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, size, (VOID **)&map)) || !map) {
        outc(theme()->error, L"memory map unavailable\r\n");
        return;
    }
    if (EFI_IS_ERROR(ST->BootServices->GetMemoryMap(&size, map, &key, &desc_size, &version))) {
        ST->BootServices->FreePool(map);
        outc(theme()->error, L"memory map unavailable\r\n");
        return;
    }
    UINT64 total = 0, free_pages = 0;
    for (UINT8 *p = (UINT8 *)map; p < (UINT8 *)map + size; p += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)p;
        if (d->Type >= 1 && d->Type <= 10)
            total += d->NumberOfPages * 4096;
        if (d->Type == EFI_CONVENTIONAL_MEMORY)
            free_pages += d->NumberOfPages;
    }
    ST->BootServices->FreePool(map);

    out(L"  RAM total : ");
    out_int(total / (1024 * 1024));
    outln(L" MiB");
    out(L"  RAM free  : ");
    out_int(free_pages * 4096 / (1024 * 1024));
    outln(L" MiB");
    out(L"  reserved  : ");
    out_int(boot_reserve_mb + (hog_active ? hog_pages * 4096 / (1024 * 1024) : 0));
    outln(L" MiB by WCT");
}

static UINT64 memory_total_bytes(void)
{
    UINTN size = 0, key = 0, desc_size = 0;
    UINT32 version = 0;
    ST->BootServices->GetMemoryMap(&size, NULL, &key, &desc_size, &version);
    size += 4096;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, size, (VOID **)&map)) || !map)
        return 0;
    if (EFI_IS_ERROR(ST->BootServices->GetMemoryMap(&size, map, &key, &desc_size, &version))) {
        ST->BootServices->FreePool(map);
        return 0;
    }
    UINT64 total = 0;
    for (UINT8 *p = (UINT8 *)map; p < (UINT8 *)map + size; p += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)p;
        if (d->Type >= 1 && d->Type <= 10)
            total += d->NumberOfPages * 4096;
    }
    ST->BootServices->FreePool(map);
    return total;
}

static void boot_ok(const char *label)
{
    outc(0x0A, L"  [ OK ] ");
    out_ascii(label);
    outln(L"");
    log_add("boot", label);
    ST->BootServices->Stall(120000);
}

static void draw_wct_logo(void)
{
    ST->ConOut->SetAttribute(ST->ConOut, 0x09);
    for (int i = 0; i < 5; ++i) {
        out(L"   ");
        out(wct_logo[i]);
        outln(L"");
    }
}

static int boot_menu(void)
{
    clear_graphics();
    ST->ConOut->ClearScreen(ST->ConOut);
    draw_wct_logo();
    outln(L"");
    outc(0x0B, L"        WCT-OS LTS 1.0_ET  /  Boot Manager");
    outln(L"");
    outln(L"  1. WCT-OS LTS 1.0        Full (Linux + Native)");
    outln(L"  2. WCT-OS Native Only    Native only (No Linux)");
    outln(L"");
    outc(0x0A, L"  Press 1 or 2, Enter for default (1)...");
    outln(L"");
    for (int i = 0; i < 500; ++i) {
        EFI_INPUT_KEY key;
        if (!EFI_IS_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key))) {
            if (key.UnicodeChar == 0x32) return 2;
            if (key.UnicodeChar == 0x31 || key.UnicodeChar == '\r' || key.UnicodeChar == '\n') return 1;
        }
        ST->BootServices->Stall(20000);
    }
    return 1;
}

static int maybe_f11_menu(void)
{
    outc(0x0B, L"  Press F11 for boot menu...");
    outln(L"");
    for (int i = 0; i < 150; ++i) {
        EFI_INPUT_KEY key;
        if (!EFI_IS_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key))) {
            if (key.ScanCode == 0x15) return boot_menu();
        }
        ST->BootServices->Stall(20000);
    }
    return 1;
}

static void boot_splash(void)
{
    clear_graphics();
    ST->ConOut->ClearScreen(ST->ConOut);
    draw_wct_logo();
    outc(0x0B, L"        WCT-OS LTS 1.0_ET  /  Native Boot");
    outln(L"");
    outln(L"");

    outc(0x0A, L"  [ OK ] ");
    outc(0x0B, L"CPU: ");
    UINT32 a, b, c, d;
    char vendor[13] = { 0 };
    cpuid(0, 0, &a, &b, &c, &d);
    *(UINT32 *)(vendor + 0) = b;
    *(UINT32 *)(vendor + 4) = d;
    *(UINT32 *)(vendor + 8) = c;
    out_ascii(vendor);
    cpuid(0x80000000U, 0, &a, &b, &c, &d);
    if (a >= 0x80000004U) {
        char brand[49] = { 0 };
        for (UINT32 leaf = 0; leaf < 3; ++leaf) {
            cpuid(0x80000002U + leaf, 0, &a, &b, &c, &d);
            *(UINT32 *)(brand + leaf * 16 + 0) = a;
            *(UINT32 *)(brand + leaf * 16 + 4) = b;
            *(UINT32 *)(brand + leaf * 16 + 8) = c;
            *(UINT32 *)(brand + leaf * 16 + 12) = d;
        }
        out(L" / ");
        out_ascii(brand);
    }
    outln(L"");
    log_add("boot", "CPU detected");
    ST->BootServices->Stall(120000);

    outc(0x0A, L"  [ OK ] ");
    outc(0x0B, L"Mainboard: ");
    if (ST->FirmwareVendor) out(ST->FirmwareVendor);
    outln(L" UEFI platform");
    log_add("boot", "Mainboard platform detected");
    ST->BootServices->Stall(120000);

    outc(0x0A, L"  [ OK ] ");
    outc(0x0B, L"Firmware revision: ");
    out_int(ST->FirmwareRevision);
    outln(L"");
    log_add("boot", "Firmware revision detected");
    ST->BootServices->Stall(120000);

    UINT64 total = memory_total_bytes();
    UINT64 total_mb = total / (1024 * 1024);
    UINT64 main_mb = total_mb * 60 / 100;
    UINT64 reserve_mb = total_mb - main_mb;
    boot_total_mb = total_mb;
    boot_main_mb = main_mb;
    boot_reserve_mb = reserve_mb;

    outc(0x0A, L"  [ OK ] ");
    outc(0x0B, L"Max memory: ");
    out_int(total_mb);
    outln(L" MiB");
    ST->BootServices->Stall(120000);

    outc(0x0A, L"  [ OK ] ");
    outc(0x0B, L"Main memory (60%): ");
    out_int(main_mb);
    outln(L" MiB");
    ST->BootServices->Stall(120000);

    outc(0x0A, L"  [ OK ] ");
    outc(0x0B, L"Overflow reserve (40%): ");
    out_int(reserve_mb);
    outln(L" MiB");
    log_add_int("boot", "Max memory MiB", total_mb);
    log_add_int("boot", "Main memory MiB (60%)", main_mb);
    log_add_int("boot", "Overflow reserve MiB (40%)", reserve_mb);
    ST->BootServices->Stall(120000);

    boot_ok("FAT32 shared volume mounted");
    boot_ok("Native shell ready");
    outln(L"");
    outc(0x0B, L"  Starting WCT-OS_ET...");
    outln(L"");
    log_save(1);
    ST->BootServices->Stall(3000000);
}

static void cmd_hw(void)
{
    UINT32 a, b, c, d;
    char vendor[13] = { 0 };
    cpuid(0, 0, &a, &b, &c, &d);
    *(UINT32 *)(vendor + 0) = b;
    *(UINT32 *)(vendor + 4) = d;
    *(UINT32 *)(vendor + 8) = c;

    outln(L"Hardware");
    out(L"  CPU vendor: ");
    out_ascii(vendor);
    outln(L"");
    cpuid(0x80000000U, 0, &a, &b, &c, &d);
    if (a >= 0x80000004U) {
        char brand[49] = { 0 };
        for (UINT32 leaf = 0; leaf < 3; ++leaf) {
            cpuid(0x80000002U + leaf, 0, &a, &b, &c, &d);
            *(UINT32 *)(brand + leaf * 16 + 0) = a;
            *(UINT32 *)(brand + leaf * 16 + 4) = b;
            *(UINT32 *)(brand + leaf * 16 + 8) = c;
            *(UINT32 *)(brand + leaf * 16 + 12) = d;
        }
        for (int i = 47; i >= 0 && (brand[i] == ' ' || brand[i] == 0); --i)
            brand[i] = 0;
        out(L"  CPU model : ");
        out_ascii(brand);
        outln(L"");
    }
    out(L"  Firmware  : ");
    if (ST->FirmwareVendor) out(ST->FirmwareVendor);
    out(L"  rev ");
    out_int(ST->FirmwareRevision);
    outln(L"");
    UINTN cols = 0, rows = 0;
    ST->ConOut->QueryMode(ST->ConOut, ST->ConOut->Mode->Mode, &cols, &rows);
    out(L"  console   : ");
    out_int(cols);
    out(L"x");
    out_int(rows);
    outln(L"");
    print_memory_summary();
}

static void cmd_mem(CHAR16 *args)
{
    if (!args[0] || weq(args, L"info")) {
        outln(L"Memory");
        print_memory_summary();
        out(L"  WCT boot reserve : ");
        out_int(boot_reserve_mb);
        outln(L" MiB (40% overflow reserve)");
        if (hog_active) {
            out(L"  WCT hold  : ");
            out_int(hog_pages * 4096 / (1024 * 1024));
            outln(L" MiB (mem free to release)");
        }
        outln(L"  use: mem 64   |   mem free");
        return;
    }
    if (weq(args, L"free")) {
        if (!hog_active) {
            outln(L"mem: nothing allocated");
            return;
        }
        ST->BootServices->FreePages(hog_address, hog_pages);
        out(L"released ");
        out_int(hog_pages * 4096 / (1024 * 1024));
        outln(L" MiB");
        hog_active = 0;
        hog_pages = 0;
        hog_address = 0;
        return;
    }

    UINTN mb = 0;
    for (UINTN i = 0; args[i]; ++i) {
        if (args[i] < L'0' || args[i] > L'9') {
            outln(L"mem: usage: mem SIZE_MB | mem free");
            return;
        }
        mb = mb * 10 + (UINTN)(args[i] - L'0');
    }
    if (mb < 1 || mb > 4096) {
        outln(L"mem: size must be 1-4096 MiB");
        return;
    }
    if (hog_active) {
        outln(L"mem: already holding memory; run 'mem free' first");
        return;
    }
    UINTN pages = mb * 256;
    EFI_PHYSICAL_ADDRESS address = 0;
    if (EFI_IS_ERROR(ST->BootServices->AllocatePages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, pages, &address))) {
        outc(theme()->error, L"mem: allocation failed\r\n");
        return;
    }
    volatile UINT8 *p = (volatile UINT8 *)(UINTN)address;
    for (UINTN i = 0; i < pages; ++i)
        p[i * 4096] = 0;
    hog_address = address;
    hog_pages = pages;
    hog_active = 1;
    out(L"holding ");
    out_int(mb);
    outln(L" MiB (mem free to release)");
}

static void read_line(CHAR16 *line, UINTN cap)
{
    UINTN len = 0;
    EFI_INPUT_KEY key;
    line[0] = 0;
    last_control = 0;
    hist_pos = hist_count;
    input_start_col = ST->ConOut->Mode->CursorColumn;
    input_start_row = ST->ConOut->Mode->CursorRow;
    drawn_total = 0;

    for (;;) {
        EFI_STATUS status = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_IS_ERROR(status)) {
            ST->BootServices->Stall(15000);
            continue;
        }
        if (key.ScanCode == 0x01) {
            if (hist_pos > 0) {
                --hist_pos;
                wcopy(line, cap, hist[hist_pos]);
                len = wlen(line);
                redraw_line(line, len, 1);
            }
            continue;
        }
        if (key.ScanCode == 0x02) {
            if (hist_pos + 1 < hist_count) {
                ++hist_pos;
                wcopy(line, cap, hist[hist_pos]);
            } else if (hist_pos < hist_count) {
                hist_pos = hist_count;
                line[0] = 0;
            } else {
                continue;
            }
            len = wlen(line);
            redraw_line(line, len, 1);
            continue;
        }
        if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            redraw_line(line, len, 0);
            out(L"\r\n");
            line[len] = 0;
            if (len > 0) {
                int duplicate = hist_count > 0 && weq(line, hist[hist_count - 1]);
                if (!duplicate) {
                    if (hist_count < HIST_MAX) {
                        wcopy(hist[hist_count], 192, line);
                        ++hist_count;
                    } else {
                        for (int i = 1; i < HIST_MAX; ++i)
                            wcopy(hist[i - 1], 192, hist[i]);
                        wcopy(hist[HIST_MAX - 1], 192, line);
                    }
                }
            }
            hist_pos = hist_count;
            return;
        }
        if (key.UnicodeChar == 9) {
            CHAR16 suffix[192];
            if (completion_suffix(line, suffix, sizeof(suffix) / sizeof(suffix[0]))) {
                UINTN add = wlen(suffix);
                if (len + add + 1 < cap) {
                    for (UINTN i = 0; i < add; ++i) line[len + i] = suffix[i];
                    len += add;
                    line[len] = 0;
                    redraw_line(line, len, 1);
                }
            }
            continue;
        }
        if (key.UnicodeChar == 8) {
            if (len > 0) {
                --len;
                line[len] = 0;
                redraw_line(line, len, 1);
            }
            continue;
        }
        if (key.UnicodeChar == 15 || key.UnicodeChar == 17 ||
            key.UnicodeChar == 19 || key.UnicodeChar == 24) {
            last_control = key.UnicodeChar;
            redraw_line(line, len, 0);
            out(L"\r\n");
            line[0] = 0;
            return;
        }
        if (key.UnicodeChar == 3) {
            redraw_line(line, len, 0);
            out(L"^C\r\n");
            line[0] = 0;
            return;
        }
        if (key.UnicodeChar == 27) {
            line[0] = 0;
            len = 0;
            redraw_line(line, 0, 0);
            out(L"\r\n");
            return;
        }
        if (key.UnicodeChar >= 32 && key.UnicodeChar < 127 && len + 1 < cap) {
            line[len++] = key.UnicodeChar;
            line[len] = 0;
            redraw_line(line, len, 1);
        }
    }
}

static void reset_system(void)
{
    if (ST->RuntimeServices && ST->RuntimeServices->ResetSystem)
        ST->RuntimeServices->ResetSystem(EFI_RESET_COLD, EFI_SUCCESS, 0, NULL);
    for (;;)
        ST->BootServices->Stall(1000000);
}

static void path_for(CHAR16 *outbuf, UINTN cap, const CHAR16 *arg)
{
    outbuf[0] = 0;
    if (!arg[0] || weq(arg, L".")) {
        wcopy(outbuf, cap, CWD);
        return;
    }
    if (arg[0] == L'\\' || arg[0] == L'/') {
        for (UINTN i = 0; arg[i] && i + 1 < cap; ++i)
            outbuf[i] = (arg[i] == L'/') ? L'\\' : arg[i];
        outbuf[wlen(outbuf)] = 0;
        return;
    }
    wcopy(outbuf, cap, CWD);
    if (wlen(outbuf) && outbuf[wlen(outbuf) - 1] != L'\\')
        wcat(outbuf, cap, L"\\");
    CHAR16 temp[128];
    UINTN i = 0;
    while (arg[i] && i + 1 < sizeof(temp) / sizeof(temp[0])) {
        temp[i] = (arg[i] == L'/') ? L'\\' : arg[i];
        ++i;
    }
    temp[i] = 0;
    wcat(outbuf, cap, temp);
}

static void split_command(CHAR16 *line, CHAR16 **command, CHAR16 **args)
{
    while (*line == L' ')
        ++line;
    *command = line;
    while (*line && *line != L' ')
        ++line;
    if (*line) {
        *line++ = 0;
        while (*line == L' ')
            ++line;
    }
    *args = line;
}

static void split_first(CHAR16 *args, CHAR16 **first, CHAR16 **rest)
{
    while (*args == L' ')
        ++args;
    *first = args;
    while (*args && *args != L' ')
        ++args;
    if (*args) {
        *args++ = 0;
        while (*args == L' ')
            ++args;
    }
    *rest = args;
}

static int open_path(const CHAR16 *path, EFI_FILE_PROTOCOL **file, UINT64 mode, UINT64 attr)
{
    *file = NULL;
    if (!ROOT)
        return 0;
    EFI_FILE_PROTOCOL *root = ROOT;
    CHAR16 relative[256];
    const CHAR16 *use_path = path;
    if (resolve_mount(path, &root, relative, sizeof(relative) / sizeof(relative[0])))
        use_path = relative;
    EFI_STATUS status = root->Open(root, file, (CHAR16 *)use_path, mode, attr);
    return !EFI_IS_ERROR(status) && *file;
}

static void scan_volumes(void)
{
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    vol_count = 0;
    vol_boot = -1;
    EFI_STATUS status = ST->BootServices->LocateHandleBuffer(
        2, &simple_fs_guid, NULL, &handle_count, &handles);
    if (EFI_IS_ERROR(status) || !handles)
        return;

    for (UINTN i = 0; i < handle_count && vol_count < MAX_VOLUMES; ++i) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        if (EFI_IS_ERROR(ST->BootServices->HandleProtocol(handles[i], &simple_fs_guid, (VOID **)&fs)) || !fs)
            continue;
        EFI_FILE_PROTOCOL *root = NULL;
        if (EFI_IS_ERROR(fs->OpenVolume(fs, &root)) || !root)
            continue;

        int idx = vol_count++;
        vol_roots[idx] = root;
        vol_sizes[idx] = 0;
        wcopy(vol_labels[idx], 32, L"NO NAME");

        static UINT8 info_buf[1024];
        UINTN info_size = sizeof(info_buf);
        if (!EFI_IS_ERROR(root->GetInfo(root, &fs_info_guid, &info_size, info_buf))) {
            EFI_FILE_SYSTEM_INFO *info = (EFI_FILE_SYSTEM_INFO *)info_buf;
            if (info->VolumeLabel[0])
                wcopy(vol_labels[idx], 32, info->VolumeLabel);
            vol_sizes[idx] = info->VolumeSize / (1024 * 1024);
        }

        if (g_loaded && handles[i] == g_loaded->DeviceHandle)
            vol_boot = idx;
    }
    ST->BootServices->FreePool(handles);

    if (vol_count == 0)
        return;
    if (vol_boot < 0)
        vol_boot = 0;
    vol_current = vol_boot;
    ROOT = vol_roots[vol_boot];
    BOOT_ROOT = ROOT;
}

static int usb_volume_for_number(UINTN number)
{
    int index = 0;
    for (int i = 0; i < vol_count; ++i) {
        if (i == vol_boot) continue;
        if ((UINTN)index == number) return i;
        ++index;
    }
    return -1;
}

static int resolve_mount(const CHAR16 *path, EFI_FILE_PROTOCOL **root, CHAR16 *relative, UINTN cap)
{
    if (!path || path[0] != L'\\' || !vol_count) return 0;
    const CHAR16 *p = path + 1;
    if ((p[0] != L'u' && p[0] != L'U') ||
        (p[1] != L's' && p[1] != L'S') ||
        (p[2] != L'b' && p[2] != L'B'))
        return 0;
    p += 3;
    UINTN number = 0;
    int digits = 0;
    while (*p >= L'0' && *p <= L'9') {
        number = number * 10 + (UINTN)(*p - L'0');
        ++p;
        ++digits;
    }
    if (!digits || (*p && *p != L'\\' && *p != L'/'))
        return 0;
    int vol = usb_volume_for_number(number);
    if (vol < 0) return 0;
    *root = vol_roots[vol];
    if (!*p) {
        wcopy(relative, cap, L"\\");
    } else {
        UINTN n = 0;
        while (p[n] && n + 1 < cap) {
            relative[n] = (p[n] == L'/') ? L'\\' : p[n];
            ++n;
        }
        relative[n] = 0;
    }
    return 1;
}

static void cmd_usb(CHAR16 *args)
{
    if (weq(args, L"rescan")) {
        scan_volumes();
        outln(L"USB volumes rescanned");
        return;
    }
    if (weq(args, L"boot")) {
        wcopy(CWD, sizeof(CWD) / sizeof(CWD[0]), L"\\");
        outln(L"switched to boot root");
        return;
    }
    if (args[0]) {
        UINTN n = 0;
        for (UINTN i = 0; args[i]; ++i) {
            if (args[i] < L'0' || args[i] > L'9') {
                outln(L"usb: usage: usb | usb N | usb boot | usb rescan");
                return;
            }
            n = n * 10 + (UINTN)(args[i] - L'0');
        }
        int vol = usb_volume_for_number(n);
        if (vol < 0) {
            outc(theme()->error, L"usb: mount not found\r\n");
            return;
        }
        wcopy(CWD, sizeof(CWD) / sizeof(CWD[0]), L"\\usb");
        CHAR16 digits[12];
        int d = 0;
        if (n == 0) digits[d++] = L'0';
        while (n > 0 && d < 11) { digits[d++] = (CHAR16)(L'0' + n % 10); n /= 10; }
        UINTN pos = wlen(CWD);
        while (d > 0) CWD[pos++] = digits[--d];
        CWD[pos] = 0;
        out(L"switched to ");
        outln(CWD);
        return;
    }

    outln(L"USB mounts:");
    int index = 0;
    for (int i = 0; i < vol_count; ++i) {
        if (i == vol_boot) continue;
        out(L"  usb");
        out_int(index);
        out(L"/  ->  ");
        out(vol_labels[i]);
        out(L"  ");
        out_int(vol_sizes[i]);
        outln(L" MiB");
        ++index;
    }
    if (index == 0) outln(L"  (none)");
    outln(L"use: cd \\usb0 | usb 0 | usb boot | usb rescan");
}

static const char *native_commands[] = {
    "help", "ls", "cat", "edit", "write", "mkdir", "cd", "pwd", "run", "echo", "cp",
    "sleep", "theme", "font", "banner", "date", "hw", "mem", "log", "ps", "pid",
    "kill", "sudo", "poweroff", "reboot", "linux", "native", "clear", "new", "pkg", "api", "usb",
    "create", "touch", "script", "ver", "about", "color"
};

static int ci_prefix(const CHAR16 *text, const char *prefix)
{
    while (*prefix) {
        CHAR16 a = *text++;
        CHAR16 b = (CHAR16)(unsigned char)*prefix++;
        if (a >= L'A' && a <= L'Z') a = (CHAR16)(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = (CHAR16)(b - L'A' + L'a');
        if (a != b) return 0;
    }
    return 1;
}

static int wi_prefix(const CHAR16 *text, const CHAR16 *prefix, UINTN prefix_len)
{
    for (UINTN i = 0; i < prefix_len; ++i) {
        CHAR16 a = text[i], b = prefix[i];
        if (a >= L'A' && a <= L'Z') a = (CHAR16)(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = (CHAR16)(b - L'A' + L'a');
        if (!a || a != b) return 0;
    }
    return 1;
}

static int completion_suffix(const CHAR16 *line, CHAR16 *suffix, UINTN cap)
{
    UINTN len = wlen(line);
    if (!len || line[len - 1] == L' ') return 0;
    UINTN token_start = len;
    while (token_start > 0 && line[token_start - 1] != L' ') --token_start;
    const CHAR16 *token = line + token_start;
    UINTN token_len = wlen(token);
    suffix[0] = 0;

    if (token_start == 0) {
        for (UINTN i = 0; i < sizeof(native_commands) / sizeof(native_commands[0]); ++i) {
            const char *name = native_commands[i];
            int match = 1;
            for (UINTN j = 0; token[j]; ++j) {
                CHAR16 a = token[j];
                CHAR16 b = (CHAR16)(unsigned char)name[j];
                if (a >= L'A' && a <= L'Z') a = (CHAR16)(a - L'A' + L'a');
                if (b >= L'A' && b <= L'Z') b = (CHAR16)(b - L'A' + L'a');
                if (!name[j] || a != b) { match = 0; break; }
            }
            if (!match) continue;
            UINTN n = 0;
            while (name[n]) ++n;
            if (n <= token_len) continue;
            UINTN k = 0;
            for (UINTN j = token_len; name[j] && k + 1 < cap; ++j)
                suffix[k++] = (CHAR16)(unsigned char)name[j];
            suffix[k] = 0;
            return 1;
        }
        return 0;
    }

    UINTN dir_len = 0;
    for (UINTN i = 0; i < token_len; ++i)
        if (token[i] == L'\\' || token[i] == L'/') dir_len = i + 1;
    const CHAR16 *base = token + dir_len;
    UINTN base_len = wlen(base);
    CHAR16 dir_arg[256];
    for (UINTN i = 0; i < dir_len && i + 1 < sizeof(dir_arg) / sizeof(dir_arg[0]); ++i)
        dir_arg[i] = token[i];
    dir_arg[dir_len] = 0;
    CHAR16 dir_path[256];
    path_for(dir_path, sizeof(dir_path) / sizeof(dir_path[0]), dir_arg);
    EFI_FILE_PROTOCOL *dir;
    if (!open_path(dir_path, &dir, EFI_FILE_MODE_READ, 0))
        return 0;

    static UINT8 buffer[2048];
    int found = 0;
    for (;;) {
        UINTN size = sizeof(buffer);
        if (EFI_IS_ERROR(dir->Read(dir, &size, buffer)) || size == 0)
            break;
        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buffer;
        if (!info->FileName[0]) break;
        if (weq(info->FileName, L".") || weq(info->FileName, L"..")) continue;
        if (!wi_prefix(info->FileName, base, base_len)) continue;
        UINTN name_len = wlen(info->FileName);
        if (name_len < base_len) continue;
        UINTN k = 0;
        for (UINTN i = base_len; i < name_len && k + 1 < cap; ++i)
            suffix[k++] = info->FileName[i];
        if ((info->Attribute & EFI_FILE_DIRECTORY) && k + 1 < cap)
            suffix[k++] = L'\\';
        suffix[k] = 0;
        found = 1;
        break;
    }
    dir->Close(dir);
    return found;
}

static void remove_path(const CHAR16 *path)
{
    EFI_FILE_PROTOCOL *file;
    if (open_path(path, &file, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))
        file->Delete(file);
}

static int write_bytes(const CHAR16 *path, const char *data, UINTN size)
{
    remove_path(path);
    EFI_FILE_PROTOCOL *file;
    if (!open_path(path, &file, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ, 0))
        return 0;
    UINTN remaining = size;
    char *p = (char *)data;
    while (remaining) {
        UINTN chunk = remaining;
        if (EFI_IS_ERROR(file->Write(file, &chunk, p)) || chunk == 0)
            break;
        p += chunk;
        remaining -= chunk;
    }
    file->Close(file);
    return remaining == 0;
}

static void make_log_path(char *out, UINTN cap, const char *date, int number)
{
    int n = 0;
    const char *prefix = "\\WCT-";
    for (int i = 0; prefix[i] && n + 1 < cap; ++i) out[n++] = prefix[i];
    for (int i = 0; date[i] && n + 1 < cap; ++i) out[n++] = date[i];
    if (n + 1 < cap) out[n++] = '-';
    char digits[12];
    int d = 0;
    if (number <= 0) number = 1;
    while (number > 0 && d < 11) { digits[d++] = (char)('0' + number % 10); number /= 10; }
    while (d > 0 && n + 1 < cap) out[n++] = digits[--d];
    const char *suffix = ".log";
    for (int i = 0; suffix[i] && n + 1 < cap; ++i) out[n++] = suffix[i];
    out[n] = 0;
}

static int log_save(int quiet)
{
    EFI_TIME now;
    char date[11] = "0000-00-00";
    char clock[9] = "00:00:00";
    if (ST->RuntimeServices && ST->RuntimeServices->GetTime &&
        !EFI_IS_ERROR(ST->RuntimeServices->GetTime(&now, NULL))) {
        date[0] = (char)('0' + now.Year / 1000);
        date[1] = (char)('0' + (now.Year / 100) % 10);
        date[2] = (char)('0' + (now.Year / 10) % 10);
        date[3] = (char)('0' + now.Year % 10);
        date[5] = (char)('0' + now.Month / 10);
        date[6] = (char)('0' + now.Month % 10);
        date[8] = (char)('0' + now.Day / 10);
        date[9] = (char)('0' + now.Day % 10);
        clock[0] = (char)('0' + now.Hour / 10);
        clock[1] = (char)('0' + now.Hour % 10);
        clock[3] = (char)('0' + now.Minute / 10);
        clock[4] = (char)('0' + now.Minute % 10);
        clock[6] = (char)('0' + now.Second / 10);
        clock[7] = (char)('0' + now.Second % 10);
    }

    int same_date = 1;
    for (int i = 0; i < 10; ++i) if (log_file_date[i] != date[i]) { same_date = 0; break; }
    if (log_file_number == 0 || !same_date) {
        log_file_number = 1;
        for (int candidate = 1; candidate < 999; ++candidate) {
            char test_ascii[48];
            make_log_path(test_ascii, sizeof(test_ascii), date, candidate);
            CHAR16 test_path[64];
            int ti = 0;
            while (test_ascii[ti] && ti < 63) { test_path[ti] = (CHAR16)(unsigned char)test_ascii[ti]; ++ti; }
            test_path[ti] = 0;
            EFI_FILE_PROTOCOL *test = NULL;
            if (!open_path(test_path, &test, EFI_FILE_MODE_READ, 0)) {
                log_file_number = candidate;
                break;
            }
            test->Close(test);
        }
        for (int i = 0; i < 11; ++i) log_file_date[i] = date[i];
    }

    char path_ascii[48];
    make_log_path(path_ascii, sizeof(path_ascii), date, log_file_number);
    int pn = 0;
    while (path_ascii[pn]) ++pn;

    CHAR16 path[64];
    for (int i = 0; i <= pn && i < 63; ++i) path[i] = (CHAR16)(unsigned char)path_ascii[i];

    static char buffer[32768];
    int n = 0;
    const char header[] = "WCT-OS LTS 1.0_ET native log\r\n";
    for (int i = 0; header[i] && n < (int)sizeof(buffer) - 1; ++i) buffer[n++] = header[i];
    const char stamp[] = "date: ";
    for (int i = 0; stamp[i] && n < (int)sizeof(buffer) - 1; ++i) buffer[n++] = stamp[i];
    for (int i = 0; date[i] && n < (int)sizeof(buffer) - 1; ++i) buffer[n++] = date[i];
    buffer[n++] = ' ';
    for (int i = 0; clock[i] && n < (int)sizeof(buffer) - 1; ++i) buffer[n++] = clock[i];
    buffer[n++] = '\r'; buffer[n++] = '\n';
    buffer[n++] = '\r'; buffer[n++] = '\n';

    for (int i = 0; i < log_count; ++i) {
        const char *lv = log_entries[i].level == LOG_ERROR ? "ERROR" :
                         log_entries[i].level == LOG_WARN ? "WARN" : "INFO";
        buffer[n++] = '[';
        for (int j = 0; log_entries[i].time[j] && n < (int)sizeof(buffer) - 1; ++j) buffer[n++] = log_entries[i].time[j];
        buffer[n++] = ']'; buffer[n++] = ' ';
        buffer[n++] = '['; 
        for (int j = 0; lv[j] && n < (int)sizeof(buffer) - 1; ++j) buffer[n++] = lv[j];
        buffer[n++] = ']'; buffer[n++] = ' ';
        buffer[n++] = '[';
        for (int j = 0; log_entries[i].tag[j] && n < (int)sizeof(buffer) - 1; ++j) buffer[n++] = log_entries[i].tag[j];
        buffer[n++] = ']'; buffer[n++] = ' ';
        for (int j = 0; log_entries[i].text[j] && n < (int)sizeof(buffer) - 1; ++j) buffer[n++] = log_entries[i].text[j];
        buffer[n++] = '\r'; buffer[n++] = '\n';
    }
    buffer[n] = 0;

    EFI_FILE_PROTOCOL *saved_root = ROOT;
    if (BOOT_ROOT) ROOT = BOOT_ROOT;
    int ok = write_bytes(path, buffer, n);
    ROOT = saved_root;
    if (!quiet) {
        if (ok) {
            out(L"saved: ");
            outln(path);
        } else {
            outc(theme()->error, L"log save failed\r\n");
        }
    }
    return ok;
}

static void normalize_path(const CHAR16 *arg, CHAR16 *out, UINTN cap)
{
    CHAR16 combined[256];
    combined[0] = 0;
    if (arg[0] == L'\\' || arg[0] == L'/') {
        wcopy(combined, sizeof(combined) / sizeof(combined[0]), arg);
    } else {
        wcopy(combined, sizeof(combined) / sizeof(combined[0]), CWD);
        if (wlen(combined) && combined[wlen(combined) - 1] != L'\\')
            wcat(combined, sizeof(combined) / sizeof(combined[0]), L"\\");
        wcat(combined, sizeof(combined) / sizeof(combined[0]), arg);
    }

    CHAR16 parts[24][64];
    int count = 0;
    UINTN i = 0;
    while (combined[i]) {
        while (combined[i] == L'\\' || combined[i] == L'/')
            ++i;
        if (!combined[i])
            break;
        CHAR16 token[64];
        UINTN n = 0;
        while (combined[i] && combined[i] != L'\\' && combined[i] != L'/' && n + 1 < 64)
            token[n++] = combined[i++];
        token[n] = 0;
        if (weq(token, L".") || !token[0])
            continue;
        if (weq(token, L"..")) {
            if (count > 0) --count;
            continue;
        }
        if (count < 24) {
            wcopy(parts[count], 64, token);
            ++count;
        }
    }

    out[0] = L'\\';
    UINTN pos = 1;
    for (int p = 0; p < count; ++p) {
        UINTN n = wlen(parts[p]);
        if (pos + n + 2 >= cap)
            break;
        for (UINTN k = 0; k < n; ++k)
            out[pos++] = parts[p][k];
        if (p + 1 < count)
            out[pos++] = L'\\';
    }
    out[pos] = 0;
}

static void cmd_cd(CHAR16 *args)
{
    CHAR16 target[256];
    if (!args[0])
        wcopy(target, sizeof(target) / sizeof(target[0]), L"\\");
    else
        normalize_path(args, target, sizeof(target) / sizeof(target[0]));

    EFI_FILE_PROTOCOL *dir;
    if (!open_path(target, &dir, EFI_FILE_MODE_READ, 0)) {
        outc(theme()->error, L"cd: no such directory\r\n");
        return;
    }
    dir->Close(dir);
    wcopy(CWD, sizeof(CWD) / sizeof(CWD[0]), target);
}

static void cmd_pwd(void)
{
    outln(CWD);
}

static UINTN read_whole_file(const CHAR16 *path, UINT8 *buffer, UINTN cap)
{
    EFI_FILE_PROTOCOL *file;
    if (!open_path(path, &file, EFI_FILE_MODE_READ, 0))
        return 0;
    UINTN total = 0;
    for (;;) {
        UINTN chunk = cap - total;
        if (chunk == 0)
            break;
        EFI_STATUS status = file->Read(file, &chunk, buffer + total);
        if (EFI_IS_ERROR(status) || chunk == 0)
            break;
        total += chunk;
    }
    file->Close(file);
    return total;
}

static int finish_device_path(UINTN prefix)
{
    CHAR16 *target = L"\\wct\\bootx64.efi";
    UINTN node_len = 4 + (wlen(target) + 1) * 2;
    if (prefix + node_len + 4 > sizeof(boot_device_path))
        return 0;

    UINT8 *node = boot_device_path + prefix;
    node[0] = 0x04;
    node[1] = 0x04;
    node[2] = (UINT8)(node_len & 0xFF);
    node[3] = (UINT8)(node_len >> 8);
    CHAR16 *text = (CHAR16 *)(node + 4);
    wcopy(text, (node_len - 4) / 2, target);

    UINT8 *end = boot_device_path + prefix + node_len;
    end[0] = 0x7F;
    end[1] = 0xFF;
    end[2] = 0x04;
    end[3] = 0x00;
    return 1;
}

static int build_linux_device_path(void)
{
    if (!g_loaded)
        return 0;

    EFI_DEVICE_PATH_PROTOCOL *volume_path = NULL;
    if (ST->BootServices->HandleProtocol(g_loaded->DeviceHandle, &device_path_guid, (VOID **)&volume_path) == EFI_SUCCESS &&
        volume_path) {
        UINT8 *src = (UINT8 *)volume_path;
        UINTN offset = 0;
        for (;;) {
            UINT8 type = src[offset];
            UINT8 subtype = src[offset + 1];
            UINT16 length = (UINT16)(src[offset + 2] | (src[offset + 3] << 8));
            if (length < 4 || offset > 900)
                return 0;
            if (type == 0x7F && subtype == 0xFF)
                break;
            offset += length;
        }
        if (offset == 0 || offset + 4 > sizeof(boot_device_path))
            return 0;
        for (UINTN i = 0; i < offset; ++i)
            boot_device_path[i] = src[i];
        return finish_device_path(offset);
    }

    if (!g_loaded->FilePath)
        return 0;
    UINT8 *src = (UINT8 *)g_loaded->FilePath;
    UINTN offset = 0;
    UINTN file_offset = 0;
    int found_file = 0;
    for (;;) {
        UINT8 type = src[offset];
        UINT8 subtype = src[offset + 1];
        UINT16 length = (UINT16)(src[offset + 2] | (src[offset + 3] << 8));
        if (length < 4 || offset > 900)
            return 0;
        if (type == 0x7F && subtype == 0xFF)
            break;
        if (type == 0x04 && subtype == 0x04) {
            file_offset = offset;
            found_file = 1;
        }
        offset += length;
    }
    if (!found_file)
        return 0;
    for (UINTN i = 0; i < file_offset; ++i)
        boot_device_path[i] = src[i];
    return finish_device_path(file_offset);
}

static void cmd_ls(CHAR16 *args)
{
    CHAR16 path[256];
    path_for(path, sizeof(path) / sizeof(path[0]), args);
    EFI_FILE_PROTOCOL *dir;
    if (!open_path(path, &dir, EFI_FILE_MODE_READ, 0)) {
        outln(L"ls: not found");
        return;
    }

    static UINT8 buffer[2048];
    outc(theme()->accent, L"Directory ");
    outln(path);
    for (;;) {
        UINTN size = sizeof(buffer);
        EFI_STATUS status = dir->Read(dir, &size, buffer);
        if (EFI_IS_ERROR(status) || size == 0)
            break;
        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buffer;
        if (info->FileName[0] == 0)
            break;
        if (weq(info->FileName, L".") || weq(info->FileName, L".."))
            continue;
        out(L"  ");
        if (info->Attribute & EFI_FILE_DIRECTORY) {
            outc(theme()->dir, info->FileName);
            outc(theme()->dir, L"/");
        } else {
            outc(theme()->text, info->FileName);
            out(L"  ");
            set_color(theme()->dim);
            out_int(info->FileSize);
            out(L" bytes");
            reset_color();
        }
        outln(L"");
    }
    if (weq(path, L"\\")) {
        int index = 0;
        for (int i = 0; i < vol_count; ++i) {
            if (i == vol_boot) continue;
            out(L"  ");
            outc(theme()->dir, L"usb");
            out_int(index);
            outc(theme()->dir, L"/");
            out(L"  ");
            set_color(theme()->dim);
            out(vol_labels[i]);
            reset_color();
            outln(L"");
            ++index;
        }
    }
    dir->Close(dir);
}

static void cmd_cat(CHAR16 *args)
{
    if (!args[0]) {
        outln(L"cat: missing file");
        return;
    }
    CHAR16 path[256];
    path_for(path, sizeof(path) / sizeof(path[0]), args);
    EFI_FILE_PROTOCOL *file;
    if (!open_path(path, &file, EFI_FILE_MODE_READ, 0)) {
        outln(L"cat: not found");
        return;
    }
    char buffer[512];
    for (;;) {
        UINTN size = sizeof(buffer);
        EFI_STATUS status = file->Read(file, &size, buffer);
        if (EFI_IS_ERROR(status) || size == 0)
            break;
        for (UINTN i = 0; i < size; ++i) {
            unsigned char c = (unsigned char)buffer[i];
            if (c == '\n')
                out(L"\r\n");
            else if (c == '\r')
                continue;
            else if (c >= 32 && c < 127) {
                CHAR16 s[2] = { (CHAR16)c, 0 };
                out(s);
            } else if (c >= 128) {
                out(L"?");
            }
        }
    }
    file->Close(file);
    out(L"\r\n");
}

static void cmd_write(CHAR16 *args)
{
    CHAR16 *file_arg, *text;
    split_first(args, &file_arg, &text);
    if (!file_arg[0]) {
        outln(L"write: usage: write FILE TEXT");
        return;
    }
    CHAR16 path[256];
    path_for(path, sizeof(path) / sizeof(path[0]), file_arg);
    char data[256];
    UINTN n = 0;
    for (UINTN i = 0; text[i] && n + 1 < sizeof(data); ++i)
        data[n++] = (text[i] >= 32 && text[i] < 127) ? (char)text[i] : '?';
    data[n++] = '\n';
    if (write_bytes(path, data, n)) {
        out(L"wrote ");
        outln(path);
    } else {
        outln(L"write: failed");
    }
}

static void cmd_mkdir(CHAR16 *args)
{
    if (!args[0]) {
        outln(L"mkdir: missing directory");
        return;
    }
    CHAR16 path[256];
    path_for(path, sizeof(path) / sizeof(path[0]), args);
    EFI_FILE_PROTOCOL *dir;
    if (open_path(path, &dir, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ, EFI_FILE_DIRECTORY)) {
        dir->Close(dir);
        out(L"created ");
        outln(path);
    } else {
        outln(L"mkdir: failed");
    }
}

static void cmd_rm(CHAR16 *args)
{
    if (!args[0]) {
        outln(L"rm: missing file");
        return;
    }
    CHAR16 path[256];
    path_for(path, sizeof(path) / sizeof(path[0]), args);
    EFI_FILE_PROTOCOL *file;
    if (open_path(path, &file, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) {
        file->Delete(file);
        out(L"removed ");
        outln(path);
    } else {
        outln(L"rm: not found");
    }
}

static void cmd_cp(CHAR16 *args)
{
    CHAR16 *src_arg, *dst_arg;
    split_first(args, &src_arg, &dst_arg);
    if (!src_arg[0] || !dst_arg[0]) {
        outln(L"cp: usage: cp SOURCE DEST");
        return;
    }
    CHAR16 src[256], dst[256];
    path_for(src, sizeof(src) / sizeof(src[0]), src_arg);
    path_for(dst, sizeof(dst) / sizeof(dst[0]), dst_arg);

    EFI_FILE_PROTOCOL *in;
    if (!open_path(src, &in, EFI_FILE_MODE_READ, 0)) {
        outc(theme()->error, L"cp: source not found\r\n");
        return;
    }
    remove_path(dst);
    EFI_FILE_PROTOCOL *dst_file;
    if (!open_path(dst, &dst_file, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ, 0)) {
        in->Close(in);
        outc(theme()->error, L"cp: cannot create destination\r\n");
        return;
    }

    static UINT8 buffer[65536];
    int ok = 1;
    for (;;) {
        UINTN size = sizeof(buffer);
        if (EFI_IS_ERROR(in->Read(in, &size, buffer)) || size == 0)
            break;
        UINTN offset = 0;
        while (offset < size) {
            UINTN chunk = size - offset;
            if (EFI_IS_ERROR(dst_file->Write(dst_file, &chunk, buffer + offset)) || chunk == 0) {
                ok = 0;
                break;
            }
            offset += chunk;
        }
        if (!ok) break;
    }
    dst_file->Close(dst_file);
    in->Close(in);
    if (ok) {
        out(L"copied ");
        out(src);
        out(L" -> ");
        outln(dst);
    } else {
        outc(theme()->error, L"cp: write failed\r\n");
    }
}

static void cmd_date(void)
{
    EFI_TIME t;
    if (ST->RuntimeServices && ST->RuntimeServices->GetTime &&
        !EFI_IS_ERROR(ST->RuntimeServices->GetTime(&t, NULL))) {
        out_int(t.Year);
        out(L"-");
        if (t.Month < 10) out(L"0");
        out_int(t.Month);
        out(L"-");
        if (t.Day < 10) out(L"0");
        out_int(t.Day);
        out(L" ");
        if (t.Hour < 10) out(L"0");
        out_int(t.Hour);
        out(L":");
        if (t.Minute < 10) out(L"0");
        out_int(t.Minute);
        out(L":");
        if (t.Second < 10) out(L"0");
        out_int(t.Second);
        out(L" UTC\r\n");
    } else {
        outln(L"date: unavailable");
    }
}

static void cmd_linux(void)
{
    if (!linux_available) {
        outc(theme()->error, L"This boot mode has no Linux subsystem.\r\n");
        return;
    }
    outln(L"Starting WCT-OS Linux compatibility subsystem...");
    outln(L"CPython 3.11.2 / nano / zhcon will be available there.");
    if (!build_linux_device_path()) {
        outln(L"linux: could not construct the embedded loader path.");
        return;
    }
    if (!ST->BootServices->LoadImage || !ST->BootServices->StartImage) {
        outln(L"linux: UEFI image services unavailable.");
        return;
    }

    EFI_HANDLE child = NULL;
    EFI_STATUS status = ST->BootServices->LoadImage(
        FALSE, g_image, boot_device_path, NULL, 0, &child);
    if (EFI_IS_ERROR(status) || !child) {
        outln(L"linux: could not load embedded Linux loader.");
        return;
    }

    UINTN exit_size = 0;
    CHAR16 *exit_data = NULL;
    status = ST->BootServices->StartImage(child, &exit_size, &exit_data);
    if (EFI_IS_ERROR(status)) {
        outln(L"linux: Linux subsystem returned an error.");
        if (exit_data)
            outln(exit_data);
    } else {
        outln(L"Linux subsystem returned to the native shell.");
    }
}

static void cmd_native(void)
{
    outln(L"WCT-OS Native is the primary boot environment.");
}

typedef struct {
    char name[32];
    char version[16];
    char author[48];
    char command[32];
    char description[96];
    char color[16];
} PkgMeta;

static void pkg_meta(const CHAR16 *file, PkgMeta *meta)
{
    for (UINTN i = 0; i < sizeof(*meta); ++i) ((UINT8 *)meta)[i] = 0;
    static UINT8 data[4096];
    UINTN size = read_whole_file(file, data, sizeof(data) - 1);
    if (!size) return;
    data[size] = 0;
    char *p = (char *)data;
    while (*p) {
        char line[256];
        UINTN n = 0;
        while (*p && *p != '\n' && n + 1 < sizeof(line)) line[n++] = *p++;
        if (*p == '\n') ++p;
        line[n] = 0;
        char *s = strim(line);
        if (s[0] != '#') continue;
        ++s;
        while (*s == ' ' || *s == '\t') ++s;
        char *colon = s;
        while (*colon && *colon != ':') ++colon;
        if (*colon != ':') continue;
        *colon = 0;
        char *key = strim(s);
        char *value = strim(colon + 1);
        if (!scmp(key, "name")) scopy(meta->name, sizeof(meta->name), value);
        else if (!scmp(key, "version")) scopy(meta->version, sizeof(meta->version), value);
        else if (!scmp(key, "author")) scopy(meta->author, sizeof(meta->author), value);
        else if (!scmp(key, "command")) scopy(meta->command, sizeof(meta->command), value);
        else if (!scmp(key, "description") || !scmp(key, "desc")) scopy(meta->description, sizeof(meta->description), value);
        else if (!scmp(key, "color")) scopy(meta->color, sizeof(meta->color), value);
    }
}

static void package_command_name(const CHAR16 *file, CHAR16 *out, UINTN cap)
{
    PkgMeta meta;
    pkg_meta(file, &meta);
    if (meta.command[0]) {
        UINTN i = 0;
        while (meta.command[i] && i + 1 < cap) { out[i] = (CHAR16)(unsigned char)meta.command[i]; ++i; }
        out[i] = 0;
        return;
    }
    wcopy(out, cap, file);
    UINTN len = wlen(out);
    if (len >= 7 && weq(out + len - 7, L".wctpkg")) out[len - 7] = 0;
    else if (len >= 4 && weq(out + len - 4, L".wct")) out[len - 4] = 0;
}

static int list_package_commands(void)
{
    EFI_FILE_PROTOCOL *dir;
    if (!open_path(L"\\packages", &dir, EFI_FILE_MODE_READ, 0)) return 0;
    static UINT8 buf[2048];
    int count = 0;
    for (;;) {
        UINTN size = sizeof(buf);
        if (EFI_IS_ERROR(dir->Read(dir, &size, buf)) || size == 0) break;
        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        if (!info->FileName[0]) break;
        if (weq(info->FileName, L".") || weq(info->FileName, L"..")) continue;
        CHAR16 path[256];
        wcopy(path, sizeof(path) / sizeof(path[0]), L"\\packages\\");
        wcat(path, sizeof(path) / sizeof(path[0]), info->FileName);
        CHAR16 name[128];
        package_command_name(path, name, sizeof(name) / sizeof(name[0]));
        PkgMeta meta;
        pkg_meta(path, &meta);
        outc(theme()->prompt, name);
        if (meta.version[0]) {
            out(L"  v");
            out_ascii(meta.version);
        }
        if (meta.description[0]) {
            out(L"  ");
            set_color(theme()->dim);
            out_ascii(meta.description);
            reset_color();
        }
        outln(L"");
        ++count;
    }
    dir->Close(dir);
    return count;
}

static void cmd_help(void)
{
    outc(theme()->accent, L"WCT-OS Native");
    outln(L"  commands");
    outln(L"  ----------------------------------------------------------");
    outc(theme()->prompt, L"  help");
    outln(L"              this command list");
    outc(theme()->prompt, L"  ls [PATH]");
    outln(L"         list FAT32 directory");
    outc(theme()->prompt, L"  cat FILE");
    outln(L"          show a text file");
    outc(theme()->prompt, L"  edit FILE");
    outln(L"         terminal editor (creates if missing)");
    outc(theme()->prompt, L"  new FILE");
    outln(L"          create an empty file");
    outc(theme()->prompt, L"  write FILE TEXT");
    outln(L"   create or overwrite a file");
    outc(theme()->prompt, L"  rm FILE");
    outln(L"           delete a file");
    outc(theme()->prompt, L"  cp SRC DST");
    outln(L"        copy a file");
    outc(theme()->prompt, L"  mkdir DIR");
    outln(L"         create a directory");
    outc(theme()->prompt, L"  cd [DIR]");
    outln(L"          change directory");
    outc(theme()->prompt, L"  pwd");
    outln(L"               show current directory");
    outc(theme()->prompt, L"  run FILE.wct");
    outln(L"      run a WCT script");
    outc(theme()->prompt, L"  echo TEXT");
    outln(L"         print text");
    outc(theme()->prompt, L"  sleep N");
    outln(L"           pause 1-5 seconds");
    outc(theme()->prompt, L"  theme [NAME]");
    outln(L"      switch colour theme");
    outc(theme()->prompt, L"  font [N|next]");
    outln(L"     switch console font/mode");
    outc(theme()->prompt, L"  banner");
    outln(L"            redraw header");
    outc(theme()->prompt, L"  date");
    outln(L"              UEFI clock");
    outc(theme()->prompt, L"  hw");
    outln(L"                hardware information");
    outc(theme()->prompt, L"  mem [SIZE|free]");
    outln(L"   hold RAM or show memory");
    outc(theme()->prompt, L"  ver");
    outln(L"               native system version");
    outc(theme()->prompt, L"  about");
    outln(L"             author and kernel version");
    outc(theme()->prompt, L"  color NAME");
    outln(L"        set terminal text colour");
    outc(theme()->prompt, L"  log [save|clear|LEVEL TEXT]");
    outln(L"  boot and command log");
    outc(theme()->prompt, L"  ps / pid / kill PID");
    outln(L"  process manager");
    outc(theme()->prompt, L"  sudo kill PID");
    outln(L"   force-kill protected process");
    outc(theme()->prompt, L"  usb [N|boot|rescan]");
    outln(L"  list and switch storage volumes");
    outc(theme()->prompt, L"  pkg / api");
    outln(L"           packages and system API");
    outc(theme()->prompt, L"  poweroff");
    outln(L"          shut down the machine");
    outc(theme()->prompt, L"  clear");
    outln(L"             clear screen");
    outc(theme()->prompt, L"  linux");
    outln(L"             start embedded Linux subsystem");
    outc(theme()->prompt, L"  native");
    outln(L"            show primary boot status");
    outc(theme()->prompt, L"  reboot");
    outln(L"            reboot the machine");
    outln(L"  ----------------------------------------------------------");
    outln(L"  Shared FAT32: root is \\, Linux sees it as /fat");
    outln(L"  Themes: ubuntu  wct  matrix  amber  ice");
    outln(L"  Package commands:");
    if (!list_package_commands()) outln(L"  (none installed)");
}

static void cmd_ver(void)
{
    outln(L"WCT-OS Native Shell 1.0_ET");
    outln(L"Freestanding C  /  x86_64 UEFI  /  FAT32 shared volume");
    outln(L"Compatibility subsystem: WCT-OS Linux (CPython 3.11.2)");
}

static void cmd_color(CHAR16 *args)
{
    if (!args[0] || weq(args, L"reset") || weq(args, L"default")) {
        output_color_override = 0;
        reset_color();
        outln(L"color reset");
        return;
    }
    UINTN attr = 0;
    int found = 1;
    if (weq(args, L"black")) attr = 0x00;
    else if (weq(args, L"blue")) attr = 0x01;
    else if (weq(args, L"green")) attr = 0x02;
    else if (weq(args, L"cyan")) attr = 0x03;
    else if (weq(args, L"red")) attr = 0x04;
    else if (weq(args, L"magenta")) attr = 0x05;
    else if (weq(args, L"brown")) attr = 0x06;
    else if (weq(args, L"gray") || weq(args, L"lightgray")) attr = 0x07;
    else if (weq(args, L"darkgray")) attr = 0x08;
    else if (weq(args, L"lightblue")) attr = 0x09;
    else if (weq(args, L"lightgreen")) attr = 0x0A;
    else if (weq(args, L"lightcyan")) attr = 0x0B;
    else if (weq(args, L"lightred")) attr = 0x0C;
    else if (weq(args, L"lightmagenta") || weq(args, L"purple")) attr = 0x0D;
    else if (weq(args, L"yellow")) attr = 0x0E;
    else if (weq(args, L"white")) attr = 0x0F;
    else found = 0;
    if (!found) {
        outln(L"color: black blue green cyan red magenta brown gray darkgray lightblue lightgreen lightcyan lightred purple yellow white");
        return;
    }
    output_color_override = attr;
    out(L"color set: ");
    outln(args);
}

static void cmd_about(void)
{
    outc(theme()->accent, L"WCT-OS LTS 1.0");
    outln(L"");
    out(L"  Author : ");
    outc(theme()->prompt, L"aqtwWCT95");
    outln(L"");
    out(L"  Kernel : ");
    outc(theme()->bright, L"4.0.441");
    outln(L"");
    outln(L"  Native C UEFI shell / FAT32 / package API");
    outln(L"  Linux compatibility subsystem embedded");
}

static UINTN slen(const char *s)
{
    UINTN n = 0;
    while (s && s[n]) ++n;
    return n;
}

static void scopy(char *dst, UINTN cap, const char *src)
{
    UINTN i = 0;
    if (!cap) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static int sprefix(const char *text, const char *prefix)
{
    while (*prefix) {
        if (*text++ != *prefix++)
            return 0;
    }
    return 1;
}

static char *strim(char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r')
        ++text;
    char *end = text + slen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        --end;
    *end = 0;
    return text;
}

static int script_var_find(const char *name)
{
    for (int i = 0; i < script_var_count; ++i) {
        int same = 1;
        for (int j = 0;; ++j) {
            if (script_var_names[i][j] != name[j]) { same = 0; break; }
            if (!name[j]) break;
        }
        if (same) return i;
    }
    return -1;
}

static void script_var_set(const char *name, const char *value)
{
    int idx = script_var_find(name);
    if (idx < 0) {
        if (script_var_count >= SCRIPT_VARS)
            return;
        idx = script_var_count++;
    }
    scopy(script_var_names[idx], sizeof(script_var_names[idx]), name);
    scopy(script_var_values[idx], sizeof(script_var_values[idx]), value);
}

static void script_expand(const char *in, char *out, UINTN cap)
{
    UINTN pos = 0;
    while (*in && pos + 1 < cap) {
        if (*in == '$') {
            const char *p = in + 1;
            char name[24];
            UINTN n = 0;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                   (*p >= '0' && *p <= '9') || *p == '_') {
                if (n + 1 < sizeof(name)) name[n++] = *p;
                ++p;
            }
            name[n] = 0;
            int idx = n ? script_var_find(name) : -1;
            if (idx >= 0) {
                const char *v = script_var_values[idx];
                while (*v && pos + 1 < cap) out[pos++] = *v++;
                in = p;
                continue;
            }
        }
        out[pos++] = *in++;
    }
    out[pos] = 0;
}

static void wide_from_ascii(const char *src, CHAR16 *dst, UINTN cap)
{
    UINTN i = 0;
    if (!cap) return;
    while (src[i] && i + 1 < cap) {
        dst[i] = (src[i] == '\n' || src[i] == '\r') ? L' ' : (CHAR16)(unsigned char)src[i];
        ++i;
    }
    dst[i] = 0;
}

static int script_depth;

static int scmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int script_is_num(const char *s)
{
    if (*s == '-' || *s == '+') ++s;
    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        ++s;
    }
    return 1;
}

static int snum(const char *s)
{
    int sign = 1, n = 0;
    if (*s == '-') { sign = -1; ++s; }
    else if (*s == '+') ++s;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n * sign;
}

static void script_var_add(const char *name, int delta)
{
    int idx = script_var_find(name);
    if (idx < 0) return;
    int value = snum(script_var_values[idx]) + delta;
    char buf[32];
    int n = 0;
    if (value < 0) { buf[n++] = '-'; value = -value; }
    char digits[16];
    int d = 0;
    if (value == 0) digits[d++] = '0';
    while (value > 0 && d < 16) { digits[d++] = (char)('0' + value % 10); value /= 10; }
    while (d > 0) buf[n++] = digits[--d];
    buf[n] = 0;
    script_var_set(name, buf);
}

static int script_condition(const char *condition)
{
    char expanded[512];
    script_expand(condition, expanded, sizeof(expanded));
    char *s = strim(expanded);
    if (!s[0]) return 0;
    if (!scmp(s, "true") || !scmp(s, "1") || !scmp(s, "yes")) return 1;
    if (!scmp(s, "false") || !scmp(s, "0") || !scmp(s, "no")) return 0;

    if (sprefix(s, "exists ")) {
        CHAR16 path[256];
        CHAR16 arg[256];
        wide_from_ascii(strim(s + 7), arg, sizeof(arg) / sizeof(arg[0]));
        path_for(path, sizeof(path) / sizeof(path[0]), arg);
        EFI_FILE_PROTOCOL *file;
        if (open_path(path, &file, EFI_FILE_MODE_READ, 0)) {
            file->Close(file);
            return 1;
        }
        return 0;
    }

    char *op = NULL;
    const char *ops[] = { "==", "!=", "<=", ">=", "<", ">" };
    for (int i = 0; i < 6; ++i) {
        op = NULL;
        for (char *p = s; *p; ++p) {
            if (sprefix(p, ops[i])) { op = p; break; }
        }
        if (op) break;
    }
    if (!op) return snum(s) != 0;

    char left[256], right[256];
    UINTN left_len = (UINTN)(op - s);
    if (left_len >= sizeof(left)) left_len = sizeof(left) - 1;
    for (UINTN i = 0; i < left_len; ++i) left[i] = s[i];
    left[left_len] = 0;
    UINTN oplen = (op[1] == '=') ? 2 : 1;
    scopy(right, sizeof(right), op + oplen);
    char *l = strim(left);
    char *r = strim(right);

    if (script_is_num(l) && script_is_num(r)) {
        int a = snum(l), b = snum(r);
        if (op[0] == '=' && op[1] == '=') return a == b;
        if (op[0] == '!' && op[1] == '=') return a != b;
        if (op[0] == '<' && op[1] == '=') return a <= b;
        if (op[0] == '>' && op[1] == '=') return a >= b;
        if (op[0] == '<') return a < b;
        if (op[0] == '>') return a > b;
    } else {
        int c = scmp(l, r);
        if (op[0] == '=' && op[1] == '=') return c == 0;
        if (op[0] == '!' && op[1] == '=') return c != 0;
        if (op[0] == '<') return c < 0;
        if (op[0] == '>') return c > 0;
    }
    return 0;
}

static void script_exec_line(char *line)
{
    if (sprefix(line, "set ")) {
        char *rest = strim(line + 4);
        char *value = rest;
        while (*value && *value != ' ') ++value;
        if (*value) *value++ = 0;
        script_var_set(strim(rest), strim(value));
        return;
    }
    if (sprefix(line, "inc ")) {
        char *rest = strim(line + 4);
        char *name = rest;
        while (*rest && *rest != ' ') ++rest;
        if (*rest) *rest++ = 0;
        script_var_add(strim(name), *rest ? snum(strim(rest)) : 1);
        return;
    }
    if (sprefix(line, "echo ")) {
        char expanded[512];
        script_expand(line + 5, expanded, sizeof(expanded));
        out_ascii(expanded);
        outln(L"");
        return;
    }
    char expanded[512];
    script_expand(line, expanded, sizeof(expanded));
    CHAR16 wide[512];
    wide_from_ascii(expanded, wide, sizeof(wide) / sizeof(wide[0]));
    process_command(wide);
}

static int script_exec_block(char lines[][256], int start, int end, int depth)
{
    for (int i = start; i < end; ++i) {
        char line[256];
        scopy(line, sizeof(line), lines[i]);
        char *s = strim(line);
        if (!s[0] || s[0] == '#' || s[0] == '}' || !scmp(s, "end"))
            continue;

        if (sprefix(s, "while ")) {
            char cond[256];
            scopy(cond, sizeof(cond), s + 6);
            char *c = strim(cond);
            UINTN clen = slen(c);
            int brace_style = 0;
            if (clen && c[clen - 1] == '{') {
                brace_style = 1;
                c[clen - 1] = 0;
            }
            c = strim(c);

            int close = -1;
            if (brace_style) {
                int braces = 0;
                for (int j = i + 1; j < end; ++j) {
                    for (const char *p = lines[j]; *p; ++p) {
                        if (*p == '{') ++braces;
                        else if (*p == '}') {
                            if (braces == 0) { close = j; break; }
                            --braces;
                        }
                    }
                    if (close >= 0) break;
                }
                if (close < 0) {
                    outc(theme()->error, L"while: missing }\r\n");
                    return 0;
                }
            } else {
                int nest = 0;
                for (int j = i + 1; j < end; ++j) {
                    char t[256];
                    scopy(t, sizeof(t), lines[j]);
                    char *q = strim(t);
                    if (sprefix(q, "while ")) {
                        UINTN ql = slen(q);
                        if (ql && q[ql - 1] == '{') {
                            int b = 0;
                            int k = j;
                            for (; k < end; ++k) {
                                for (const char *p = lines[k]; *p; ++p) {
                                    if (*p == '{') ++b;
                                    else if (*p == '}') {
                                        if (b > 0) --b;
                                        if (b == 0) break;
                                    }
                                }
                                if (b == 0) break;
                            }
                            j = k;
                        } else {
                            ++nest;
                        }
                    } else if (!scmp(q, "end")) {
                        if (nest == 0) { close = j; break; }
                        --nest;
                    }
                }
                if (close < 0) {
                    outc(theme()->error, L"while: missing end\r\n");
                    return 0;
                }
            }

            int iterations = 0;
            while (script_condition(c)) {
                if (iterations++ > 10000) {
                    outc(theme()->error, L"while: iteration limit (10000)\r\n");
                    break;
                }
                int r = script_exec_block(lines, i + 1, close, depth + 1);
                if (r == 1) break;
                if (r != 2) {
                    // normal body execution
                }
            }
            i = close;
            continue;
        }

        if (!scmp(s, "break")) {
            if (depth == 0) outln(L"break: outside while");
            else return 1;
            continue;
        }
        if (!scmp(s, "continue")) {
            if (depth == 0) outln(L"continue: outside while");
            else return 2;
            continue;
        }

        script_exec_line(s);
    }
    return 0;
}

static void run_script(const CHAR16 *path)
{
    static UINT8 data[8192];
    CHAR16 resolved[256];
    path_for(resolved, sizeof(resolved) / sizeof(resolved[0]), path);
    UINTN size = read_whole_file(resolved, data, sizeof(data) - 1);
    if (size == 0) {
        outc(theme()->error, L"run: script not found or empty\r\n");
        return;
    }
    if (size >= sizeof(data) - 1)
        size = sizeof(data) - 2;
    data[size] = 0;

    if (script_depth >= 3) {
        outc(theme()->error, L"run: recursion limit reached\r\n");
        return;
    }
    ++script_depth;

    static char lines[256][256];
    int count = 0;
    char *p = (char *)data;
    while (*p && count < 256) {
        char raw[256];
        UINTN n = 0;
        while (*p && *p != '\n' && n + 1 < sizeof(raw))
            raw[n++] = *p++;
        if (*p == '\n') ++p;
        raw[n] = 0;
        char *line = strim(raw);
        if (!line[0] || line[0] == '#')
            continue;
        scopy(lines[count], sizeof(lines[count]), line);
        ++count;
    }

    script_exec_block(lines, 0, count, 0);
    --script_depth;
}

static void cmd_run(CHAR16 *args)
{
    if (!args[0]) {
        outln(L"run: usage: run FILE.wct");
        return;
    }
    run_script(args);
}

static void cmd_echo(CHAR16 *args)
{
    for (UINTN i = 0; args[i]; ++i) {
        CHAR16 s[2] = { args[i], 0 };
        out(s);
    }
    outln(L"");
}

static void cmd_sleep(CHAR16 *args)
{
    UINTN seconds = 0;
    for (UINTN i = 0; args[i]; ++i) {
        if (args[i] < L'0' || args[i] > L'9') {
            outln(L"sleep: usage: sleep SECONDS");
            return;
        }
        seconds = seconds * 10 + (UINTN)(args[i] - L'0');
    }
    if (seconds < 1) seconds = 1;
    if (seconds > 5) seconds = 5;
    ST->BootServices->Stall(seconds * 1000000);
}

static int editor_num(const char *s, int *value)
{
    int n = 0, digits = 0;
    while (*s == ' ') ++s;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        ++s;
        ++digits;
    }
    *value = n;
    return digits > 0;
}

static void editor_show(char lines[][128], int count)
{
    outc(theme()->accent, L"--- editor ---\r\n");
    for (int i = 0; i < count; ++i) {
        out_int(i + 1);
        out(L": ");
        out_ascii(lines[i]);
        outln(L"");
    }
    outc(theme()->dim, L"a/i/r/d/l/w/wq/q/m  |  q saves  Ctrl+S save  Ctrl+Q quit  Ctrl+X save+quit\r\n");
}

static int editor_save(const CHAR16 *path, char lines[][128], int count)
{
    static char data[8192];
    UINTN pos = 0;
    for (int i = 0; i < count; ++i) {
        UINTN n = slen(lines[i]);
        if (pos + n + 2 >= sizeof(data)) break;
        for (UINTN j = 0; j < n; ++j) data[pos++] = lines[i][j];
        data[pos++] = '\n';
    }
    return write_bytes(path, data, pos);
}

static void cmd_new(CHAR16 *args)
{
    if (!args[0]) {
        outln(L"new: usage: new FILE");
        return;
    }
    CHAR16 path[256];
    path_for(path, sizeof(path) / sizeof(path[0]), args);
    EFI_FILE_PROTOCOL *file;
    if (open_path(path, &file, EFI_FILE_MODE_READ, 0)) {
        file->Close(file);
        outln(L"new: file already exists");
        return;
    }
    if (write_bytes(path, "", 0)) {
        out(L"created ");
        outln(path);
    } else {
        outc(theme()->error, L"new: could not create file\r\n");
    }
}

static void cmd_edit(CHAR16 *args)
{
    if (!args[0]) {
        outln(L"edit: usage: edit FILE");
        return;
    }
    CHAR16 path[256];
    path_for(path, sizeof(path) / sizeof(path[0]), args);
    EFI_FILE_PROTOCOL *existing;
    if (open_path(path, &existing, EFI_FILE_MODE_READ, 0)) {
        existing->Close(existing);
    } else if (write_bytes(path, "", 0)) {
        outc(theme()->accent, L"new file created\r\n");
    } else {
        outc(theme()->error, L"edit: could not create file\r\n");
        return;
    }
    static char lines[40][128];
    static UINT8 raw[8192];
    int count = 0;
    UINTN size = read_whole_file(path, raw, sizeof(raw) - 1);
    raw[size] = 0;
    char *p = (char *)raw;
    while (*p && count < 40) {
        int n = 0;
        while (*p && *p != '\n' && n < 127) lines[count][n++] = *p++;
        if (*p == '\n') ++p;
        lines[count][n] = 0;
        ++count;
    }
    editor_show(lines, count);

    CHAR16 input[192];
    for (;;) {
        outc(theme()->prompt, L"edit");
        outc(theme()->dim, L"> ");
        read_line(input, sizeof(input) / sizeof(input[0]));
        if (last_control == 17) return;
        if (last_control == 15 || last_control == 19) {
            if (editor_save(path, lines, count)) outln(L"saved");
            else outc(theme()->error, L"save failed\r\n");
            continue;
        }
        if (last_control == 24) {
            if (editor_save(path, lines, count)) outln(L"saved");
            else outc(theme()->error, L"save failed\r\n");
            return;
        }
        char cmd[192];
        UINTN ci = 0;
        for (; input[ci] && ci < sizeof(cmd) - 1; ++ci)
            cmd[ci] = (char)input[ci];
        cmd[ci] = 0;
        char *line = strim(cmd);
        if (!line[0]) continue;
        if (weq(input, L"q")) {
            if (editor_save(path, lines, count)) {
                outln(L"saved");
                return;
            }
            outc(theme()->error, L"save failed\r\n");
            continue;
        }
        if (weq(input, L"w") || weq(input, L"wq")) {
            if (editor_save(path, lines, count)) outln(L"saved");
            else outc(theme()->error, L"save failed\r\n");
            if (weq(input, L"wq")) return;
            continue;
        }
        if (weq(input, L"l")) { editor_show(lines, count); continue; }
        if (weq(input, L"h")) { editor_show(lines, count); continue; }
        if (weq(input, L"m")) {
            outln(L"multi-line mode: Enter starts a new line; empty line finishes");
            for (;;) {
                outc(theme()->dim, L"> ");
                read_line(input, sizeof(input) / sizeof(input[0]));
                if (last_control == 17) return;
                if (last_control == 15 || last_control == 19) {
                    if (editor_save(path, lines, count)) outln(L"saved");
                    else outc(theme()->error, L"save failed\r\n");
                    continue;
                }
                if (last_control == 24) {
                    if (editor_save(path, lines, count)) outln(L"saved");
                    else outc(theme()->error, L"save failed\r\n");
                    return;
                }
                if (!input[0]) break;
                if (count < 40) {
                    UINTN n = 0;
                    for (; input[n] && n < 127; ++n)
                        lines[count][n] = (char)input[n];
                    lines[count][n] = 0;
                    ++count;
                }
            }
            continue;
        }
        if (sprefix(line, "a ")) {
            if (count < 40) {
                scopy(lines[count], 128, strim(line + 2));
                ++count;
                outln(L"appended");
            }
            continue;
        }
        if (sprefix(line, "i ")) {
            char *rest = strim(line + 2);
            int pos = 0;
            if (!editor_num(rest, &pos)) continue;
            while (*rest && *rest != ' ') ++rest;
            rest = strim(rest);
            if (pos < 1) pos = 1;
            if (pos > count + 1) pos = count + 1;
            if (count < 40) {
                for (int i = count; i >= pos; --i) scopy(lines[i], 128, lines[i - 1]);
                scopy(lines[pos - 1], 128, rest);
                ++count;
                outln(L"inserted");
            }
            continue;
        }
        if (sprefix(line, "r ")) {
            char *rest = strim(line + 2);
            int pos = 0;
            if (!editor_num(rest, &pos)) continue;
            while (*rest && *rest != ' ') ++rest;
            rest = strim(rest);
            if (pos >= 1 && pos <= count) {
                scopy(lines[pos - 1], 128, rest);
                outln(L"replaced");
            }
            continue;
        }
        if (sprefix(line, "d ")) {
            int pos = 0;
            if (!editor_num(line + 2, &pos)) continue;
            if (pos >= 1 && pos <= count) {
                for (int i = pos; i < count; ++i) scopy(lines[i - 1], 128, lines[i]);
                --count;
                outln(L"deleted");
            }
            continue;
        }
        outln(L"unknown edit command (h for help)");
    }
}

static void cmd_log(CHAR16 *args)
{
    if (weq(args, L"save")) {
        log_save(0);
        return;
    }
    if (weq(args, L"clear")) {
        log_count = 0;
        log_add("log", "log cleared");
        outln(L"log cleared");
        return;
    }
    int level = -1;
    const CHAR16 *text = NULL;
    if (wprefix(args, L"info ")) { level = LOG_INFO; text = args + 5; }
    else if (wprefix(args, L"warn ")) { level = LOG_WARN; text = args + 5; }
    else if (wprefix(args, L"error ")) { level = LOG_ERROR; text = args + 6; }
    if (level >= 0) {
        char msg[160];
        int n = 0;
        while (text[n] && n < 159) { msg[n] = (char)text[n]; ++n; }
        msg[n] = 0;
        log_add_level(level, "user", msg);
        outln(L"logged");
        return;
    }

    outc(theme()->accent, L"WCT-OS logs");
    outln(L"  (log info|warn|error TEXT)");
    for (int i = 0; i < log_count; ++i) {
        outc(theme()->dim, L"[");
        out_ascii(log_entries[i].time);
        outc(theme()->dim, L"] ");
        UINTN color = log_entries[i].level == LOG_ERROR ? theme()->error :
                      log_entries[i].level == LOG_WARN ? 0x0E : theme()->accent;
        outc(color, log_entries[i].level == LOG_ERROR ? L"[ERROR] " :
                     log_entries[i].level == LOG_WARN ? L"[WARN]  " : L"[INFO]  ");
        outc(theme()->dim, L"[");
        out_ascii(log_entries[i].tag);
        outc(theme()->dim, L"] ");
        out_ascii(log_entries[i].text);
        outln(L"");
    }
}

static void cmd_ps(void)
{
    outc(theme()->accent, L"  PID  NAME        STATE");
    outln(L"");
    for (int i = 0; i < proc_count; ++i) {
        out(L"  ");
        out_int(procs[i].pid);
        out(L"    ");
        out_ascii(procs[i].name);
        for (int n = (int)slen(procs[i].name); n < 12; ++n) out(L" ");
        UINTN color = !scmp(procs[i].state, "running") ? theme()->prompt :
                      !scmp(procs[i].state, "killed") ? theme()->error : 0x0E;
        outc(color, L"");
        out_ascii(procs[i].state);
        outln(L"");
    }
}

static void cmd_pid(void)
{
    out(L"PID ");
    out_int(2);
    outln(L"  wct-shell  (parent 1: wct-init)");
}

static void cmd_kill(CHAR16 *args, int sudo)
{
    UINTN pid = 0;
    if (!args[0]) {
        outln(L"kill: usage: kill PID");
        return;
    }
    for (UINTN i = 0; args[i]; ++i) {
        if (args[i] < L'0' || args[i] > L'9') {
            outln(L"kill: invalid PID");
            return;
        }
        pid = pid * 10 + (UINTN)(args[i] - L'0');
    }
    if ((pid == 1 || pid == 2) && !sudo) {
        outc(theme()->error, L"kill: cannot kill the shell/init process (use sudo kill PID)\r\n");
        log_add_level(LOG_ERROR, "proc", "kill denied for init/shell");
        return;
    }
    for (int i = 0; i < proc_count; ++i) {
        if (procs[i].pid == pid) {
            if (!scmp(procs[i].state, "killed")) {
                log_add_level(LOG_WARN, "proc", "process already dead");
                outln(L"kill: process already dead");
                return;
            }
            scopy(procs[i].state, sizeof(procs[i].state), "killed");
            if (sudo) log_add_level(LOG_WARN, "sudo", "protected process killed");
            out(L"killed PID ");
            out_int(pid);
            outln(L"");
            log_add_level(LOG_WARN, "proc", "process killed");
            return;
        }
    }
    outc(theme()->error, L"kill: no such process\r\n");
    log_add_level(LOG_ERROR, "proc", "kill: no such process");
}

static void cmd_sudo(CHAR16 *args)
{
    if (wprefix(args, L"kill ")) {
        cmd_kill(args + 5, 1);
        return;
    }
    outln(L"sudo: usage: sudo kill PID");
}

static void cmd_poweroff(void)
{
    log_add_level(LOG_WARN, "power", "poweroff requested");
    clear_graphics();
    ST->ConOut->ClearScreen(ST->ConOut);
    draw_wct_logo();
    outc(0x0B, L"        WCT-OS LTS 1.0_ET  /  Shutdown");
    outln(L"");
    outln(L"");
    outc(0x0A, L"  [ OK ] ");
    outc(0x0B, L"Saving log...");
    outln(L"");
    log_save(0);
    outc(0x0A, L"  [ OK ] ");
    outc(0x0B, L"Log saved. Powering off...");
    outln(L"");
    ST->BootServices->Stall(2000000);
    if (ST->RuntimeServices && ST->RuntimeServices->ResetSystem)
        ST->RuntimeServices->ResetSystem(EFI_RESET_SHUTDOWN, EFI_SUCCESS, 0, NULL);
    outln(L"poweroff: firmware does not support shutdown");
}

static int ensure_pkg_dir(void)
{
    EFI_FILE_PROTOCOL *dir;
    if (open_path(L"\\packages", &dir, EFI_FILE_MODE_READ, 0)) {
        dir->Close(dir);
        return 1;
    }
    if (open_path(L"\\packages", &dir, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ, EFI_FILE_DIRECTORY)) {
        dir->Close(dir);
        return 1;
    }
    return 0;
}

static int ascii_wide_eq(const char *a, const CHAR16 *b)
{
    while (*a && *b) {
        if ((CHAR16)(unsigned char)*a != *b) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

static void run_package_script(const CHAR16 *path)
{
    PkgMeta meta;
    pkg_meta(path, &meta);
    if (meta.color[0]) {
        CHAR16 color[16];
        UINTN i = 0;
        while (meta.color[i] && i + 1 < sizeof(color) / sizeof(color[0])) {
            color[i] = (CHAR16)(unsigned char)meta.color[i];
            ++i;
        }
        color[i] = 0;
        cmd_color(color);
    }
    run_script(path);
}

static int find_package(const CHAR16 *name, CHAR16 *out)
{
    if (!name[0]) return 0;
    CHAR16 path[256];
    wcopy(path, sizeof(path) / sizeof(path[0]), L"\\packages\\");
    wcat(path, sizeof(path) / sizeof(path[0]), name);
    EFI_FILE_PROTOCOL *f = NULL;
    if (open_path(path, &f, EFI_FILE_MODE_READ, 0)) {
        f->Close(f);
        wcopy(out, 256, path);
        return 1;
    }
    const CHAR16 *exts[] = { L".wctpkg", L".wct" };
    for (int e = 0; e < 2; ++e) {
        CHAR16 alt[256];
        wcopy(alt, sizeof(alt) / sizeof(alt[0]), path);
        wcat(alt, sizeof(alt) / sizeof(alt[0]), exts[e]);
        if (open_path(alt, &f, EFI_FILE_MODE_READ, 0)) {
            f->Close(f);
            wcopy(out, 256, alt);
            return 1;
        }
    }

    EFI_FILE_PROTOCOL *dir;
    if (!open_path(L"\\packages", &dir, EFI_FILE_MODE_READ, 0)) return 0;
    static UINT8 buf[2048];
    int found = 0;
    for (;;) {
        UINTN size = sizeof(buf);
        if (EFI_IS_ERROR(dir->Read(dir, &size, buf)) || size == 0) break;
        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        if (!info->FileName[0]) break;
        if (weq(info->FileName, L".") || weq(info->FileName, L"..")) continue;
        CHAR16 file[256];
        wcopy(file, sizeof(file) / sizeof(file[0]), L"\\packages\\");
        wcat(file, sizeof(file) / sizeof(file[0]), info->FileName);
        PkgMeta meta;
        pkg_meta(file, &meta);
        if (meta.command[0] && ascii_wide_eq(meta.command, name)) {
            wcopy(out, 256, file);
            found = 1;
            break;
        }
    }
    dir->Close(dir);
    return found;
}

static void cmd_pkg(CHAR16 *args)
{
    CHAR16 *sub, *rest;
    split_first(args, &sub, &rest);
    if (!sub[0] || weq(sub, L"list")) {
        if (!ensure_pkg_dir()) {
            outc(theme()->error, L"pkg: cannot create \\packages\r\n");
            return;
        }
        outln(L"Installed packages (command : file):");
        if (!list_package_commands()) outln(L"  (none)");
        outln(L"use: pkg install FILE | pkg run NAME | pkg remove NAME");
        return;
    }
    if (weq(sub, L"install")) {
        if (!rest[0]) { outln(L"pkg install: missing source file"); return; }
        if (!ensure_pkg_dir()) { outc(theme()->error, L"pkg: cannot create \\packages\r\n"); return; }
        const CHAR16 *base = rest;
        for (const CHAR16 *p = rest; *p; ++p)
            if (*p == L'\\' || *p == L'/') base = p + 1;
        if (!base[0]) { outln(L"pkg install: invalid file name"); return; }
        CHAR16 dest[256];
        wcopy(dest, sizeof(dest) / sizeof(dest[0]), L"\\packages\\");
        wcat(dest, sizeof(dest) / sizeof(dest[0]), base);
        CHAR16 copy_args[520];
        wcopy(copy_args, sizeof(copy_args) / sizeof(copy_args[0]), rest);
        wcat(copy_args, sizeof(copy_args) / sizeof(copy_args[0]), L" ");
        wcat(copy_args, sizeof(copy_args) / sizeof(copy_args[0]), dest);
        cmd_cp(copy_args);
        return;
    }
    if (weq(sub, L"run")) {
        if (!rest[0]) { outln(L"pkg run: missing package name"); return; }
        CHAR16 path[256];
        if (!find_package(rest, path)) {
            outc(theme()->error, L"pkg run: package not found\r\n");
            return;
        }
        run_package_script(path);
        return;
    }
    if (weq(sub, L"remove")) {
        if (!rest[0]) { outln(L"pkg remove: missing package name"); return; }
        CHAR16 path[256];
        if (!find_package(rest, path)) {
            outc(theme()->error, L"pkg remove: package not found\r\n");
            return;
        }
        remove_path(path);
        outln(L"removed");
        return;
    }
    if (weq(sub, L"info")) {
        CHAR16 path[256];
        if (!find_package(rest, path)) {
            outc(theme()->error, L"pkg info: package not found\r\n");
            return;
        }
        PkgMeta meta;
        pkg_meta(path, &meta);
        out(L"name    : "); out_ascii(meta.name[0] ? meta.name : "?"); outln(L"");
        out(L"version : "); out_ascii(meta.version[0] ? meta.version : "0.0"); outln(L"");
        out(L"author  : "); out_ascii(meta.author[0] ? meta.author : "?"); outln(L"");
        out(L"command : "); out_ascii(meta.command[0] ? meta.command : "?"); outln(L"");
        out(L"color   : "); out_ascii(meta.color[0] ? meta.color : "default"); outln(L"");
        out(L"desc    : "); out_ascii(meta.description); outln(L"");
        outln(L"---");
        cmd_cat(path);
        return;
    }
    outln(L"pkg: list | install FILE | run NAME | remove NAME | info NAME");
}

static int try_package_command(const CHAR16 *name)
{
    CHAR16 path[256];
    if (!find_package(name, path)) return 0;
    run_package_script(path);
    return 1;
}

static void cmd_api(CHAR16 *args)
{
    CHAR16 *sub, *rest, *method, *params;
    split_first(args, &sub, &rest);
    if (!sub[0] || weq(sub, L"list")) {
        outln(L"WCT-OS Package API");
        outln(L"  term.write TEXT     print text to terminal");
        outln(L"  term.color NAME     set text colour");
        outln(L"  term.clear          clear screen, redraw banner");
        outln(L"  term.banner         redraw WCT-OS banner");
        outln(L"  term.exec CMD       run any shell command");
        outln(L"  task.list           list native process table");
        outln(L"  task.kill PID       kill process (1/2 protected)");
        outln(L"  task.poweroff       save log, show logo, power off");
        outln(L"  fs.ls [PATH]        list FAT32 directory");
        outln(L"  fs.cat FILE         print a file");
        outln(L"  fs.write FILE TEXT  overwrite file with one line");
        outln(L"  fs.cp SRC DST       copy a file");
        outln(L"  fs.rm FILE          delete a file");
        outln(L"  fs.mkdir DIR        create a directory");
        outln(L"  pkg.list            list installed packages");
        outln(L"  pkg.run NAME        run a package");
        outln(L"  sys.hw              show CPU/firmware/memory");
        outln(L"  sys.log [ARGS]      view/write/clear/save log");
        outln(L"  cmd.list            list all native commands");
        outln(L"  cmd.NAME ARGS       call native command NAME");
        outln(L"  usage: api call METHOD ARGS");
        return;
    }
    if (!weq(sub, L"call")) {
        outln(L"api: usage: api list | api call METHOD ARGS");
        return;
    }
    split_first(rest, &method, &params);
    if (!method[0]) { outln(L"api call: missing method"); return; }

    if (weq(method, L"term.write")) cmd_echo(params);
    else if (weq(method, L"term.color")) cmd_color(params);
    else if (weq(method, L"term.clear")) cmd_clear();
    else if (weq(method, L"term.banner")) { ST->ConOut->ClearScreen(ST->ConOut); print_banner(); }
    else if (weq(method, L"term.exec")) { if (params[0]) process_command(params); }
    else if (weq(method, L"task.list")) cmd_ps();
    else if (weq(method, L"task.kill")) cmd_kill(params, 0);
    else if (weq(method, L"task.poweroff")) cmd_poweroff();
    else if (weq(method, L"fs.ls")) cmd_ls(params);
    else if (weq(method, L"fs.cat")) cmd_cat(params);
    else if (weq(method, L"fs.write")) cmd_write(params);
    else if (weq(method, L"fs.cp")) cmd_cp(params);
    else if (weq(method, L"fs.rm")) cmd_rm(params);
    else if (weq(method, L"fs.mkdir")) cmd_mkdir(params);
    else if (weq(method, L"pkg.list")) cmd_pkg(L"list");
    else if (weq(method, L"pkg.run")) {
        CHAR16 pkgargs[256];
        wcopy(pkgargs, sizeof(pkgargs) / sizeof(pkgargs[0]), L"run ");
        wcat(pkgargs, sizeof(pkgargs) / sizeof(pkgargs[0]), params);
        cmd_pkg(pkgargs);
    }
    else if (weq(method, L"cmd.list")) {
        for (UINTN i = 0; i < sizeof(native_commands) / sizeof(native_commands[0]); ++i) {
            out_ascii(native_commands[i]); outln(L"");
        }
        outln(L"[package commands]");
        list_package_commands();
    }
    else if (wprefix(method, L"cmd.")) {
        CHAR16 call[512];
        wcopy(call, sizeof(call) / sizeof(call[0]), method + 4);
        if (params[0]) { wcat(call, sizeof(call) / sizeof(call[0]), L" "); wcat(call, sizeof(call) / sizeof(call[0]), params); }
        process_command(call);
    }
    else if (weq(method, L"sys.hw")) cmd_hw();
    else if (weq(method, L"sys.log")) cmd_log(params);
    else {
        outc(theme()->error, L"api: unknown method ");
        outln(method);
    }
}

static void print_banner(void)
{
    UINTN cols = 0, rows = 0;
    ST->ConOut->QueryMode(ST->ConOut, ST->ConOut->Mode->Mode, &cols, &rows);

    draw_wct_logo();
    outln(L"");
    outc(0x0B, L"   WCT-OS Native 1.0_ET  /  C / x86_64 UEFI / FAT32");
    outln(L"");
    outln(linux_available ? L"   Self-developed native shell / Linux subsystem" : L"   Self-contained native-only build");
    outln(L"");
    reset_color();

    out(L"  theme: ");
    set_color(theme()->prompt);
    out_ascii(theme()->name);
    reset_color();
    out(L"   font: ");
    out_int(cols);
    out(L"x");
    out_int(rows);
    out(L"   mode: ");
    out_int(ST->ConOut->Mode->Mode);
    outln(L"");
    out(L"  Type ");
    set_color(theme()->accent);
    out(L"help");
    reset_color();
    outln(L" for commands.");
    if (linux_available) {
        out(L"  Type ");
        set_color(theme()->accent);
        out(L"linux");
        reset_color();
        outln(L" to start the Linux subsystem.");
    }
    outln(L"");
}

static void cmd_theme(CHAR16 *args)
{
    if (!args[0]) {
        outln(L"Themes:");
        for (int i = 0; i < 5; ++i) {
            out(L"  ");
            outc(themes[i].prompt, L"[]");
            out(L" ");
            if (i == theme_index)
                outc(themes[i].bright, L"* ");
            else
                out(L"  ");
            out_ascii(themes[i].name);
            outln(L"");
        }
        outln(L"  use: theme ubuntu | wct | matrix | amber | ice");
        return;
    }
    for (int i = 0; i < 5; ++i) {
        int match = 1;
        for (int j = 0; args[j] && themes[i].name[j]; ++j) {
            if (args[j] != (CHAR16)themes[i].name[j]) { match = 0; break; }
        }
        if (match) {
            theme_index = i;
            ST->ConOut->ClearScreen(ST->ConOut);
            reset_color();
            print_banner();
            return;
        }
    }
    outln(L"theme: unknown name");
}

static void cmd_font(CHAR16 *args)
{
    UINTN max = ST->ConOut->Mode->MaxMode;
    UINTN current = ST->ConOut->Mode->Mode;

    if (!args[0]) {
        outln(L"Console modes (font / geometry):");
        for (UINTN i = 0; i < max && i < 12; ++i) {
            UINTN cols = 0, rows = 0;
            if (EFI_IS_ERROR(ST->ConOut->QueryMode(ST->ConOut, i, &cols, &rows)))
                continue;
            out(L"  ");
            if (i == current)
                outc(theme()->bright, L"* ");
            else
                out(L"  ");
            out_int(i);
            out(L": ");
            out_int(cols);
            out(L"x");
            out_int(rows);
            outln(L"");
        }
        outln(L"  use: font next   or   font NUMBER");
        return;
    }

    UINTN target = current;
    if (weq(args, L"next")) {
        target = (current + 1) % max;
    } else {
        target = 0;
        for (UINTN i = 0; args[i]; ++i) {
            if (args[i] < L'0' || args[i] > L'9') {
                outln(L"font: invalid number");
                return;
            }
            target = target * 10 + (UINTN)(args[i] - L'0');
        }
    }
    if (EFI_IS_ERROR(ST->ConOut->SetMode(ST->ConOut, target))) {
        outln(L"font: mode not available");
        return;
    }
    ST->ConOut->ClearScreen(ST->ConOut);
    reset_color();
    print_banner();
}

static void cmd_clear(void)
{
    ST->ConOut->ClearScreen(ST->ConOut);
    print_banner();
}

static void process_command(CHAR16 *line)
{
    CHAR16 *command, *args;
    split_command(line, &command, &args);
    if (!command[0])
        return;

    if (weq(command, L"help")) cmd_help();
    else if (weq(command, L"ls")) cmd_ls(args);
    else if (weq(command, L"cat")) cmd_cat(args);
    else if (weq(command, L"write")) cmd_write(args);
    else if (weq(command, L"rm")) cmd_rm(args);
    else if (weq(command, L"cp")) cmd_cp(args);
    else if (weq(command, L"mkdir")) cmd_mkdir(args);
    else if (weq(command, L"cd")) cmd_cd(args);
    else if (weq(command, L"pwd")) cmd_pwd();
    else if (weq(command, L"echo")) cmd_echo(args);
    else if (weq(command, L"sleep")) cmd_sleep(args);
    else if (weq(command, L"run") || weq(command, L"script")) cmd_run(args);
    else if (weq(command, L"edit")) cmd_edit(args);
    else if (weq(command, L"new") || weq(command, L"create") || weq(command, L"touch")) cmd_new(args);
    else if (weq(command, L"date")) cmd_date();
    else if (weq(command, L"hw") || weq(command, L"sysinfo")) cmd_hw();
    else if (weq(command, L"mem")) cmd_mem(args);
    else if (weq(command, L"log")) cmd_log(args);
    else if (weq(command, L"ps")) cmd_ps();
    else if (weq(command, L"pid")) cmd_pid();
    else if (weq(command, L"kill")) cmd_kill(args, 0);
    else if (weq(command, L"sudo")) cmd_sudo(args);
    else if (weq(command, L"poweroff")) cmd_poweroff();
    else if (weq(command, L"pkg")) cmd_pkg(args);
    else if (weq(command, L"api")) cmd_api(args);
    else if (weq(command, L"usb")) cmd_usb(args);
    else if (weq(command, L"ver")) cmd_ver();
    else if (weq(command, L"about")) cmd_about();
    else if (weq(command, L"color")) cmd_color(args);
    else if (weq(command, L"clear")) cmd_clear();
    else if (weq(command, L"banner")) { ST->ConOut->ClearScreen(ST->ConOut); print_banner(); }
    else if (weq(command, L"theme") || weq(command, L"color")) cmd_theme(args);
    else if (weq(command, L"font")) cmd_font(args);
    else if (weq(command, L"linux")) cmd_linux();
    else if (weq(command, L"native")) cmd_native();
    else if (weq(command, L"reboot")) reset_system();
    else if (!try_package_command(command)) {
        outc(theme()->error, L"unknown command: ");
        outln(command);
        outln(L"Type 'help' or install a package command.");
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    ST = st;
    g_image = image_handle;
    if (!st || !st->ConOut || !st->ConIn)
        return EFI_INVALID_PARAMETER;

    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
    st->ConIn->Reset(st->ConIn, FALSE);
    st->ConOut->Reset(st->ConOut, FALSE);
    st->ConOut->SetAttribute(st->ConOut, 0x07);
    current_attr = 0x07;

    g_loaded = NULL;
    if (!EFI_IS_ERROR(st->BootServices->HandleProtocol(image_handle, &loaded_image_guid, (VOID **)&g_loaded)) && g_loaded) {
        if (!EFI_IS_ERROR(st->BootServices->HandleProtocol(g_loaded->DeviceHandle, &simple_fs_guid, (VOID **)&FS)) && FS) {
            FS->OpenVolume(FS, &ROOT);
        }
    }
    scan_volumes();
    if (!BOOT_ROOT) BOOT_ROOT = ROOT;
    if (ROOT) {
        EFI_FILE_PROTOCOL *linux_loader = NULL;
        if (open_path(L"\\wct\\bootx64.efi", &linux_loader, EFI_FILE_MODE_READ, 0)) {
            linux_available = 1;
            linux_loader->Close(linux_loader);
        }
    }

    if (linux_available) {
        int choice = maybe_f11_menu();
        if (choice == 2) {
            linux_available = 0;
            boot_no_linux = 1;
        }
    }

    boot_splash();
    clear_graphics();
    ST->ConOut->ClearScreen(ST->ConOut);
    reset_color();
    print_banner();

    if (!ROOT) {
        outc(theme()->error, L"warning: FAT32 volume not available\r\n");
    }

    CHAR16 line[192];
    for (;;) {
        ST->ConOut->EnableCursor(ST->ConOut, TRUE);
        outc(theme()->prompt, L"wct@wct-os");
        outc(theme()->accent, L":");
        outc(theme()->dim, CWD);
        outc(theme()->dim, L"> ");
        read_line(line, sizeof(line) / sizeof(line[0]));
        char logcmd[192];
        UINTN li = 0;
        for (; line[li] && li < sizeof(logcmd) - 1; ++li) logcmd[li] = (char)line[li];
        logcmd[li] = 0;
        if (logcmd[0]) log_add("cmd", logcmd);
        process_command(line);
        log_save(1);
    }
}
