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

# 从 cmdline 读取模式
MODE="install"
for x in $(cat /proc/cmdline); do
    case "$x" in
        dueslin_mode=*) MODE="${x#dueslin_mode=}" ;;
    esac
done

# 加载内核模块
for m in loop squashfs overlay cdrom iso9660 sd_mod sr_mod ahci libahci \
         ata_piix ata_generic usb-storage uas xhci-pci xhci-hcd ehci-pci \
         ehci-hcd ohci-pci ohci-hcd nvme ext4 vfat nls_cp437 nls_iso8859-1 \
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
