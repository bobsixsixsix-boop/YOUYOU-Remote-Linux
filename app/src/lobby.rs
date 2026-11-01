//! The pages before a stream: first-run setup, QR and SMS login, the computer
//! list, stream settings and the account. Login and device state is upstream's
//! snapshot; the pages draw it and send upstream commands.
use crate::bridge::Core;
use crate::ui::{edit, Kind, Rect, Ui, ACCENT, BG, GREEN, INK, LINE, LINE_TEXT, MUTED, PALE, RADIUS, RED, SURFACE};
use crate::{online, Prefs, Screen};
use serde_json::{json, Value};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

pub enum Exit {
    Connect(Value),
    Quit,
}

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Home,
    Settings,
    Size,
    Account,
}

#[derive(Clone, Copy, PartialEq)]
enum Field {
    Country,
    Mobile,
    Code,
    Width,
    Height,
}

const BAR: f32 = 72.0;
const MARGIN: f32 = 40.0;
const FPS: [u64; 5] = [30, 60, 120, 144, 164];
const MBPS: [u64; 6] = [0, 20, 40, 60, 80, 100];

struct Lobby {
    page: Page,
    field: Field,
    country: String,
    mobile: String,
    code: String,
    width: String,
    height: String,
    /// Why the last stream ended, a setup failure, or a bad custom size.
    notice: String,
    download: Option<mpsc::Receiver<std::io::Result<()>>>,
    logout_armed: bool,
    /// The SMS resend countdown is showing: redraw each poll.
    ticking: bool,
}

/// Runs the pages until a computer is picked or the window closes. `core` is
/// None until the signing material is in place (the setup page provides it).
pub fn run(screen: &mut Screen, core: &mut Option<Core>, prefs: &mut Prefs, notice: String) -> Result<Exit, String> {
    let mut lobby = Lobby {
        page: Page::Home,
        field: Field::Mobile,
        country: "86".into(),
        mobile: String::new(),
        code: String::new(),
        width: String::new(),
        height: String::new(),
        notice,
        download: None,
        logout_armed: false,
        ticking: false,
    };
    let mut snap = core.as_ref().map_or(Value::Null, Core::snapshot);
    let mut polled = Instant::now();
    screen.mouse.show_cursor(true);
    screen.ui.dirty = true;
    loop {
        let timeout = if screen.ui.dirty { 0 } else if screen.ui.animating { 8 } else { 250 };
        let first = screen.pump.wait_event_timeout(timeout);
        let events: Vec<_> = first.into_iter().chain(screen.pump.poll_iter()).collect();
        for e in events {
            if let sdl2::event::Event::Quit { .. } = e {
                return Ok(Exit::Quit);
            }
            screen.plug(&e);
            screen.ui.feed(&e);
        }
        if let Some(done) = lobby.download.as_ref().and_then(|rx| rx.try_recv().ok()) {
            lobby.download = None;
            match done.map_err(|e| e.to_string()).and_then(|()| crate::open_core()) {
                Ok(opened) => *core = opened,
                Err(e) => lobby.notice = format!("准备失败：{e}"),
            }
            snap = core.as_ref().map_or(Value::Null, Core::snapshot);
            screen.ui.dirty = true;
        }
        if let Some(c) = core.as_ref().filter(|_| polled.elapsed() >= Duration::from_millis(250)) {
            polled = Instant::now();
            let s = c.snapshot();
            if s != snap || lobby.ticking {
                snap = s;
                screen.ui.dirty = true;
            }
        }
        if screen.ui.dirty || screen.ui.animating {
            screen.ui.begin(screen.window.size(), screen.window.drawable_size());
            screen.ui.clear(BG);
            let panel = screen.window.drawable_size();
            let exit = lobby.draw(&mut screen.ui, core.as_ref(), &mut snap, prefs, panel);
            screen.ui.end();
            screen.window.gl_swap_window();
            if exit.is_some() {
                return Ok(exit.expect("checked"));
            }
        }
    }
}

fn brand(ui: &mut Ui, x: f32, cy: f32) {
    ui.mark(x, cy - 18.0, 36.0);
    ui.text("YOUYOU 远程", x + 48.0, cy, 21.0, true, INK, 0.0);
}

