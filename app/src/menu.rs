//! What the stream shows over the picture: the side drawer (opened by an edge
//! swipe, Select+Start or F8) and the connecting / reconnecting panel.
use crate::ui::{alpha, Kind, Rect, Ui, BLACK, INK, LINE, MUTED, RADIUS, RED, SURFACE};
use serde_json::Value;

pub enum Action {
    Close,
    Disconnect,
    Touch(bool),
    /// Windows display scale of the dedicated desktop, in percent.
    Scale(u32),
    /// Upstream PrivacyMode: 1 normal display, 2 privacy screensaver, 3 display off.
    Privacy(u32),
}

const WIDTH: f32 = 420.0;
const PAD: f32 = 28.0;

/// The drawer sliding in from the left (`side` < 0) or right edge. Returns how
/// far it is out (0 = gone, so the caller can stop drawing) and what was picked.
pub fn drawer(ui: &mut Ui, open: bool, side: f32, alias: &str, s: &Value, touch_mouse: bool) -> (f32, Option<Action>) {
    let t = ui.tween("drawer", open as u8 as f32, if open { 240.0 } else { 200.0 });
    if t <= 0.0 {
        return (0.0, None);
    }
    let (w, h) = ui.size;
    let pw = WIDTH.min(w - 40.0);
    let x = if side < 0.0 { -pw * (1.0 - t) } else { w - pw * t };
    let panel = Rect::new(x, 0.0, pw, h);
    ui.rect(Rect::new(0.0, 0.0, w, h), 0.0, alpha(BLACK, 0.32 * t));
    ui.rect(panel, 0.0, SURFACE);
    let mut action = None;
    if open && (ui.back || ui.tap(Rect::new(0.0, 0.0, w, h)) && !ui.tap(panel)) {
        action = Some(Action::Close);
    }

    let (x, cw) = (x + PAD, pw - 2.0 * PAD);
    let mut y = PAD + 8.0;
    let name = ui.clip(alias, cw, 24.0, true);
    ui.text(&name, x, y + 14.0, 24.0, true, INK, 0.0);
    y += 46.0;
    let (m, n) = (&s["media"], &s["network"]);
    // `play` has no upstream snapshot.
    if m["width"].is_u64() {
        let codec = m["codec"].as_str().unwrap_or("").to_uppercase();
        let line = format!("{codec} · {}×{} · {:.0} 帧", m["width"], m["height"], m["fps"].as_f64().unwrap_or(0.0));
        ui.text(&line, x, y + 10.0, 16.0, false, MUTED, 0.0);
        y += 30.0;
        let f = |v: &Value| v.as_f64().unwrap_or(0.0);
        let line = format!("解码 {:.1} ms · 往返 {:.0} ms · {:.0} Mbps · 丢包 {:.1}%", f(&m["decodeMs"]), f(&n["rttMs"]),
            f(&n["bitrateMbps"]), f(&n["lossPercent"]));
        ui.text(&line, x, y + 10.0, 16.0, false, MUTED, 0.0);
        y += 44.0;
    }
    ui.rect(Rect::new(x, y, cw, 1.0), 0.0, LINE);
    y += 28.0;

    ui.text("触屏", x, y + 10.0, 17.0, true, INK, 0.0);
    y += 32.0;
    let labels = ["多点触控".to_string(), "桌面鼠标".to_string()];
    let (pick, used) = ui.chips("menu/touch", x, y, cw, &labels, Some(touch_mouse as usize));
    if let Some(i) = pick {
        action = Some(Action::Touch(i == 1));
    }
    y += used + 32.0;

    // Only the dedicated desktop: on a physical screen this would rescale the PC's own monitor.
    let d = &s["desktopScale"];
    let options: Vec<u64> = d["options"].as_array().map_or(vec![], |a| a.iter().filter_map(Value::as_u64).collect());
    if d["virtual"] == true && !options.is_empty() {
        ui.text("桌面缩放", x, y + 10.0, 17.0, true, INK, 0.0);
        y += 32.0;
        let labels: Vec<String> = options.iter().map(|v| format!("{v}%")).collect();
        let want = d["requested"].as_u64().filter(|&v| v > 0).or(d["current"].as_u64());
        let (pick, used) = ui.chips("menu/scale", x, y, cw, &labels, options.iter().position(|&v| Some(v) == want));
        if let Some(i) = pick {
            action = Some(Action::Scale(options[i] as u32));
        }
        y += used;
        let notice = d["notice"].as_str().unwrap_or("");
        y += if notice.is_empty() { 0.0 } else { ui.paragraph(notice, x, y + 8.0, cw, 14.0, MUTED, 0.0) + 8.0 };
        y += 32.0;
    }

    ui.text("电脑屏幕", x, y + 10.0, 17.0, true, INK, 0.0);
    y += 32.0;
    // GameViewer answers 508 to modes 2 and 3 while its virtual display is up (tools/results/20261102-input.txt).
    if d["virtual"] == true {
        ui.paragraph("专用桌面下电脑不接受隐私屏保和关闭显示屏，要用请在设置里关掉专用桌面再连接。", x, y + 8.0, cw, 14.0, MUTED, 0.0);
    } else {
        privacy(ui, x, y, cw, &s["privacy"], &mut action);
    }

    let half = (cw - 12.0) / 2.0;
    let bottom = h - PAD - 56.0;
    if ui.button("menu/close", Rect::new(x, bottom, half, 56.0), "继续串流", None, Kind::Plain, true) {
        action = Some(Action::Close);
    }
    if ui.button("menu/disconnect", Rect::new(x + half + 12.0, bottom, half, 56.0), "断开连接", Some("exit"), Kind::Danger, true) {
        action = Some(Action::Disconnect);
    }
    (t, action.filter(|_| open))
}

