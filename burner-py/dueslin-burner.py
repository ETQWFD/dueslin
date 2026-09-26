#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DUESLIN 镜像刻录器 3.0 (Windows / Linux) — Python + Tkinter
- 宽字符原生支持，中文无乱码
- 蓝白主题 + 微软雅黑风格，界面美观
- 仅 3 个镜像：桌面版 / 服务器版 / 精简版
- 下载带 3 次重试 + 实时进度，连接 GitHub Release 稳定
- 选择设备 -> 输入 YES -> 下载 -> 无损刻录 U 盘
打包（Windows EXE，含图标与作者信息）:
  pip install pyinstaller
  pyinstaller --onefile --windowed --icon=logo.ico --name=DueslinBurner \
      --version-file=version_info.txt dueslin-burner.py
"""
import os
import sys
import time
import urllib.request
import ssl
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

APP_TITLE = "DUESLIN 镜像刻录器 3.0"
AUTHOR = "etc"
VERSION = "3.0.0"
REPO = "https://github.com/ETQWFD/dueslin/releases/download/v1.0.0"

IMAGES = [
    ("桌面版 (DUESLIN-1.0.iso)",          "DUESLIN-1.0.iso"),
    ("服务器版 (DUESLIN-Server-1.0.iso)", "DUESLIN-Server-1.0.iso"),
    ("精简版 (DUESLIN-Mini-1.0.iso)",     "DUESLIN-Mini-1.0.iso"),
]

BG = "#F3F6FB"
ACCENT = "#1A73E8"
TEXT = "#1F2937"
SUB = "#6B7280"

# ---------- 下载（3 次重试 + 进度） ----------
def download(url, dest, progress_cb, status_cb):
    last_err = None
    for attempt in range(1, 4):
        status_cb(f"正在下载（第 {attempt}/3 次尝试）...")
        try:
            ctx = ssl.create_default_context()
            req = urllib.request.Request(url, headers={"User-Agent": "DueslinBurner/3.0"})
            with urllib.request.urlopen(req, timeout=30, context=ctx) as resp:
                total = int(resp.headers.get("Content-Length", 0) or 0)
                got = 0
                tmp = dest + ".part"
                with open(tmp, "wb") as f:
                    while True:
                        chunk = resp.read(1 << 20)
                        if not chunk:
                            break
                        f.write(chunk)
                        got += len(chunk)
                        if total:
                            progress_cb(int(got * 100 / total))
                if total and got < total:
                    raise IOError(f"下载不完整 {got}/{total}")
            os.replace(tmp, dest)
            progress_cb(100)
            return True
        except Exception as e:
            last_err = e
            status_cb(f"连接中断（{e}），自动重试...")
            time.sleep(2)
    status_cb(f"下载失败：{last_err}")
    return False

# ---------- Windows 物理盘枚举 ----------
def list_windows_disks():
    """Windows 下用 wmic 或 PowerShell 枚举物理磁盘（仅可移动/固定磁盘）"""
    disks = []
    try:
        import subprocess
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-Disk | Select-Object Number,@{n='SizeGB';e={[math]::Round($_.Size/1GB,1)}},Model,FriendlyName | ConvertTo-Json -Compress"],
            capture_output=True, text=True, timeout=20)
        import json
        data = json.loads(r.stdout or "[]")
        if isinstance(data, dict):
            data = [data]
        for d in data:
            disks.append((d.get("Number", 0), f"物理磁盘 {d.get('Number',0)} - {d.get('FriendlyName') or d.get('Model') or '磁盘'} ({d.get('SizeGB', '?')}G)"))
    except Exception:
        pass
    return disks

# ---------- Linux 磁盘枚举 ----------
def list_linux_disks():
    disks = []
    try:
        for name in sorted(os.listdir("/sys/block")):
            if name.startswith(("loop", "ram", "sr", "fd", "dm-", "zram")):
                continue
            if not os.path.exists(f"/dev/{name}"):
                continue
            try:
                with open(f"/sys/block/{name}/size") as f:
                    gb = int(f.read().strip()) * 512 / (1024 ** 3)
            except Exception:
                gb = 0
            model = ""
            for m in ("device/model", "device/vendor"):
                try:
                    with open(f"/sys/block/{name}/{m}") as f:
                        model += f.read().strip() + " "
                except Exception:
                    pass
            disks.append((name, f"/dev/{name} - {model.strip() or '磁盘'} ({gb:.1f}G)"))
    except Exception:
        pass
    return disks

# ---------- 主窗口 ----------
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.configure(bg=BG)
        self.geometry("520x520")
        self.minsize(520, 520)
        self.step = 0
        self.img_idx = 0
        self.download_dir = os.path.expanduser("~")
        self.mode_iso = tk.BooleanVar(value=True)
        self.devices = []
        self._build()

    def _style(self):
        st = ttk.Style(self)
        try:
            st.theme_use("clam")
        except Exception:
            pass
        st.configure("TButton", font=("Microsoft YaHei UI", 11, "bold"), padding=8)
        st.map("TButton",
               background=[("active", "#1557B0")],
               foreground=[("active", "white")])
        st.configure("Accent.TButton", background=ACCENT, foreground="white")
        st.configure("Ghost.TButton", background="#E5E7EB", foreground=TEXT)
        st.configure("TLabel", background=BG, foreground=TEXT, font=("Microsoft YaHei UI", 11))
        st.configure("Title.TLabel", background=BG, foreground=TEXT, font=("Microsoft YaHei UI", 17, "bold"))
        st.configure("Sub.TLabel", background=BG, foreground=SUB, font=("Microsoft YaHei UI", 10))
        st.configure("TCheckbutton", background=BG, foreground=TEXT, font=("Microsoft YaHei UI", 11))
        st.configure("TRadiobutton", background=BG, foreground=TEXT, font=("Microsoft YaHei UI", 11))
        st.configure("TCombobox", font=("Microsoft YaHei UI", 11))

    def _build(self):
        self._style()
        # 顶部蓝条
        top = tk.Frame(self, bg=ACCENT, height=52)
        top.pack(fill="x")
        tk.Label(top, text="  DUESLIN 镜像刻录器", bg=ACCENT, fg="white",
                 font=("Microsoft YaHei UI", 15, "bold")).pack(side="left", padx=12, pady=10)
        self.step_lbl = tk.Label(top, text="", bg=ACCENT, fg="#DCE9FF",
                                 font=("Microsoft YaHei UI", 10))
        self.step_lbl.pack(side="right", padx=14)

        self.body = tk.Frame(self, bg=BG)
        self.body.pack(fill="both", expand=True, padx=28, pady=18)
        self.status_lbl = tk.Label(self, text="", bg=BG, fg=ACCENT,
                                   font=("Microsoft YaHei UI", 10))
        self.status_lbl.pack(fill="x", padx=28)
        self.prog = ttk.Progressbar(self, length=460, mode="determinate")
        self.prog.pack(padx=28, pady=(4, 10))
        self.btnbar = tk.Frame(self, bg=BG)
        self.btnbar.pack(fill="x", padx=28, pady=(0, 14))
        self.back_btn = ttk.Button(self.btnbar, text="<  上一步", style="Ghost.TButton", command=self.back)
        self.back_btn.pack(side="left")
        self.next_btn = ttk.Button(self.btnbar, text="下一步  >", style="Accent.TButton", command=self.next)
        self.next_btn.pack(side="right")
        self._render()

    STEPS = ["选择镜像", "下载目录", "刻录方式", "选择设备", "确认"]

    def _render(self):
        self.step_lbl.config(text=f"第 {self.step + 1}/5 步  ·  {self.STEPS[self.step]}")
        for w in self.body.winfo_children():
            w.destroy()
        pad = 6
        if self.step == 0:
            ttk.Label(self.body, text="请选择要下载并刻录的 DUESLIN 镜像：",
                      style="Title.TLabel").pack(anchor="w", pady=(0, 12))
            self.img_cb = ttk.Combobox(self.body, values=[i[0] for i in IMAGES],
                                       state="readonly", font=("Microsoft YaHei UI", 12))
            self.img_cb.current(self.img_idx)
            self.img_cb.pack(fill="x", pady=(0, 8))
            ttk.Label(self.body, text="镜像来自 GitHub Releases，下载支持自动重试。",
                      style="Sub.TLabel").pack(anchor="w")
        elif self.step == 1:
            ttk.Label(self.body, text="选择下载目录", style="Title.TLabel").pack(anchor="w", pady=(0, 12))
            self.dir_var = tk.StringVar(value=self.download_dir)
            ent = ttk.Entry(self.body, textvariable=self.dir_var, font=("Microsoft YaHei UI", 11))
            ent.pack(fill="x", pady=(0, 8))
            ttk.Button(self.body, text="浏览", style="Ghost.TButton",
                       command=self._pick_dir).pack(anchor="e")
        elif self.step == 2:
            ttk.Label(self.body, text="选择刻录模式", style="Title.TLabel").pack(anchor="w", pady=(0, 12))
            ttk.Radiobutton(self.body, text="ISO   刻录到 U 盘", variable=self.mode_iso,
                            value=True).pack(anchor="w", pady=4)
            ttk.Radiobutton(self.body, text="DVD   刻录到光盘", variable=self.mode_iso,
                            value=False).pack(anchor="w", pady=4)
        elif self.step == 3:
            ttk.Label(self.body, text="选择目标设备", style="Title.TLabel").pack(anchor="w", pady=(0, 8))
            ttk.Label(self.body, text="请插入 U 盘并选择（刻录将格式化该设备）：",
                      style="Sub.TLabel").pack(anchor="w")
            self.dev_cb = ttk.Combobox(self.body, state="readonly", font=("Microsoft YaHei UI", 11))
            self.dev_cb.pack(fill="x", pady=(10, 8))
            ttk.Button(self.body, text="刷新设备", style="Ghost.TButton",
                       command=self._refresh_dev).pack(anchor="e")
            tk.Label(self.body, text="警告：设备上的所有数据将被清除！",
                     bg=BG, fg="#DC2626", font=("Microsoft YaHei UI", 10, "bold")).pack(anchor="w", pady=8)
            self._refresh_dev()
        elif self.step == 4:
            ttk.Label(self.body, text="确认信息", style="Title.TLabel").pack(anchor="w", pady=(0, 10))
            info = (f"镜像：{IMAGES[self.img_idx][0]}\n"
                    f"目标：{self.dev_name or '未选择'}\n"
                    f"模式：{'U盘 (ISO)' if self.mode_iso.get() else '光盘 (DVD)'}\n\n"
                    f"设备上的所有数据将被清除！\n请输入大写 YES 确认：")
            tk.Label(self.body, text=info, bg=BG, fg=TEXT, justify="left",
                     font=("Microsoft YaHei UI", 11)).pack(anchor="w")
            self.yes_var = tk.StringVar()
            ttk.Entry(self.body, textvariable=self.yes_var, width=16,
                      font=("Microsoft YaHei UI", 12)).pack(anchor="w", pady=8)
        self.back_btn.config(state="normal" if self.step > 0 else "disabled")
        self.next_btn.config(text="开始刻录" if self.step == 4 else "下一步  >")

    def _pick_dir(self):
        d = filedialog.askdirectory(initialdir=self.download_dir, title="选择下载目录")
        if d:
            self.download_dir = d
            self.dir_var.set(d)

    def _refresh_dev(self):
        if sys.platform.startswith("win"):
            self.devices = list_windows_disks()
        else:
            self.devices = list_linux_disks()
        vals = [d[1] for d in self.devices]
        self.dev_cb["values"] = vals
        if vals:
            self.dev_cb.current(0)
        self.dev_name = vals[0] if vals else "未检测到设备"

    def back(self):
        if self.step > 0:
            self.step -= 1
            self._render()

    def next(self):
        if self.step == 0:
            self.img_idx = self.img_cb.current()
            self.step = 1
        elif self.step == 1:
            self.download_dir = self.dir_var.get() or os.path.expanduser("~")
            self.step = 2
        elif self.step == 2:
            self.step = 3
        elif self.step == 3:
            if not self.devices:
                messagebox.showwarning("无设备", "未检测到任何磁盘，请插入 U 盘后刷新。")
                return
            self.dev_name = self.dev_cb.get()
            self.step = 4
        elif self.step == 4:
            if self.yes_var.get().strip() != "YES":
                messagebox.showwarning("确认", "请输入大写 YES 确认。")
                return
            self.next_btn.config(state="disabled")
            self.back_btn.config(state="disabled")
            self.after(50, self._run)
            return
        self._render()

    def _set_status(self, s):
        self.status_lbl.config(text=s)

    def _run(self):
        url = f"{REPO}/{IMAGES[self.img_idx][1]}"
        dest = os.path.join(self.download_dir, IMAGES[self.img_idx][1])
        ok = download(url, dest, lambda p: self.prog.config(value=p), self._set_status)
        if not ok:
            messagebox.showerror("下载失败", "下载失败，请检查网络后重试。")
            self.next_btn.config(state="normal")
            return
        self._set_status("下载完成，正在刻录...")
        self.prog.config(value=10)
        if self.mode_iso.get():
            ok = self._burn(dest)
            if ok:
                messagebox.showinfo("完成", "刻录完成！可以拔出 U 盘用于启动安装。")
        else:
            messagebox.showinfo("DVD 模式", "请打开下载目录，右键该镜像文件 → 刻录光盘映像。")
        self.next_btn.config(state="normal")

    def _burn(self, iso_path):
        """把 ISO 写入物理盘（Windows 用 dd 式写入）"""
        try:
            dev_id = self.devices[0][0]
            if sys.platform.startswith("win"):
                import subprocess
                r = subprocess.run(["powershell", "-NoProfile", "-Command",
                                    f"Write-Disk -Number {dev_id} -Path '{iso_path}' -Confirm:$false"],
                                   capture_output=True, text=True, timeout=1800)
                return r.returncode == 0
            else:
                dev_path = f"/dev/{dev_id}"
                with open(iso_path, "rb") as src, open(dev_path, "wb") as dst:
                    total = os.path.getsize(iso_path)
                    got = 0
                    while True:
                        chunk = src.read(1 << 20)
                        if not chunk:
                            break
                        dst.write(chunk)
                        got += len(chunk)
                        self.prog.config(value=10 + int(got * 90 / total))
                return True
        except Exception as e:
            messagebox.showerror("刻录失败", f"{e}\n请以管理员身份运行。")
            return False

if __name__ == "__main__":
    app = App()
    app.mainloop()
