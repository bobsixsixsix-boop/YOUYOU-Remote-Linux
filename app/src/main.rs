//! YOUYOU for Linux: a UU Remote client for the Lenovo Legion Y700 tablet.
//! The protocol is upstream's (core/, libyouyou_core.so); this binary is the
//! player and its pages: SDL window, zero-copy video, input, lobby and menu.
mod bridge;
mod clip;
mod input;
mod lobby;
mod menu;
mod ui;
mod video;

use bridge::{Core, Frame, RawFrame};
use sdl2::controller::{Button, GameController};
use sdl2::event::{Event, WindowEvent};
use sdl2::keyboard::Scancode;
use sdl2::mouse::MouseWheelDirection;
use sdl2::video::{GLProfile, SwapInterval};
use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use std::ptr::null_mut;
use std::thread;
use std::time::{Duration, Instant};

const USAGE: &str = "用法：
  youyou                                打开界面：登录、选电脑、串流
  youyou devices                        列出账号下的电脑
  youyou connect [电脑名或设备 ID]       直接全屏串流（默认第一台在线的电脑）
  youyou play <片段.h264|.hevc> [帧率]   不联网的解码与显示自测（默认 120 帧）
  youyou import-apk <官方电视版 APK> | import-key <signing.key> | download-key
环境变量：YY_PACE=1|0 临时盖过界面里的画面节奏（流畅|低延迟）；YY_VSYNC=1 低延迟时也开垂直同步；YY_TOUCH=mouse|touch、YY_DESKTOP=1|0 临时盖过界面里的触屏和专用桌面设置；
  connect 时 YY_STREAM=2560x1600@120、YY_MBPS=60 临时改串流尺寸、帧率和码率上限";
const TOUCH_MOUSE: u32 = u32::MAX; // SDL_TOUCH_MOUSEID: mouse events synthesized from touch
/// A touch starting this close to the left or right edge (dp) and moving this far in opens the menu.
const EDGE: f32 = 24.0;
const SWIPE: f32 = 40.0;

fn main() {
    if let Err(e) = run(std::env::args().skip(1).collect()) {
        eprintln!("youyou: {e}");
        std::process::exit(1);
    }
}

fn run(args: Vec<String>) -> Result<(), String> {
    let arg = |i: usize| args.get(i).map(String::as_str);
    let path = |i| arg(i).map(Path::new).ok_or_else(|| USAGE.to_string());
    let text = |e: std::io::Error| e.to_string();
    match arg(0) {
        None => app(),
        Some("play") => play(path(1)?, arg(2).map_or(Ok(120.0), str::parse).map_err(|_| USAGE.to_string())?),
        Some("devices") => {
            for d in devices(&session()?)? {
                let state = if online(&d) { "在线" } else { "离线" };
                println!("{}\t{state}（{}）\t{}", d["device_id"].as_str().unwrap_or(""), d["status"], d["alias"].as_str().unwrap_or(""));
            }
            Ok(())
        }
        Some("connect") => {
            let core = session()?;
            let device = devices(&core)?
                .into_iter()
                .find(|d| arg(1).map_or(online(d), |want| d["device_id"] == want || d["alias"] == want))
                .ok_or("没有找到要连接的电脑（youyou devices 看列表）")?;
            if let Ok(spec) = std::env::var("YY_STREAM") {
                let mbps = std::env::var("YY_MBPS").ok().and_then(|m| m.parse().ok()).unwrap_or(0);
                core.command("quality-settings", &stream_settings(&spec, mbps).ok_or(USAGE)?).map_err(text)?;
            }
            let end = stream(&mut Screen::new()?, Some(&core), &device, &mut Prefs::load());
            disconnect(&core);
            match end? {
                End::Error(e) => Err(e),
                _ => Ok(()),
            }
        }
        Some("import-apk") => youyou_account::import_apk(path(1)?).map_err(text),
        Some("import-key") => youyou_account::import_key(path(1)?).map_err(text),
        Some("download-key") => youyou_account::download_official().map_err(text),
        _ => Err(USAGE.into()),
    }
}

