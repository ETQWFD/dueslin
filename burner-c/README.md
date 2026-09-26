# DUESLIN 镜像刻录器 2.0（Windows · C 语言）

C 语言 + Win32 API 编写，单文件 EXE，内置 DUESLIN 官方 Logo 图标。
右键属性可见：作者 etc、版本 2.0.0、产品描述。

## 修复内容（2.0）
- 全宽字符 UTF-16，中文不再乱码
- 内置视觉样式 Manifest（现代控件外观）+ 微软雅黑 + 蓝白主题 + 圆角按钮
- VERSIONINFO：作者 etc、版本号（右键属性可查）
- 仅 3 个镜像：桌面版 / 服务器版 / 精简版（x86-64）
- 下载带 3 次自动重试与进度显示，GitHub Release 连接更稳定
- 需管理员权限运行（Manifest 已声明，自动弹 UAC）

## 功能流程
1. 选择镜像（桌面版 / 服务器版 / 精简版）
2. 选择下载目录
3. 选择刻录模式：ISO（U盘）/ DVD（光盘）
4. 选择目标设备（自动枚举物理磁盘，可刷新）
5. 输入大写 YES 确认 -> 下载（自动重试）-> 无损刻录到 U 盘

## 编译（MinGW-w64，单文件）
    x86_64-w64-mingw32-windres logo.rc -O coff -o logo.res
    x86_64-w64-mingw32-gcc -O2 dueslin-burner.c logo.res \
        -lcomctl32 -lwininet -lole32 -lcomdlg32 -lurlmon -lgdi32 \
        -o DueslinBurner.exe \
        -static-libgcc -static-libstdc++ -Wl,-subsystem,windows

## 说明
- 刻录 U 盘需管理员权限（程序已自动请求）
- DVD 模式调用系统刻录（资源管理器刻录光盘映像）
- 下载源为 GitHub Releases
