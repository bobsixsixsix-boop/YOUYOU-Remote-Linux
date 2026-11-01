# YOUYOU 远程 Linux 版

在运行 Ubuntu 26.04 ARM64 和 KDE Plasma 的联想 Y700 上，用触屏或拯救者 G9 手柄串流 Windows 桌面。电脑端使用官方 UU 远程，与平板登录同一账号。

## 感谢两位 Switch 客户端作者

- 感谢 **Antman2023** 开发最初的 Switch 原生客户端，打通扫码和短信登录、设备列表、WebRTC 音视频与手柄输入，让自制客户端能直接连接原版 Windows 电脑端。[UURemote-ns](https://github.com/Antman2023/UURemote-ns)
- 感谢 **doke69** 在这个基础上改进设备首页和串流菜单，扩展 H.265 播放、手柄与多点触控、画质设置和网络诊断。Y700 的设备卡与手柄导航由这套界面移植而来。本项目使用其 0.3.74 Beta 代码作为直接上游。[YOUYOU-Remote-ns](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/README.md) · [上游致谢](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/docs/credits.md)

## 安装与登录

在 Y700 上下载最新的 `arm64.deb` 安装包，进入只放这一版本安装包的目录后执行安装命令。[下载 ARM64 安装包](https://github.com/bobsixsixsix-boop/YOUYOU-Remote-Linux/releases/latest)

```sh
sudo apt install ./youyou-linux_*_arm64.deb
```

从 KDE 应用菜单打开「YOUYOU 远程」。首次启动选择「从网易官网下载并准备」，或选择已有的官方 UU 远程 TV 3.5.0 APK。客户端在本机提取并校验此版本的请求签名材料。

用手机 UU 远程 App 扫码确认，或选择「短信登录」。在设备列表选择在线电脑，点「进入桌面」。

## 平板操作

![Y700 横屏设备首页，使用离线演示数据](docs/home.webp)

| 操作 | 方法 |
| --- | --- |
| 移动焦点、确认、返回 | G9 十字键或左摇杆移动，A 确认，B 返回 |
| 用触屏开关串流工具栏 | 从左边缘向右或从右边缘向左轻扫即可打开。在任何一层，点面板外的串流画面或右上角的 × 即可回到串流；左上角的返回箭头只退一层 |
| 用手柄或键盘开关工具栏 | G9 同时按 Select + Start，或按键盘 F8 |
| 用手指点按、拖动 Windows 桌面 | 在工具栏打开「外设 → 触屏操作 → 桌面鼠标」。手指落下即按下鼠标左键，按住移动即可拖窗口，抬手松开 |
| 切换全屏 | 按 F11 |
| 连接前调整参数 | 首页进入「串流设置」。横屏左栏显示当前配置，右栏调整选项 |
| 切换主机屏幕 | 串流工具栏进入「显示 → 主机显示器」 |
| 创建平板专用桌面 | 「显示 → 平板专用桌面」使用 Windows 端的 UU 虚拟显示驱动。要移除本次创建的屏幕，先切回另一块屏幕 |
| 记住专用桌面的位置 | 在「平板专用桌面」中开启「记忆专用桌面」，主机按客户端身份保存布局，断开后收回屏幕，下次连接时恢复位置。切换此选项会重新连接 |
| 被控端防窥 | 工具栏点「防窥」，可启用隐私屏保、关闭被控显示屏或恢复正常显示。当前主机版本在「记忆专用桌面」模式下拒绝防窥，页面提供「切换普通桌面并重连」入口。切换会关闭专用桌面，窗口可能移动。当前状态以电脑回报为准 |

如果 Y700 桌面安装了 `y700-edge` 系统返回手势，在平板的 KDE 会话里用当前用户运行一次：

```sh
youyou-enable-y700-gestures
```

设置后，前台串流窗口接管左右边缘，手指可以从屏幕外直接划进来。回到设备首页、切换应用或关闭客户端后，系统边缘返回自动恢复。若要撤销这次设置，运行 `youyou-enable-y700-gestures --restore`。

触屏模式的选择会保存在平板上。默认的「原生多点触控」适合使用双指缩放等 Windows 触屏手势；切换到「桌面鼠标」后，单指直接定位指针，边缘侧滑仍打开本地工具栏。

## 画面与带宽

- **分辨率。** 可按平板比例选择短边 720、1080、1440 或原生宽高的 50%、75%、100%，也可选择常用 16∶9 档或自定义宽高。3040 × 1904 屏幕的 1080 档请求 1724 × 1080，50% 请求 1520 × 952。连接后，尺寸页分别显示桌面尺寸、串流目标与实际收到的画面。主机可能采用相邻档位。
- **帧率与码率。** 帧率上限可选 30、60、120、144 或 164 FPS，实际帧率受主机屏幕刷新率、编码能力和网络限制。目前实测的 UU 虚拟屏最高为 144 Hz，选择 164 FPS 时主机仍按 144 FPS 输出。客户端垂直同步默认关闭。带宽紧张时，可在「画面设置 → 码率上限」选择 5 至 100 Mbps 的预设档。实际带宽还包括声音、传输开销和重传。
- **编码。** 支持 AV1 优先、H.265 和 H.264。AV1 优先会提供回退选项，重新连接时与主机协商。编码页可查看本次实际收到的格式。
- **硬件解码。** 在 Y700 上已验证 VA-API 硬解 H.264、H.265 和 1080p120 AV1 测试片。真实串流的编码和帧率取决于主机与网络。用 `UU_DECODER=software youyou-linux` 可比较软件解码，`UU_VAAPI_DEVICE` 可指定 VA-API 设备。

实验 GPU 显示将兼容的 VA-API 解码画面直接交给 GPU 呈现，减少画面回传 CPU 的开销，并在新帧就绪时唤醒显示线程。此功能默认关闭。退出客户端后，在 `${XDG_STATE_HOME:-$HOME/.local/state}/youyou-linux/config.ini` 将 `gpu_video` 设为 `1`，缺少此项时添加 `gpu_video=1`，再从桌面启动即可启用。改为 `gpu_video=0` 或临时用 `YOUYOU_GPU_VIDEO=0 youyou-linux` 启动可关闭。渲染器或画面格式不兼容时会回退到原有显示路径。

## 预览与自检

```sh
youyou-linux --preview home
youyou-linux --self-test
```

离线预览使用演示设备。自检使用合成画面、音频和输入，检查解码、显示与菜单导航。账号配置和会话保存在 `${XDG_STATE_HOME:-$HOME/.local/state}/youyou-linux/`。可从「账号」页退出登录。

## 从源码构建

以下命令适用于 Ubuntu 26.04 ARM64 的本机编译。使用发行版提供的 Rust 工具链，生成 Debian 包时会读取对应 Rust 标准库的版权文件。

```sh
sudo apt install git build-essential cmake ninja-build pkg-config patch python3 cargo rustc \
  libsdl2-dev libsdl2-ttf-dev libavcodec-dev libavutil-dev libswscale-dev \
  libopus-dev libcurl4-openssl-dev libva-dev libegl-dev libgles-dev \
  librsvg2-2 libcairo2 ca-certificates curl xz-utils
git clone https://github.com/bobsixsixsix-boop/YOUYOU-Remote-Linux.git
cd YOUYOU-Remote-Linux
sh build.sh --deb
```

安装包生成在 `build/`。从 Git 工作树首次构建需要联网获取固定的上游快照和依赖。原生代码按提交和 SHA-256 校验，Rust 依赖按 `Cargo.lock` 锁定。每个二进制 Release 同时提供与该版对应的完整源码包，内含原生与 Rust 依赖。在已装好系统构建工具的设备上，将完整源码包解压后进入包含 `build.sh` 的源码目录，运行同一条 `sh build.sh --deb` 即可构建。[Release 源码与安装包](https://github.com/bobsixsixsix-boop/YOUYOU-Remote-Linux/releases/latest)

Rust 负责 Linux 程序入口、账号材料导入、登录界面、窗口、画面呈现和输入处理。C++ 层沿用上游的登录与信令、WebRTC 和媒体管线，Linux 移植在这里接入 VA-API，并适配平板界面。

## 许可与资源

应用代码采用 GPL-3.0-or-later。公开版用项目绘制的双屏图标，源码与发行包均不附带官方 APK、请求签名密钥或用户会话。本项目由社区维护，与网易、任天堂和联想无隶属关系。字体、依赖和上游资源适用各自的许可。[项目许可证](LICENSE) · [作者与来源](NOTICE.md) · [第三方许可清单](packaging/THIRD-PARTY.md)
