# windows_update_in_linux — Linux TTY 直渲：伪 Windows 更新界面

## 致谢

本项目借鉴了 [heyManNice/bsod](https://github.com/heyManNice/bsod) 的 DRM 直渲思路，结合了 **C + libdrm + FreeType** 技术。

同时项目内嵌了 [heyManNice/bsod](https://github.com/heyManNice/bsod) v1.0.1 的构建产物，作为"失败蓝屏"。

感谢 [heyManNice](https://github.com/heyManNice) 的贡献。

## 运行（需要root）

如果是精简版系统，可能需要下载

```bash
sudo apt install libdrm2 libfreetype6 libfontconfig1 libsystemd0
```

然后之间去Releases下载 `windows_update_in_linux` 可执行文件，放到任意目录，运行。

```bash
sudo ./windows_update_in_linux                 # 50/50：成功（后台真跑 apt）重启 / 失败蓝屏（不更新）
sudo ./windows_update_in_linux --no-reboot     # 不真重启：成功/失败都恢复桌面退出，蓝屏也不重启
sudo ./windows_update_in_linux --timeout=10    # 更快预览（默认 20s）
./windows_update_in_linux --help
```

| 场景             | 方法                                                                         |
| ---------------- | ---------------------------------------------------------------------------- |
| 强制本次"成功"   | `WINDOWS_UPDATE_MODE=success sudo ./windows_update_in_linux --no-reboot`     |
| 强制本次"失败"   | `WINDOWS_UPDATE_MODE=failure sudo ./windows_update_in_linux --no-reboot`     |
| 不真重启（测试） | `sudo ./windows_update_in_linux --no-reboot`                                 |
| 改最少等待时长   | `sudo ./windows_update_in_linux --timeout=30` 或 `WINDOWS_UPDATE_TIMEOUT=30` |
| 想看帮助         | `./windows_update_in_linux --help`                                           |

## 简介

用 **C + libdrm + FreeType** 写的整活程序：**绕过桌面**，切换到空闲 tty、
抢占 DRM Master，把"Windows 式更新"界面**直接渲染到物理屏幕**。每次运行
**50% 概率更新成功、50% 概率更新失败**。

## 源码布局

```
CMakeLists.txt            构建系统（CMake + pkg-config）
src/main.c                入口：参数解析 + 调用直渲
src/ttydrm.c/.h           VT 切换 + DRM 帧缓冲 + FreeType 文字 + 50/50 结局 + 内嵌 BSOD
src/bsod_data.h           内嵌的 bsod v1.0.1 二进制（构建时生成）
windows_update_in_linux   编译产物（根目录，直接 ./ 运行，已 gitignore）
```

因为显示器归本进程所有、桌面整体被挂起，受害者**无法 Alt+Tab 逃逸**，
在 X11 和 Wayland 下都生效。集成自 [heyManNice/bsod](https://github.com/heyManNice/bsod)
的构建产物（已内嵌进本二进制）作为"失败蓝屏"。默认会**真正重启系统**；
加 `--no-reboot` 可只恢复桌面退出（测试/保险用）。

## 特性

- **单一可执行文件**：编译产物就在根目录，`./windows_update_in_linux` 直接跑
- **极轻依赖**：只用 `libdrm` + `freetype2` + `libsystemd`（`fontconfig` 可选），无 GTK / 无 WebKit / 无 X11
- **不可逃逸**：桌面被挂起，物理屏幕直渲，X11 / Wayland 通吃
- **50/50 结局**：随机更新成功（进度到 100% 后重启）或失败（内置 BSOD 蓝屏接管）
- **玄学进度**：成功时一开始快、后面慢地逼近 99%；失败时从 0% 慢慢爬到 35%~42% 随机封顶值后停住
- **假戏真做（成功才更新）**：只有随机到"更新成功"才在后台真跑 `apt-get update && apt-get upgrade` 并**等它跑完**再显示 100%；随机到"失败/蓝屏"则不做任何系统更新（日志 `windows-update-real.log` 在当前目录）
- **内置 BSOD**：内嵌 [heyManNice/bsod](https://github.com/heyManNice/bsod) v1.0.1 构建产物，失败时直接展示真实蓝屏（含二维码，原因按语言显示"Linux 在更新时出错"）
- **单语言文案**：按 `LANG` 环境变量自动切换——`zh*` 显示中文，其他显示英文（字体同样按语言选择，fontconfig 或回退扫描系统字体目录）
- **安全兜底**：`--no-reboot` 不真重启，恢复桌面退出（蓝屏也会以 `--restore` 模式运行、同样不重启）；进度至少 20s（可用 `--timeout` 调整）
- **真机友好**：DRM 设备自动探测 `/dev/dri/card0..7`

## 依赖（仅编译需要；运行只需几个很小的运行库）

编译：

| 发行版        | 安装命令                                                                                                         |
| ------------- | ---------------------------------------------------------------------------------------------------------------- |
| Debian/Ubuntu | `sudo apt install libdrm-dev libfreetype-dev libfontconfig1-dev libsystemd-dev build-essential cmake pkg-config` |
| Fedora        | `sudo dnf install libdrm-devel freetype-devel fontconfig-devel gcc cmake pkgconf-pkg-config`                     |
| Arch          | `sudo pacman -S libdrm freetype2 fontconfig base-devel cmake pkgconf`                                            |

> `libfontconfig1-dev` 可选；不装也能编译（回退到扫描常见字体目录）。

## 编译（一次性）

```bash
cmake -B build && cmake --build build
```

编译完成后，二进制直接出现在项目根目录：`./windows_update_in_linux`。
