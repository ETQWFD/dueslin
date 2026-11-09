#!/bin/bash
# ============================================================
# DUESLIN 构建脚本 3/3 —— 制作可启动 ISO（BIOS + UEFI 双支持）
# 用法: build-iso.sh <live_rootfs> <wim_file> <out_iso>
# ISO 结构:
#   /boot/grub/          GRUB 引导（BIOS+EFI）
#   /boot/dueslin/       内核、initramfs、live.squashfs、DUESLIN.wim
# ============================================================
set -e
LIVE="${1:?用法: build-iso.sh <live_rootfs> <wim_file> <out_iso>}"
WIM="${2:?用法: build-iso.sh <live_rootfs> <wim_file> <out_iso>}"
OUTISO="${3:?用法: build-iso.sh <live_rootfs> <wim_file> <out_iso>}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

echo "[1/5] 准备 ISO 目录结构 ..."
mkdir -p "$STAGE/boot/grub" "$STAGE/boot/dueslin"
cp "$LIVE/boot/vmlinuz-lts"    "$STAGE/boot/dueslin/vmlinuz"
cp "$LIVE/boot/dueslin-initramfs" "$STAGE/boot/dueslin/initramfs"

echo "[2/5] 压缩 live 环境为 squashfs ..."
mksquashfs "$LIVE" "$STAGE/boot/dueslin/live.squashfs" \
    -comp xz -b 1M \
    -e proc sys dev tmp run mnt media boot var/cache root/.cache \
    -noappend

echo "[3/5] 复制 WIM 安装源 + 引导配对文件 ..."
cp "$WIM" "$STAGE/boot/dueslin/DUESLIN.wim"
# 引导配对：boot.img 与 core.img 必须同版本，安装器直接 dd（不再依赖 grub-install）
if [ -f "$LIVE/usr/share/dueslin/boot.img" ] && [ -f "$LIVE/usr/share/dueslin/core.img" ]; then
    mkdir -p "$STAGE/dueslin"
    cp "$LIVE/usr/share/dueslin/boot.img" "$STAGE/dueslin/boot.img"
    cp "$LIVE/usr/share/dueslin/core.img" "$STAGE/dueslin/core.img"
    echo "  引导配对已打包: boot.img + core.img"
    # GRUB 模块目录（cos 需要 normal.mod 等），安装器复制进 ESP
    mkdir -p "$STAGE/dueslin/grub-modules"
    cp /usr/lib/grub/i386-pc/* "$STAGE/dueslin/grub-modules/" 2>/dev/null || true
    echo "  GRUB 模块 $(ls "$STAGE/dueslin/grub-modules" | wc -l) 个"
fi

echo "[4/5] 生成 GRUB 菜单（英文标签，避免 GRUB 无中文字体导致乱码）..."
if [ -f "$LIVE/boot/dueslin-boot.elf" ]; then
    cp "$LIVE/boot/dueslin-boot.elf" "$STAGE/boot/dueslin/dueslin-boot.elf"
    cat > "$STAGE/boot/grub/grub.cfg" <<'EOF'
set timeout=3
set default=${DUESLIN_GRUB_DEFAULT:-1}
insmod all_video
insmod multiboot
menuentry "DUESLIN Boot Kernel (self-made)" {
    multiboot /boot/dueslin/dueslin-boot.elf
}
menuentry "DUESLIN 1.0_ET Installer (graphical)" {
    linux /boot/dueslin/vmlinuz dueslin_mode=install rw quiet loglevel=3
    initrd /boot/dueslin/initramfs
}
menuentry "DUESLIN 1.0_ET Installer (auto / unattended)" {
    linux /boot/dueslin/vmlinuz dueslin_mode=install rw quiet loglevel=3 autoinstall
    initrd /boot/dueslin/initramfs
}
menuentry "DUESLIN 1.0_ET Installer (safe mode / VGA)" {
    linux /boot/dueslin/vmlinuz dueslin_mode=install rw quiet loglevel=3
    initrd /boot/dueslin/initramfs
}
menuentry "Boot from local disk" {
    exit
}
EOF
else
    cat > "$STAGE/boot/grub/grub.cfg" <<'EOF'
set timeout=8
set default=${DUESLIN_GRUB_DEFAULT:-0}
insmod all_video
menuentry "DUESLIN 1.0_ET Installer (graphical)" {
    linux /boot/dueslin/vmlinuz dueslin_mode=install rw quiet loglevel=3
    initrd /boot/dueslin/initramfs
}
menuentry "DUESLIN 1.0_ET Installer (auto / unattended)" {
    linux /boot/dueslin/vmlinuz dueslin_mode=install rw quiet loglevel=3 autoinstall
    initrd /boot/dueslin/initramfs
}
menuentry "DUESLIN 1.0_ET Installer (safe mode / VGA)" {
    linux /boot/dueslin/vmlinuz dueslin_mode=install rw quiet loglevel=3
    initrd /boot/dueslin/initramfs
}
menuentry "Boot from local disk" {
    exit
}
EOF
fi

# 展开 GRUB 默认启动项（VM 版传 DUESLIN_GRUB_DEFAULT=2 直进 auto 安装）
sed -i 's/\${DUESLIN_GRUB_DEFAULT:-\?[01]}/'"${DUESLIN_GRUB_DEFAULT:-1}"'/g' "$STAGE/boot/grub/grub.cfg"

echo "[5/5] 生成 ISO ..."
grub-mkrescue -o "$OUTISO" "$STAGE" --xorriso=xorriso 2>&1 | tail -3
echo "完成: $OUTISO ($(du -h "$OUTISO" | cut -f1))"