/// The window app: pages until a computer is picked, the stream, back to the pages.
fn app() -> Result<(), String> {
    let mut core = open_core()?;
    let mut prefs = Prefs::load();
    let mut screen = Screen::new()?;
    let mut notice = String::new();
    loop {
        let device = match lobby::run(&mut screen, &mut core, &mut prefs, std::mem::take(&mut notice))? {
            lobby::Exit::Quit => return Ok(()),
            lobby::Exit::Connect(device) => device,
        };
        let end = stream(&mut screen, core.as_ref(), &device, &mut prefs);
        if let Some(core) = &core {
            disconnect(core);
        }
        match end? {
            End::Quit => return Ok(()),
            End::User => {}
            End::Error(e) => notice = e,
        }
    }
}

/// The upstream client for the saved account, or None while there is no signing material yet.
fn open_core() -> Result<Option<Core>, String> {
    let account = match youyou_account::open() {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        opened => opened.map_err(|e| e.to_string())?,
    };
    let core = start(account)?;
    // Startup restores the saved login or asks for a QR code within moments; waiting
    // spares the login page a flash of "QR expired" from the initial idle state.
    let deadline = Instant::now() + Duration::from_secs(2);
    while Instant::now() < deadline && core.snapshot()["stage"] == "idle" && core.snapshot()["authenticated"] != true {
        thread::sleep(Duration::from_millis(20));
    }
    Ok(Some(core))
}

/// The upstream client, running inside the account's state directory.
fn session() -> Result<Core, String> {
    start(youyou_account::open().map_err(|e| e.to_string())?)
}

fn start(account: youyou_account::Account) -> Result<Core, String> {
    std::env::set_current_dir(account.state_dir()).map_err(|e| e.to_string())?;
    let core = Core::open(account).map_err(|e| e.to_string())?;
    core.command("startup", &json!({})).map_err(|e| e.to_string())?;
    Ok(core)
}

/// Waits for upstream to restore the saved login and sync the device list.
fn devices(core: &Core) -> Result<Vec<Value>, String> {
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let s = core.snapshot();
        match s["stage"].as_str() {
            Some("login" | "sms") => return Err("没有保存的登录：运行 youyou 打开界面扫码登录".into()),
            Some("idle") if s["authenticated"] == true => {
                if let Some(detail) = s["detail"].as_str().filter(|d| !d.is_empty()) {
                    eprintln!("{detail}");
                }
                return Ok(s["devices"].as_array().cloned().unwrap_or_default());
            }
            _ if Instant::now() > deadline => return Err(format!("同步设备超时：{}", s["detail"])),
            _ => thread::sleep(Duration::from_millis(100)),
        }
    }
}

/// Ends the session with the computer. Dropping the core alone does not: the
/// computer would stay "being controlled" until its heartbeat times out.
fn disconnect(core: &Core) {
    let device = core.snapshot()["activeDeviceId"].clone();
    if device.as_str().is_none_or(str::is_empty) {
        return;
    }
    let _ = core.command("disconnect", &json!({"restoreSound": false, "deviceId": device}));
    // Upstream clears activeDeviceId as the last step of its disconnect.
    let deadline = Instant::now() + Duration::from_secs(3);
    while core.snapshot()["activeDeviceId"].as_str().is_some_and(|d| !d.is_empty()) && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(20));
    }
}

/// TV 3.5.0 (upstream live_frontend.inc, docs/user-guide.md): only the exact status
/// "CONNECTED" is online, anything else is offline. Checking for "offline" showed offline PCs as online.
fn online(device: &Value) -> bool {
    device["status"] == "CONNECTED"
}

/// The app's own choices (upstream keeps the stream settings in its config.ini),
/// saved as app.ini in the state directory. YY_DESKTOP, YY_TOUCH and YY_PACE override them.
struct Prefs {
    /// Ask the computer for a dedicated virtual screen (upstream's secondScreen).
    desktop: bool,
    /// One finger drives the mouse instead of multi-touch.
    touch_mouse: bool,
    /// Frame pacer ("流畅", the default): every picture in order, one per refresh.
    pace: bool,
    path: Option<PathBuf>,
}