/// The three privacy chips and the host's last answer under them.
fn privacy(ui: &mut Ui, x: f32, y: f32, cw: f32, p: &Value, action: &mut Option<Action>) {
    let labels = ["正常显示".to_string(), "隐私屏保".to_string(), "关闭显示屏".to_string()];
    let mode = if p["pending"] == true { &p["requested"] } else { &p["mode"] };
    let chosen = mode.as_u64().filter(|m| (1..=3).contains(m) && (p["known"] == true || p["pending"] == true));
    let (pick, used) = ui.chips("menu/privacy", x, y, cw, &labels, chosen.map(|m| m as usize - 1));
    if let Some(i) = pick {
        *action = Some(Action::Privacy(i as u32 + 1));
    }
    ui.paragraph(p["notice"].as_str().unwrap_or(""), x, y + used + 8.0, cw, 14.0, MUTED, 0.0);
}

/// The panel shown while connecting or reconnecting; true when cancelled.
pub fn status(ui: &mut Ui, title: &str, detail: &str, error: bool) -> bool {
    let (w, h) = ui.size;
    let (pw, ph) = (480.0, 236.0);
    let r = Rect::new(((w - pw) / 2.0).round(), ((h - ph) / 2.0).round(), pw, ph);
    ui.rect(r, RADIUS + 2.0, SURFACE);
    let title = ui.clip(title, pw - 64.0, 22.0, true);
    ui.text(&title, r.x + 32.0, r.y + 48.0, 22.0, true, INK, 0.0);
    // Upstream's reconnect text names the Switch's B button; the pad's B works here too.
    let detail = detail.trim_end_matches("，B 取消");
    ui.paragraph(detail, r.x + 32.0, r.y + 76.0, pw - 64.0, 16.0, if error { RED } else { MUTED }, 0.0);
    ui.button("status/cancel", Rect::new(r.x + pw - 32.0 - 140.0, r.y + ph - 32.0 - 52.0, 140.0, 52.0), "取消", None, Kind::Plain, true)
        || ui.back
}
