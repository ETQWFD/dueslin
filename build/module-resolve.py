#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""解析内核模块依赖：给定模块名列表，从 modules.dep 递归解析并复制到目标目录"""
import os
import re
import shutil
import sys

LIVE = sys.argv[1]        # live rootfs
STAGE = sys.argv[2]       # initramfs staging lib/modules dir
KVER = sys.argv[3]        # kernel version

MODS_DIR = f"{LIVE}/lib/modules/{KVER}"
DEP_FILE = f"{MODS_DIR}/modules.dep"
TARGET = f"{STAGE}/lib/modules/{KVER}"

NEED = ["loop", "squashfs", "overlay", "cdrom", "isofs", "sd_mod", "sr_mod",
        "ahci", "libahci", "ata_piix", "ata_generic", "usb-storage", "uas",
        "xhci-pci", "xhci-hcd", "ehci-pci", "ehci-hcd", "ohci-pci", "ohci-hcd",
        "nvme", "virtio", "virtio_ring", "virtio_pci", "virtio_blk",
        "virtio_scsi", "virtio_net", "vmw_pvscsi", "vmw_vmci",
        "vmw_vsock_vmci_transport", "ext4", "vfat", "nls_cp437", "nls_iso8859-1", "fuse",
        "drm", "drm_kms_helper", "ttm", "sysfb", "simplefb", "fbdev",
        "fb_sys_fops", "vmwgfx", "cirrus", "bochs", "drm_memory"]

os.makedirs(TARGET, exist_ok=True)

# 读取 modules.dep
deps = {}
with open(DEP_FILE) as f:
    for line in f:
        line = line.strip()
        if not line or ":" not in line:
            continue
        mod, _, rest = line.partition(":")
        deps[mod] = [d for d in rest.split() if d]

# 模块名 -> 文件路径
name_to_file = {}
for path in deps:
    base = os.path.basename(path)
    if base.endswith(".ko") or base.endswith(".ko.gz") or base.endswith(".ko.xz") or base.endswith(".ko.zst"):
        name_to_file[base.split(".")[0]] = path

# 递归收集
collected = set()

def resolve(name):
    if name in collected:
        return
    path = name_to_file.get(name)
    if not path:
        # 可能在子目录同名
        for p, lst in deps.items():
            if os.path.basename(p).startswith(name + ".ko"):
                path = p
                break
    if not path:
        print(f"  [skip] 找不到模块 {name}", file=sys.stderr)
        return
    collected.add(name)
    for dep in deps.get(path, []):
        dname = os.path.basename(dep).split(".")[0]
        resolve(dname)

for m in NEED:
    resolve(m)

# 复制
copied = 0
for name in collected:
    path = name_to_file[name]
    src = f"{MODS_DIR}/{path}"
    dst = f"{TARGET}/{path}"
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    copied += 1

# 复制辅助文件
for aux in ("modules.dep", "modules.builtin", "modules.alias", "modules.symbols", "modules.softdep"):
    src = f"{MODS_DIR}/{aux}"
    if os.path.exists(src):
        shutil.copy2(src, f"{TARGET}/{aux}")

print(f"解析完成: 复制 {copied} 个模块")
