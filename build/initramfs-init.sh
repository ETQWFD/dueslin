#!/bin/sh
# ============================================================
# DUESLIN initramfs init — 用于 ISO 安装环境 / 磁盘 PE 修复模式
# 启动流程: 挂载基础 -> 扫描系统镜像(iso9660/硬盘) -> 挂载 squashfs
#          -> overlay 可写层 -> switch_root 进入 DUESLIN 环境
# 内核参数:
#   dueslin_mode=install  安装模式（ISO）
#   dueslin_mode=pe       修复模式（已安装系统的 /boot/pe）
# ============================================================

export PATH=/sbin:/bin:/usr/sbin:/usr/bin
export SHELL=/bin/sh

/bin/mount -t proc proc /proc 2>/dev/null
/bin/mount -t sysfs sysfs /sys 2>/dev/null
/bin/mount -t devtmpfs devtmpfs /dev 2>/dev/null

# 从 cmdline 读取模式
MODE="install"
for x in $(cat /proc/cmdline); do
    case "$x" in
        dueslin_mode=*) MODE="${x#dueslin_mode=}" ;;
    esac
done
echo "[DUESLIN] 启动模式: $MODE"

# 加载基础内核模块（loop/squashfs/overlay/CD/USB/SATA/NVMe/ext4/vfat）
for m in loop squashfs overlay cdrom iso9660 sd_mod sr_mod ahci libahci \
         ata_piix ata_generic usb-storage uas xhci-pci xhci-hcd ehci-pci \
         ehci-hcd ohci-pci ohci-hcd nvme ext4 vfat nls_cp437 nls_iso8859-1 \
         mbcache jbd2 crc16 crc32c libcrc32c; do
    [ -d "/lib/modules/$(uname -r)" ] && /sbin/modprobe "$m" 2>/dev/null
done

find_image() {
    case "$MODE" in
        pe)   NEED="/boot/pe/pe.squashfs" ;;
        *)    NEED="/boot/dueslin/live.squashfs" ;;
    esac
    /bin/mkdir -p /mnt/src
    for dev in $(ls /dev/sd[a-z]* /dev/hd[a-z]* /dev/vd[a-z]* /dev/sr[0-9]* /dev/mmcblk*p* /dev/nvme[0-9]n[0-9]p* 2>/dev/null); do
        for fs in iso9660 vfat ext4 btrfs xfs; do
            /bin/mount -t "$fs" -o ro "$dev" /mnt/src 2>/dev/null || continue
            if [ -f "/mnt/src/$NEED" ]; then
                echo "[DUESLIN] 找到系统镜像: $dev -> $NEED"
                SRC_DEV="$dev"
                return 0
            fi
            /bin/umount /mnt/src 2>/dev/null
        done
    done
    return 1
}

if ! find_image; then
    echo "[DUESLIN] 错误: 找不到系统镜像 ($NEED)"
    echo "[DUESLIN] 请检查安装介质或 PE 分区是否完好"
    exec /bin/sh
fi

# 挂载只读 squashfs 为根
/bin/mkdir -p /mnt/root /mnt/ovl/up /mnt/ovl/work /sysroot
/bin/mount -t squashfs -o loop,ro "/mnt/src/$NEED" /mnt/root || {
    echo "[DUESLIN] 错误: squashfs 挂载失败"; exec /bin/sh; }

# overlay 可写层（失败则直接只读根）
/bin/mount -t tmpfs tmpfs /mnt/ovl 2>/dev/null
/bin/mount -t overlay overlay -o lowerdir=/mnt/root,upperdir=/mnt/ovl/up,workdir=/mnt/ovl/work /sysroot || {
    echo "[DUESLIN] overlay 失败，直接使用只读根"
    /bin/mount --bind /mnt/root /sysroot; }

# 在新根内部挂载 devtmpfs/proc/sys，保证 switch_root 后设备节点（/dev/tty1 等）可用
/bin/mkdir -p /sysroot/dev /sysroot/proc /sysroot/sys
/bin/mount -t devtmpfs devtmpfs /sysroot/dev 2>/dev/null
/bin/mount -t proc proc /sysroot/proc 2>/dev/null
/bin/mount -t sysfs sysfs /sysroot/sys 2>/dev/null

# 切换到真实环境（新根内已挂好 /dev /proc /sys，不再卸载旧根）
exec /bin/switch_root /sysroot /sbin/init
