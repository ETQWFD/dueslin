#!/bin/sh
# ============================================================
# DUESLIN initramfs init
# ============================================================

export PATH=/sbin:/bin:/usr/sbin:/usr/bin
export SHELL=/bin/sh

/bin/mount -t proc proc /proc 2>/dev/null
/bin/mount -t sysfs sysfs /sys 2>/dev/null
/bin/mount -t devtmpfs devtmpfs /dev 2>/dev/null

# ===== 蓝色启动画面 + 转圈 =====
printf '\033[2J\033[H\033[44m\033[1;37m'
SPIN='|/-\'
i=1
while [ $i -le 20 ]; do
    c=$(printf '%s' "$SPIN" | cut -c $((i%4+1)))
    printf '\033[H\033[2J'
    printf '\n\n\n\n\n'
    printf '                        +---------+\n'
    printf '                        |    D    |\n'
    printf '                        +---------+\n'
    printf '\n                           %s\n' "$c"
    printf '\n\n                        DUESLIN 1.0\n'
    i=$((i+1))
    sleep 0.15
done
printf '\033[0m'

# 从 cmdline 读取模式 与 root 设备
MODE="install"
ROOTDEV=""
for x in $(cat /proc/cmdline); do
    case "$x" in
        dueslin_mode=*) MODE="${x#dueslin_mode=}" ;;
        root=*) ROOTDEV="${x#root=}" ;;
    esac
done

# ===== 已安装系统引导: root 直接挂载 (不依赖 Alpine mkinitfs/nlplug) =====
if [ -n "$ROOTDEV" ] && [ "$ROOTDEV" != "/dev/ram0" ]; then
    printf '\033[2J\033[H\033[44m\033[1;37m  DUESLIN 1.0 - Booting installed system\033[0m\n'
    echo "[DUESLIN] root=$ROOTDEV"
    # 加载磁盘/文件系统驱动（阶段调试）
    for m in sd_mod sr_mod scsi_mod ahci libahci ata_piix ata_generic \
             usb-storage uas xhci-pci xhci-hcd ehci-pci nvme \
             virtio virtio_ring virtio_pci virtio_blk virtio_scsi virtio_net \
             vmw_pvscsi vmw_vmci vmw_vsock_vmci_transport \
             ext4 vfat nls_cp437 nls_iso8859-1 mbcache jbd2 crc16 crc32c \
             libcrc32c loop squashfs overlay; do
        [ -d "/lib/modules/$(uname -r)" ] && { echo "[DUESLIN] +mod $m"; /sbin/modprobe "$m" 2>/dev/null; }
    done
    echo "[DUESLIN] modprobe done"
    sleep 2
    /bin/mkdir -p /sysroot
    /bin/mount -t devtmpfs devtmpfs /dev 2>/dev/null
    /bin/mount -t proc proc /proc 2>/dev/null
    /bin/mount -t sysfs sysfs /sys 2>/dev/null
    echo "[DUESLIN] dev/proc/sys mounted"
    sleep 2
    echo "[DUESLIN] mounting $ROOTDEV -> /sysroot"
    if ! /bin/mount "$ROOTDEV" /sysroot 2>/dev/null; then
        echo "[DUESLIN] 无法挂载根设备 $ROOTDEV (rc=$?)"
        exec /bin/sh
    fi
    echo "[DUESLIN] root mounted OK"
    /bin/mount -t devtmpfs devtmpfs /sysroot/dev 2>/dev/null
    /bin/mount -t proc proc /sysroot/proc 2>/dev/null
    /bin/mount -t sysfs sysfs /sysroot/sys 2>/dev/null
    echo "[DUESLIN] switch_root 前检查:"
    chmod 755 /sysroot 2>/dev/null
    chmod +x /sysroot/sbin/init /sysroot/bin/busybox 2>/dev/null
    echo "[DUESLIN] switching root (chroot + 静态 init)..."
    # 用 initramfs 的静态 busybox 覆盖 /sbin/init（规避动态 ELF exec 的 EACCES 问题）
    /bin/cp /bin/busybox /sysroot/dueslin-bb 2>/dev/null
    /bin/chmod 755 /sysroot/dueslin-bb 2>/dev/null
    /bin/rm -f /sysroot/sbin/init
    /bin/ln -sf /dueslin-bb /sysroot/sbin/init
    exec /bin/chroot /sysroot /sbin/init
    echo "[DUESLIN] !! chroot init 失败"
fi

# 加载内核模块
for m in loop squashfs overlay cdrom iso9660 sd_mod sr_mod ahci libahci \
         ata_piix ata_generic usb-storage uas xhci-pci xhci-hcd ehci-pci \
         ehci-hcd ohci-pci ohci-hcd nvme \
         virtio virtio_ring virtio_pci virtio_blk virtio_scsi virtio_net \
         vmw_pvscsi vmw_vmci vmw_vsock_vmci_transport \
         ext4 vfat nls_cp437 nls_iso8859-1 \
         mbcache jbd2 crc16 crc32c libcrc32c \
         drm drm_kms_helper ttm sysfb simplefb fbdev fb_sys_fops \
         vmwgfx cirrus bochs drm_memory; do
    [ -d "/lib/modules/$(uname -r)" ] && /sbin/modprobe "$m" 2>/dev/null
done

find_image() {
    NEED="/boot/dueslin/live.squashfs"
    [ "$MODE" = "pe" ] && NEED="/boot/pe/pe.squashfs"
    /bin/mkdir -p /mnt/src
    for dev in $(ls /dev/sd[a-z]* /dev/sr[0-9]* /dev/nvme[0-9]n[0-9]p* 2>/dev/null); do
        for fs in iso9660 vfat ext4; do
            /bin/mount -t "$fs" -o ro "$dev" /mnt/src 2>/dev/null || continue
            if [ -f "/mnt/src/$NEED" ]; then
                SRC_DEV="$dev"
                return 0
            fi
            /bin/umount /mnt/src 2>/dev/null
        done
    done
    return 1
}

if ! find_image; then
    echo "[DUESLIN] 错误: 找不到系统镜像"
    exec /bin/sh
fi

/bin/mkdir -p /mnt/root /mnt/ovl /sysroot
/bin/mount -t squashfs -o loop,ro "/mnt/src/$NEED" /mnt/root || exec /bin/sh
/bin/mount -t tmpfs tmpfs /mnt/ovl 2>/dev/null
/bin/mkdir -p /mnt/ovl/up /mnt/ovl/work
/bin/mount -t overlay overlay -o lowerdir=/mnt/root,upperdir=/mnt/ovl/up,workdir=/mnt/ovl/work /sysroot || {
    /bin/mount --bind /mnt/root /sysroot; }

/bin/mkdir -p /sysroot/dev /sysroot/proc /sysroot/sys
/bin/mount -t devtmpfs devtmpfs /sysroot/dev 2>/dev/null
if [ ! -e /sysroot/dev/tty1 ]; then
    /bin/mknod -m 622 /sysroot/dev/console c 5 1 2>/dev/null
    /bin/mknod -m 666 /sysroot/dev/null c 1 3 2>/dev/null
    /bin/mknod -m 666 /sysroot/dev/tty c 5 0 2>/dev/null
    for n in 1 2 3 4 5 6; do
        /bin/mknod -m 620 /sysroot/dev/tty$n c 4 $n 2>/dev/null
    done
fi
/bin/mount -t proc proc /sysroot/proc 2>/dev/null
/bin/mount -t sysfs sysfs /sysroot/sys 2>/dev/null

exec /bin/switch_root /sysroot /sbin/init