impl Prefs {
    fn load() -> Self {
        let path = youyou_account::state_dir().ok().map(|d| d.join("app.ini"));
        let text = path.as_ref().and_then(|p| std::fs::read_to_string(p).ok()).unwrap_or_default();
        let saved = |key: &str| text.lines().any(|l| l.trim() == format!("{key}=1"));
        let env = |key: &str, on: &str| std::env::var(key).ok().map(|v| v == on);
        Self {
            desktop: env("YY_DESKTOP", "1").unwrap_or_else(|| saved("desktop")),
            touch_mouse: env("YY_TOUCH", "mouse").unwrap_or_else(|| saved("touch_mouse")),
            pace: env("YY_PACE", "1").unwrap_or_else(|| !text.lines().any(|l| l.trim() == "pace=0")),
            path,
        }
    }
    fn save(&self) {
        if let Some(p) = &self.path {
            let _ = std::fs::write(p, format!("desktop={}\ntouch_mouse={}\npace={}\n", self.desktop as u8, self.touch_mouse as u8, self.pace as u8));
        }
    }
}

/// The fullscreen window, shared by the pages and the stream.
struct Screen {
    // GL objects first: they are dropped before the context they live in.
    renderer: video::Renderer,
    ui: ui::Ui,
    _context: sdl2::video::GLContext,
    window: sdl2::video::Window,
    pump: sdl2::EventPump,
    mouse: sdl2::mouse::MouseUtil,
    events: sdl2::EventSubsystem,
    frame_ready: u32,
    /// YY_VSYNC=1: swap interval 1 without the pacer too.
    vsync: bool,
    controllers: Option<sdl2::GameControllerSubsystem>,
    pad: Option<GameController>,
    video: sdl2::VideoSubsystem,
    _sdl: sdl2::Sdl,
}

impl Screen {
    fn new() -> Result<Self, String> {
        sdl2::hint::set("SDL_VIDEODRIVER", "wayland,x11");
        // Touch and mouse stay separate: fingers go out as native touch (or the
        // touch-mouse mode), the mouse as a mouse.
        sdl2::hint::set("SDL_TOUCH_MOUSE_EVENTS", "0");
        sdl2::hint::set("SDL_MOUSE_TOUCH_EVENTS", "0");
        // The Wayland app_id (SDL2 has no hint for it): KWin matches it to youyou-linux.desktop for the
        // icon, and y700-back.conf to send XF86Back instead of Esc on a side swipe (README, Y700).
        if std::env::var_os("SDL_VIDEO_WAYLAND_WMCLASS").is_none() {
            std::env::set_var("SDL_VIDEO_WAYLAND_WMCLASS", "youyou-linux");
        }
        let sdl = sdl2::init()?;
        let video = sdl.video()?;
        let attr = video.gl_attr();
        attr.set_context_profile(GLProfile::GLES);
        attr.set_context_version(3, 0);
        attr.set_depth_size(0);
        let window = video
            .window("YOUYOU 远程", 1280, 800)
            .opengl()
            .allow_highdpi()
            .fullscreen_desktop()
            .build()
            .map_err(|e| e.to_string())?;
        let context = window.gl_create_context()?;
        // Without the pacer, off by default: KWin still flips on vblank (no tearing),
        // but at 120 fps on 120 Hz swap interval 1 holds each picture one more refresh
        // (tools/results/20261102-pacing.txt). stream() sets it per session.
        let vsync = std::env::var("YY_VSYNC").is_ok_and(|v| v == "1");
        video.gl_set_swap_interval(if vsync { SwapInterval::VSync } else { SwapInterval::Immediate })?;
        let renderer = video::Renderer::new(&video)?;
        let ui = ui::Ui::new(&video)?;

        // The decoder wakes the event loop through SDL, so it sleeps between pictures.
        let events = sdl.event()?;
        let frame_ready = unsafe { events.register_event()? };
        let sender = events.event_sender();
        thread::spawn(move || {
            let mut serial = 0;
            loop {
                serial = match bridge::wait(serial, 1000) {
                    0 => continue,
                    next => next,
                };
                let ready = Event::User { timestamp: 0, window_id: 0, type_: frame_ready, code: 0, data1: null_mut(), data2: null_mut() };
                if sender.push_event(ready).is_err() {
                    return;
                }
            }
        });
        Ok(Self {
            renderer,
            ui,
            _context: context,
            window,
            pump: sdl.event_pump()?,
            mouse: sdl.mouse(),
            events,
            frame_ready,
            vsync,
            controllers: sdl.game_controller().ok(),
            pad: None,
            video,
            _sdl: sdl,
        })
    }