fn bar(ui: &mut Ui) {
    let w = ui.size.0;
    ui.rect(Rect::new(0.0, 0.0, w, BAR), 0.0, SURFACE);
    ui.rect(Rect::new(0.0, BAR - 1.0, w, 1.0), 0.0, LINE);
}

/// The bar of a sub page: back arrow and title. True when going back.
fn sub_bar(ui: &mut Ui, title: &str) -> bool {
    bar(ui);
    let back = ui.button("back", Rect::new(MARGIN - 12.0, 12.0, 48.0, 48.0), "", Some("back"), Kind::Text, true);
    ui.text(title, MARGIN + 48.0, BAR / 2.0, 21.0, true, INK, 0.0);
    back || ui.back
}

/// Content column: left edge and width.
fn column(ui: &Ui, max: f32) -> (f32, f32) {
    let w = (ui.size.0 - 2.0 * MARGIN).min(max);
    ((ui.size.0 - w) / 2.0, w)
}

/// Upstream's own stream-size rule (stream_limits.hpp streamSize).
fn valid_size(w: u32, h: u32) -> bool {
    let (long, short) = (w.max(h), w.min(h));
    w % 2 == 0 && h % 2 == 0 && (320..=4096).contains(&long) && (180..=2160).contains(&short)
}

/// Stream sizes offered for a `w`×`h` landscape panel: its own aspect at 100,
/// 75 and 50 %, then the 16:9 sizes hosts encode natively.
fn presets(w: u32, h: u32) -> Vec<(String, u32, u32)> {
    let even = |v: f32| (v / 2.0).round() as u32 * 2;
    let mut out: Vec<(String, u32, u32)> = [("原生", 1.0), ("75%", 0.75), ("50%", 0.5)]
        .into_iter()
        .map(|(name, f)| (name, even(w as f32 * f), even(h as f32 * f)))
        .filter(|&(_, w, h)| valid_size(w, h))
        .map(|(name, w, h)| (format!("{name} · {w}×{h}"), w, h))
        .collect();
    for (w, h) in [(1920, 1080), (2560, 1440)] {
        if !out.iter().any(|p| (p.1, p.2) == (w, h)) {
            out.push((format!("{w}×{h}"), w, h));
        }
    }
    out
}

/// Upstream's quality settings (it saves them in config.ini) with `change`
/// applied. `s` is updated too, so the page shows the choice before the next snapshot.
fn apply(core: &Core, s: &mut Value, change: impl FnOnce(&mut Value)) {
    let mut q = json!({"width": s["width"], "height": s["height"], "fps": s["fps"], "quality": s["quality"],
        "max_bitrate_mbps": s["maxBitrateMbps"], "hevc": s["hevc"]});
    change(&mut q);
    if core.command("quality-settings", &q).is_ok() {
        for (from, to) in [("width", "width"), ("height", "height"), ("fps", "fps"), ("quality", "quality"),
            ("max_bitrate_mbps", "maxBitrateMbps"), ("hevc", "hevc")]
        {
            s[to] = q[from].clone();
        }
    }
}

/// "2280×1428 · 120 帧 · H.265 · 专用桌面"
fn summary(s: &Value, prefs: &Prefs) -> String {
    let mut text = format!("{}×{} · {} 帧 · {}", s["width"], s["height"], s["fps"], if s["hevc"] == true { "H.265" } else { "H.264" });
    if s["quality"] == 6 {
        text += &format!(" · {} Mbps", s["maxBitrateMbps"]);
    }
    if prefs.desktop {
        text += " · 专用桌面";
    }
    text
}

impl Lobby {
    fn draw(&mut self, ui: &mut Ui, core: Option<&Core>, s: &mut Value, prefs: &mut Prefs, panel: (u32, u32)) -> Option<Exit> {
        let Some(core) = core else {
            self.setup(ui);
            return None;
        };
        if s["authenticated"] != true {
            if s["loginMode"] == "sms" {
                self.sms(ui, core, s);
            } else {
                self.qr(ui, core, s);
            }
            return None;
        }
        self.ticking = false;
        match self.page {
            Page::Home => return self.home(ui, core, s, prefs),
            Page::Settings => self.settings(ui, core, s, prefs, panel),
            Page::Size => self.size(ui, core, s),
            Page::Account => self.account(ui, core, s),
        }
        None
    }

