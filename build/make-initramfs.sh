#!/bin/bash
# ============================================================
# DUESLIN 构建脚本 1/3 —— 制作 live/PE 环境的 initramfs
# 用法: make-initramfs.sh <live_rootfs>
# 输出: <live_rootfs>/boot/dueslin-initramfs
# ============================================================
set -e
LIVE="${1:?用法: make-initramfs.sh <live_rootfs>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VER="$(ls "$LIVE/lib/modules" | head -1)"

echo "[1/3] 使用内核版本: $VER"
STAGE=$(mktemp -d)
mkdir -p "$STAGE/bin" "$STAGE/lib/modules/$VER" "$STAGE/etc" "$STAGE/root" "$STAGE/sbin"

# 1. busybox（必须用静态版 busybox.static，否则 /init 因缺 musl 加载器而无法启动）
STATIC_BB="$LIVE/bin/busybox.static"
[ -f "$STATIC_BB" ] || STATIC_BB="$LIVE/bin/busybox"
cp "$STATIC_BB" "$STAGE/bin/busybox"
cp "$SCRIPT_DIR/initramfs-init.sh" "$STAGE/init"
chmod +x "$STAGE/init" "$STAGE/bin/busybox"
# /bin 与 /sbin 都建符号链接，保证 init 里 /sbin/modprobe 等可找到
for d in bin sbin; do
    mkdir -p "$STAGE/$d"
    for a in sh mount umount modprobe switch_root mkdir ls cp uname cat exec mktemp echo sleep mv insmod rmmod lsmod poweroff reboot; do
        ln -sf ../bin/busybox "$STAGE/$d/$a" 2>/dev/null || true
    done
done

# 2. 解析并复制启动所需内核模块（含依赖）
echo "[2/3] 解析内核模块依赖 ..."
python3 "$SCRIPT_DIR/module-resolve.py" "$LIVE" "$STAGE" "$VER"
echo "    initramfs 模块大小: $(du -sh "$STAGE/lib/modules" | cut -f1)"

# 3. 打包 initramfs（cpio + gzip —— 兼容所有内核，避免 zstd 不支持导致 panic）
echo "[3/3] 打包 initramfs ..."
(cd "$STAGE" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > /tmp/dueslin-initramfs)
sudo install -m644 /tmp/dueslin-initramfs "$LIVE/boot/dueslin-initramfs"
rm -f /tmp/dueslin-initramfs
echo "完成: $LIVE/boot/dueslin-initramfs ($(sudo du -h "$LIVE/boot/dueslin-initramfs" | cut -f1))"
rm -rf "$STAGE"
