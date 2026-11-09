# DUESLIN 1.0_ET 系统源码（完整）

| 目录 | 内容 |
|---|---|
| installer/dueslin-installer | 安装器：键盘操作(n/k/o/w)、全格式磁盘扫描、gpm 鼠标光标、确定性引导写入 |
| live/inittab, live/dueslin-live-start | live 启动：inittab 修复、光驱幂等挂载、gpm 鼠标、安装/桌面模式分流 |
| iso/*.sh | ISO / WIM / initramfs 构建脚本（含 GRUB 引导配对与模块打包） |
| bootloader/src | 自研 UEFI 引导器 C 源码（启动动画含 _ET）、efi.h、font8x16.h |
| bootloader/prebuilt | BOOTX64.EFI（已编译，_ET 版）、boot.img / core.img（同版本 BIOS 引导配对） |
| desktop.c | 桌面主程序源码 |

## 本版修复的关键点
1. 启动动画加入 `_ET`（WCT-OS LTS 1.0_ET）。
2. inittab 非法条目（12–24 行）→ init 报 `Bad inittab entry`，已清理为合法条目。
3. 光驱挂载失败（`Can't mount, would change RO state`）→ 改为 `mount -t iso9660 -o ro` + 多设备重试，安装源可正常找到。
4. 重启进不了系统 → 原 GPT+bios_grub 方案 stage1 找不到 core、grub.cfg 指向不存在的 `/boot/dueslin/vmlinuz`。现改为：
   - MBR 分区表（p1 ESP/FAT32，p2 root/ext4）
   - 仅写前 446 字节 boot code（保留分区表）、core.img 置 LBA1（26KB < 31KB 限制）
   - GRUB 模块与 grub.cfg 写入 ESP `/grub`，内核/initramfs 复制到 ESP 根与 root `/boot`
5. 磁盘扫描：改读 `/sys/block`，支持 SATA/SCSI/NVMe/virtio/IDE/USB/MMC/RAID，排除光驱/内存盘/loop。
6. 安装环境鼠标：内置 gpm，`-A` 绘制光标，安装界面可见鼠标。

## 分区/引导约定
- p1 `DUESLIN_ESP`(FAT32) 2MiB–514MiB；p2 `DUESLIN_ROOT`(ext4) 514MiB–末尾
- BIOS: MBR(446B) + core@LBA1 + ESP:/grub/{i386-pc,grub.cfg}
- grub.cfg: `set root=(hd0,msdos1)` → `linux /vmlinuz root=/dev/sda2 rw quiet` → `initrd /initramfs`