    fn setup(&mut self, ui: &mut Ui) {
        let (w, h) = ui.size;
        brand(ui, MARGIN, 44.0);
        let (x, cw) = (w / 2.0 - 300.0, 600.0);
        let mut y = ((h - 420.0) / 2.0).max(96.0);
        ui.text("首次使用：准备签名材料", w / 2.0, y + 18.0, 28.0, true, INK, 0.5);
        y += 60.0;
        y += ui.paragraph("客户端要用官方 UU 远程电视版 3.5.0 里的请求签名材料。点下面的按钮，会从网易官网下载这个安装包，在本机提取材料，不保留安装包。",
            x, y, cw, 17.0, MUTED, 0.5) + 28.0;
        let busy = self.download.is_some();
        let label = if busy { "正在下载…" } else { "从网易官网下载并准备" };
        if ui.button("download", Rect::new(w / 2.0 - 180.0, y, 360.0, 56.0), label, None, Kind::Primary, !busy) {
            let (tx, rx) = mpsc::channel();
            thread::spawn(move || {
                let _ = tx.send(youyou_account::download_official());
            });
            self.download = Some(rx);
            self.notice.clear();
        }
        y += 84.0;
        y += ui.paragraph("也可以在终端运行 youyou import-apk <安装包路径>，导入已经下载好的安装包。", x, y, cw, 15.0, MUTED, 0.5) + 12.0;
        ui.paragraph(&self.notice, x, y, cw, 15.0, RED, 0.5);
    }

    fn qr(&mut self, ui: &mut Ui, core: &Core, s: &Value) {
        let (w, h) = ui.size;
        brand(ui, MARGIN, 44.0);
        let cx = w / 2.0;
        let mut y = ((h - 600.0) / 2.0).max(96.0);
        let stage = s["stage"].as_str().unwrap_or("");
        ui.text("扫码登录", cx, y + 18.0, 30.0, true, INK, 0.5);
        y += 52.0;
        let detail = s["detail"].as_str().unwrap_or("");
        y += ui.paragraph(detail, cx - 300.0, y, 600.0, 17.0, if stage == "error" { RED } else { MUTED }, 0.5) + 16.0;
        let frame = Rect::new(cx - 150.0, y, 300.0, 300.0);
        ui.rect(frame, RADIUS, SURFACE);
        ui.outline(frame, RADIUS, 1.0, LINE);
        match s["qrUrl"].as_str().filter(|u| !u.is_empty() && stage == "login") {
            Some(url) => ui.qr(url, frame.grow(-20.0)),
            None => {
                let text = if stage == "login" { "正在获取二维码…" } else { "二维码已失效" };
                ui.text(text, cx, frame.y + 150.0, 17.0, false, MUTED, 0.5);
            }
        }
        y += 332.0;
        if ui.button("sms-open", Rect::new(cx - 228.0, y, 220.0, 52.0), "短信验证码登录", None, Kind::Plain, true) {
            let _ = core.command("sms-open", &json!({}));
        }
        if ui.button("qr-refresh", Rect::new(cx + 8.0, y, 220.0, 52.0), "刷新二维码", Some("refresh"), Kind::Plain, true) {
            let _ = core.command("login", &json!({}));
        }
        ui.text("非官方客户端 · 感谢 Antman2023、doke69", cx, h - 28.0, 14.0, false, MUTED, 0.5);
    }

    fn key(&mut self, k: char) {
        let (value, max) = match self.field {
            Field::Country => (&mut self.country, 4),
            Field::Mobile => (&mut self.mobile, 15),
            Field::Code => (&mut self.code, 6),
            Field::Width => (&mut self.width, 4),
            Field::Height => (&mut self.height, 4),
        };
        edit(value, k, max);
    }

