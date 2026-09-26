/* DUESLIN Boot Kernel - 入口点 */
extern "C" void kernel_main(unsigned int, unsigned int);

extern "C" __attribute__((noreturn))
void _start(void) {
    /* GRUB 加载 multiboot 内核后: eax=0x2BADB002 (magic), ebx=mb_info 地址 */
    unsigned int magic, mbinfo;
    __asm__ volatile("" : "=a"(magic), "=b"(mbinfo));
    kernel_main(magic, mbinfo);
    for (;;) __asm__("hlt");
}
