# YOUYOU Remote Linux 来源与许可

本项目的代码采用 GPL-3.0-or-later：Rust 应用（`app/`、`account/`）、Linux 媒体层与 C 接口（`core/*.cpp`）和打包脚本。感谢 Antman2023 提供原始 Switch 客户端的协议、音视频与输入基础，也感谢 doke69 维护直接上游并扩展界面、控制和硬件显示。[作者说明](../NOTICE.md) · [项目许可证](../LICENSE)

## 上游与项目图形

- 协议核心是 doke69 的 0.3.74 Beta，以 git subtree 原样放在 `core/upstream`，固定提交 `ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b`。上游锁文件固定原生依赖的提交与归档哈希。[固定版本](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/README.md) · [原生依赖清单](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/scripts/sources.lock.json)
- doke69 的直接上游为 Antman2023 的 UURemote-ns，共同祖先提交 `d56405991886de59809987c23161521fb3eafcba`。[原始 Switch 项目](https://github.com/Antman2023/UURemote-ns/tree/d56405991886de59809987c23161521fb3eafcba) · [上游资源和研究参考](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/docs/credits.md)
- 启动器的双屏 SVG 及由其生成的 PNG、RGBA 图形采用本项目的 GPL-3.0-or-later 许可。界面图标是本项目自绘的 SVG。登录所需的官方 APK 与签名材料由使用者在本机准备，不随包分发。UU 远程名称及商标属于各自权利人。[启动器 SVG](../assets/launcher.svg)

## 静态链接进 libyouyou_core.so 的组件

| 组件 | 许可 |
| --- | --- |
| libdatachannel、libjuice | MPL-2.0 |
| Mbed TLS | Apache-2.0 OR GPL-2.0-or-later。Everest 模块保留各文件声明 |
| usrsctp、libSRTP | BSD-3-Clause |
| plog、nlohmann/json | MIT |

Rust 依赖（SDL2 绑定、glow、ab_glyph、resvg、qrcode、serde_json、zip、sha2 等）随程序静态链接，版本与包校验值由 [app/Cargo.lock](../app/Cargo.lock) 记录。

SDL2、Opus、libcurl 和 GLVND 的 EGL / OpenGL ES 接口通过系统共享库使用；界面字体由 fontconfig 从系统里找（Ubuntu 上是 fonts-noto-cjk 的思源黑体），不随包分发。Ubuntu 系统包的完整版权见 `/usr/share/doc/<包名>/copyright`。

Debian 包中的 `/usr/share/doc/youyou-linux/copyright` 收录本次构建实际链接的原生依赖和 Rust 库的许可文本及作者声明，由 [licenses.py](licenses.py) 在构建机上生成。