    /// Fields on the left (top in portrait) and a keypad; physical digits work too.
    fn form(&mut self, ui: &mut Ui, title: &str, fields: &[(Field, &str, String, &str)], height: f32) -> (f32, f32, f32) {
        let (w, h) = ui.size;
        for k in ui.typed.clone() {
            self.key(k);
        }
        let (form_w, pad) = (440.0, (312.0, 318.0));
        let wide = w > h;
        let total = if wide { (form_w + 64.0 + pad.0, height.max(pad.1 + 80.0)) } else { (form_w, height + 40.0 + pad.1) };
        let x = (w - total.0) / 2.0;
        let mut y = ((h - total.1) / 2.0).max(96.0);
        let top = y;
        // The size page has its title in the bar already.
        let head = if title.is_empty() { 0.0 } else { 64.0 };
        ui.text(title, x, y + 18.0, 30.0, true, INK, 0.0);
        y += head;
        for (i, (field, label, value, placeholder)) in fields.iter().enumerate() {
            let shown = if *field == Field::Country { format!("+{value}") } else { value.clone() };
            if ui.field(&format!("field/{i}"), Rect::new(x, y, form_w, 60.0), label, &shown, placeholder, self.field == *field) {
                self.field = *field;
            }
            y += 72.0;
        }
        let keys = if wide { Rect::new(x + form_w + 64.0, top + head, pad.0, pad.1) } else { Rect::new(w / 2.0 - pad.0 / 2.0, top + height + 40.0, pad.0, pad.1) };
        if let Some(k) = ui.keypad("keypad", keys) {
            self.key(k);
        }
        (x, y, form_w)
    }

    fn sms(&mut self, ui: &mut Ui, core: &Core, s: &Value) {
        brand(ui, MARGIN, 44.0);
        let stage = s["stage"].as_str().unwrap_or("");
        let busy = matches!(stage, "sms-sending" | "sms-verifying");
        let fields = [
            (Field::Country, "国家/地区", self.country.clone(), ""),
            (Field::Mobile, "手机号码", self.mobile.clone(), "输入手机号码"),
            (Field::Code, "验证码", self.code.clone(), "6 位验证码"),
        ];
        if !matches!(self.field, Field::Country | Field::Mobile | Field::Code) {
            self.field = Field::Mobile;
        }
        let (x, mut y, form_w) = self.form(ui, "短信验证码登录", &fields, 440.0);
        let wait = (s["smsRetryAt"].as_i64().unwrap_or(0) - crate::now_us() / 1000 + 999) / 1000;
        self.ticking = wait > 0;
        let half = (form_w - 12.0) / 2.0;
        let send = if wait > 0 { format!("重新发送（{wait} 秒）") } else { "发送验证码".into() };
        if ui.button("sms-code", Rect::new(x, y, half, 56.0), &send, None, Kind::Plain, !busy && wait <= 0 && self.mobile.len() >= 5) {
            let _ = core.command("sms-code", &json!({"countryCode": self.country, "mobile": self.mobile}));
            self.field = Field::Code;
        }
        let ready = !busy && self.code.len() >= 4 && self.mobile.len() >= 5;
        if ui.button("sms-login", Rect::new(x + half + 12.0, y, half, 56.0), "登录", None, Kind::Primary, ready) {
            let _ = core.command("sms-login", &json!({"countryCode": self.country, "mobile": self.mobile, "code": self.code}));
        }
        y += 68.0;
        if ui.button("qr", Rect::new(x, y, form_w, 48.0), "改用扫码登录", None, Kind::Text, true) || ui.back {
            let _ = core.command("login", &json!({}));
        }
        y += 60.0;
        let detail = s["detail"].as_str().unwrap_or("");
        ui.paragraph(detail, x, y, form_w, 15.0, if stage == "error" { RED } else { MUTED }, 0.0);
    }

