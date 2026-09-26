#!/bin/bash
# ============================================================
# DUESLIN 构建脚本 2/3 —— 将完整系统打包为 .wim（WIM 压缩镜像）
# 用法: make-wim.sh <full_rootfs> <out_dir>
# 输出: <out_dir>/DUESLIN.wim
# 说明: 安装时使用 wimlib-imagex 将 .wim 解压复制到目标磁盘
# ============================================================
set -e
ROOT="${1:?用法: make-wim.sh <full_rootfs> <out_dir>}"
OUT="${2:?用法: make-wim.sh <full_rootfs> <out_dir>}"
mkdir -p "$OUT"

echo "[DUESLIN] 正在以 WIM(LZX) 格式压缩系统 ..."
echo "         源: $ROOT"

CFG=$(mktemp)
cat > "$CFG" <<'EOCFG'
[ExclusionList]
/proc/*
/sys/*
/dev/*
/tmp/*
/run/*
/mnt/*
/media/*
/var/cache/*
/var/log/*
/var/tmp/*
/root/.cache/*
/boot/grub/*
/dueslin-src/*
EOCFG
sudo wimlib-imagex capture "$ROOT" "$OUT/DUESLIN.wim" DUESLIN "DUESLIN 1.0 系统镜像" \
    --compress=LZX --config="$CFG" --unix-data
rm -f "$CFG"

echo "完成: $OUT/DUESLIN.wim ($(du -h "$OUT/DUESLIN.wim" | cut -f1))"
