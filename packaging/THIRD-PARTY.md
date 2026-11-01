# YOUYOU Linux 来源与许可

项目的应用代码采用 GPL-3.0-or-later，包含 Linux 前端、播放器、账号材料导入、原生接口和构建适配。感谢 Antman2023 提供原始 Switch 客户端的协议、音视频与输入基础，也感谢 doke69 维护直接上游并扩展界面、控制和硬件显示。[作者说明](../NOTICE.md) · [项目许可证](../LICENSE)

## 上游与项目图形

- Linux 移植直接基于 doke69 的 0.3.74 Beta，固定提交为 `ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b`。构建脚本校验下载归档的 SHA-256，上游锁文件固定原生依赖的提交与归档哈希。[固定版本](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/README.md) · [原生依赖清单](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/scripts/sources.lock.json)
- doke69 的直接上游为 Antman2023 的 UURemote-ns。与此移植共同的祖先提交是 `d56405991886de59809987c23161521fb3eafcba`。[原始 Switch 项目](https://github.com/Antman2023/UURemote-ns/tree/d56405991886de59809987c23161521fb3eafcba) · [上游资源和研究参考](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/docs/credits.md)
- 公开版启动器的双屏 SVG 及由其生成的 PNG、RGBA 图形采用本项目的 GPL-3.0-or-later 许可。登录所需的官方 APK 与签名材料由使用者在本机准备。UU 远程名称及商标属于各自权利人。[启动器 SVG](../assets/launcher.svg)

## 链接与分发的组件

| 组件 | 许可 |
| --- | --- |
| libdatachannel、libjuice | MPL-2.0 |
| Mbed TLS | Apache-2.0 OR GPL-2.0-or-later。Everest 模块保留各文件声明 |
| usrsctp、libSRTP | BSD-3-Clause |
| plog、nlohmann/json | MIT |
| Noto Sans SC Regular / Bold | SIL Open Font License 1.1 |

客户端使用上游随附的 Noto Sans SC 字体，安装包同时安装字体的 OFL 文本。[项目字体来源](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/assets/fonts/OFL.txt) · [Noto CJK](https://github.com/notofonts/noto-cjk)

SDL2、SDL_ttf、Opus、FFmpeg、libcurl、libva 及 GLVND 的 EGL / OpenGL ES 接口通过系统共享库链接。libva 主体采用 MIT（Expat）许可，GLVND 主体采用 MIT 许可，部分 Khronos 头文件采用 Apache-2.0。Ubuntu 系统包的完整版权和各文件许可可在 `/usr/share/doc/<包名>/copyright` 查看。Rust 依赖的版本与包校验值由锁文件记录。[Cargo.lock](../Cargo.lock)

Debian 包中的 `/usr/share/doc/youyou-linux/copyright` 收录本次构建使用的原生依赖、Rust 库和静态组件的许可文本及作者声明。每个二进制发行版另附对应版本的完整源码、补丁和构建脚本。[生成版权清单的脚本](licenses.py) · [发行文件](https://github.com/bobsixsixsix-boop/YOUYOU-Remote-Linux/releases/latest)
