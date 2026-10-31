# Authorship and attribution

YOUYOU Remote Linux is a community Linux port of doke69's YOUYOU-Remote-ns, itself based on Antman2023's UURemote-ns. We thank both authors for the work this port builds on.

- **Antman2023** created the original Nintendo Switch client, including login, device discovery, WebRTC media reception and input. The common upstream revision for this port is `d56405991886de59809987c23161521fb3eafcba`. [Original Switch project](https://github.com/Antman2023/UURemote-ns/tree/d56405991886de59809987c23161521fb3eafcba)
- **doke69** extended the Switch interface, controller and touch input, hardware presentation, quality controls and network diagnostics. The Linux build uses the 0.3.74 Beta source at `ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b`. [Direct upstream](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/README.md) · [Upstream notices](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/NOTICE.md) · [Upstream credits](https://github.com/doke69/YOUYOU-Remote-ns/blob/ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b/docs/credits.md)

The Linux port adds a Rust application and player, a Linux native bridge, VA-API support, tablet layouts and Debian packaging. The public Linux launcher uses an original dual-display graphic. [Launcher source](assets/launcher.svg)

Application code is licensed under GPL-3.0-or-later. Upstream authorship, copyright and license notices are retained. Third-party libraries, fonts and other components keep their own licenses. The Debian package provides notices for its build in `/usr/share/doc/youyou-linux/copyright`. [Project license](LICENSE) · [Third-party notices](packaging/THIRD-PARTY.md)

UU Remote, Nintendo Switch and Lenovo product names identify interoperability targets. This port is independent of NetEase, Nintendo and Lenovo.
