//! YOUYOU for Linux: a UU Remote client for the Lenovo Legion Y700 tablet.
//! The protocol is upstream's (core/, libyouyou_core.so); this binary is the
//! player: SDL window, zero-copy video, input.
mod bridge;
mod clip;
mod input;
mod video;

use bridge::{Core, Frame, RawFrame};
use sdl2::controller::GameController;
use sdl2::event::{Event, WindowEvent};
use sdl2::mouse::MouseWheelDirection;
use sdl2::video::{GLProfile, SwapInterval};
use serde_json::{json, Value};
use std::path::Path;
use std::ptr::null_mut;
use std::thread;
use std::time::{Duration, Instant};

const USAGE: &str = "用法：
  youyou devices                        列出账号下的电脑
  youyou connect [电脑名或设备 ID]       全屏串流（默认第一台在线的电脑）
  youyou play <片段.h264|.hevc> [帧率]   不联网的解码与显示自测（默认 120 帧）
  youyou import-apk <官方电视版 APK> | import-key <signing.key> | download-key
界面做好前的临时开关：YY_VSYNC=1 开垂直同步（默认关），YY_TOUCH=mouse 触屏当鼠标用";
const TOUCH_MOUSE: u32 = u32::MAX; // SDL_TOUCH_MOUSEID: mouse events synthesized from touch

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
        Some("play") => play(path(1)?, arg(2).map_or(Ok(120.0), str::parse).map_err(|_| USAGE.to_string())?),
        Some("devices") => {
            for d in devices(&session()?)? {
                let state = if online(&d) { "在线" } else { "离线" };
                println!("{}\t{state}\t{}", d["device_id"].as_str().unwrap_or(""), d["alias"].as_str().unwrap_or(""));
            }
            Ok(())
        }
        Some("connect") => {
            let core = session()?;
            let device = devices(&core)?
                .into_iter()
                .find(|d| arg(1).map_or(online(d), |want| d["device_id"] == want || d["alias"] == want))
                .ok_or("没有找到要连接的电脑（youyou devices 看列表）")?;
            core.command("connect", &json!({"deviceId": device["device_id"]})).map_err(text)?;
            show(&sdl()?, Some(&core))
        }
        Some("import-apk") => youyou_account::import_apk(path(1)?).map_err(text),
        Some("import-key") => youyou_account::import_key(path(1)?).map_err(text),
        Some("download-key") => youyou_account::download_official().map_err(text),
        _ => Err(USAGE.into()),
    }
}

/// The upstream client, running inside the account's state directory.
fn session() -> Result<Core, String> {
    let account = youyou_account::open().map_err(|e| e.to_string())?;
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
            Some("login" | "sms") => return Err("没有保存的登录，需要扫码登录（登录界面还没做）".into()),
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

fn online(device: &Value) -> bool {
    device["status"] != "offline"
}

fn sdl() -> Result<sdl2::Sdl, String> {
    sdl2::hint::set("SDL_VIDEODRIVER", "wayland,x11");
    // Touch and mouse stay separate: fingers go out as native touch (or the
    // touch-mouse mode), the mouse as a mouse.
    sdl2::hint::set("SDL_TOUCH_MOUSE_EVENTS", "0");
    sdl2::hint::set("SDL_MOUSE_TOUCH_EVENTS", "0");
    sdl2::init()
}

/// Offline self-test: feeds a clip to the decoder at `fps` and shows it like a stream.
fn play(path: &Path, fps: f64) -> Result<(), String> {
    let data = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let hevc = path.extension().is_some_and(|e| e == "hevc" || e == "h265" || e == "265");
    let units: Vec<Vec<u8>> = clip::access_units(&data, hevc).into_iter().map(<[u8]>::to_vec).collect();
    eprintln!("{} 个访问单元，{fps} 帧/秒", units.len());
    let sdl = sdl()?;
    let events = sdl.event()?;
    let sender = events.event_sender();
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
    show(&sdl, None)
}

fn now_us() -> i64 {
    let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut t) };
    t.tv_sec * 1_000_000 + t.tv_nsec / 1000
}

/// Presentation statistics, printed when the window closes.
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
}

