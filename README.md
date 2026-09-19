# DUESLIN 1.0 —— 简洁 · 快速 · 兼容旧设备

DUESLIN 是一款全新的轻量级桌面操作系统，基于 Alpine Linux 与 Linux 内核构建。
它的安装方式与 Windows 一致（图形化安装器），拥有 Windows 风格的任务栏/开始菜单，
全新的简化命令系统，内置 Firefox 浏览器与 WiFi / 蓝牙 / 音频驱动，
仅需 **400MB+ 磁盘空间、1GB 内存** 即可流畅运行，完美支持旧机器。

## ✨ 特性

| 特性 | 说明 |
|---|---|
| 🚀 快速启动 | 精简系统 + 轻量桌面，老机器也能快速进入桌面 |
| 💾 占用极小 | 安装仅需 400MB+ 磁盘，1GB 内存可流畅运行 |
| 🖥 Windows 风格 | 底部任务栏、开始菜单、托盘、右键体验 |
| 🍎 苹果风安装器 | 简洁美观的图形化安装界面，和 Windows 安装一样简单 |
| 📦 WIM 压缩镜像 | 系统以 .wim 格式压缩，安装时自动解压复制 |
| 🛠 PE 修复模式 | `pe te=1` 进入 PE 修复环境，可修复引导/磁盘/重装 |
| ⌨ 全新命令系统 | 简化命令（`install`、`files`、`wifi`…），Linux/cmd 命令同样可用 |
| 🌐 内置 Firefox | 开箱即用的浏览器 |
| 📶 驱动齐全 | WiFi、蓝牙、音频、显卡（Intel/AMD）驱动开箱即用 |
| 🔒 权限控制 | 普通进程随意结束；核心进程需 `su retc` 提权 |

## 📋 硬件要求

- CPU：任意 x86_64（32 位老机器请选 32 位版）
- 内存：1GB 以上（推荐 2GB）
- 磁盘：400MB+（推荐 8GB）
- 支持：UEFI / 传统 BIOS 双引导

## 🔧 安装方法

1. 下载 DUESLIN ISO 镜像，写入 U 盘（`dd if=DUESLIN.iso of=/dev/sdX bs=4M`）或用 Rufus/Ventoy 制作启动盘；
2. 从 U 盘启动，进入图形化安装界面；
3. 选择安装磁盘 → 自动分区 → 解压 .wim 系统镜像 → 安装引导 → 重启；
4. 重启后进入首次设置：创建用户/密码、连接 WiFi、驱动初始化；
5. 进入桌面，开始使用！

## ⌨ 命令速览

```
install <软件包>        安装软件           files            查看文件
remove <软件包>         卸载软件           goto <目录>      进入目录
update / upgrade        更新/升级         open <文件>      打开文件
wifi                    扫描WiFi          lock             锁屏
wifi connect <名> [密码] 连接WiFi          shutdown / reboot 关机/重启
pe te=1 / pe te=2       启用/卸载 PE 修复  task             任务管理器
su retc                 提权到最高权限     help             命令手册
```

完整手册见 `docs/COMMANDS.md`。

## 🛠 技术架构

```
┌─────────────────────────────────────────────┐
│  DUESLIN 桌面（XFCE + 自定义开始菜单/任务栏）   │
│  ├─ DUESLIN Shell（全新简化命令系统）          │
│  ├─ 任务管理器 / 首启向导 / PE 修复菜单         │
│  ├─ Firefox · NetworkManager · PipeWire       │
│  └─ WiFi / 蓝牙 / 音频 / 显卡驱动              │
├─────────────────────────────────────────────┤
│  Alpine Linux base（OpenRC 服务管理）          │
├─────────────────────────────────────────────┤
│  Linux LTS 内核 6.18                            │
│  GRUB 引导（BIOS + UEFI）                      │
└─────────────────────────────────────────────┘
```

## 📁 目录结构

```
├── src/                  DUESLIN 系统组件源码
│   ├── dueslin           全新命令解释器
│   ├── dueslin-installer 图形化安装器
│   ├── dueslin-firstboot 首次启动向导
│   ├── dueslin-taskmgr   任务管理器
│   ├── dueslin-pe        PE 修复模式控制
│   └── dueslin-pe-menu   PE 修复菜单
├── desktop/              桌面配置（任务栏/菜单/快捷键/主题）
├── system/               系统级配置（GRUB 模板/自动登录）
├── build/                构建脚本（initramfs/WIM/ISO）
├── brand/                Logo 与壁纸
├── docs/                 文档（命令手册等）
└── website/              官方网站
```

## 🔨 从源码构建 ISO

```bash
# 1. 准备 Alpine 3.24 根文件系统（见 build/README.md）
# 2. 安装组件与配置
./build/install-dueslin.sh
# 3. 制作 initramfs / WIM / ISO
./build/make-initramfs.sh dueslin-build/live
./build/make-wim.sh dueslin-build/rootfs dueslin-build/iso
./build/build-iso.sh dueslin-build/live dueslin-build/iso/DUESLIN.wim DUESLIN.iso
```

## 📄 许可

DUESLIN 基于 Alpine Linux（GPL/LGPL 等开源许可）构建，遵循相应开源协议。
