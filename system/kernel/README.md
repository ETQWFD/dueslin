# WCT 内核 K0（wctk.efi）

自研 UEFI 原生内核，**不依赖 Linux 内核**。当前里程碑 K0 已实现：

- UEFI 原生引导（EFI 应用形式，可直接被 native.efi 加载，也可作为 BOOTX64.EFI 启动）
- GOP 帧缓冲图形栈：双缓冲、窗口合成、重叠窗口、拖拽、标题栏
- 内置 8x16 点阵字体与宽字符渲染
- 输入：USB HID（UsbIo 协议）+ Simple Pointer + Absolute Pointer，三重回退，启动时轮询等待驱动注册（最多 3 秒）
- 内置 WCT Shell：help / version / uname / ls / clear / exit
- 最小内核 C 库：memcpy、wlen、wcat、normalize_path、open_path
- WCTFS 抽象层（当前挂在 FAT 卷上，后续替换为原生 wctfs + wcte 压缩）

## 构建
```
x86_64-w64-mingw32-gcc -ffreestanding -fno-stack-protector -fno-builtin -mno-red-zone -O2 \
  -I. -c kernel.c -o k.o
x86_64-w64-mingw32-gcc -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main \
  -Wl,--image-base=0x10000000 -Wl,--enable-reloc-section -Wl,--gc-sections \
  -o wctk.efi k.o -lgcc
```
产物 `wctk.efi` ≈ 24 KB，prebuilt/ 下为已编译版本。