    /// Opens the first pad plugged in; what to tell the host when it goes.
    fn plug(&mut self, e: &Event) -> Option<Value> {
        match *e {
            Event::ControllerDeviceAdded { which, .. } if self.pad.is_none() => {
                self.pad = self.controllers.as_ref().and_then(|c| c.open(which).ok());
                None
            }
            Event::ControllerDeviceRemoved { which, .. } if self.pad.as_ref().is_some_and(|p| p.instance_id() == which) => {
                self.pad = None;
                Some(json!({"action": "gpd_disconnect", "index": 0}))
            }
            _ => None,
        }
    }
}

/// Offline self-test: feeds a clip to the decoder at `fps` and shows it like a stream.
fn play(path: &Path, fps: f64) -> Result<(), String> {
    let data = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let hevc = path.extension().is_some_and(|e| e == "hevc" || e == "h265" || e == "265");
    let units: Vec<Vec<u8>> = clip::access_units(&data, hevc).into_iter().map(<[u8]>::to_vec).collect();
    eprintln!("{} 个访问单元，{fps} 帧/秒", units.len());
    let mut screen = Screen::new()?;
    let sender = screen.events.event_sender();
    thread::spawn(move || {
        // Start once the window is up, so the statistics see a steady screen.
        let start = Instant::now() + Duration::from_secs(1);
        for (i, unit) in units.iter().enumerate() {
            thread::sleep((start + Duration::from_secs_f64(i as f64 / fps)).saturating_duration_since(Instant::now()));
            bridge::debug_video(unit, hevc);
        }
        thread::sleep(Duration::from_millis(300));
        let _ = sender.push_event(Event::Quit { timestamp: 0 });
    });
    stream(&mut screen, None, &Value::Null, &mut Prefs::load()).map(|_| ())
}

/// "2560x1600@120" (+ a bitrate cap in Mbps, 0 = the host's own quality
/// preset) as upstream's quality-settings command. H.265: the Y700 decodes it
/// faster than H.264 (tools/results/20261102-decode-latency.txt).
fn stream_settings(spec: &str, mbps: u32) -> Option<Value> {
    let (size, fps) = spec.split_once('@')?;
    let (width, height) = size.split_once('x')?;
    let (width, height, fps): (u32, u32, u32) = (width.parse().ok()?, height.parse().ok()?, fps.parse().ok()?);
    let quality = if mbps > 0 { 6 } else { 3 }; // 3 = upstream's default preset
    Some(json!({"width": width, "height": height, "fps": fps, "quality": quality, "max_bitrate_mbps": mbps, "hevc": true}))
}

fn now_us() -> i64 {
    let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut t) };
    t.tv_sec * 1_000_000 + t.tv_nsec / 1000
}

/// Presentation statistics, printed when the stream ends.
#[derive(Default)]
struct Stats {
    presented: u64,
    skipped: u64,
    serial: u64,
    last_us: i64,
    latency_us: Vec<i64>,
    interval_us: Vec<i64>,
}