    fn home(&mut self, ui: &mut Ui, core: &Core, s: &Value, prefs: &Prefs) -> Option<Exit> {
        let (w, h) = ui.size;
        bar(ui);
        brand(ui, MARGIN, BAR / 2.0);
        let nickname = s["nickname"].as_str().filter(|n| !n.is_empty()).unwrap_or("账号");
        let nickname = ui.clip(nickname, 180.0, 16.0, true);
        let account = Rect::new(w - MARGIN - (ui.measure(&nickname, 16.0, true) + 64.0), 14.0, ui.measure(&nickname, 16.0, true) + 64.0, 44.0);
        if ui.button("account", account, &nickname, Some("account"), Kind::Text, true) {
            self.page = Page::Account;
        }
        let loading = s["stage"] == "loading";
        let refresh = Rect::new(account.x - 116.0, 14.0, 108.0, 44.0);
        if ui.button("refresh", refresh, if loading { "同步中" } else { "刷新" }, Some("refresh"), Kind::Text, !loading) {
            self.notice.clear();
            let _ = core.command("refresh", &json!({}));
        }

        let (x0, cw) = column(ui, 1280.0);
        let mut y = BAR + 32.0;
        let mut devices = s["devices"].as_array().cloned().unwrap_or_default();
        devices.sort_by_key(|d| !online(d));
        let title = ui.text("我的电脑", x0, y + 22.0, 26.0, true, INK, 0.0);
        ui.text(&format!("{} 台", devices.len()), x0 + title + 12.0, y + 22.0, 16.0, false, MUTED, 0.0);
        let settings = Rect::new(x0 + cw - 136.0, y, 136.0, 44.0);
        if ui.button("settings", settings, "串流设置", Some("settings"), Kind::Plain, true) {
            self.page = Page::Settings;
        }
        ui.text(&summary(s, prefs), settings.x - 16.0, y + 22.0, 15.0, false, MUTED, 1.0);
        y += 68.0;

        let (gap, card_h) = (20.0, 172.0);
        let cols = ((cw + gap) / 340.0).floor().max(1.0);
        let card_w = (cw - gap * (cols - 1.0)) / cols;
        let mut picked = None;
        for (i, d) in devices.iter().enumerate() {
            let (col, row) = ((i as f32 % cols).floor(), (i as f32 / cols).floor());
            if card(ui, d, Rect::new(x0 + col * (card_w + gap), y + row * (card_h + gap), card_w, card_h)) {
                picked = Some(d.clone());
            }
        }
        // shortcut: no scrolling; 3 columns × 3 rows fit a Y700 in landscape. Add it when someone has more computers.
        y += (devices.len() as f32 / cols).ceil() * (card_h + gap) + 4.0;
        if !self.notice.is_empty() {
            y += ui.paragraph(&self.notice, x0, y, cw, 16.0, RED, 0.0) + 4.0;
        }
        let detail = s["detail"].as_str().unwrap_or("");
        ui.paragraph(detail, x0, y, cw, 16.0, if s["stage"] == "error" { RED } else { MUTED }, 0.0);
        ui.text("连接后从屏幕左右边缘向内滑出菜单 · 手柄 Select+Start · 键盘 F8", w / 2.0, h - 28.0, 14.0, false, MUTED, 0.5);
        if picked.is_some() {
            self.notice.clear();
        }
        picked.map(Exit::Connect)
    }

