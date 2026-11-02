# YOUYOU 远程 Linux 版

在联想拯救者 Y700（Ubuntu 26.04 ARM64 + KDE Plasma）上串流 Windows 电脑，可以用触屏、键盘鼠标或手柄（拯救者 G9、Xbox 手柄）操作。电脑上装官方 UU 远程（GameViewer），和平板登录同一个账号。

v0.2 是重写版，只保留上游的协议核心。解码直接走 V4L2 硬件解码，画面零拷贝交给 GPU，默认开帧节奏器。

## 感谢两位 Switch 客户端作者

- 感谢 **Antman2023** 开发最初的 Switch 原生客户端，打通扫码和短信登录、设备列表、WebRTC 音视频与手柄输入，让自制客户端能直接连接原版 Windows 电脑端。[UURemote-ns](https://github.com/Antman2023/UURemote-ns)
- 感谢 **doke69** 在这个基础上改进设备首页和串流菜单，扩展 H.265 播放、手柄与多点触控、画质设置和网络诊断。本项目使用其 0.3.74 Beta 代码作为直接上游，原样放在 `core/upstream`。[YOUYOU-Remote-ns](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/README.md) · [上游致谢](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/docs/credits.md)

## 安装

在 Y700 上下载最新的 `youyou-linux_<版本>_arm64.deb`，在它所在的目录里运行下面的命令。也可以自己构建（见「构建」）。[下载安装包](https://github.com/bobsixsixsix-boop/YOUYOU-Remote-Linux/releases/latest)

```sh
sudo apt install ./youyou-linux_0.2.0_arm64.deb
```

- 包名还是 `youyou-linux`，会直接替换 v0.1.15。登录状态在 `~/.local/state/youyou-linux/`，升级后不用重新登录。
- 如果在 v0.1.15 里运行过 `youyou-enable-y700-gestures`，升级**前**先运行一次 `youyou-enable-y700-gestures --restore`，把 y700-edge 改回原样。v0.2 不再带这个脚本，改用下面「Y700 上的设置」里的一行配置。
- 装好后从 KDE 应用菜单打开「YOUYOU 远程」，命令是 `youyou`。

装进去的东西：`/usr/lib/youyou-linux/{youyou,libyouyou_core.so}`、`/usr/bin/youyou`（链接）、`youyou-linux.desktop`、图标（SVG 和 256 px PNG）、`/usr/share/doc/youyou-linux/{README.md,copyright}`。依赖 SDL2、Opus、libcurl、EGL/GLES、fontconfig、`fonts-noto-cjk`（界面字体）、`ca-certificates`。

## 使用

第一次打开：

1. 程序要用官方 UU 远程电视版 3.5.0 里的请求签名材料。点「从网易官网下载并准备」，它会下载安装包、在本机提取材料，不保留安装包。没网时可以在终端运行 `youyou import-apk <安装包>` 或 `youyou import-key <signing.key>`。
2. 用手机上的 UU 远程 App 扫码，或者用短信验证码登录。
3. 首页列出账号下的电脑，点在线的那台就开始串流。

设置页（首页右上角）：

| 设置 | 说明 |
| --- | --- |
| 分辨率 | 原生、75 %、50 %、1080p、1440p、自定义（偶数宽高，长边 320–4096，短边 180–2160） |
| 帧率 | 30 / 60 / 120 / 144 / 164 |
| 编码 | H.265 或 H.264 |
| 码率上限 | 自动、20、40、60、80、100 Mbps |
| 专用桌面 | 开启后电脑为平板新建一块虚拟屏，尺寸跟分辨率一样，电脑会记住它的位置和缩放 |
| 触屏 | 多点触控（Windows 触屏手势），或桌面鼠标（单指按下就是鼠标左键，可以直接拖窗口） |
| 画面节奏 | 流畅（默认，帧节奏器）或低延迟，见「实现要点」 |

前四项是上游的画质设置，存在 `config.ini`；后三项存在 `app.ini`。都在下一次连接时生效。

串流中没有角落按钮。下面任一方式打开侧边菜单，点菜单外面、手柄 B 或 Esc 关上：

- 从屏幕左边或右边往里划；
- 手柄同时按 Select（Back）和 Start；
- 键盘 F8。

菜单里能：

- 切换触屏模式；
- 改专用桌面的缩放（电脑给的档位，常见 100 %～225 %；只在专用桌面时出现）；
- 切换电脑屏幕的防窥模式（正常显示 / 隐私屏保 / 关闭显示屏；只在电脑的物理屏上能用，原因见「有意不做」）；
- 看实时统计（编码、尺寸、帧率、解码耗时、往返延迟、码率、丢包）；
- 继续串流或断开连接。

命令行：

```sh
youyou                          # 打开界面
youyou devices                  # 用已保存的登录列出电脑
youyou connect [电脑名或设备 ID]  # 跳过首页直接全屏串流（默认第一台在线的电脑）
youyou play clip.hevc [帧率]     # 不联网：解码并全屏显示一段 H.264/HEVC，打印统计
youyou import-apk <APK> | import-key <signing.key> | download-key
```

环境变量（只管这一次运行）：

- `YY_PACE=1|0`：盖过界面里的画面节奏（流畅 | 低延迟）。
- `YY_VSYNC=1`：低延迟时也开垂直同步。
- `YY_TOUCH=mouse|touch`、`YY_DESKTOP=1|0`：盖过界面里的触屏模式和专用桌面。
- `connect` 时 `YY_STREAM=2280x1428@120`、`YY_MBPS=60`：改串流尺寸、帧率和码率上限。这两个会在连接前存进 `config.ini`，不是临时的。

## Y700 上的设置

- **侧滑开菜单**：Y700 的 y700-edge 会先吃掉屏幕最外面那条边，交给控制中心当「返回」。在 `~/.config/y700-back.conf` 里加一行，让它对本程序发 XF86Back，程序收到就开关菜单：

  ```ini
  youyou-linux = back
  ```

  控制中心每次侧滑都重新读这个文件，不用重启。没有这一行时，最外面那条边划过来会给电脑发 Esc；从离边约 16 个逻辑像素的地方起划，程序自己也能认出侧滑。
- **面板刷新率要是串流帧率的整数倍**：120 帧配 120 Hz，144 帧配 144 Hz。164 Hz 面板放 120 帧必然一抖一抖（见「实测」）。程序不自己切刷新率，由系统（Y700 上是 y700 项目的刷新率守护）管。

## 结构

| 路径 | 是什么 |
| --- | --- |
| `core/upstream/` | doke69/YOUYOU-Remote-ns v0.3.74-beta（`ba4d7f9`）的 git subtree。只做最小的 Linux 适配和「实现要点」里的功能补丁，补丁处都有注释；同步上游用 `git subtree pull`，冲突按注释合。依赖由它自己的 `scripts/fetch_deps.py` 和 `patch_deps.py` 按哈希拉取、打补丁。 |
| `core/upstream/include/stream_limits.hpp` | 补丁新增：允许的帧率（30/60/120/144/164）和串流尺寸（偶数，最大 4096×2160），以及按尺寸和帧率算 SDP 的 H.264/H.265 level。 |
| `core/CMakeLists.txt` | 把上游的登录、信令、WebRTC 和 Opus 音频，加上下面两个文件，编成 `libyouyou_core.so`。`youyou_core.map` 让它只导出 `yy_*`。 |
| `core/bridge.h`、`bridge.cpp` | 给 Rust 用的 C 接口：`yy_open/close`、`yy_command`（直接转给上游 `Client::command`）、`yy_snapshot`（JSON 状态）、`yy_frame_acquire/release/wait`，以及自测用的 `yy_debug_video`。 |
| `core/media_linux.cpp` | Linux 媒体层：把收到的访问单元直接送进 V4L2 有状态解码器（高通 iris），用 `VIDIOC_EXPBUF` 把解出的 NV12 缓冲作为 DMA-BUF 借给播放器。CPU 不碰像素。 |
| `core/tests/decode_test.cpp` | 解码自测：整段、重置后、从 GOP 中间开始，三种情况都要出画面。 |
| `core/tests/protocol_test.cpp` | 协议补丁自测（无依赖，断言）：帧率档位、尺寸校验、屏幕列表解析（含显示缩放，打包 / 不打包 / 截断三种）、虚拟屏尺寸请求的参数校验、缩放请求的字段 9。 |
| `app/` | Rust 程序 `youyou`。自己一个 Cargo 工程（`[workspace]` 空表），`build.rs` 链接 `YY_CORE_LIB_DIR`（默认 `../build`）里的核心库，rpath 是 `$ORIGIN`。 |
| `app/src/bridge.rs` | C 接口的封装。`Core` 拿着账号锁，drop 时先 `yy_close` 再放锁；`Frame` drop 时关 fd 并还缓冲。 |
| `app/src/video.rs` | 零拷贝显示：每个解码缓冲导入成两张 EGLImage（亮度 R8、色度 GR88），着色器里做 YUV→RGB；`fit` 算黑边。 |
| `app/src/input.rs` | 触屏、鼠标、键盘、手柄到 UU 输入协议的映射（来自 v0.1.15，审过，有单测）。 |
| `app/src/clip.rs` | `youyou play` 用：把 Annex-B 文件切成访问单元。 |
| `app/src/main.rs` | 命令行入口；`Screen`（SDL 窗口、GL 上下文、手柄，大厅和串流共用一个窗口，Wayland app_id 设成 `youyou-linux`）；`stream()` 串流循环：转发输入、侧滑 / XF86Back / Select+Start / F8 开菜单、帧节奏器、轮询状态、连接中面板；`Prefs`（`app.ini`）；统计。 |
| `app/src/ui.rs` | 即时模式界面库，和 `video::Renderer` 共用一个 GL 上下文：圆角矩形（SDF 着色器）、文字（ab_glyph，字体用 `fc-match` 找 Noto Sans CJK）、图标（resvg 把 SVG 栅格化成纹理）、二维码、按钮、选项片、输入框、补间动画、段落折行（句末标点不放行首）。尺寸单位是 Android 的 dp。 |
| `app/src/lobby.rs` | 不在串流时的全部页面：下载签名材料、扫码登录、短信登录、电脑列表、设置、自定义尺寸、账号（退出登录要点两次）。返回要连的电脑或退出。 |
| `app/src/menu.rs` | 串流时盖在画面上的东西：侧边抽屉（触屏模式、专用桌面的缩放、防窥模式、继续、断开），以及连接中 / 重连中的面板。 |
| `account/` | 账号材料：从官方 APK 提取签名、client-id、状态目录（0700）、单实例锁。 |
| `assets/icons/*.svg` | 界面图标（7 个），编进程序里（`ui.rs` 的 `ICONS`）。 |
| `assets/launcher.svg` | 双屏启动器图形，也是 deb 里的 SVG 图标。由它生成 `youyou-linux.png`（256 px，deb 图标）和 `youyou-mark-192.rgba`（192×192 RGBA8，登录页和首页的标志）。 |
| `packaging/deb.sh` | 在 Y700 上从构建结果打 deb，见「构建」。 |
| `packaging/source.sh` | 发版用的完整源码包：tag 导出的源码 + 上游锁定的原生依赖（`core/upstream/external`）+ `cargo vendor` 出的 Rust 库，解开就能离线构建。 |
| `packaging/licenses.py` | 生成 deb 里的 `copyright`：本项目和上游的许可与作者声明、静态链接进核心库的原生依赖（按上游 `sources.lock.json`）、实际链接的 59 个 Rust 库（`cargo metadata`，按 `Cargo.lock` 的校验值）。 |
| `packaging/THIRD-PARTY.md`、`NOTICE.md` | 来源与许可说明、作者致谢。 |
| `packaging/youyou-linux.desktop` | 菜单项。`StartupWMClass=youyou-linux` 和程序的 app_id 对上，KWin 才能找到图标。 |
| `tools/` | 测量和测试工具，结果在 `tools/results/`，原始输出在 `tools/results/runs/`（不进 git）：<br>• `decbench.c`、`v4l2bench.c`：解码延迟对比。<br>• `pacing-run.sh`：本地片段的上屏节奏，跑完把面板切回原来的模式。<br>• `desktop-run.py` + `stream-run.sh`：真网络专用桌面测量。电脑端跑前者，它在平板上启动后者。<br>• `vpad.py`：平板上的虚拟 Xbox 360 手柄和键盘（uinput，从 FIFO 收命令），不用真手柄测手柄和按键路径。<br>• `input-watch.py`：电脑端记录 XInput 手柄和显示器状态，和 `vpad.py` 配对用。 |

## 构建

在 Y700 上（Ubuntu 26.04 ARM64）本机编译：

```sh
# 一次：
sudo apt install build-essential cmake ninja-build pkg-config python3 cargo rustc \
  libsdl2-dev libopus-dev libcurl4-openssl-dev libegl-dev libgles-dev dpkg-dev
# 只有自测（-DYY_TESTS=ON）要：sudo apt install libavformat-dev libavcodec-dev libavutil-dev
# 拉上游依赖（按哈希校验，要联网）：
(cd core/upstream && python3 -I scripts/fetch_deps.py && python3 -I scripts/patch_deps.py)
# 核心库和自测：
cmake -S core -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DYY_TESTS=ON
cmake --build build
# 程序和单测：
(cd app && YY_CORE_LIB_DIR=../build cargo build --release && cp ../build/libyouyou_core.so target/release/)
(cd app && LD_LIBRARY_PATH=../build cargo test --release)
(cd account && cargo test --release)
# 打包：
sh packaging/deb.sh out
```

- `deb.sh` 要求 `build/libyouyou_core.so` 和 `app/target/release/youyou` 已经编好。版本号取 `app/Cargo.toml`；共享库依赖由 `dpkg-shlibdeps` 从二进制算出来，再加上运行时才打开的 EGL/GLES、fontconfig、字体和 CA 证书。
- 用 Release 里的源码包 `youyou-linux-<版本>.tar.xz` 构建时不用联网：原生依赖已经在里面（`fetch_deps.py`、`patch_deps.py` 发现做过就跳过），Rust 库在 `vendor/`，`.cargo/config.toml` 让 cargo 只用它。
- 自测程序在 `build/` 下：`protocol_test`（无依赖）、`decode_test <片段.h264|.hevc> [帧率]`（要 iris 解码器；FFmpeg 只用来读片段）。

## 实现要点

- **解码**：直接用 V4L2，不走 FFmpeg 或 VA-API。同样的片子，从送进去到能显示（p50）：HEVC 2560×1600 4.5 ms 对 VA-API 12.1 ms，HEVC 1080p 3.7 对 7.6（`tools/results/20261102-decode-latency.txt`）。丢包或积压时跟上游一样，丢到下一个随机访问点并请求关键帧。HEVC 的 CRA 也算随机访问点，因为 x265 的周期关键帧就是 CRA。
- **交给播放器**：核心只留最新的一帧，播放器没来得及取的帧直接还给解码器。节奏由播放器管，因为只有它知道屏幕什么时候刷新。`yy_frame_wait` 让播放器的唤醒线程睡到有新帧为止，不轮询。
- **显示**：解码缓冲按缓冲号导入一次，之后复用；解码器重新初始化（`epoch` 变了）时全部释放。导入**必须**带显式的 `DRM_FORMAT_MOD_LINEAR` modifier，原因见「踩过的坑」。颜色矩阵在自己的着色器里做（BT.709 有限范围），不依赖驱动的 YUV 采样。
- **画面节奏**（`tools/results/20261102-pacer.txt`）：
  - 流畅（默认）：手里最多压 3 帧，每次刷新交出最旧的一帧，交换间隔 1（等 vblank）。如果半秒里每次都压着不止一帧，就丢一帧追上（Moonlight 的做法）。三组 A/B 里，一帧一刷都是 99.8–99.9 %，没有一帧停超过两刷。代价是出解码器到交换完成多 10–14 ms，到眼睛的实际差别约一个刷新（8.3 ms）。
  - 低延迟：每次只显示最新的一帧，交换间隔 0。网络一抖就会一刷来两帧、下一刷没有：同样三组里一帧一刷只有 94.4–99.5 %。
  - 交换间隔 0 不会撕裂：KWin 总在 vblank 翻页，SDL2 2.32 也没实现 Wayland 的撕裂协议。
- **输入**：指针移动在一轮事件里合并成一次，按键、按钮、滚轮之前先发掉，所以不会积压。上游把输入放进队列，工作线程最多等 4 ms。协议里只有绝对坐标（`mouse_move_absolute`），没有相对鼠标。手柄由 GameViewer 在电脑上变成 XInput 手柄 0，电脑上不用装东西。
- **串流中的菜单**：
  - 从左右边缘 24 dp 内按下、往里划 40 dp 才算开菜单，起点附近的触摸不发给电脑。
  - Y700 最外面那条边归 KWin，程序收不到触摸；配了 `y700-back.conf` 后控制中心发 XF86Back（SDL 的 `AcBack`），程序把它和 F8 一样当开关。
  - 打开菜单时先松开电脑上所有按着的触点、键和手柄（本地 + 上游的 `release`），避免卡键。断开走上游 `disconnect`，回到首页。
- **Wayland app_id**：SDL2 没有设 app_id 的提示，只能在建窗口前设环境变量 `SDL_VIDEO_WAYLAND_WMCLASS=youyou-linux`（已经设了就不动）。KWin 按它找 `youyou-linux.desktop` 的图标，控制中心按它查 `y700-back.conf`。
- **高帧率**（补丁，审自 v0.1.15 的 high_fps）：`gv_pb.FPS` 最高只到 144 档（4）。120、144、164 都用 4 档，确切帧率放在 `CaptureParams.fpsCount`（连接时字段 11，RPC 字段 18）。电脑回报的只有档位，所以回报 4 档只能证明是 144。帧率专用的 `FpsSettingRequest` 只有枚举，所以改到 120/164 要重连。
- **任意尺寸**（补丁，审自 v0.1.15 的 custom_resolution）：偶数宽高、最大 4096×2160 都放行。不是 720p/1080p 时，能力 JSON 里带上自定义尺寸。SDP 的 level 按亮度采样率算（1080p、120 帧以内保持原来的报价）。
- **专用桌面**（补丁）：连接选项字段 1 填 6（`CT_SECOND_SCREEN`）而不是 1（`CT_DESKTOP`），字段 9 填本机设备 ID。电脑会为这次会话新建一块虚拟屏，并按客户端记住它的尺寸和位置。
- **虚拟屏尺寸**（补丁，审自 v0.1.15 virtual_display 的 Initialize）：电脑按记住的尺寸建屏，不看这次请求的尺寸。所以专用桌面会话收到屏幕列表后，如果当前屏是虚拟屏（类型 1）、尺寸不对、而它的模式列表里有串流尺寸，就发一次「选屏 + 桌面尺寸 + 编码尺寸」（方法 2）。电脑应答后会改虚拟屏，也会记住新尺寸。发过之后，本会话不再发上游的视频缩放请求（原因见「踩过的坑」）。
- **专用桌面的缩放**（补丁）：
  - 屏幕列表里每块屏带一个 `DpiScale`（`Screen` 字段 9：1 当前、2 推荐、3 可选档位，百分数，档位可能打包也可能不打包）。`quality_rpc.cpp` 解析它，`client.cpp` 放进快照的 `desktopScale`。
  - 改缩放就是再发一次画质更新，`CaptureSettingRequest` 多带字段 9 `dpi_scale`。电脑应答后在 `CaptureSettingSync` 字段 8 回报新值；出错时菜单显示错误码，选项片回到当前值。
  - 只在当前屏是虚拟屏（类型 1）时出现。在物理屏上它改的是电脑自己显示器的缩放，会弄乱坐在电脑前的人的桌面。
  - 电脑按客户端记住虚拟屏的缩放（实测：设成 200 % 后断开重连还是 200 %），所以平板不另存，也不在连接时主动发。2280×1428 的虚拟屏默认 100 %，在 8.8 英寸上字太小，建议 200 %。
- **防窥模式**：上游的 `privacy-screen` 命令（1 正常显示、2 隐私屏保、3 关闭显示屏），只在串流中、设备 ID 对得上时有效。等电脑应答期间，选项片显示请求的模式；应答后显示 `mode`。电脑拒绝时菜单里显示错误码。专用桌面上电脑一律回 508，所以那时菜单不放选项片，只写一句原因（`tools/results/20261102-input.txt`）。
- **退出时断开**：上游的 `Client` 析构时不通知电脑断开，电脑会一直显示「被控制中」，直到心跳超时。播放器退出前先发 `disconnect`，等 `activeDeviceId` 清空（上游断开的最后一步），最多 3 秒。
- **界面尺寸按 Android 的 dp 算**：1 dp = 1/160 英寸，Y700 是 8.8 英寸 3040×1904，约 408 ppi，所以 1 dp = 2.55 像素，整屏约 1192×747 dp，页面在玻璃上和官方安卓 App 一样大。KDE 自己的缩放是 1.9，照它画的话 16 号字只有 1.9 mm 高，在平板上看不清。合成器报的屏幕物理尺寸是错的（168×268 mm，等于 12.4 英寸），运行时算不出真实密度，所以 `ui.rs` 里写死 `PANEL_PPI = 408`。换别的设备要改这个数；比合成器缩放还小时取合成器的。
- **界面是即时模式**：每帧从状态重画整页，不存控件树。没动静时不画：事件循环在没事件、没动画时睡 250 ms（只为轮询上游状态），有补间动画时 8 ms 一帧。串流中只有菜单或面板开着才画界面层，画面本身照常按新帧上屏，打开菜单不卡画面（实测 379 帧跳 1 帧）。
- **deb 的版权文件**：`licenses.py` 只收这次构建真的链接进去的东西。Rust 库用 `cargo metadata --offline --locked --filter-platform aarch64-unknown-linux-gnu` 走正常依赖的闭包，不收只在别的平台或只在构建时用的库。

## 实测

### 本地片段（Y700，HEVC 2560×1600，120 帧）

| 面板 | 一帧一刷 | 一帧停两刷 | 1 % low |
| --- | --- | --- | --- |
| 164 Hz | 63.4 % | 36.6 %（每秒 44 次，均匀分布） | 75–82 fps |
| 120 Hz | 99.9 % | 0.06–0.11 % | 107–113 fps |

164 不是 120 的整数倍，所以在 164 Hz 面板上放 120 帧必然一抖一抖。这跟播放器无关，换什么播放器都一样。

### 真网络，加帧节奏器之前（Wi-Fi 连 Windows 电脑，面板 120 Hz）

`tools/results/20261102-network.txt`，各只有一次 30 秒的样本，只能看个大概：

| 实际串流 | 一帧一刷 | 1 % low | 解码 |
| --- | --- | --- | --- |
| HEVC 2560×1440，120 帧，专用桌面 | 95.75 % | 37.4 fps | 4.5 ms，119.5 fps，0 丢 |
| HEVC 1920×1080，120 帧，专用桌面 | 98.70 % | 42.9 fps | 2.4 ms |
| HEVC 2280×1428，120 帧，专用桌面（n1） | 99.52 % | 64.6 fps | 3.6 ms，0 丢 |
| HEVC 2280×1428，120 帧，专用桌面（n2，隔两分钟） | 95.92 % | 56.7 fps | 3.5 ms，0 丢 |

n1 和 n2 一样的程序、一样的设置，差了 3.6 个百分点。看 `ml-flips.py` 的相位列：n1 每帧在上次翻页后约 3 ms 出解码器，n2 约 5.8 ms，正好卡在 KWin 决定下一刷放哪帧的时间点上，一点网络抖动就让帧在「赶上」和「迟到」之间来回跳。这就是后来加帧节奏器的原因。

### 帧节奏器 A/B（2280×1428@120，H.265，面板 120 Hz，三组背靠背）

`tools/results/20261102-pacer.txt`：

| | 一帧一刷 | 1 % low | 最长一帧 | 出解码器→交换完成 p50 |
| --- | --- | --- | --- | --- |
| 低延迟（`pace=0`） | 94.44–99.53 % | 53.7–77.8 fps | 33–50 ms | 0.4–0.5 ms |
| 流畅（`pace=1`） | 99.81–99.89 % | 100–107.7 fps | 16.7 ms | 10.6–14.5 ms |

### 输入和防窥（`tools/results/20261102-input.txt`）

- 虚拟手柄（`vpad.py`）的 A、Y、Back、十字键、左摇杆、右扳机都原样到了电脑的 XInput 手柄 0，摇杆向上是正值。
- Back+Start 开菜单，菜单里 B 关上，B 不会发到电脑；F8 开关菜单。
- 防窥：电脑物理屏上 2、3、1 都应答 0，串流不断；专用桌面上 2、3 都回 508。

## 踩过的坑（根因）

- **EGL 导入报 `EGL_BAD_ALLOC`**（Mesa 26 freedreno，Adreno 840）：
  - 不带 modifier（即 `DRM_FORMAT_MOD_INVALID`）时，按单张 NV12 导入或按 R8/GR88 分平面导入都会被拒。
  - 显式传 `EGL_DMA_BUF_PLANE0_MODIFIER_LO/HI_EXT = 0`（LINEAR）就好了。
  - 按单张 NV12 导入还有第二个问题：Mesa 会把它拆成 `r8_g8b8_420` 两个平面，960×540 的色度平面被拒。所以用两张图。
  - 导出时用 `O_RDONLY` 就够了，`O_RDWR` 试过，不是原因。
- **退出时 abort（`std::system_error: Invalid argument`）**：静态的 `uu::Media` 和帧槽在 `exit()` 析构时，别的线程还在用它们。两者都改成故意不析构（`*new`）。
- **iris 的可见尺寸**：要在 `S_FMT` 之前读 CAPTURE 的 crop。之后 iris 会把 crop 重置成编码尺寸（1080 变 1088）。
- **自测片段有 B 帧**：x264 的 `-preset fast` 会加 B 帧，播放器看到的帧序就不是显示序，统计成跳帧 25 %。测试片要加 `-bf 0`。真实串流没有 B 帧。
- **测试脚本发 SIGINT，播放器没反应**：bash 会让后台任务忽略 SIGINT，SDL 看到「已忽略」就不装处理函数。结果统计没打印、也没发断开，最后被 SIGKILL。要让播放器正常退出，用 SIGTERM。
- **请求 2560×1600，收到的是 2560×1440**：电脑按客户端记住的尺寸建虚拟屏（当时是 2280×1428），然后把编码套到最近的标准档位，宽高比也变了。解决办法是「虚拟屏尺寸」请求。这次请求还让电脑把记住的尺寸改成了 2560×1440，下一次会话就按这个建屏。
- **虚拟屏改好后又被改回去**：上游的视频缩放请求在字段 5/6 填的是当时观察到的画面尺寸（旧的 2560×1440）。它和桌面尺寸请求几乎同时发出，电脑先改成 2280×1428，又被它改回 2560×1440，编码最后落在 1920×1080。所以发过桌面尺寸请求的会话，不再发视频缩放请求。
- **串流时电脑的物理显示器关不掉**（只影响测试）：GameViewer 连接时和串流中约每半秒请求一次「显示器保持亮」；强行熄屏，Windows 就不再合成任何屏幕（包括虚拟屏），串流掉到个位数帧；把物理屏从显示布局里摘掉，GameViewer 约 1 秒内会按记住的布局重新启用。所以在电脑上测专用桌面时让物理显示器亮着。
- **贴着屏幕边缘划不开菜单**：y700-edge 会吃掉起点离屏幕边约 6 个逻辑像素以内的触摸，程序收不到。v0.1.15 的做法是改 KWin 脚本，v0.2 用 y700 自己留的 `y700-back.conf` 配一行就行（见「Y700 上的设置」）。
- **字太小**（真机反馈）：一开始按 SDL 窗口单位（= KDE 的 1.9 缩放）排版，以为是 12 英寸屏。改成按 dp 排版，见「实现要点」。
- **合成触摸点不中**（测试工具）：y700 的 vtouch 坐标是 KWin 的逻辑坐标（缩放 1.9，整屏 1600×1002），而截图和程序的布局按 200 % 算（1520×952）。程序坐标要乘 2/1.9 才是 vtouch 坐标，屏幕右边缘是 x≈1599。
- **deb 根目录权限是 drwx------**：`mktemp -d` 建的目录是 0700，会原样成为包里的 `./`，装上后 `/` 的权限被改。`deb.sh` 先 `chmod 755`。
- **`dpkg-shlibdeps` 解析不了 `$ORIGIN`**：它从二进制所在目录往上找有 `DEBIAN/` 的目录当包根，再按包根解析 rpath。所以要先建 `DEBIAN/`，而且二进制要传绝对路径：传相对路径时它找到最上一级（`usr`）就停了，不查当前目录。
- **`cargo metadata --offline` 报缺包**：不带 `--filter-platform` 时它要解析所有平台的依赖（比如 wasm 用的 bumpalo），离线时没下载过就失败。

## 有意不做

- **AV1**：GameViewer 4.45 做不到。它的 protobuf 里 `CodecType` 只有 H264 和 H265；客户端报价 AV1，电脑照样回 HEVC。平板这边是能解的（iris 解 1080p AV1 约 4.5 ms；RTP 收到的每帧前面要补时间分隔符 OBU `12 00` 才能送进解码器）。v0.1.15 的接收补丁留在 tag `v0.1.15` 的 `native/av1-receiver.patch`，等电脑端支持再搬。
- **专用桌面上的防窥**：电脑拒绝（508）。v0.1.15 在菜单里放了「切换普通桌面并重连」，v0.2 不放：切换会收掉专用桌面、电脑上的窗口会挪位置，这种事应该在设置里明确关掉专用桌面再连。
- **Back 键不等 Start**：手柄 Back 是真按键，单按要立刻发给电脑；Back+Start 开菜单时 Back 已经先到电脑，开菜单时会一起松开。为了组合键给每次 Back 加延迟不值得。
- **播放器不自己切屏幕刷新率**：Wayland 没有让普通程序改显示模式的协议。面板配哪个刷新率由系统决定。
- **只按 BT.709 有限范围转色**：UU 主机发的就是这个。哪天主机发别的，再读码流的 VUI（V4L2 colorspace）。
- **没有相对鼠标**：协议里没有。
- **首页不滚动**：一屏放 3 列 × 3 行，最多 9 台电脑。等有人的电脑更多再加（代码里有 `shortcut:` 注释）。
- **没有屏幕键盘**：手机号、验证码、自定义尺寸都用页面里自带的数字键盘输入，这几处只要数字。
- **本地导入 APK / 签名只有命令行**：界面里只有下载，导入是给没网或要自己提供材料的人用的，少数情况。
- **启动时的错误只到 stderr**：已有账号材料但打不开（如账号目录权限不是 0700）时，程序不开窗口直接退出，原因打在终端或 journal 里。第一次下载材料后打不开的，页面上会显示「准备失败」。
- **`PANEL_PPI` 写死成 Y700**：原因见「实现要点」。本项目只针对 Y700。
- **没搬 v0.1.15 的整套虚拟屏状态机**（会话中新建、切换、移除虚拟屏，主机显示器切换，「记忆专用桌面」开关，`encoderBounds` 放宽编码档位）：「连成专用桌面、桌面就是串流尺寸」只需要一个请求。要选屏时再按需搬。
- **没搬 v0.1.15 的 `--preview`、`--self-test`、GPU 显示开关、F11**：离线自测用 `youyou play`；零拷贝是唯一的显示路径，没有可关的；窗口总是全屏。
- **帧率回报只到档位**：120 和 164 的「已应用」帧率显示为未知，因为电脑只回 4 档（等于 144）。

## 还没弄清 / 还没测

- 电脑对 60 Mbps 的码率上限回报「已应用 100 Mbps」，原因还没查。
- 真 G9 手柄（震动、它自己的按键映射）还没测，只用虚拟 Xbox 360 手柄测过。
- 「关闭显示屏」时 Windows 的显示器状态一直是「开」：GameViewer 不是走电源接口熄屏的，只能看电脑的应答或请电脑前的人确认。

## 维护清单

- [ ] `git subtree pull` 上游之后：改 `packaging/licenses.py` 的 `UPSTREAM_REF`、`packaging/THIRD-PARTY.md` 和 `NOTICE.md` 里的提交号；跑 `protocol_test`、`decode_test`；真机连一次。
- [ ] 改了 `assets/launcher.svg`：重新生成 `assets/youyou-linux.png`（256×256）和 `assets/youyou-mark-192.rgba`（192×192，RGBA8 直排）。
- [ ] 加、删 Rust 依赖之后：打一次 deb，看 `licenses.py` 有没有报缺许可文本。
- [ ] 发版（推 GitHub、发 Release 每次都要主人点头）：
  1. 改 `app/Cargo.toml` 的版本和 README 里安装命令的文件名，commit、push，打 tag `v<版本>` 并推送。
  2. 在 Y700 上把 tag 用 `git archive` 导出成干净目录，跑 `fetch_deps.py`、`patch_deps.py`，再 `sh packaging/source.sh out`。
  3. 把这个源码包解到另一个目录，按「构建」离线编一遍、跑全部自测，再 `deb.sh`。发出去的 deb 就是从发出去的源码包编出来的。
  4. 装到 Y700 上从菜单打开，连一次电脑、侧滑开关菜单、断开。
  5. `sha256sum` 两个文件写进 `SHA256SUMS`，三个文件一起传到 GitHub Release。
- [ ] GameViewer 大版本更新之后：重测专用桌面上的防窥（还回不回 508）、AV1（`CodecType` 有没有加）、码率上限回报。

## 许可与资源

应用代码采用 GPL-3.0-or-later。源码与安装包都不附带官方 APK、请求签名密钥或用户会话。本项目由社区维护，与网易、任天堂和联想无隶属关系。依赖和上游资源适用各自的许可。[项目许可证](LICENSE) · [作者与来源](NOTICE.md) · [第三方许可清单](packaging/THIRD-PARTY.md)
