# windows_update_in_linux — Linux TTY 直渲：伪 Windows 更新界面

用 **C + libdrm + FreeType** 写的整活程序：**绕过桌面**，切换到空闲 tty、
抢占 DRM Master，把"Windows 式更新"界面**直接渲染到物理屏幕**（0→35% 卡死 +
转圈动画）。超时后自动恢复原桌面并退出，**绝不真正重启**，不会动任何系统文件。

编译后就是一个**单一可执行文件**，放在项目根目录，终端直接 `./` 就能运行，
**不需要安装、不需要 .deb、不需要配 PATH**：

```
sudo ./windows_update_in_linux --timeout=60
        │
        ▼
┌────────────────────────────────────────────┐
│ 1. 切到空闲 tty6（桌面挂起、释放 DRM）       │
│ 2. 抢占 /dev/dri/card* DRM Master           │
│ 3. dumb framebuffer + FreeType 直渲          │
│    伪进度 0→35% 卡死 + 转圈                  │
│ 4. 超时 → 恢复 CRTC → 切回桌面 tty → 退出    │
└────────────────────────────────────────────┘
```

因为显示器归本进程所有、桌面整体被挂起，受害者**无法 Alt+Tab 逃逸**，
在 X11 和 Wayland 下都生效（借鉴 [heyManNice/bsod](https://github.com/heyManNice/bsod)
的 VT+DRM 思路，但保留我们的安全底线：超时恢复、绝不真重启）。

## 特性

- **单一可执行文件**：编译产物就在根目录，`./windows_update_in_linux` 直接跑
- **极轻依赖**：只用 `libdrm` + `freetype2`（`fontconfig` 可选），无 GTK / 无 WebKit / 无 X11
- **不可逃逸**：桌面被挂起，物理屏幕直渲，X11 / Wayland 通吃
- **玄学进度**：0→35% 伪随机步长 2~3 秒冲完，随后卡死在 35%，转圈不停
- **安全保险**：超时（默认 60s）自动恢复原 CRTC、切回桌面 tty 并退出，**绝不真正重启**
- **中英混合文案**：按 `LANG` 自动选 CJK 字体（fontconfig）或回退扫描系统字体目录
- **真机友好**：DRM 设备自动探测 `/dev/dri/card0..7`

## 依赖（仅编译需要；运行只需三个很小的运行库）

编译：

| 发行版        | 安装命令                                                                                          |
| ------------- | ------------------------------------------------------------------------------------------------- |
| Debian/Ubuntu | `sudo apt install libdrm-dev libfreetype-dev libfontconfig1-dev build-essential cmake pkg-config` |
| Fedora        | `sudo dnf install libdrm-devel freetype-devel fontconfig-devel gcc cmake pkgconf-pkg-config`      |
| Arch          | `sudo pacman -S libdrm freetype2 fontconfig base-devel cmake pkgconf`                             |

> `libfontconfig1-dev` 可选；不装也能编译（回退到扫描常见字体目录）。

## 编译（一次性）

```bash
cmake -B build && cmake --build build
```

编译完成后，二进制直接出现在项目根目录：`./windows_update_in_linux`。

## 运行（直接 ./，需 root）

```bash
sudo ./windows_update_in_linux --timeout=60    # 整活 60s 后自动恢复桌面
sudo ./windows_update_in_linux --timeout=10    # 快速预览
./windows_update_in_linux --help
```

| 场景         | 方法                                                                         |
| ------------ | ---------------------------------------------------------------------------- |
| 界面卡死 35% | 等超时自动恢复桌面                                                           |
| 改等待时长   | `sudo ./windows_update_in_linux --timeout=30` 或 `WINDOWS_UPDATE_TIMEOUT=30` |
| 想看帮助     | `./windows_update_in_linux --help`                                           |

## 便携分发

想发给朋友，**直接把 `./windows_update_in_linux` 这一个文件拷过去**就行，
对方只需要三个很小的运行库（普通安装，无需任何开发包/工具链）：

```bash
sudo apt install libdrm2 libfreetype6 libfontconfig1
./windows_update_in_linux --timeout=60
```

## 自动发布 Release（GitHub Actions）

仓库已配好 `.github/workflows/release.yml`：**只要推送一个 `v*` 标签**，
Actions 会在 ubuntu-24.04 上自动编译，并新建一个带 `windows_update_in_linux`
二进制的 Release（用内置 `GITHUB_TOKEN`，无需 PAT、无需手动上传）：

```bash
git push -u origin main
git tag v1.0.0
git push origin v1.0.0        # 触发自动发布
```

发布后到仓库 **Releases** 页即可下载该二进制。

## 源码布局

```
CMakeLists.txt            构建系统（CMake + pkg-config，仅 3 个依赖）
src/main.c                入口：参数解析 + 调用直渲
src/ttydrm.c/.h           VT 切换 + DRM 帧缓冲 + FreeType 文字 + 伪进度动画
windows_update_in_linux   编译产物（根目录，直接 ./ 运行，已 gitignore）
.github/workflows/        自动发布 Release（推送 v* 标签触发）
```

## 已知限制（如实说明）

- 需 **root**（VT ioctl + DRM master），所以用 `sudo ./windows_update_in_linux`
- `Ctrl+Alt+F1..F7` 等 **TTY 切换是内核级机制**，用户态无法屏蔽——被整的人
  仍能切回桌面 tty（与 bsod 相同）
- 整活期间桌面短暂挂起；进程异常退出时也会尽力恢复桌面
- 若中途被强杀，可能停留在 tty6，`Ctrl+Alt+F1` 可切回

## 免责声明

仅供熟人之间整活娱乐，程序内置超时强制恢复，不会阻止系统关机，也不会
修改/删除任何系统文件。