    fn settings(&mut self, ui: &mut Ui, core: &Core, s: &mut Value, prefs: &mut Prefs, panel: (u32, u32)) {
        if sub_bar(ui, "串流设置") {
            self.page = Page::Home;
            return;
        }
        let (x0, cw) = column(ui, 1100.0);
        ui.text("改动从下一次连接开始生效", x0 + cw, BAR / 2.0, 15.0, false, MUTED, 1.0);
        let (lw, mut y) = (150.0, BAR + 36.0);
        let (x, width) = (x0 + lw, cw - lw);
        let row = |ui: &mut Ui, y: f32, title: &str| {
            ui.text(title, x0, y + 23.0, 18.0, true, INK, 0.0);
        };
        let note = |ui: &mut Ui, y: f32, text: &str| ui.paragraph(text, x, y + 8.0, width, 15.0, MUTED, 0.0) + 8.0;

        let current = (s["width"].as_u64().unwrap_or(0) as u32, s["height"].as_u64().unwrap_or(0) as u32);
        let mut sizes = presets(panel.0.max(panel.1), panel.0.min(panel.1));
        if !sizes.iter().any(|p| (p.1, p.2) == current) {
            sizes.push((format!("{}×{}", current.0, current.1), current.0, current.1));
        }
        let mut labels: Vec<String> = sizes.iter().map(|p| p.0.clone()).collect();
        labels.push("自定义…".into());
        row(ui, y, "分辨率");
        let (pick, used) = ui.chips("size", x, y, width, &labels, sizes.iter().position(|p| (p.1, p.2) == current));
        match pick {
            Some(i) if i == sizes.len() => {
                (self.width, self.height, self.field) = (current.0.to_string(), current.1.to_string(), Field::Width);
                self.notice.clear();
                self.page = Page::Size;
            }
            Some(i) => apply(core, s, |q| (q["width"], q["height"]) = (sizes[i].1.into(), sizes[i].2.into())),
            None => {}
        }
        y += used + 28.0;

        row(ui, y, "帧率");
        let labels: Vec<String> = FPS.iter().map(|f| format!("{f} 帧")).collect();
        let (pick, used) = ui.chips("fps", x, y, width, &labels, FPS.iter().position(|f| s["fps"] == *f));
        if let Some(i) = pick {
            apply(core, s, |q| q["fps"] = FPS[i].into());
        }
        y += used + note(ui, y + used, "面板刷新率要是帧率的整数倍才顺：120 帧配 120 Hz，144 帧配 144 Hz。") + 20.0;

        row(ui, y, "编码");
        let labels = ["H.265".to_string(), "H.264".to_string()];
        let (pick, used) = ui.chips("codec", x, y, width, &labels, Some(if s["hevc"] == true { 0 } else { 1 }));
        if let Some(i) = pick {
            apply(core, s, |q| q["hevc"] = (i == 0).into());
        }
        y += used + 28.0;

        row(ui, y, "码率上限");
        let labels: Vec<String> = MBPS.iter().map(|&m| if m == 0 { "自动".into() } else { format!("{m} Mbps") }).collect();
        let chosen = if s["quality"] == 6 { MBPS.iter().position(|m| s["maxBitrateMbps"] == *m) } else { Some(0) };
        let (pick, used) = ui.chips("mbps", x, y, width, &labels, chosen);
        if let Some(i) = pick {
            // 3 is upstream's default preset; 6 is its custom bitrate.
            apply(core, s, |q| (q["quality"], q["max_bitrate_mbps"]) = ((if MBPS[i] == 0 { 3 } else { 6 }).into(), MBPS[i].into()));
        }
        y += used + 28.0;

        row(ui, y, "专用桌面");
        let labels = ["关闭".to_string(), "开启".to_string()];
        let (pick, used) = ui.chips("desktop", x, y, width, &labels, Some(prefs.desktop as usize));
        if let Some(i) = pick {
            prefs.desktop = i == 1;
            prefs.save();
        }
        y += used + note(ui, y + used, "开启后电脑为平板新建一块虚拟屏，尺寸跟分辨率一致，电脑会记住它的位置。") + 20.0;

        row(ui, y, "触屏");
        let labels = ["多点触控".to_string(), "桌面鼠标".to_string()];
        let (pick, used) = ui.chips("touch", x, y, width, &labels, Some(prefs.touch_mouse as usize));
        if let Some(i) = pick {
            prefs.touch_mouse = i == 1;
            prefs.save();
        }
        y += used + note(ui, y + used, "桌面鼠标：单指按下就是鼠标左键，可以直接拖窗口。串流中也能在菜单里切换。") + 20.0;

        row(ui, y, "画面节奏");
        let labels = ["流畅".to_string(), "低延迟".to_string()];
        let (pick, used) = ui.chips("pace", x, y, width, &labels, Some(!prefs.pace as usize));
        if let Some(i) = pick {
            prefs.pace = i == 0;
            prefs.save();
        }
        note(ui, y + used, "流畅：每帧按顺序各显示一次，不重复不跳帧，多约 10 毫秒延迟。低延迟：总显示最新一帧，网络一抖就偶尔重复或跳帧。");
    }

    fn size(&mut self, ui: &mut Ui, core: &Core, s: &mut Value) {
        if sub_bar(ui, "自定义分辨率") {
            self.page = Page::Settings;
            return;
        }
        if !matches!(self.field, Field::Width | Field::Height) {
            self.field = Field::Width;
        }
        let fields = [(Field::Width, "宽", self.width.clone(), "像素"), (Field::Height, "高", self.height.clone(), "像素")];
        let (x, mut y, form_w) = self.form(ui, "", &fields, 266.0);
        let half = (form_w - 12.0) / 2.0;
        if ui.button("size-cancel", Rect::new(x, y, half, 56.0), "取消", None, Kind::Plain, true) {
            self.page = Page::Settings;
        }
        if ui.button("size-ok", Rect::new(x + half + 12.0, y, half, 56.0), "确定", None, Kind::Primary, true) {
            match (self.width.parse(), self.height.parse()) {
                (Ok(w), Ok(h)) if valid_size(w, h) => {
                    apply(core, s, |q| (q["width"], q["height"]) = (w.into(), h.into()));
                    self.page = Page::Settings;
                }
                _ => self.notice = "宽高都要是偶数，长边 320–4096，短边 180–2160。".into(),
            }
        }
        y += 72.0;
        let (text, color) = if self.notice.is_empty() { ("宽高都要是偶数，长边 320–4096，短边 180–2160。", MUTED) } else { (self.notice.as_str(), RED) };
        let text = text.to_string();
        ui.paragraph(&text, x, y, form_w, 15.0, color, 0.0);
    }

