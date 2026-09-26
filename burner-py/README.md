# DUESLIN 镜像刻录器 3.0（Python + Tkinter）

用 Python 编写，Tkinter 界面（蓝白主题 + 微软雅黑），作者：etc。

## 修复/特性
- 中文原生支持，无乱码
- 仅 3 个镜像：桌面版 / 服务器版 / 精简版（x86-64）
- 下载带 3 次自动重试 + 实时进度，GitHub Release 连接稳定
- 选择设备（自动枚举磁盘，可刷新）→ 输入 YES → 下载 → 无损刻录 U 盘
- 打包含版本信息（右键属性作者 etc）与 DUESLIN Logo 图标

## 打包 Windows EXE（pyinstaller）
    pip install pyinstaller
    pyinstaller --onefile --windowed --icon=logo.ico \
        --version-file=version_info.txt --name=DueslinBurner dueslin-burner.py

## 打包 Linux 版
    pyinstaller --onefile --windowed --icon=logo.ico --name=DueslinBurner-linux dueslin-burner.py

## 说明
- Windows 刻录 U 盘需管理员权限
- DVD 模式：镜像下载后提示用资源管理器刻录光盘映像
- 下载源：GitHub Releases（v1.0.0）