impl Stats {
    fn shown(&mut self, f: &RawFrame, now: i64) {
        if self.presented > 0 {
            self.skipped += f.serial.saturating_sub(self.serial + 1);
            self.interval_us.push(now - self.last_us);
        }
        self.presented += 1;
        (self.serial, self.last_us) = (f.serial, now);
        self.latency_us.push(now - f.ready_us);
    }
    fn report(&mut self) -> String {
        let ms = |v: &mut Vec<i64>, p: f64| {
            v.sort_unstable();
            v.get(((v.len().max(1) - 1) as f64 * p) as usize).map_or(-1.0, |us| *us as f64 / 1000.0)
        };
        format!(
            "显示 {} 帧，跳过 {} 帧 | 出解码器→交换完成 p50 {:.1} p95 {:.1} ms | 交换间隔 p50 {:.2} p95 {:.2} p99 {:.2} 最大 {:.2} ms",
            self.presented, self.skipped, ms(&mut self.latency_us, 0.5), ms(&mut self.latency_us, 0.95),
            ms(&mut self.interval_us, 0.5), ms(&mut self.interval_us, 0.95), ms(&mut self.interval_us, 0.99),
            ms(&mut self.interval_us, 1.0)
        )
    }
    fn print(&mut self, core: Option<&Core>) {
        eprintln!("{}", self.report());
        if let Some(core) = core {
            let s = core.snapshot();
            eprintln!("解码器: {} | 收到 {}x{}@{} | 应用 {}", s["media"], s["width"], s["height"], s["fps"], s["applied"]);
        }
    }
}

/// How a stream ended.
enum End {
    /// Disconnect from the menu or cancelled while connecting.
    User,
    /// The window closed.
    Quit,
    /// The connection failed or the computer ended it: upstream's reason.
    Error(String),
}

/// Lets go of everything held on the host: keys, mouse buttons, touches, the pad.
fn release(core: Option<&Core>, input: &mut input::InputState) {
    let Some(core) = core else { return };
    for v in input.release() {
        let _ = core.command("input", &v);
    }
    let _ = core.command("release", &json!({}));
}

