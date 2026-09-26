DUESLIN 镜像刻录工具 - Windows 打包说明
==========================================

一、打包为单个 EXE（含左上角图标）
------------------------------------
1. 安装 Python 3.10+ 并加入 PATH
2. 安装依赖:
     pip install pyinstaller

3. 在源码目录（与 dueslin-burner.py、logo.ico 同目录）执行:

     pyinstaller --onefile --windowed --icon=logo.ico ^
                 --name DueslinBurner dueslin-burner.py

   生成的单个文件位于 dist\DueslinBurner.exe

   * --onefile   : 打包为单个 EXE
   * --windowed  : 运行时不弹出黑色控制台窗口
   * --icon      : 将 logo.ico 嵌入 EXE，窗口左上角/任务栏显示系统图标

4. 运行 EXE 后左侧品牌栏、任务栏、Alt+Tab 都会显示 DUESLIN 蓝底黑 D 图标。

二、也可以用 auto-py-to-exe（图形界面打包）
--------------------------------------------
   pip install auto-py-to-exe
   auto-py-to-exe
   在界面中:
   - Script Location: 选择 dueslin-burner.py
   - One File: 勾选
   - Window Based: 勾选
   - Icon: 选择 logo.ico
   - 点击 Convert 生成 EXE

三、功能说明
------------
   - 自动查询 GitHub Release 最新版本（可选六个镜像: 桌面/服务器/精简版 × x64/ARM64）
   - 选择下载目录 -> 选择刻录模式 (U盘/光盘) -> 选择目标设备 -> 输入 YES 确认 -> 自动下载+刻录
   - U 盘刻录需要以【管理员身份】运行（右键 -> 以管理员身份运行），
     否则无法打开物理磁盘进行原始写入
   - 下载断点续传: 已下载完成的镜像会复用，不会重复下载

四、常见问题
------------
   Q: 提示"无法打开物理磁盘"?
   A: 右键 EXE -> 以管理员身份运行

   Q: 杀毒软件报毒?
   A: 原始写入磁盘需要底层 API，属正常行为; 可添加白名单

   Q: 没有 U 盘列表?
   A: 本工具通过写物理磁盘工作，Windows 下请确认 U 盘已连接且
      未被"安全弹出"。ARM64 镜像若 GitHub 无对应文件会提示下载失败。
