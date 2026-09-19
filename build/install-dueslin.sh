#!/bin/bash
# ============================================================
# DUESLIN 系统集成脚本 — 将 DUESLIN 组件/配置写入构建环境
# 用法: install-dueslin.sh
# 环境变量: BASE 构建目录（默认本项目 dueslin-build）
# ============================================================
set -e
BASE="${BASE:-$(cd "$(dirname "$0")/../.." && pwd)/dueslin-build}"
ROOT="$BASE/rootfs"
LIVE="$BASE/live"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
CH="$BASE/chroot-helper.sh"

# 拷贝项目源码进 chroot（容器内 bind mount 受限）
copy_src() {
    rm -rf "$1/dueslin-src"
    cp -a "$SRC" "$1/dueslin-src"
}
clean_src() {
    rm -rf "$1/dueslin-src"
}

echo "==> 集成到完整系统 rootfs ($ROOT)"
copy_src "$ROOT"
$CH "$ROOT" /bin/sh -c '
set -e
# ---- 安装 DUESLIN 组件 ----
install -Dm755 /dueslin-src/src/dueslin          /usr/bin/dueslin
install -Dm755 /dueslin-src/src/dueslin-taskmgr   /usr/bin/dueslin-taskmgr
install -Dm755 /dueslin-src/src/dueslin-firstboot /usr/bin/dueslin-firstboot
install -Dm755 /dueslin-src/src/dueslin-pe        /usr/sbin/dueslin-pe
install -Dm644 /dueslin-src/system/grub.cfg.template /etc/dueslin/grub.cfg.template
mkdir -p /usr/share/dueslin
cp /dueslin-src/brand/logo.png /dueslin-src/brand/logo-48.png /dueslin-src/brand/wallpaper.png /usr/share/dueslin/
cp /dueslin-src/docs/COMMANDS.md /usr/share/dueslin/COMMANDS.md
# 终端仿真器别名
ln -sf /usr/bin/xfce4-terminal /usr/bin/x-terminal-emulator
# 应用菜单项
install -Dm644 /dueslin-src/desktop/dueslin-taskmgr.desktop /usr/share/applications/dueslin-taskmgr.desktop
install -Dm644 /dueslin-src/desktop/dueslin-help.desktop    /usr/share/applications/dueslin-help.desktop
install -Dm644 /dueslin-src/desktop/dueslin-firstboot.desktop /usr/share/xsessions/dueslin-firstboot.desktop
install -Dm644 /dueslin-src/desktop/dueslin-setup.desktop   /etc/xdg/autostart/dueslin-setup.desktop
install -Dm755 /dueslin-src/desktop/dueslin-setup-desktop   /usr/bin/dueslin-setup-desktop
# 桌面图标（桌面快捷方式）
mkdir -p /etc/skel/Desktop
cp /dueslin-src/desktop/dueslin-desktop.desktop /etc/skel/Desktop/ 2>/dev/null || true

# ---- 面板默认配置（Windows 风格底部任务栏） ----
install -Dm644 /dueslin-src/desktop/panel-default.xml /etc/xdg/xfce4/panel/default.xml

# ---- 用户 dueslin（默认管理账户） ----
grep -q "^dueslin:" /etc/passwd || useradd -m -s /usr/bin/dueslin -G wheel,video,audio,netdev,users,lp,disk,input,cdrom dueslin
echo "dueslin:dueslin" | chpasswd
echo "dueslin ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/dueslin
chmod 440 /etc/sudoers.d/dueslin

# ---- LightDM：首次启动自动登录进入首启向导 ----
cat > /etc/lightdm/lightdm.conf <<EOF
[Seat:*]
greeter-session=lightdm-gtk-greeter
user-session=dueslin-firstboot
autologin-user=dueslin
autologin-session=dueslin-firstboot
autologin-user-timeout=0
EOF
cat > /etc/lightdm/lightdm-gtk-greeter.conf <<EOF
[greeter]
theme-name=Paper
icon-theme-name=Adwaita
background=/usr/share/dueslin/wallpaper.png
font-name=Noto Sans CJK SC 10
user-wallpaper=/usr/share/dueslin/wallpaper.png
default-user-image=/usr/share/dueslin/logo.png
EOF

# ---- 主机名 ----
echo "DUESLIN" > /etc/hostname
grep -q "DUESLIN" /etc/hosts || sed -i "1i 127.0.0.1 DUESLIN localhost" /etc/hosts

# ---- 服务自启 ----
rc-update add dbus default 2>/dev/null || true
rc-update add eudev default 2>/dev/null || true
rc-update add elogind default 2>/dev/null || true
rc-update add networkmanager default 2>/dev/null || true
rc-update add bluetooth default 2>/dev/null || true
rc-update add alsa default 2>/dev/null || true
rc-update add lightdm default 2>/dev/null || true
rc-update add local default 2>/dev/null || true
rc-update del networking 2>/dev/null || true

# ---- 音频：ALSA 默认音量与 PipeWire 用户自启 ----
alsactl init 2>/dev/null || true
# ---- 首启标记 ----
mkdir -p /etc/dueslin
touch /etc/dueslin/.firstboot

# ---- 用户配置模板（skel） ----
mkdir -p /etc/skel/.config/xfce4/xfconf/xfce-perchannel-xml
cp /dueslin-src/desktop/xfsettingsd.xml /etc/skel/.config/xfce4/xfconf/xfce-perchannel-xml/ 2>/dev/null || true
echo done
'

echo "==> 同步 dueslin 用户初始配置"
$CH "$ROOT" /bin/sh -c '
cp -a /etc/skel/. /home/dueslin/ 2>/dev/null || true
chown -R dueslin:dueslin /home/dueslin
'
clean_src "$ROOT"

echo "==> 集成到 live/PE 环境 ($LIVE)"
copy_src "$LIVE"
$CH "$LIVE" /bin/sh -c '
set -e
install -Dm755 /dueslin-src/src/dueslin-installer /usr/bin/dueslin-installer
install -Dm755 /dueslin-src/src/dueslin-pe-menu  /usr/bin/dueslin-pe-menu
install -Dm755 /dueslin-src/src/dueslin          /usr/bin/dueslin
mkdir -p /usr/share/dueslin
cp /dueslin-src/brand/logo.png /dueslin-src/brand/logo-48.png /dueslin-src/brand/wallpaper.png /usr/share/dueslin/
# tty1 自动登录 root 并启动 X
install -Dm755 /dueslin-src/system/live-autologin.sh /root/autologin.sh
install -Dm755 /dueslin-src/system/live-xinitrc /root/.xinitrc
cat > /root/.profile <<EOF
if [ -z "\$DISPLAY" ] && [ "\$(tty)" = "/dev/tty1" ]; then
    exec startx
fi
EOF
# inittab: tty1 自动登录
sed -i "s|^tty1::.*|tty1::respawn:/sbin/getty -n -l /root/autologin.sh 38400 tty1|" /etc/inittab
echo "DUESLIN-LIVE" > /etc/hostname
rc-update add dbus default 2>/dev/null || true
rc-update add eudev default 2>/dev/null || true
rc-update add elogind default 2>/dev/null || true
rc-update del networking 2>/dev/null || true
echo done
'
clean_src "$LIVE"

echo "==> 集成完成"
