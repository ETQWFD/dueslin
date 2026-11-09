# WCT 内核 K0（wctk.efi）

UEFI 原生内核，不依赖 Linux。已实现：

- GOP 帧缓冲双缓冲、窗口合成、重叠/拖拽/标题栏、任务栏
- 内置 8x16 点阵字体与宽字符
- 输入：USB HID(UsbIo) → Simple Pointer → Absolute Pointer 三重回退，
  启动时轮询等待驱动注册（最多 3 秒，修复"启动瞬间查不到指针设备"）
- 颜色：PixelFormat 0/1 通道序正确映射（修复红蓝互换导致主题偏色）
- 桌面：Files / Terminal / Settings / Theme / Exit 五个图标
- **Settings 应用**：5 套主题 Blue(默认) / Midnight / Green / Matrix / Amber，
  鼠标点行或键盘 1-5 实时切换；窗口管理说明与内核信息
- WCT Shell：help / version / uname / ls / clear / exit
- 最小内核 C 库：memcpy / wlen / wcat / normalize_path / open_path

产物约 26 KB，prebuilt/WCTK.EFI 为已编译版本。
