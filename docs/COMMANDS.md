# DUESLIN 命令手册

DUESLIN 拥有全新的简化命令系统，用法与 Windows 的 CMD、Linux 终端一致，但更简洁。
打开终端后即可输入命令。所有 Linux / cmd 常用命令（`ls`、`cd`、`cat`、`ping` …）同样可直接使用。

## 一、软件管理

| 命令 | 说明 | 示例 |
|---|---|---|
| `install <软件包>` | 安装软件 | `install python` |
| `remove <软件包>` | 卸载软件 | `remove python` |
| `update` | 更新软件源 | `update` |
| `upgrade` | 升级整个系统 | `upgrade` |

常用软件包名：`python`、`firefox`、`vim`、`git`、`vlc`、`gimp`、`libreoffice`、`gcc` 等。
（DUESLIN 会自动把简化名映射到实际软件包，如 `install python` = 安装 Python 3。）

## 二、文件与目录

| 命令 | 说明 | 示例 |
|---|---|---|
| `files` | 查看当前目录的文件 | `files` |
| `goto <目录>` | 进入目录 | `goto /home` |
| `here` | 显示当前所在位置 | `here` |
| `open <文件>` | 用默认程序打开文件 | `open 照片.png` |
| `find <名称>` | 查找文件 | `find 报告` |

## 三、网络与 WiFi

| 命令 | 说明 | 示例 |
|---|---|---|
| `wifi` | 扫描附近 WiFi | `wifi` |
| `wifi connect <名称> [密码]` | 连接 WiFi | `wifi connect HomeWiFi 12345678` |
| `net` | 查看网络状态 | `net` |
| `ping <地址>` | 测试网络 | `ping baidu.com` |

## 四、系统控制

| 命令 | 说明 | 示例 |
|---|---|---|
| `shutdown` | 关闭电脑 | `shutdown` |
| `reboot` | 重启电脑 | `reboot` |
| `lock` | 锁屏 | `lock` |
| `task` | 打开任务管理器 | `task` |
| `about` | 系统信息 | `about` |
| `pe te=1` | 进入 PE 修复模式（开机菜单出现修复选项） | `pe te=1` |
| `pe te=2` | 卸载 PE 修复模式 | `pe te=2` |
| `su retc` | 提权到最高权限（retc） | `su retc` |

### 关于提权
DUESLIN 的最高权限用户名为 `retc`。执行 `su retc` 后：
- 若 `retc` 设置了密码 → 输入密码后获得最高权限；
- 若未设置密码 → 直接提升到最高权限。

任务管理器中，普通程序可直接结束；系统核心进程需要先 `su retc` 提权。

### 关于 PE 修复模式
- 安装 DUESLIN 时已内置 PE 修复环境（位于 `/boot/pe`）。
- 输入 `pe te=1` 启用后，重启时开机菜单会出现「DUESLIN 修复模式 (PE)」，可进行
  引导修复、磁盘检查、终端修复、重装系统等操作。
- 输入 `pe te=2` 可卸载 PE 菜单项（PE 数据仍保留在磁盘上）。

## 五、其他

| 命令 | 说明 |
|---|---|
| `clear` | 清屏 |
| `history` | 查看历史命令 |
| `help` | 查看命令手册 |
| `exit` / `quit` | 退出终端 |

## 六、快捷键

| 快捷键 | 功能 |
|---|---|
| `Win`（Super） | 打开开始菜单 |
| `Ctrl + Shift + Esc` | 打开任务管理器 |
| `Ctrl + Alt + T` | 打开终端 |
| `Win + L` | 锁屏 |
| `Ctrl + Alt + Delete` | 打开任务管理器 |

## 七、常见 Linux 命令（同样可用）

```
ls  cd  cp  mv  rm  mkdir  cat  grep  find  ping  curl  wget  top  df  du
```