    fn account(&mut self, ui: &mut Ui, core: &Core, s: &Value) {
        if sub_bar(ui, "账号") {
            (self.page, self.logout_armed) = (Page::Home, false);
            return;
        }
        let (w, h) = ui.size;
        let cx = w / 2.0;
        let mut y = BAR + 56.0;
        ui.rect(Rect::new(cx - 40.0, y, 80.0, 80.0), 40.0, PALE);
        ui.icon("account", cx, y + 40.0, 40.0, ACCENT);
        y += 116.0;
        let name = s["nickname"].as_str().filter(|n| !n.is_empty()).unwrap_or("UU 远程账号");
        ui.text(name, cx, y, 24.0, true, INK, 0.5);
        y += 38.0;
        ui.text("已登录，与电脑端使用同一个账号", cx, y, 16.0, false, MUTED, 0.5);
        y += 44.0;
        let label = if self.logout_armed { "再点一次确认退出" } else { "退出登录" };
        if ui.button("logout", Rect::new(cx - 140.0, y, 280.0, 54.0), label, Some("exit"), Kind::Danger, true) {
            if self.logout_armed {
                let _ = core.command("logout", &json!({}));
                let _ = core.command("login", &json!({}));
                (self.page, self.logout_armed) = (Page::Home, false);
            } else {
                self.logout_armed = true;
            }
        }
        let about = format!("YOUYOU 远程 {} · 非官方的 Linux 客户端，与网易、联想无关。\n协议实现来自 Antman2023 和 doke69 的 Switch 客户端，感谢两位作者。",
            env!("CARGO_PKG_VERSION"));
        ui.paragraph(&about, cx - 320.0, h - 110.0, 640.0, 14.0, MUTED, 0.5);
    }
}

/// One computer; true when its connect button is clicked.
fn card(ui: &mut Ui, d: &Value, r: Rect) -> bool {
    let on = online(d);
    ui.rect(r, RADIUS + 2.0, SURFACE);
    ui.outline(r, RADIUS + 2.0, 1.0, LINE);
    let icon = Rect::new(r.x + 20.0, r.y + 20.0, 56.0, 56.0);
    ui.rect(icon, RADIUS, if on { PALE } else { BG });
    let (ix, iy) = icon.center();
    ui.icon("device", ix, iy, 30.0, if on { ACCENT } else { MUTED });
    let name = ui.clip(d["alias"].as_str().unwrap_or("电脑"), r.w - 116.0, 20.0, true);
    ui.text(&name, r.x + 92.0, r.y + 36.0, 20.0, true, INK, 0.0);
    ui.rect(Rect::new(r.x + 92.0, r.y + 58.0, 8.0, 8.0), 4.0, if on { GREEN } else { LINE_TEXT });
    ui.text(if on { "在线" } else { "离线" }, r.x + 106.0, r.y + 62.0, 15.0, false, MUTED, 0.0);
    if !on {
        ui.text("电脑离线时无法连接", r.x + 20.0, r.y + r.h - 42.0, 14.0, false, MUTED, 0.0);
        return false;
    }
    let id = d["device_id"].as_str().unwrap_or("");
    ui.button(&format!("connect/{id}"), Rect::new(r.x + r.w - 140.0, r.y + r.h - 68.0, 120.0, 48.0), "连接", None, Kind::Primary, true)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn panel_presets() {
        let p: Vec<_> = presets(3040, 1904).into_iter().map(|p| (p.1, p.2)).collect();
        assert_eq!(p, [(3040, 1904), (2280, 1428), (1520, 952), (1920, 1080), (2560, 1440)]);
        // a 4K panel: native is over the 4096x2160 limit, 1920x1080 is its 50 %
        let p: Vec<_> = presets(3840, 2400).into_iter().map(|p| (p.1, p.2)).collect();
        assert_eq!(p, [(2880, 1800), (1920, 1200), (1920, 1080), (2560, 1440)]);
        assert!(valid_size(2280, 1428) && valid_size(1428, 2280) && !valid_size(2281, 1428) && !valid_size(4098, 2160));
    }
}
