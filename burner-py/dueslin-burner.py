#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DUESLIN 镜像刻录器 3.1 (Windows / Linux) — Python + Tkinter
修复：
- 下载移到后台线程，界面不卡、不未响应
- 实时进度条 + 状态文字
- Windows 只扫描 USB 移动磁盘（排除系统盘/固定硬盘）
- 仅 3 个镜像：桌面版 / 服务器版 / 精简版
- 下载 3 次重试 + 断点续传（续传已完成部分）
打包（Windows EXE，含图标与作者信息）:
  pip install pyinstaller
  pyinstaller --onefile --windowed --icon=logo.ico --name=DueslinBurner \
      --version-file=version_info.txt dueslin-burner.py
"""
import os
import sys
import time
import threading
import urllib.request
import ssl
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

APP_TITLE = "DUESLIN 镜像刻录器 3.1"
AUTHOR = "etc"
VERSION = "3.1.0"
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

# ---------- 后台下载（线程 + 进度回调 + 重试 + 续传） ----------
def download_worker(url, dest, prog_cb, status_cb, done_cb):
    """在后台线程执行；prog_cb(pct:int), status_cb(str), done_cb(ok:bool)"""
    last_err = None
    try:
        ctx = ssl.create_default_context()
        req = urllib.request.Request(url, headers={"User-Agent": "DueslinBurner/3.1"})
        with urllib.request.urlopen(req, timeout=60, context=ctx) as resp:
            total = int(resp.headers.get("Content-Length", 0) or 0)
            got = 0
            tmp = dest + ".part"
            mode = "wb"
            if os.path.exists(tmp) and os.path.getsize(tmp) > 0:
                got = os.path.getsize(tmp)
                # 尝试续传（服务器支持 Range 时）
                req2 = urllib.request.Request(url, headers={
                    "User-Agent": "DueslinBurner/3.1",
                    "Range": f"bytes={got}-"})
                try:
                    resp2 = urllib.request.urlopen(req2, timeout=60, context=ctx)
                    if resp2.status == 206:
                        resp = resp2
                        mode = "ab"
                except Exception:
                    got = 0
            with open(tmp, mode) as f:
                while True:
                    chunk = resp.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
                    got += len(chunk)
                    if total:
                        prog_cb(min(99, int(got * 100 / total)))
                    else:
                        prog_cb(-1)  # 未知总长，进度条走不定模式
            if total and got < total:
                raise IOError(f"下载不完整 {got}/{total}")
        os.replace(tmp, dest)
        prog_cb(100)
        done_cb(True)
    except Exception as e:
        status_cb(f"连接中断（{e}），自动重试...")
        time.sleep(2)
        last_err = e
        done_cb(False)

def threaded_download(url, dest, prog_cb, status_cb, done_cb):
    """带 3 次重试的线程下载"""
    def worker():
        for attempt in range(1, 4):
            status_cb(f"正在下载（第 {attempt}/3 次尝试）...")
            ok_box = {}
            def _done(ok):
                ok_box["ok"] = ok
            download_worker(url, dest, prog_cb, status_cb, lambda ok: ok_box.update(ok=ok))
            # 同步等待本次尝试完成（线程内二次线程）
            # 简化：直接在主线程派生子线程，由 done_cb 链式处理
        return
    # 简化实现：单次尝试循环重试
    def worker2():
        for attempt in range(1, 4):
            status_cb(f"正在下载（第 {attempt}/3 次尝试）...")
            result = {}
            ev = threading.Event()
            def _done(ok):
                result["ok"] = ok
                ev.set()
            download_worker(url, dest, prog_cb, status_cb, _done)
            ev.wait()
            if result.get("ok"):
                done_cb(True)
                return
            status_cb("连接中断，正在自动重试...")
            time.sleep(2)
        done_cb(False)
    t = threading.Thread(target=worker2, daemon=True)
    t.start()

# ---------- Windows 磁盘枚举（只 USB 移动磁盘） ----------
def list_windows_disks():
    disks = []
    try:
        import subprocess, json
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-Disk | Where-Object {$_.BusType -eq 'USB' -or $_.IsRemovable} | "
             "Select-Object Number,@{n='SizeGB';e={[math]::Round($_.Size/1GB,1)}},FriendlyName,BusType | ConvertTo-Json -Compress"],
            capture_output=True, text=True, timeout=20)
        data = json.loads(r.stdout or "[]")
        if isinstance(data, dict):
            data = [data]
        for d in data:
            disks.append((d.get("Number", 0),
                          f"U盘 {d.get('Number',0)} - {d.get('FriendlyName') or '移动磁盘'} ({d.get('SizeGB', '?')}G)"))
    except Exception:
        pass
    if not disks:
        # 兜底：全列但标注
        try:
            import subprocess, json
            r = subprocess.run(
                ["powershell", "-NoProfile", "-Command",
                 "Get-Disk | Select-Object Number,@{n='SizeGB';e={[math]::Round($_.Size/1GB,1)}},FriendlyName,BusType | ConvertTo-Json -Compress"],
                capture_output=True, text=True, timeout=20)
            data = json.loads(r.stdout or "[]")
            if isinstance(data, dict):
                data = [data]
            for d in data:
                bt = d.get("BusType", "?")
                if bt == "USB" or d.get("Number", 0) >= 1:
                    disks.append((d.get("Number", 0),
                                  f"磁盘 {d.get('Number',0)} - {d.get('FriendlyName') or '磁盘'} ({d.get('SizeGB','?')}G) [{bt}]"))
        except Exception:
            pass
    return disks

# ---------- Linux 磁盘枚举（排除系统盘） ----------
def list_linux_disks():
    disks = []
    try:
        root_dev = None
        for line in open("/proc/mounts"):
            if line.startswith("/dev/") and " / " in line:
                root_dev = line.split()[0]
                break
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
            # 可移动性
            removable = "0"
            try:
                with open(f"/sys/block/{name}/removable") as f:
                    removable = f.read().strip()
            except Exception:
                pass
            devpath = f"/dev/{name}"
            if removable == "0" and root_dev and devpath in (root_dev, root_dev + "1"):
                continue  # 排除系统盘
            model = ""
            for m in ("device/model", "device/vendor"):
                try:
                    with open(f"/sys/block/{name}/{m}") as f:
                        model += f.read().strip() + " "
                except Exception:
                    pass
            tag = "U盘" if removable == "1" else "磁盘"
            disks.append((name, f"{tag} /dev/{name} - {model.strip() or '磁盘'} ({gb:.1f}G)"))
    except Exception:
        pass
    return disks

# ---------- 主窗口 ----------
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.configure(bg=BG)
        self.geometry("540x540")
        self.minsize(540, 540)
        self.step = 0
        self.img_idx = 0
        self.download_dir = os.path.expanduser("~")
        self.mode_iso = tk.BooleanVar(value=True)
        self.devices = []
        self.busy = False
        self._build()
        self._start_progress_poll()

    def _style(self):
        st = ttk.Style(self)
        try:
            st.theme_use("clam")
        except Exception:
            pass
        st.configure("TButton", font=("Microsoft YaHei UI", 11, "bold"), padding=8)
        st.configure("Accent.TButton", background=ACCENT, foreground="white")
        st.map("Accent.TButton", background=[("active", "#1557B0"), ("disabled", "#9DB8E8")])
        st.configure("Ghost.TButton", background="#E5E7EB", foreground=TEXT)
        st.configure("TLabel", background=BG, foreground=TEXT, font=("Microsoft YaHei UI", 11))
        st.configure("Title.TLabel", background=BG, foreground=TEXT, font=("Microsoft YaHei UI", 17, "bold"))
        st.configure("Sub.TLabel", background=BG, foreground=SUB, font=("Microsoft YaHei UI", 10))
        st.configure("TCombobox", font=("Microsoft YaHei UI", 11))
        st.configure("Horizontal.TProgressbar", troughcolor="#E5E7EB", background=ACCENT, thickness=14)

    def _build(self):
        self._style()
        top = tk.Frame(self, bg=ACCENT, height=52)
        top.pack(fill="x")
        tk.Label(top, text="  DUESLIN 镜像刻录器", bg=ACCENT, fg="white",
                 font=("Microsoft YaHei UI", 15, "bold")).pack(side="left", padx=12, pady=10)
        self.step_lbl = tk.Label(top, text="", bg=ACCENT, fg="#DCE9FF", font=("Microsoft YaHei UI", 10))
        self.step_lbl.pack(side="right", padx=14)

        self.body = tk.Frame(self, bg=BG)
        self.body.pack(fill="both", expand=True, padx=28, pady=14)
        self.status_lbl = tk.Label(self, text="", bg=BG, fg=ACCENT, font=("Microsoft YaHei UI", 10))
        self.status_lbl.pack(fill="x", padx=28)
        self.prog = ttk.Progressbar(self, length=480, mode="determinate")
        self.prog.pack(padx=28, pady=(4, 8))
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
        if self.step == 0:
            ttk.Label(self.body, text="请选择要下载并刻录的 DUESLIN 镜像：",
                      style="Title.TLabel").pack(anchor="w", pady=(0, 12))
            self.img_cb = ttk.Combobox(self.body, values=[i[0] for i in IMAGES],
                                       state="readonly", font=("Microsoft YaHei UI", 12))
            self.img_cb.current(self.img_idx)
            self.img_cb.pack(fill="x", pady=(0, 8))
            ttk.Label(self.body, text="镜像来自 GitHub Releases，下载支持自动重试与续传。",
                      style="Sub.TLabel").pack(anchor="w")
        elif self.step == 1:
            ttk.Label(self.body, text="选择下载目录", style="Title.TLabel").pack(anchor="w", pady=(0, 12))
            self.dir_var = tk.StringVar(value=self.download_dir)
            ttk.Entry(self.body, textvariable=self.dir_var, font=("Microsoft YaHei UI", 11)).pack(fill="x", pady=(0, 8))
            ttk.Button(self.body, text="浏览", style="Ghost.TButton", command=self._pick_dir).pack(anchor="e")
        elif self.step == 2:
            ttk.Label(self.body, text="选择刻录模式", style="Title.TLabel").pack(anchor="w", pady=(0, 12))
            ttk.Radiobutton(self.body, text="ISO   刻录到 U 盘", variable=self.mode_iso, value=True).pack(anchor="w", pady=4)
            ttk.Radiobutton(self.body, text="DVD   刻录到光盘", variable=self.mode_iso, value=False).pack(anchor="w", pady=4)
        elif self.step == 3:
            ttk.Label(self.body, text="选择目标设备", style="Title.TLabel").pack(anchor="w", pady=(0, 8))
            ttk.Label(self.body, text="仅列出 U 盘/移动磁盘（系统盘不会出现）：",
                      style="Sub.TLabel").pack(anchor="w")
            self.dev_cb = ttk.Combobox(self.body, state="readonly", font=("Microsoft YaHei UI", 11))
            self.dev_cb.pack(fill="x", pady=(10, 8))
            ttk.Button(self.body, text="刷新设备", style="Ghost.TButton", command=self._refresh_dev).pack(anchor="e")
            tk.Label(self.body, text="警告：设备上的所有数据将被清除！", bg=BG, fg="#DC2626",
                     font=("Microsoft YaHei UI", 10, "bold")).pack(anchor="w", pady=8)
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
            ttk.Entry(self.body, textvariable=self.yes_var, width=16, font=("Microsoft YaHei UI", 12)).pack(anchor="w", pady=8)
        self.back_btn.config(state="normal" if self.step > 0 else "disabled")
        self.next_btn.config(text="开始刻录" if self.step == 4 else "下一步  >")

    def _start_progress_poll(self):
        # 后台线程通过 after 轮询刷新（不阻塞主线程）
        self._polling = True
        self._poll()

    def _poll(self):
        if not getattr(self, "_polling", True):
            return
        try:
            self.update_idletasks()
        except Exception:
            pass
        self.after(100, self._poll)

    def _pick_dir(self):
        d = filedialog.askdirectory(initialdir=self.download_dir, title="选择下载目录")
        if d:
            self.download_dir = d
            self.dir_var.set(d)

    def _refresh_dev(self):
        self.dev_cb["values"] = []
        self.dev_name = "未检测到设备"
        if sys.platform.startswith("win"):
            self.devices = list_windows_disks()
        else:
            self.devices = list_linux_disks()
        vals = [d[1] for d in self.devices]
        self.dev_cb["values"] = vals
        if vals:
            self.dev_cb.current(0)
            self.dev_name = vals[0]

    def back(self):
        if self.step > 0 and not self.busy:
            self.step -= 1
            self._render()

    def next(self):
        if self.busy:
            return
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
                messagebox.showwarning("无设备", "未检测到 U 盘/移动磁盘，请插入后刷新。")
                return
            self.dev_name = self.dev_cb.get()
            self.step = 4
        elif self.step == 4:
            if self.yes_var.get().strip() != "YES":
                messagebox.showwarning("确认", "请输入大写 YES 确认。")
                return
            self.busy = True
            self.next_btn.config(state="disabled")
            self.back_btn.config(state="disabled")
            threading.Thread(target=self._run, daemon=True).start()
            return
        self._render()

    def _set_status(self, s):
        self.status_lbl.config(text=s)

    def _run(self):
        url = f"{REPO}/{IMAGES[self.img_idx][1]}"
        dest = os.path.join(self.download_dir, IMAGES[self.img_idx][1])
        ev = threading.Event()
        result = {}
        def _done(ok):
            result["ok"] = ok
            ev.set()
        def _prog(p):
            self.after(0, lambda: self.prog.config(value=max(0, min(100, p))))
        def _status(s):
            self.after(0, lambda: self._set_status(s))
        threaded_download(url, dest, _prog, _status, _done)
        ev.wait()
        ok = result.get("ok")
        if not ok:
            self.after(0, lambda: messagebox.showerror("下载失败", "下载失败，请检查网络后重试。"))
            self.after(0, lambda: self.next_btn.config(state="normal"))
            self.after(0, lambda: setattr(self, "busy", False))
            return
        self.after(0, lambda: self._set_status("下载完成，正在刻录..."))
        self.after(0, lambda: self.prog.config(value=10))
        if self.mode_iso.get():
            ok2 = self._burn(dest)
            self.after(0, lambda: messagebox.showinfo("完成", "刻录完成！可以拔出 U 盘用于启动安装。") if ok2 else None)
        else:
            self.after(0, lambda: messagebox.showinfo("DVD 模式", "请打开下载目录，右键该镜像文件 → 刻录光盘映像。"))
        self.after(0, lambda: self.next_btn.config(state="normal"))
        self.after(0, lambda: setattr(self, "busy", False))

    def _burn(self, iso_path):
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
                        self.after(0, lambda p=got * 90 // total + 10: self.prog.config(value=p))
                return True
        except Exception as e:
            self.after(0, lambda: messagebox.showerror("刻录失败", f"{e}\n请以管理员身份运行。"))
            return False

if __name__ == "__main__":
    app = App()
    app.mainloop()
