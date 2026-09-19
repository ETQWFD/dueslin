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

echo "[3/5] 复制 WIM 安装源 ..."
cp "$WIM" "$STAGE/boot/dueslin/DUESLIN.wim"

echo "[4/5] 生成 GRUB 菜单（英文标签，避免 GRUB 无中文字体导致乱码）..."
cat > "$STAGE/boot/grub/grub.cfg" <<'EOF'
set timeout=8
set default=0
insmod all_video
menuentry "DUESLIN Installer (graphical)" {
    linux /boot/dueslin/vmlinuz dueslin_mode=install rw quiet splash
    initrd /boot/dueslin/initramfs
}
menuentry "DUESLIN Installer (safe mode / VGA)" {
    linux /boot/dueslin/vmlinuz dueslin_mode=install rw nomodeset
    initrd /boot/dueslin/initramfs
}
menuentry "Boot from local disk" {
    exit
}
EOF

echo "[5/5] 生成 ISO ..."
grub-mkrescue -o "$OUTISO" "$STAGE" --xorriso=xorriso 2>&1 | tail -3
echo "完成: $OUTISO ($(du -h "$OUTISO" | cut -f1))"
