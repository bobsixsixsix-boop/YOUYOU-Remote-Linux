mod core;
mod lobby;
use std::path::Path;
use std::time::{Duration, Instant};

fn main() {
    // SDL2 in Ubuntu can prefer XWayland even inside Plasma Wayland. At a
    // fractional desktop scale that gives us a compositor-upscaled buffer.
    if std::env::var_os("SDL_VIDEODRIVER").is_none()
        && std::env::var_os("WAYLAND_DISPLAY").is_some()
        && std::env::var("XDG_SESSION_TYPE").as_deref() != Ok("x11")
    {
        std::env::set_var("SDL_VIDEODRIVER", "wayland,x11");
    }
    if let Err(e) = run() {
        eprintln!("YOUYOU 远程 · {e}");
        if std::env::args_os().len() == 1 {
            let _ = std::process::Command::new("kdialog")
                .args(["--title", "YOUYOU 远程", "--error", &e])
                .status();
        }
        std::process::exit(1);
    }
}

fn run() -> Result<(), String> {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    match args.first().and_then(|s| s.to_str()) {
        None if args.is_empty() => lobby::run(),
        Some("--preview") if args.len() <= 2 => {
            let page =
                match args.get(1).and_then(|s| s.to_str()).unwrap_or("home") {
                    "home" => 0,
                    "menu" => 1,
                    "network" => 2,
                    "quality" => 3,
                    "devices" => 4,
                    "gamepad" => 5,
                    "account" => 6,
                    "presets" => 7,
                    _ => return Err(
                        "可预览 home、menu、network、quality、devices、gamepad、account 或 presets"
                            .into(),
                    ),
                };
            lobby::preview(page)
        }
        Some("import-apk" | "import-key") if args.len() == 2 => {
            let p = Path::new(&args[1]);
            if args[0] == "import-apk" {
                youyou_account::import_apk(p)
            } else {
                youyou_account::import_key(p)
            }
            .map_err(|e| e.to_string())?;
            println!("已在本机导入签名材料。");
            Ok(())
        }
        Some("setup") if args.len() == 1 => {
            youyou_account::download_official().map_err(|e| e.to_string())?;
            println!("已从官网下载并在本机提取签名材料。");
            Ok(())
        }
        Some("--self-test") if args.len() == 1 => {
            // Exercise the codec and input paths without playing a tone or opening an account.
            std::env::set_var("SDL_AUDIODRIVER", "dummy");
            if std::env::var_os("WAYLAND_DISPLAY").is_none()
                && std::env::var_os("DISPLAY").is_none()
                && std::env::var_os("SDL_VIDEODRIVER").is_none()
            {
                std::env::set_var("SDL_VIDEODRIVER", "dummy");
            }
            core::test_media()?;
            youyou_player::self_test()?;
            youyou_player::menu_self_test()?;
            println!("PASS · H.264 解码、Opus 音频入队、SDL 画面回读、输入事件及原版菜单导航");
            Ok(())
        }
        Some("--check-login") if args.len() == 1 => {
            let c = core::Core::open(youyou_account::open().map_err(|e| e.to_string())?)?;
            c.command("login", serde_json::json!({}))?;
            let until = Instant::now() + Duration::from_secs(40);
            while Instant::now() < until {
                let s = c.state()?;
                if s.stage == "login" && !s.qr_url.is_empty() {
                    qrcode::QrCode::new(s.qr_url.as_bytes()).map_err(|e| e.to_string())?;
                    println!("PASS · UU 服务已返回可编码的登录二维码");
                    return Ok(());
                }
                if s.stage == "error" {
                    return Err(s.detail);
                }
                std::thread::sleep(Duration::from_millis(200));
            }
            Err("登录二维码请求超时".into())
        }
        Some("--version") => {
            println!("YOUYOU Linux {}", env!("CARGO_PKG_VERSION"));
            Ok(())
        }
        Some("--help" | "-h") => {
            println!("youyou-linux [setup | import-apk FILE | import-key FILE | --preview [home|menu|network|quality|devices|gamepad|account|presets] | --self-test | --check-login | --version]\n直接启动进入图形界面。");
            Ok(())
        }
        _ => Err("未知参数。使用 youyou-linux --help 查看用法".into()),
    }
}