/// Connects to `device` and shows the stream full screen until it ends. With
/// `core` None (`play`) it only shows what the decoder produces.
fn stream(screen: &mut Screen, core: Option<&Core>, device: &Value, prefs: &mut Prefs) -> Result<End, String> {
    let send = |v: Value| {
        if let Some(core) = core {
            let _ = core.command("input", &v);
        }
    };
    while Frame::acquire().is_some() {} // a picture the last stream left untaken
    if let Some(core) = core {
        core.command("connect", &json!({"deviceId": device["device_id"], "secondScreen": prefs.desktop}))
            .map_err(|e| e.to_string())?;
    }
    let alias = device["alias"].as_str().unwrap_or("电脑").to_string();
    let mut input = input::InputState::default();
    input.set_desktop_mouse(prefs.touch_mouse);
    let mut shown: Option<Frame> = None;
    let mut stats = Stats::default();
    let mut snap = core.map_or(Value::Null, Core::snapshot);
    let (mut logged, mut polled, started) = (String::new(), Instant::now(), Instant::now());
    let (mut active, mut redraw, mut cursor) = (false, true, None);
    // The menu: open, the side it slides from, how far out it was last drawn.
    let (mut menu, mut side, mut drawer) = (false, 1.0, 0.0);
    // A touch that started at a screen edge: never sent to the host, it may open the menu.
    let mut edge: Option<((i64, i64), f32)> = None;
    let mut combo = (false, false); // the pad's Select and Start held
    let pace = prefs.pace;
    let swap = if pace || screen.vsync { SwapInterval::VSync } else { SwapInterval::Immediate };
    screen.video.gl_set_swap_interval(swap)?;
    let mut backlog = false; // pictures still waiting for the pacer
    // How many were waiting at each of the pacer's last takes, half a second of refreshes.
    let mut history = std::collections::VecDeque::new();
    let half = screen.window.display_mode().map_or(60, |m| (m.refresh_rate / 2).max(1) as usize);
    loop {
        let stage = snap["stage"].as_str().unwrap_or("").to_string();
        let waiting = core.is_some() && (shown.is_none() || stage == "reconnecting");
        let overlay = menu || waiting || drawer > 0.0;
        if cursor != Some(menu || waiting) {
            cursor = Some(menu || waiting);
            screen.mouse.show_cursor(menu || waiting);
        }
        let (w, h) = screen.window.size();
        let drawable = screen.window.drawable_size();
        let view = video::fit(shown.as_ref().map_or((0, 0), |f| (f.0.width, f.0.height)), drawable);
        let local = |x: i32, y: i32| (x as f32 / w as f32, y as f32 / h as f32);
        let (mut fresh, mut pointer, mut pad_moved) = (false, None, false);
        let timeout = match (overlay, screen.ui.dirty, screen.ui.animating) {
            _ if backlog => 0,
            (true, true, _) => 0,
            (true, _, true) => 8,
            _ => 100,
        };
        let first = screen.pump.wait_event_timeout(timeout);
        let events: Vec<Event> = first.into_iter().chain(screen.pump.poll_iter()).collect();
        for event in events {
            if let Some(v) = screen.plug(&event) {
                send(v);
            }
            let toggle = match &event {
                Event::Quit { .. } => {
                    stats.print(core);
                    return Ok(End::Quit);
                }
                Event::User { type_, .. } if *type_ == screen.frame_ready => {
                    fresh = true;
                    continue;
                }
                Event::Window { win_event: WindowEvent::SizeChanged(..) | WindowEvent::Exposed, .. } => {
                    redraw = true;
                    continue;
                }
                Event::Window { win_event: WindowEvent::FocusLost, .. } => {
                    release(core, &mut input);
                    continue;
                }
                // XF86Back: the Y700's side-swipe gesture (KWin takes the outermost strip before we see it).
                Event::KeyDown { scancode: Some(Scancode::F8 | Scancode::AcBack), repeat: false, .. } => true,
                Event::ControllerButtonDown { button, .. } | Event::ControllerButtonUp { button, .. }
                    if matches!(button, Button::Back | Button::Start) =>
                {
                    let held = combo == (true, true);
                    let down = matches!(event, Event::ControllerButtonDown { .. });
                    if *button == Button::Back { combo.0 = down } else { combo.1 = down }
                    !held && combo == (true, true)
                }
                _ => false,
            };
            if toggle && shown.is_some() {
                (menu, side, redraw) = (!menu, 1.0, true);
                if menu {
                    release(core, &mut input);
                }
                continue;
            }
            if menu || waiting {
                screen.ui.feed(&event);
                continue;
            }
            let width = w as f32;
            match event {
                Event::FingerDown { touch_id, finger_id, x, .. }
                    if touch_id != -1 && shown.is_some() && (x * width < EDGE || x * width > width - EDGE) =>
                {
                    edge = Some(((touch_id, finger_id), x * width));
                    continue;
                }
                Event::FingerMotion { touch_id, finger_id, x, .. } if edge.is_some_and(|e| e.0 == (touch_id, finger_id)) => {
                    let from = edge.map_or(0.0, |e| e.1);
                    if (x * width - from).abs() >= SWIPE {
                        (menu, side, redraw, edge) = (true, if from < width / 2.0 { -1.0 } else { 1.0 }, true, None);
                        release(core, &mut input);
                    }
                    continue;
                }
                Event::FingerUp { touch_id, finger_id, .. } if edge.is_some_and(|e| e.0 == (touch_id, finger_id)) => {
                    edge = None;
                    continue;
                }
                _ => {}
            }
            match event {
                // Nothing to control before the first picture shows where the screen is.
                _ if core.is_none() || shown.is_none() => {}
                Event::KeyDown { scancode: Some(s), repeat: false, .. } => {
                    input::vk(s).and_then(|k| input.key(k, true)).map(send);
                }
                Event::KeyUp { scancode: Some(s), .. } => {
                    input::vk(s).and_then(|k| input.key(k, false)).map(send);
                }
                // A mouse sends ~1000 moves/s: keep the last per batch, but never
                // let a click or scroll overtake the move before it.
                Event::MouseMotion { which, x, y, .. } if which != TOUCH_MOUSE => {
                    pointer = input::image_point(view, drawable, local(x, y), !input.buttons.is_empty()).or(pointer);
                }
                Event::MouseButtonDown { which, mouse_btn, x, y, .. } if which != TOUCH_MOUSE => {
                    if let (Some(b), Some(p)) = (input::button(mouse_btn), input::image_point(view, drawable, local(x, y), false)) {
                        pointer = None;
                        send(input::absolute_pointer(p));
                        input.mouse(b, true).map(send);
                    }
                }
                Event::MouseButtonUp { which, mouse_btn, .. } if which != TOUCH_MOUSE => {
                    pointer.take().map(input::absolute_pointer).map(send);
                    input::button(mouse_btn).and_then(|b| input.mouse(b, false)).map(send);
                }
                Event::MouseWheel { precise_x, precise_y, direction, .. } => {
                    pointer.take().map(input::absolute_pointer).map(send);
                    let notch = if direction == MouseWheelDirection::Flipped { -120.0 } else { 120.0 };
                    send(json!({"action": "mouse_scroll", "delta_x": (notch * precise_x).round() as i32,
                        "delta_y": (notch * precise_y).round() as i32}));
                }
                // New contacts in the black bars are ignored; moving ones are clamped to the picture.
                Event::FingerDown { touch_id, finger_id, x, y, .. } if touch_id != -1 => {
                    let p = input::image_point(view, drawable, (x, y), false);
                    match input.desktop_mouse.as_mut() {
                        Some(mouse) => mouse.press((touch_id, finger_id), p).into_iter().flatten().for_each(send),
                        None => input.finger((touch_id, finger_id), p, true).into_iter().for_each(send),
                    }
                }
                Event::FingerMotion { touch_id, finger_id, x, y, .. } if touch_id != -1 => {
                    let p = input::image_point(view, drawable, (x, y), true);
                    match input.desktop_mouse.as_mut() {
                        Some(mouse) => mouse.motion((touch_id, finger_id), p).into_iter().for_each(send),
                        None => input.finger((touch_id, finger_id), p, false).into_iter().for_each(send),
                    }
                }
                Event::FingerUp { touch_id, finger_id, x, y, .. } if touch_id != -1 => {
                    let p = input::image_point(view, drawable, (x, y), true);
                    match input.desktop_mouse.as_mut() {
                        Some(mouse) => mouse.lift((touch_id, finger_id), p).into_iter().flatten().for_each(send),
                        None => input.finger((touch_id, finger_id), None, false).into_iter().for_each(send),
                    }
                }
                Event::ControllerButtonDown { .. } | Event::ControllerButtonUp { .. } | Event::ControllerAxisMotion { .. } => {
                    pad_moved = true;
                }
                _ => {}
            }
        }
        pointer.map(input::absolute_pointer).map(send);
        if let Some(p) = screen.pad.as_ref().filter(|_| pad_moved) {
            send(input::pad_state(p));
        }

        if let Some(core) = core.filter(|_| polled.elapsed() >= Duration::from_millis(250)) {
            polled = Instant::now();
            let s = core.snapshot();
            let (stage, detail) = (s["stage"].as_str().unwrap_or(""), s["detail"].as_str().unwrap_or(""));
            let line = format!("{stage} {detail}");
            if line != logged {
                eprintln!("{line}");
                logged = line;
            }
            if !matches!(stage, "idle" | "error") {
                active = true;
            } else if active || started.elapsed() > Duration::from_secs(3) {
                stats.print(Some(core));
                return Ok(End::Error(if detail.is_empty() { "连接已结束".into() } else { detail.into() }));
            }
            redraw |= overlay && s != snap;
            snap = s;
        }

        // Without the pacer, the newest picture only: whatever the decoder finished
        // while we waited for the last swap was superseded and goes back to it.
        // The pacer takes the oldest; swap interval 1 holds each for one refresh.
        // As Moonlight's pacer, it keeps the backlog while it comes and goes, and
        // catches up to the newest once none of the last half second's takes found
        // a single picture waiting: a backlog that stays only adds latency.
        let mut previous = None;
        let next = if !pace {
            fresh.then(Frame::acquire).flatten()
        } else if fresh || backlog {
            let catch_up = history.len() == half && history.iter().all(|&w| w > 1);
            let mut next = Frame::next();
            while catch_up && next.as_ref().is_some_and(|(_, waiting)| *waiting > 1) {
                next = Frame::next();
            }
            if let Some((_, waiting)) = next {
                if history.len() == half {
                    history.pop_front();
                }
                history.push_back(waiting);
            }
            backlog = next.as_ref().is_some_and(|(_, waiting)| *waiting > 1);
            next.map(|(frame, _)| frame)
        } else {
            None
        };
        if let Some(frame) = next {
            previous = Some(shown.replace(frame));
            redraw = true;
        }
        if !redraw && !(overlay && (screen.ui.dirty || screen.ui.animating)) {
            continue;
        }
        let frame = shown.as_ref().map(|f| &f.0);
        let view = video::fit(frame.map_or((0, 0), |f| (f.width, f.height)), drawable);
        screen.renderer.draw(frame, view, drawable)?;
        if core.is_none() && previous.is_some() && [60, 300].contains(&(stats.presented + 1)) {
            eprintln!("第 {} 帧取色: {:?}", stats.presented + 1, screen.renderer.probe(view, drawable.1));
        }
        let mut action = None;
        if overlay {
            screen.ui.begin(screen.window.size(), drawable);
            if waiting && !menu {
                let title = if stage == "reconnecting" { format!("正在重新连接 {alias}") } else { format!("正在连接 {alias}") };
                if menu::status(&mut screen.ui, &title, snap["detail"].as_str().unwrap_or(""), false) {
                    action = Some(menu::Action::Disconnect);
                }
            }
            let picked;
            (drawer, picked) = menu::drawer(&mut screen.ui, menu, side, &alias, &snap, input.desktop_mouse.is_some());
            action = action.or(picked);
            screen.ui.end();
        }
        screen.window.gl_swap_window();
        if let (Some(f), Some(_)) = (frame, &previous) {
            stats.shown(f, now_us());
        }
        redraw = false;
        drop(previous); // only now: the GPU has the new picture queued

        match action {
            Some(menu::Action::Close) => (menu, redraw) = (false, true),
            Some(menu::Action::Disconnect) => {
                stats.print(core);
                return Ok(End::User);
            }
            Some(menu::Action::Touch(on)) => {
                prefs.touch_mouse = on;
                prefs.save();
                input.set_desktop_mouse(on).into_iter().for_each(send);
            }
            Some(menu::Action::Screen(id)) => {
                if let Some(core) = core {
                    let _ = core.command("select-screen", &json!({"screen": id}));
                }
            }
            Some(menu::Action::Scale(scale)) => {
                if let Some(core) = core {
                    let _ = core.command("desktop-scale", &json!({"scale": scale}));
                }
            }
            Some(menu::Action::Privacy(mode)) => {
                if let Some(core) = core {
                    let _ = core.command("privacy-screen", &json!({"deviceId": device["device_id"], "mode": mode}));
                }
            }
            None => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parses_stream_settings() {
        let s = stream_settings("2560x1600@120", 60).unwrap();
        assert_eq!((s["width"].as_u64(), s["height"].as_u64(), s["fps"].as_u64()), (Some(2560), Some(1600), Some(120)));
        assert_eq!((s["quality"].as_u64(), s["max_bitrate_mbps"].as_u64()), (Some(6), Some(60)));
        assert_eq!(stream_settings("1920x1080@60", 0).unwrap()["quality"], 3);
        assert!(stream_settings("2560x1600", 0).is_none() && stream_settings("2560*1600@120", 0).is_none());
    }
    #[test]
    fn only_connected_is_online() {
        assert!(online(&json!({"status": "CONNECTED"})));
        for d in [json!({"status": "DISCONNECTED"}), json!({"status": "connected"}), json!({"status": 1}), json!({})] {
            assert!(!online(&d), "{d}");
        }
    }
}
