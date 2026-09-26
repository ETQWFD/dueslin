#!/bin/bash
# DUESLIN 三镜像重建（桌面 + 服务器 + 精简）— WIM 输出到 live 外，避免重复打包
set -e
BASE=/home/user/Doubao/chats/38442617987459330/dueslin-build
PROJ=/home/user/Doubao/chats/38442617987459330/dueslin-project
BLD=$PROJ/build
STAGE=$BASE/wim-stage
mkdir -p "$STAGE/desktop" "$STAGE/server" "$STAGE/min"

# 清理 live 内误放的 WIM（防止被打进 squashfs）
sudo rm -rf "$BASE/live/usr/share/dueslin/DUESLIN.wim"

echo "===== [1/3] 桌面版 ====="
cd "$BASE"
sudo rm -rf rootfs/dev rootfs/proc rootfs/sys rootfs/tmp rootfs/run rootfs/mnt
mkdir -p rootfs/dev rootfs/proc rootfs/sys rootfs/tmp rootfs/run rootfs/mnt
sudo "$BLD/make-wim.sh" rootfs "$STAGE/desktop"
cd "$BLD" && sudo bash build-iso.sh "$BASE/live" "$STAGE/desktop/DUESLIN.wim" "$BASE/iso/DUESLIN-1.0.iso"

echo "===== [2/3] 服务器版 ====="
cd "$BASE"
sudo rm -rf server/dev server/proc server/sys server/tmp server/run
mkdir -p server/dev server/proc server/sys server/tmp server/run
sudo "$BLD/make-wim.sh" server "$STAGE/server"
cd "$BLD" && sudo bash build-iso.sh "$BASE/live" "$STAGE/server/DUESLIN.wim" "$BASE/server-iso/DUESLIN-Server-1.0.iso"

echo "===== [3/3] 精简版 ====="
cd "$BASE"
sudo rm -rf min-rootfs/dev min-rootfs/proc min-rootfs/sys min-rootfs/tmp min-rootfs/run
mkdir -p min-rootfs/dev min-rootfs/proc min-rootfs/sys min-rootfs/tmp min-rootfs/run
sudo "$BLD/make-wim.sh" min-rootfs "$STAGE/min"
cd "$BLD" && sudo bash build-iso.sh "$BASE/live" "$STAGE/min/DUESLIN.wim" "$BASE/min-iso/DUESLIN-Mini-1.0.iso"

echo "===== 全部重建完成 ====="
ls -la "$BASE/iso/DUESLIN-1.0.iso" "$BASE/server-iso/DUESLIN-Server-1.0.iso" "$BASE/min-iso/DUESLIN-Mini-1.0.iso"