/// The fullscreen player. With `core`, input goes to the host; without it
/// (`play`), it only shows what the decoder produces.
fn show(sdl: &sdl2::Sdl, core: Option<&Core>) -> Result<(), String> {
    let video = sdl.video()?;
    let attr = video.gl_attr();
    attr.set_context_profile(GLProfile::GLES);
    attr.set_context_version(3, 0);
    attr.set_depth_size(0);
    let window = video
        .window("YOUYOU", 1280, 800)
        .opengl()
        .allow_highdpi()
        .fullscreen_desktop()
        .build()
        .map_err(|e| e.to_string())?;
    let _context = window.gl_create_context()?;
    // Off by default: KWin still flips on vblank (no tearing) and pacing is the same
    // either way, but at 120 fps on 120 Hz swap interval 1 holds each picture one
    // more refresh (tools/results/20261102-pacing.txt).
    let vsync = std::env::var("YY_VSYNC").is_ok_and(|v| v == "1");
    video.gl_set_swap_interval(if vsync { SwapInterval::VSync } else { SwapInterval::Immediate })?;
    let mut renderer = video::Renderer::new(&video)?;
    sdl.mouse().show_cursor(false);

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

    let send = |v: Value| {
        if let Some(core) = core {
            let _ = core.command("input", &v);
        }
    };
    let mut pump = sdl.event_pump()?;
    let controllers = sdl.game_controller().ok();
    let mut pad: Option<GameController> = None;
    let mut input = input::InputState::default();
    input.set_desktop_mouse(std::env::var("YY_TOUCH").as_deref() == Ok("mouse"));
    let mut shown: Option<Frame> = None;
    let mut stats = Stats::default();
    let mut stage = String::new();
    let mut polled = Instant::now();
    let mut redraw = true;
    loop {
        let (w, h) = window.size();
        let drawable = window.drawable_size();
        let view = video::fit(shown.as_ref().map_or((0, 0), |f| (f.0.width, f.0.height)), drawable);
        let local = |x: i32, y: i32| (x as f32 / w as f32, y as f32 / h as f32);
        let (mut fresh, mut pointer, mut pad_moved) = (false, None, false);
        let first = pump.wait_event_timeout(100);
        for event in first.into_iter().chain(pump.poll_iter()) {
            match event {
                Event::Quit { .. } => {
                    eprintln!("{}", stats.report());
                    return Ok(());
                }
                Event::User { type_, .. } if type_ == frame_ready => fresh = true,
                Event::Window { win_event: WindowEvent::SizeChanged(..) | WindowEvent::Exposed, .. } => redraw = true,
                Event::Window { win_event: WindowEvent::FocusLost, .. } => {
                    input.release().into_iter().for_each(send);
                    if let Some(core) = core {
                        let _ = core.command("release", &json!({}));
                    }
                }
                Event::ControllerDeviceAdded { which, .. } if pad.is_none() => {
                    pad = controllers.as_ref().and_then(|c| c.open(which).ok());
                }
                Event::ControllerDeviceRemoved { which, .. } if pad.as_ref().is_some_and(|p| p.instance_id() == which) => {
                    pad = None;
                    send(json!({"action": "gpd_disconnect", "index": 0}));
                }
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
        if let Some(p) = pad.as_ref().filter(|_| pad_moved) {
            send(input::pad_state(p));
        }

        // Newest picture only: whatever the decoder finished while we waited
        // for the last swap was superseded and has already gone back to it.
        let mut previous = None;
        if fresh {
            if let Some(frame) = Frame::acquire() {
                previous = Some(shown.replace(frame));
                redraw = true;
            }
        }
        if redraw {
            let frame = shown.as_ref().map(|f| &f.0);
            let view = video::fit(frame.map_or((0, 0), |f| (f.width, f.height)), drawable);
            renderer.draw(frame, view, drawable)?;
            if core.is_none() && previous.is_some() && [60, 300].contains(&(stats.presented + 1)) {
                eprintln!("第 {} 帧取色: {:?}", stats.presented + 1, renderer.probe(view, drawable.1));
            }
            window.gl_swap_window();
            if let (Some(f), Some(_)) = (frame, &previous) {
                stats.shown(f, now_us());
            }
            redraw = false;
        }
        drop(previous); // only now: the GPU has the new picture queued

        if let Some(core) = core.filter(|_| polled.elapsed() >= Duration::from_millis(250)) {
            polled = Instant::now();
            let s = core.snapshot();
            let now = format!("{} {}", s["stage"].as_str().unwrap_or(""), s["detail"].as_str().unwrap_or(""));
            if now != stage {
                eprintln!("{now}");
                stage = now;
            }
        }
    }
}
