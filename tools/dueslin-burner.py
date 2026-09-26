#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DUESLIN 镜像下载刻录工具 (Microsoft Media Creation Tool 风格向导)
流程: 选镜像 -> 下载目录 -> 刻录模式 -> 目标设备 -> 确认 -> 下载+刻录 -> 完成
跨平台: Linux(macOS) 直接 dd; Windows 需要管理员权限原始写入
打包: pyinstaller --onefile --windowed --icon=logo.ico --name DueslinBurner dueslin-burner.py
"""
import os, sys, json, time, shutil, subprocess, threading, urllib.request, glob, platform

REPO = "ETQWFD/dueslin"
RELEASE_API = f"https://api.github.com/repos/{REPO}/releases/latest"

# 六个镜像: (显示名, Release asset 文件名)
ISOS = [
    ("DUESLIN 桌面版 (x64)  Desktop x64",       "DUESLIN-1.0.iso"),
    ("DUESLIN 桌面版 (ARM64)  Desktop ARM64",   "DUESLIN-1.0-ARM64.iso"),
    ("DUESLIN 服务器版 (x64)  Server x64",      "DUESLIN-Server-1.0.iso"),
    ("DUESLIN 服务器版 (ARM64) Server ARM64",   "DUESLIN-Server-1.0-ARM64.iso"),
    ("DUESLIN 精简版 (x64)  Mini x64",          "DUESLIN-Mini-1.0.iso"),
    ("DUESLIN 精简版 (ARM64) Mini ARM64",       "DUESLIN-Mini-1.0-ARM64.iso"),
]

BRAND = "#0067B8"   # Windows 蓝
BRAND_DK = "#005A9E"
BG = "#F3F3F3"

def resource_path(rel):
    try:
        base = sys._MEIPASS
    except Exception:
        base = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(base, rel)

def is_windows():
    return platform.system() == "Windows"

def list_usb_devices():
    """列出可写块设备 (Linux)"""
    devs = []
    if is_windows():
        return devs
    for pat in ["/dev/sd[a-z]", "/dev/vd[a-z]", "/dev/mmcblk[0-9]"]:
        for d in sorted(glob.glob(pat)):
            if d.endswith(("0","1")) and os.path.exists(d):
                try:
                    size = int(subprocess.run(["blockdev","--getsize64",d],capture_output=True,text=True).stdout.strip())
                    if size > 2*1024*1024*1024:  # >2GB 才可能是 U 盘/移动盘
                        removable = os.path.exists(f"/sys/block/{os.path.basename(d)}/removable")
                        lbl = "可移动设备 Removable" if removable else "固定磁盘 Fixed"
                        devs.append((d, f"{d}  ({size//(1024**3)}GB) {lbl}"))
                except Exception:
                    pass
    return devs

def list_dvd_drives():
    if is_windows():
        return [("D:", "DVD 光驱 (D:)")]
    drives = []
    for d in sorted(glob.glob("/dev/sr[0-9]")):
        drives.append((d, f"{d}  光盘驱动器"))
    return drives

def dd_write(iso, dev, progress_cb, cancel_event):
    """Linux: 用 dd 写入; 返回 (ok, err)"""
    if is_windows():
        return windows_raw_write(iso, dev, progress_cb)
    total = os.path.getsize(iso)
    proc = subprocess.Popen(["dd", f"if={iso}", f"of={dev}", "bs=4M", "status=none"],
                            stderr=subprocess.PIPE, stdout=subprocess.DEVNULL)
    # dd 无进度，轮询目标设备已写字节不可行 → 用总大小倒计时
    while proc.poll() is None:
        progress_cb(50)  # 显示进行中
        time.sleep(1)
    if proc.returncode == 0:
        progress_cb(100)
        return True, ""
    return False, proc.stderr.read().decode(errors="ignore")

def windows_raw_write(iso, dev, progress_cb):
    """Windows: 原始写入物理磁盘 (需管理员)"""
    import ctypes
    from ctypes import wintypes
    try:
        drive = dev.rstrip("\\").rstrip(":")  # e.g. "E"
        handle = ctypes.windll.kernel32.CreateFileW(
            f"\\\\.\\{drive}:", 0xC0000000, 0, None, 3, 0, None)
        if handle == -1 or handle == 0:
            return False, "无法打开物理磁盘，请以管理员身份运行 (Right-click -> Run as administrator)"
        bufsz = 4*1024*1024
        with open(iso, "rb") as f:
            written = 0
            total = os.path.getsize(iso)
            while True:
                chunk = f.read(bufsz)
                if not chunk:
                    break
                buf = ctypes.create_string_buffer(chunk)
                nw = wintypes.DWORD(0)
                ok = ctypes.windll.kernel32.WriteFile(handle, buf, len(chunk), ctypes.byref(nw), None)
                if not ok:
                    ctypes.windll.kernel32.CloseHandle(handle)
                    return False, "写入失败，可能磁盘被占用或权限不足"
                written += nw.value
                progress_cb(int(written/total*100))
        ctypes.windll.kernel32.FlushFileBuffers(handle)
        ctypes.windll.kernel32.CloseHandle(handle)
        progress_cb(100)
        return True, ""
    except Exception as e:
        return False, str(e)

# ============================================================ UI
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

class BurnerApp:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("DUESLIN 镜像下载与刻录工具")
        self.root.geometry("760x540")
        self.root.minsize(720, 500)
        self.root.configure(bg=BG)
        try:
            self.root.iconbitmap(resource_path("logo.ico"))
        except Exception:
            pass
        # 微软风格字体
        self.f_title = ("Microsoft YaHei UI", 13, "bold") if is_windows() else ("Sans", 12, "bold")
        self.f_body = ("Microsoft YaHei UI", 10) if is_windows() else ("Sans", 10)
        # 状态
        self.step = 0
        self.iso_choice = tk.StringVar(value=ISOS[0][0])
        self.dl_dir = tk.StringVar(value=os.path.expanduser("~/Downloads"))
        self.mode = tk.StringVar(value="USB")
        self.device = tk.StringVar()
        self.confirm = tk.StringVar()
        self.release_tag = "v1.0.0"
        self.assets = {}
        self.downloading = False
        self._build()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build(self):
        main = tk.Frame(self.root, bg=BG)
        main.pack(fill=tk.BOTH, expand=True)
        # 左侧品牌条
        side = tk.Frame(main, bg=BRAND, width=200)
        side.pack(side=tk.LEFT, fill=tk.Y)
        side.pack_propagate(False)
        logo = tk.Label(side, text="D", font=("Arial Black", 40, "bold"),
                        fg="#000000", bg=BRAND)
        logo.pack(pady=(30, 6))
        tk.Label(side, text="DUESLIN", font=("Segoe UI", 18, "bold"),
                 fg="white", bg=BRAND).pack()
        tk.Label(side, text="镜像下载与刻录工具", font=("Segoe UI", 9),
                 fg="#D4E6FB", bg=BRAND).pack(pady=(4, 20))
        self.side_steps = []
        for i, s in enumerate(["选择镜像", "下载目录", "刻录模式", "目标设备", "确认", "完成"]):
            lb = tk.Label(side, text=f"  {i+1}. {s}", font=("Segoe UI", 9),
                          fg="#B8D4F0", bg=BRAND, anchor="w")
            lb.pack(fill=tk.X, padx=16, pady=3)
            self.side_steps.append(lb)
        ver = tk.Label(side, text="Version 2.0", font=("Segoe UI", 8),
                       fg="#9CC4E8", bg=BRAND)
        ver.pack(side=tk.BOTTOM, pady=12)

        # 右侧内容区
        right = tk.Frame(main, bg="white")
        right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.head = tk.Label(right, text="", font=self.f_title, fg="#1a1a1a",
                             bg="white", anchor="w")
        self.head.pack(fill=tk.X, padx=28, pady=(22, 2))
        self.sub = tk.Label(right, text="", font=self.f_body, fg="#666666",
                            bg="white", anchor="w")
        self.sub.pack(fill=tk.X, padx=28, pady=(0, 12))
        self.content = tk.Frame(right, bg="white")
        self.content.pack(fill=tk.BOTH, expand=True, padx=28)
        # 底部按钮
        btns = tk.Frame(right, bg="#F0F0F0", height=56)
        btns.pack(fill=tk.X, side=tk.BOTTOM)
        btns.pack_propagate(False)
        self.btn_back = tk.Button(btns, text="< 上一步", command=self._back,
                                  font=self.f_body, bg="#F0F0F0", relief=tk.FLAT,
                                  padx=20, pady=6, state=tk.DISABLED)
        self.btn_back.pack(side=tk.RIGHT, padx=(0, 8), pady=10)
        self.btn_next = tk.Button(btns, text="下一步  >", command=self._next,
                                  font=self.f_body, bg=BRAND, fg="white",
                                  relief=tk.FLAT, activebackground=BRAND_DK,
                                  activeforeground="white", padx=24, pady=6)
        self.btn_next.pack(side=tk.RIGHT, padx=8, pady=10)
        tk.Button(btns, text="取消", command=self._on_close, font=self.f_body,
                  bg="#F0F0F0", relief=tk.FLAT, padx=20, pady=6).pack(side=tk.RIGHT, padx=8)
        self._render()

    def _clear_content(self):
        for w in self.content.winfo_children():
            w.destroy()
        for i, lb in enumerate(self.side_steps):
            lb.configure(fg="#FFFFFF" if i == self.step else "#B8D4F0",
                         font=("Segoe UI", 9, "bold") if i == self.step else ("Segoe UI", 9))

    def _set_head(self, t, s):
        self.head.configure(text=t)
        self.sub.configure(text=s)

    # ---------------- 页面 ----------------
    def _render(self):
        self._clear_content()
        self.btn_back.configure(state=tk.NORMAL if self.step > 0 else tk.DISABLED)
        self.btn_next.configure(text="下一步  >", command=self._next,
                                state=tk.NORMAL)
        if self.step == 0: self._page_iso()
        elif self.step == 1: self._page_dir()
        elif self.step == 2: self._page_mode()
        elif self.step == 3: self._page_device()
        elif self.step == 4: self._page_confirm()
        elif self.step == 5: self._page_progress()

    def _page_iso(self):
        self._set_head("选择要下载和刻录的镜像", "Select the DUESLIN image to download and burn")
        self.release_lbl = tk.Label(self.content, text="正在查询最新版本...",
                                    font=self.f_body, fg="#666666", bg="white")
        self.release_lbl.pack(anchor="w", pady=(0, 10))
        for name, _f in ISOS:
            row = tk.Frame(self.content, bg="white")
            row.pack(fill=tk.X, pady=2)
            rb = tk.Radiobutton(row, text=name, variable=self.iso_choice, value=name,
                                font=self.f_body, bg="white", anchor="w", selectcolor="#E8F0FE")
            rb.pack(fill=tk.X)
        threading.Thread(target=self._fetch_release, daemon=True).start()

    def _fetch_release(self):
        try:
            req = urllib.request.Request(RELEASE_API, headers={"User-Agent": "DueslinBurner"})
            data = json.loads(urllib.request.urlopen(req, timeout=20).read())
            self.release_tag = data.get("tag_name", "v1.0.0")
            self.assets = {a["name"]: a["browser_download_url"] for a in data.get("assets", [])}
            self.root.after(0, lambda: self.release_lbl.configure(
                text=f"最新版本: {self.release_tag}   (GitHub Release)"))
        except Exception as e:
            self.root.after(0, lambda: self.release_lbl.configure(
                text=f"无法连接 GitHub（使用固定版本 v1.0.0）：{e}"))

    def _page_dir(self):
        self._set_head("选择镜像下载目录", "Choose where to save the downloaded image")
        f = tk.Frame(self.content, bg="white")
        f.pack(fill=tk.X, pady=20)
        e = tk.Entry(f, textvariable=self.dl_dir, font=self.f_body, width=46)
        e.pack(side=tk.LEFT, fill=tk.X, expand=True, ipady=4)
        tk.Button(f, text="浏览...", command=self._browse_dir, font=self.f_body,
                  bg="#F0F0F0", relief=tk.FLAT, padx=14, pady=5).pack(side=tk.LEFT, padx=(8, 0))

    def _browse_dir(self):
        d = filedialog.askdirectory()
        if d:
            self.dl_dir.set(d)

    def _page_mode(self):
        self._set_head("选择刻录模式", "Choose how to create the installation media")
        tk.Radiobutton(self.content, text="U 盘 / USB 闪存盘  (ISO 写入)", variable=self.mode,
                       value="USB", font=self.f_body, bg="white", anchor="w",
                       selectcolor="#E8F0FE").pack(fill=tk.X, pady=6)
        tk.Radiobutton(self.content, text="光盘 / DVD 光盘", variable=self.mode,
                       value="DVD", font=self.f_body, bg="white", anchor="w",
                       selectcolor="#E8F0FE").pack(fill=tk.X, pady=6)
        tk.Label(self.content, text="\nU 盘模式：将镜像原样写入 U 盘（用于从 U 盘启动安装）。\nDVD 模式：需要光盘刻录机，请提前准备空白光盘。",
                 font=self.f_body, fg="#888888", bg="white", justify=tk.LEFT).pack(pady=10, anchor="w")

    def _page_device(self):
        mode = "U 盘" if self.mode.get() == "USB" else "光盘"
        self._set_head(f"选择目标{mode}", f"Select the target {mode.lower()}")
        bar = tk.Frame(self.content, bg="white")
        bar.pack(fill=tk.X, pady=8)
        tk.Button(bar, text="刷新 Refresh", command=self._refresh_devices,
                  font=self.f_body, bg="#F0F0F0", relief=tk.FLAT,
                  padx=14, pady=4).pack(side=tk.RIGHT)
        self.dev_frame = tk.Frame(self.content, bg="white")
        self.dev_frame.pack(fill=tk.BOTH, expand=True)
        self._refresh_devices()

    def _refresh_devices(self):
        for w in self.dev_frame.winfo_children():
            w.destroy()
        devs = list_usb_devices() if self.mode.get() == "USB" else list_dvd_drives()
        if not devs:
            tk.Label(self.dev_frame, text="未检测到可用设备，请插入并点击刷新\nNo device found - insert media and refresh",
                     font=self.f_body, fg="#999999", bg="white").pack(pady=30)
            return
        for dev, label in devs:
            tk.Radiobutton(self.dev_frame, text=label, variable=self.device, value=dev,
                           font=self.f_body, bg="white", anchor="w",
                           selectcolor="#E8F0FE").pack(fill=tk.X, pady=3)

    def _page_confirm(self):
        self._set_head("确认格式化", "All data on the target device will be ERASED")
        dev = self.device.get() or "(未选择)"
        warn = tk.Label(self.content,
                        text=f"即将格式化并写入:\n  {dev}\n\n该设备上的所有数据将被永久删除，且无法恢复！\nALL DATA WILL BE LOST!",
                        font=self.f_body, fg="#C00000", bg="white", justify=tk.LEFT)
        warn.pack(anchor="w", pady=12)
        f = tk.Frame(self.content, bg="white")
        f.pack(fill=tk.X, pady=8)
        tk.Label(f, text="请输入 YES 以继续: ", font=self.f_body, bg="white").pack(side=tk.LEFT)
        e = tk.Entry(f, textvariable=self.confirm, font=self.f_body, width=12)
        e.pack(side=tk.LEFT, ipady=3)

    def _page_progress(self):
        self._set_head("正在下载并刻录...", "Downloading and writing the image")
        self.progress = ttk.Progressbar(self.content, length=560, mode="determinate")
        self.progress.pack(pady=24)
        self.pct = tk.Label(self.content, text="0%", font=("Segoe UI", 12, "bold"),
                            fg=BRAND, bg="white")
        self.pct.pack()
        self.status = tk.Label(self.content, text="准备中...", font=self.f_body,
                               fg="#666666", bg="white")
        self.status.pack(pady=8)
        self.btn_next.configure(state=tk.DISABLED)
        self.btn_back.configure(state=tk.DISABLED)
        threading.Thread(target=self._run_burn, daemon=True).start()

    def _run_burn(self):
        try:
            sel = next(n for n, f in ISOS if n == self.iso_choice.get())
            fname = sel[1]
            iso_path = os.path.join(self.dl_dir.get(), fname)
            # 1. 下载
            url = self.assets.get(fname)
            if not url:
                url = f"https://github.com/{REPO}/releases/download/{self.release_tag}/{fname}"
            self.root.after(0, lambda: self.status.configure(text=f"正在下载 {fname} ..."))
            if not os.path.exists(iso_path) or os.path.getsize(iso_path) < 1024*1024:
                req = urllib.request.Request(url, headers={"User-Agent": "DueslinBurner"})
                with urllib.request.urlopen(req, timeout=60) as r, open(iso_path, "wb") as f:
                    total = int(r.headers.get("Content-Length", 0))
                    done = 0
                    while True:
                        chunk = r.read(1024*1024)
                        if not chunk: break
                        f.write(chunk); done += len(chunk)
                        if total:
                            p = int(done/total*50)
                            self.root.after(0, lambda p=p: (self.progress.configure(value=p),
                                                            self.pct.configure(text=f"{p}%")))
            self.root.after(0, lambda: self.status.configure(text="下载完成，正在写入设备..."))
            # 2. 刻录
            ok, err = dd_write(iso_path, self.device.get(),
                               lambda p: self.root.after(0, lambda p=p: (
                                   self.progress.configure(value=50+p//2),
                                   self.pct.configure(text=f"{50+p//2}%"))),
                               None)
            if ok:
                self.root.after(0, lambda: self.status.configure(
                    text="完成！安装介质已就绪。"))
                self.root.after(0, lambda: (self.progress.configure(value=100),
                                            self.pct.configure(text="100%")))
                self.root.after(1200, self._done)
            else:
                self.root.after(0, lambda: messagebox.showerror("刻录失败", err))
                self.root.after(0, self._reset_from_error)
        except Exception as e:
            self.root.after(0, lambda: messagebox.showerror("错误", f"操作失败: {e}"))
            self.root.after(0, self._reset_from_error)

    def _done(self):
        self.step = 5
        self._render()
        self._set_head("完成", "Your installation media is ready")
        tk.Label(self.content, text="✓ 安装介质已成功创建！\n\n现在可以将其插入目标电脑，\n从 U 盘 / 光盘启动并安装 DUESLIN。",
                 font=("Segoe UI", 14, "bold"), fg="#107C10", bg="white",
                 justify=tk.LEFT).pack(pady=30)
        self.btn_back.configure(state=tk.DISABLED)
        self.btn_next.configure(text="关闭", command=self._on_close)

    def _reset_from_error(self):
        self.step = 3
        self._render()

    # ---------------- 导航 ----------------
    def _next(self):
        if self.step == 0:
            self.step = 1
        elif self.step == 1:
            d = self.dl_dir.get()
            if not os.path.isdir(d):
                messagebox.showwarning("提示", "下载目录无效，请重新选择")
                return
            self.step = 2
        elif self.step == 2:
            self.step = 3
        elif self.step == 3:
            if not self.device.get():
                messagebox.showwarning("提示", "请先选择目标设备")
                return
            self.step = 4
        elif self.step == 4:
            if self.confirm.get().strip() != "YES":
                messagebox.showwarning("提示", "请输入大写 YES 以确认格式化")
                return
            self.step = 5
        self._render()

    def _back(self):
        if self.step > 0:
            self.step -= 1
            self._render()

    def _on_close(self):
        if messagebox.askokcancel("退出", "确定要退出吗？"):
            self.root.destroy()

    def run(self):
        self.root.mainloop()

def main():
    app = BurnerApp()
    app.run()

if __name__ == "__main__":
    main()
