/* DUESLIN Boot Kernel - 自研引导内核 (C++)
 * 负责：启动画面、硬件自检、进入系统前的引导层
 * 编译: g++ -m32 -ffreestanding -nostdlib -fno-pie -fno-stack-protector \
 *       -c dueslin-boot.cpp -o boot.o
 *       ld -m elf_i386 -T linker.ld boot.o -o dueslin-boot.elf
 */

#define MULTIBOOT_MAGIC      0x1BADB002  /* multiboot 头魔数 */
#define MULTIBOOT_BOOT_MAGIC 0x2BADB002  /* GRUB 引导后传入 eax 的值 */
#define MULTIBOOT_FLAGS      0x00000003  /* align + memory info */

/* Multiboot 头（GRUB 识别） */
struct multiboot_header {
    unsigned int magic;
    unsigned int flags;
    unsigned int checksum;
} __attribute__((packed));

__attribute__((section(".multiboot"), aligned(4)))
struct multiboot_header mb_header = {
    MULTIBOOT_MAGIC,
    MULTIBOOT_FLAGS,
    (unsigned int)-(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)
};

/* VGA 文本模式: 0xB8000, 80x25 */
#define VGA_ADDR 0xB8000
#define COLS 80
#define ROWS 25

volatile unsigned short *vga = (volatile unsigned short *)VGA_ADDR;

/* 颜色: 蓝底(1) 白字(7) / 黑字(0) */
#define ATTR_BLUE_WHITE 0x17
#define ATTR_BLUE_BLACK 0x10
#define ATTR_BLACK_WHITE 0x07

void putc_at(int x, int y, char c, unsigned char attr) {
    if (x >= 0 && x < COLS && y >= 0 && y < ROWS)
        vga[y * COLS + x] = (unsigned short)((attr << 8) | (unsigned char)c);
}

void clear_screen(unsigned char attr) {
    for (int i = 0; i < COLS * ROWS; i++)
        vga[i] = (unsigned short)((attr << 8) | ' ');
}

void draw_logo(int center_x, int center_y) {
    /* 画一个 7x7 的方形，内部是 D */
    const char *art[7] = {
        "XXXXXXX",
        "X     X",
        "X  DD X",
        "X  D D X",
        "X  DD X",
        "X     X",
        "XXXXXXX"
    };
    /* 简化: 用 ASCII 方块 + D */
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 7; c++) {
            char ch = ' ';
            if (r == 0 || r == 4 || c == 0 || c == 6) ch = '#';
            if (r >= 1 && r <= 3 && c >= 1 && c <= 2) ch = '#';
            putc_at(center_x + c, center_y + r, ch, ATTR_BLUE_BLACK);
        }
    }
    /* 中间的 D */
    putc_at(center_x + 2, center_y + 2, 'D', ATTR_BLUE_BLACK);
}

/* 简易延迟（用 PIT 端口） */
void delay(int ticks) {
    for (volatile int i = 0; i < ticks; i++)
        for (volatile int j = 0; j < 1000; j++) __asm__("nop");
}

/* 硬件自检（简易） */
void hw_check() {
    /* CPUID 检测基本功能 */
    unsigned int eax = 0;
    __asm__ volatile("cpuid" : "=a"(eax) : "a"(0) : "ebx", "ecx", "edx");
    /* eax = 最大 CPUID 级别 + 厂商字符串在 ebx/edx/ecx */
    char vendor[13] = {0};
    unsigned int ebx, edx, ecx;
    __asm__ volatile("cpuid" : "=b"(ebx), "=d"(edx), "=c"(ecx) : "a"(0));
    __builtin_memcpy(vendor, &ebx, 4);
    __builtin_memcpy(vendor + 4, &edx, 4);
    __builtin_memcpy(vendor + 8, &ecx, 4);
    vendor[12] = 0;
    /* 显示在屏幕底部 */
    const char *msg = "CPU: ";
    for (int i = 0; msg[i]; i++) putc_at(i, 23, msg[i], ATTR_BLACK_WHITE);
    for (int i = 0; vendor[i]; i++) putc_at(5 + i, 23, vendor[i], ATTR_BLACK_WHITE);
}

extern "C" void kernel_main(unsigned int magic, unsigned int mbinfo) {
    (void)mbinfo;

    /* 检查 multiboot magic */
    if (magic != MULTIBOOT_BOOT_MAGIC) {
        clear_screen(ATTR_BLACK_WHITE);
        const char *err = "DUESLIN Boot Kernel: invalid multiboot magic!";
        for (int i = 0; err[i]; i++) putc_at(i, 11, err[i], ATTR_BLACK_WHITE);
        /* 打印收到的 magic 十六进制 */
        const char *hex = "0123456789abcdef";
        char buf[20] = "magic: 0x        "; /* 19 字符 + null */
        unsigned int m = magic;
        for (int i = 0; i < 8; i++) {
            buf[9 + (7 - i)] = hex[m & 0xF];
            m >>= 4;
        }
        for (int i = 0; buf[i]; i++) putc_at(i, 13, buf[i], ATTR_BLACK_WHITE);
        for (;;) __asm__("hlt");
    }

    /* 蓝色背景 */
    clear_screen(ATTR_BLUE_WHITE);

    /* 标题 */
    const char *title = "DUESLIN 1.0";
    for (int i = 0; title[i]; i++)
        putc_at(36 + i, 8, title[i], ATTR_BLUE_WHITE);

    /* 绘制 logo */
    draw_logo(36, 10);

    /* Win11 风格转圈动画 */
    const char spinner[4] = {'|', '/', '-', '\\'};
    for (int frame = 0; frame < 24; frame++) {
        /* 清除旧 spinner */
        putc_at(39, 17, ' ', ATTR_BLUE_WHITE);
        /* 绘制新 spinner */
        putc_at(39, 17, spinner[frame % 4], ATTR_BLUE_WHITE);
        delay(800000);
    }

    /* 硬件自检 */
    hw_check();

    const char *boot_msg = "Booting system...";
    for (int i = 0; boot_msg[i]; i++)
        putc_at(33 + i, 19, boot_msg[i], ATTR_BLUE_WHITE);

    delay(2000000);

    /* 完成引导层职责，返回 GRUB 继续（实际上由 GRUB 直接加载 Linux 内核） */
    /* 此处暂停，等待系统接管（由 GRUB 配置负责加载 Linux） */
    for (;;) __asm__("hlt");
}
