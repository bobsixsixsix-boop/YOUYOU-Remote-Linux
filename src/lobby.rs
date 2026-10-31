use crate::core::{Core, State};
#[path = "lobby_text.rs"]
mod text;
use qrcode::{Color as Module, QrCode};
use sdl2::{
    event::{Event, WindowEvent},
    keyboard::Keycode,
    mouse::MouseButton,
    pixels::Color,
    rect::Rect,
    render::WindowCanvas,
};
use serde_json::json;
use std::{
    io::ErrorKind,
    path::PathBuf,
    process::Command,
    sync::mpsc,
    time::{Duration, Instant},
};
use text::Fonts;
use youyou_player::{
    fullscreen,
    icons::{asset_dir, Icons},
};
use youyou_player::{Controllers, PadAction};

const INK: Color = Color::RGB(14, 25, 45);
const MUTED: Color = Color::RGB(86, 100, 119);
const ACCENT: Color = Color::RGB(11, 102, 221);
const BASE: Color = Color::RGB(240, 244, 247);
const PALE_BLUE: Color = Color::RGB(222, 240, 255);
const LINE: Color = Color::RGB(222, 229, 237);

const MARK_SIDE: u32 = 192;
const BRAND_MARK: &[u8] = include_bytes!("../assets/youyou-mark-192.rgba");

fn set_window_icon(window: &mut sdl2::video::Window) -> Result<(), String> {
    let mut rgba = BRAND_MARK.to_vec();
    let surface = sdl2::surface::Surface::from_data(
        &mut rgba,
        MARK_SIDE,
        MARK_SIDE,
        MARK_SIDE * 4,
        sdl2::pixels::PixelFormatEnum::RGBA32,
    )?;
    window.set_icon(surface);
    Ok(())
}
/// Reserve room for the form in either orientation, then use the full drawable.
fn geometry(w: u32, h: u32) -> (u32, u32, f32) {
    let (w, h) = (w.max(1) as f32, h.max(1) as f32);
    let (min_w, min_h) = if w < h { (720., 840.) } else { (1280., 720.) };
    let scale = (w / min_w).min(h / min_h);
    (
        (w / scale).round() as u32,
        (h / scale).round() as u32,
        scale,
    )
}

#[derive(Clone, PartialEq, Eq)]
enum Action {
    Download,
    Import,
    Login,
    Sms,
    Country,
    Phone,
    Code,
    SendCode,
    Verify,
    Cancel,
    Quit,
    Fullscreen,
    ShowNotice,
}
struct Button {
    area: Rect,
    text: String,
    action: Action,
    primary: bool,
    value: Option<String>,
}
fn move_focus(buttons: &[Button], current: usize, dx: i32, dy: i32) -> usize {
    let origin = buttons[current].area.center();
    buttons
        .iter()
        .enumerate()
        .filter_map(|(i, b)| {
            let x = b.area.center().x - origin.x;
            let y = b.area.center().y - origin.y;
            let forward = x * dx + y * dy;
            let side = (x * dy - y * dx).abs();
            (forward > 0).then_some((forward + 3 * side, i))
        })
        .min()
        .map_or(current, |(_, i)| i)
}
fn button(x: i32, y: i32, w: u32, text: &str, action: Action, primary: bool) -> Button {
    Button {
        area: Rect::new(x, y, w, 54),
        text: text.into(),
        action,
        primary,
        value: None,
    }
}
fn fill(canvas: &mut WindowCanvas, rect: Rect, color: Color) -> Result<(), String> {
    canvas.set_draw_color(color);
    canvas.fill_rect(rect)
}
fn rounded(canvas: &mut WindowCanvas, r: Rect, color: Color, radius: i32) -> Result<(), String> {
    fill(
        canvas,
        Rect::new(
            r.x(),
            r.y() + radius,
            r.width(),
            r.height() - 2 * radius as u32,
        ),
        color,
    )?;
    for y in 0..radius {
        let dy = (radius - y) as f32 - 0.5;
        let inset = (radius as f32 - ((radius * radius) as f32 - dy * dy).sqrt()).round() as i32;
        // Overlap inward: some SDL renderers round the position and height
        // independently at fractional DPI, leaving gaps between 1-unit rows.
        for row in [r.y() + y, r.bottom() - 2 - y] {
            fill(
                canvas,
                Rect::new(r.x() + inset, row, r.width() - 2 * inset as u32, 2),
                color,
            )?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn login_fits_both_y700_orientations_and_uses_drawable_density() {
        for (pw, ph, logical_w) in [(3040, 1904, 1280), (1904, 3040, 720)] {
            let (w, h, scale) = geometry(pw, ph);
            assert_eq!(w, logical_w);
            assert!(scale > 2.0);
            assert!((w as f32 * scale - pw as f32).abs() <= scale);
            assert!((h as f32 * scale - ph as f32).abs() <= scale);
        }
    }
    #[test]
    fn pad_focus_follows_the_visible_button_layout() {
        let buttons = vec![
            button(315, 574, 202, "重新生成", Action::Login, false),
            button(539, 574, 202, "短信登录", Action::Sms, false),
            button(763, 574, 202, "取消", Action::Cancel, false),
            button(1040, 44, 188, "退出全屏", Action::Fullscreen, false),
            button(1112, 636, 116, "退出", Action::Quit, false),
        ];
        assert_eq!(move_focus(&buttons, 0, 1, 0), 1);
        assert_eq!(move_focus(&buttons, 1, 1, 0), 2);
        assert_eq!(move_focus(&buttons, 2, -1, 0), 1);
        assert_eq!(move_focus(&buttons, 3, 0, 1), 4);
        assert_eq!(move_focus(&buttons, 2, 0, -1), 3);
        assert_eq!(move_focus(&buttons, 0, -1, 0), 0);
    }
}
fn qr(canvas: &mut WindowCanvas, value: &str, square: Rect) -> Result<(), String> {
    let qr = QrCode::new(value.as_bytes()).map_err(|e| e.to_string())?;
    let modules = qr.width();
    let side = modules + 8;
    let mut rgb = vec![255u8; side * side * 3];
    for y in 0..modules {
        for x in 0..modules {
            if qr[(x, y)] == Module::Dark {
                let at = ((y + 4) * side + x + 4) * 3;
                rgb[at..at + 3].fill(0);
            }
        }
    }
    let creator = canvas.texture_creator();
    let mut texture = creator
        .create_texture_static(
            sdl2::pixels::PixelFormatEnum::RGB24,
            side as u32,
            side as u32,
        )
        .map_err(|e| e.to_string())?;
    texture.set_scale_mode(sdl2::render::ScaleMode::Nearest);
    texture
        .update(None, &rgb, side * 3)
        .map_err(|e| e.to_string())?;
    canvas.copy(&texture, None, square)
}

fn device_illustration(
    c: &mut WindowCanvas,
    icons: &mut Icons,
    center: (i32, i32),
    density: f32,
) -> Result<(), String> {
    if icons.draw(
        c,
        "device",
        Rect::from_center(center, 128, 128),
        density,
        ACCENT,
    )? {
        return Ok(());
    }
    let (x, y) = (center.0 - 66, center.1 - 53);
    for r in [
        Rect::new(x, y, 132, 5),
        Rect::new(x, y, 5, 80),
        Rect::new(x + 127, y, 5, 80),
        Rect::new(x, y + 80, 132, 5),
        Rect::new(center.0 - 3, y + 85, 6, 17),
        Rect::new(center.0 - 31, y + 102, 62, 5),
    ] {
        fill(c, r, ACCENT)?;
    }
    Ok(())
}

struct Model<'a> {
    state: State,
    icons: Icons<'a>,
    setup: bool,
    fetching: bool,
    notice: String,
    focus: usize,
    pad_name: Option<String>,
    pressed: Option<Action>,
    country: String,
    mobile: String,
    code: String,
}

impl Model<'_> {
    fn note(&self) -> &str {
        if self.notice.is_empty() {
            &self.state.detail
        } else {
            &self.notice
        }
    }

    fn paint(&mut self, c: &mut WindowCanvas, f: &mut Fonts) -> Result<Vec<Button>, String> {
        let (pixels_w, pixels_h) = c.output_size()?;
        let (w, h, density) = geometry(pixels_w, pixels_h);
        if c.logical_size() != (w, h) {
            c.set_logical_size(w, h).map_err(|e| e.to_string())?;
        }
        f.refresh(density)?;
        let (w, h) = (w as i32, h as i32);
        let cx = w / 2;
        let offset = (h - 720) / 2;
        let narrow = w < 1000;
        let margin = if narrow { 30 } else { 60 };
        let center = |y: i32| Rect::new(margin, y + offset, (w - 2 * margin) as u32, 52);
        let centered_button =
            |x: i32, y: i32, width: u32, label: &str, action: Action, primary: bool| {
                button(cx + x, y + offset, width, label, action, primary)
            };
        c.set_draw_color(BASE);
        c.clear();
        let header_h = if narrow { 139 } else { 98 };
        fill(c, Rect::new(0, 0, w as u32, header_h), Color::WHITE)?;
        fill(c, Rect::new(0, header_h as i32 - 1, w as u32, 1), LINE)?;
        let card_w = (w - if narrow { 32 } else { 120 }).min(890);
        let card = Rect::new(cx - card_w / 2, 101 + offset, card_w as u32, 530);
        rounded(c, card, LINE, 28)?;
        rounded(
            c,
            Rect::new(
                card.x() + 1,
                card.y() + 1,
                card.width() - 2,
                card.height() - 2,
            ),
            Color::WHITE,
            27,
        )?;
        let creator = c.texture_creator();
        let mut mark = creator
            .create_texture_static(sdl2::pixels::PixelFormatEnum::RGBA32, MARK_SIDE, MARK_SIDE)
            .map_err(|e| e.to_string())?;
        mark.set_blend_mode(sdl2::render::BlendMode::Blend);
        mark.set_scale_mode(sdl2::render::ScaleMode::Linear);
        mark.update(None, BRAND_MARK, MARK_SIDE as usize * 4)
            .map_err(|e| e.to_string())?;
        c.copy(&mark, None, Rect::new(margin, 32, 48, 48))?;
        let brand = "UU 远程";
        let brand_x = margin + 62;
        let brand_w = f.title.width(brand)? + 3;
        f.title
            .draw(c, brand, Rect::new(brand_x, 25, brand_w, 62), INK, false)?;
        let version_x = brand_x + brand_w as i32 + 18;
        let platform = Rect::new(version_x, 40, 150, 34);
        rounded(c, platform, PALE_BLUE, 17)?;
        f.small.draw(c, "Linux · Y700", platform, ACCENT, true)?;
        let right = w - margin - 164;
        if let Some(name) = &self.pad_name {
            let (x, y, width) = if narrow {
                (margin, 86, w - 2 * margin)
            } else {
                (version_x + 170, 32, right - version_x - 186)
            };
            f.small.draw(
                c,
                &format!("手柄 · {name}"),
                Rect::new(x, y, width.max(1) as u32, 45),
                MUTED,
                false,
            )?;
        }
        let mut buttons = Vec::new();
        let state = &self.state;
        if self.setup {
            f.title.draw(c, "准备连接电脑", center(154), INK, true)?;
            rounded(
                c,
                Rect::from_center((cx, 318 + offset), 180, 138),
                PALE_BLUE,
                28,
            )?;
            device_illustration(c, &mut self.icons, (cx, 318 + offset), density)?;
            f.body.draw(
                c,
                "首次使用需要 UU 远程 TV 3.5.0 的签名材料",
                center(397),
                MUTED,
                true,
            )?;
            if !self.fetching {
                buttons.push(centered_button(
                    -210,
                    496,
                    420,
                    "从网易官网下载并准备",
                    Action::Download,
                    true,
                ));
                buttons.push(centered_button(
                    -150,
                    564,
                    300,
                    "选择本地 APK",
                    Action::Import,
                    false,
                ));
            }
        } else if state.login_mode == "sms" && !state.authenticated {
            f.title.draw(c, "短信验证码登录", center(114), INK, true)?;
            f.body.draw(
                c,
                "登录与电脑端相同的 UU 远程账号",
                center(168),
                MUTED,
                true,
            )?;
            if !matches!(state.stage.as_str(), "sms-sending" | "sms-verifying") {
                let width = (w - 2 * margin).min(584) as u32;
                for (idx, (name, action, value)) in [
                    ("国家 / 地区", Action::Country, format!("+{}", self.country)),
                    (
                        "手机号码",
                        Action::Phone,
                        if self.mobile.is_empty() {
                            "输入手机号码".into()
                        } else {
                            self.mobile.clone()
                        },
                    ),
                    (
                        "验证码",
                        Action::Code,
                        if self.code.is_empty() {
                            "输入验证码".into()
                        } else {
                            "•".repeat(self.code.len())
                        },
                    ),
                ]
                .into_iter()
                .enumerate()
                {
                    let mut field = centered_button(
                        -(width as i32) / 2,
                        241 + 74 * idx as i32,
                        width,
                        name,
                        action,
                        false,
                    );
                    field.value = Some(value);
                    buttons.push(field);
                }
                buttons.push(centered_button(
                    -292,
                    482,
                    278,
                    "发送验证码",
                    Action::SendCode,
                    false,
                ));
                buttons.push(centered_button(14, 482, 278, "登录", Action::Verify, true));
            }
            buttons.push(centered_button(
                -131,
                564,
                262,
                "切换扫码登录",
                Action::Login,
                false,
            ));
        } else if state.stage == "login" {
            f.title.draw(c, "扫码登录", center(116), INK, true)?;
            f.body.draw(
                c,
                "登录与电脑端相同的 UU 远程账号",
                center(159),
                MUTED,
                true,
            )?;
            let square = Rect::new(cx - 120, 241 + offset, 240, 240);
            rounded(c, Rect::new(cx - 134, 227 + offset, 268, 268), BASE, 18)?;
            fill(c, square, Color::WHITE)?;
            if state.qr_url.is_empty() {
                f.small.draw(c, "正在获取二维码…", square, MUTED, true)?;
            } else {
                qr(c, &state.qr_url, square)?;
            }
            f.body.draw(
                c,
                "请在手机 UU 远程 App 中扫码并确认",
                center(505),
                MUTED,
                true,
            )?;
            for (x, text, action) in [
                (-325, "重新生成", Action::Login),
                (-101, "短信登录", Action::Sms),
                (123, "取消", Action::Cancel),
            ] {
                buttons.push(centered_button(x, 570, 202, text, action, false));
            }
        } else {
            f.title.draw(c, "连接你的电脑", center(163), INK, true)?;
            rounded(
                c,
                Rect::from_center((cx, 292 + offset), 180, 132),
                PALE_BLUE,
                28,
            )?;
            device_illustration(c, &mut self.icons, (cx, 292 + offset), density)?;
            f.body.draw(
                c,
                "登录与电脑端相同的 UU 远程账号",
                center(363),
                MUTED,
                true,
            )?;
            if matches!(state.stage.as_str(), "idle" | "error" | "") {
                buttons.push(centered_button(
                    -200,
                    496,
                    400,
                    "扫码登录",
                    Action::Login,
                    true,
                ));
                buttons.push(centered_button(
                    -145,
                    564,
                    290,
                    "短信验证码登录",
                    Action::Sms,
                    false,
                ));
            } else {
                f.body
                    .draw(c, "正在与 UU 远程通信…", center(449), INK, true)?;
                buttons.push(centered_button(
                    -110,
                    555,
                    220,
                    "取消",
                    Action::Cancel,
                    false,
                ));
            }
        }
        buttons.push(button(
            right,
            29,
            164,
            if fullscreen::active(c) {
                "退出全屏"
            } else {
                "进入全屏"
            },
            Action::Fullscreen,
            false,
        ));
        let note = self.note();
        let notice_w = (w - 2 * margin - 134) as u32;
        let mut lines = f.small.lines(note, notice_w)?;
        if lines.len() > 2 {
            let available = notice_w - 146;
            lines = f.small.lines(note, available)?;
            buttons.push(button(
                w - margin - 251,
                h - 77,
                135,
                "查看详情",
                Action::ShowNotice,
                false,
            ));
        }
        let text_w = if lines.len() > 2 {
            notice_w - 146
        } else {
            notice_w
        };
        for (index, line) in lines.iter().take(2).enumerate() {
            let y = h - if lines.len() > 1 { 85 } else { 68 };
            f.small.draw(
                c,
                line,
                Rect::new(margin, y + 32 * index as i32, text_w, 36),
                if state.stage == "error" {
                    Color::RGB(178, 51, 47)
                } else {
                    MUTED
                },
                false,
            )?;
        }
        buttons.push(button(
            w - margin - 104,
            h - 77,
            104,
            "退出",
            Action::Quit,
            false,
        ));
        self.focus = self.focus.min(buttons.len() - 1);
        for (i, b) in buttons.iter().enumerate() {
            let active = self.pressed.as_ref() == Some(&b.action);
            if i == self.focus {
                let r = b.area;
                rounded(
                    c,
                    Rect::new(r.x() - 4, r.y() - 4, r.width() + 8, r.height() + 8),
                    ACCENT,
                    18,
                )?;
                rounded(
                    c,
                    Rect::new(r.x() - 2, r.y() - 2, r.width() + 4, r.height() + 4),
                    Color::WHITE,
                    16,
                )?;
            }
            rounded(
                c,
                b.area,
                if active && b.primary {
                    Color::RGB(9, 77, 173)
                } else if active {
                    Color::RGB(189, 225, 255)
                } else if b.primary {
                    ACCENT
                } else if b.value.is_some() {
                    BASE
                } else {
                    PALE_BLUE
                },
                14,
            )?;
            if let Some(value) = &b.value {
                let label_w = 158;
                f.small.draw(
                    c,
                    &b.text,
                    Rect::new(b.area.x() + 24, b.area.y(), label_w, b.area.height()),
                    MUTED,
                    false,
                )?;
                f.body.draw(
                    c,
                    value,
                    Rect::new(
                        b.area.x() + 24 + label_w as i32,
                        b.area.y(),
                        b.area.width() - 48 - label_w,
                        b.area.height(),
                    ),
                    INK,
                    false,
                )?;
            } else {
                let mut text = Rect::new(
                    b.area.x() + 12,
                    b.area.y(),
                    b.area.width() - 24,
                    b.area.height(),
                );
                let icon = match b.action {
                    Action::Fullscreen => Some(fullscreen::icon_name(c)),
                    Action::Import => Some("folder-open"),
                    Action::Sms => Some("phone"),
                    Action::Login if state.stage == "login" && state.login_mode != "sms" => {
                        Some("refresh")
                    }
                    Action::Login => Some("qr"),
                    _ => None,
                };
                let tint = if b.primary { Color::WHITE } else { ACCENT };
                let mut centered = true;
                if let Some(name) = icon {
                    let side = 25;
                    let gap = 9;
                    let label = f.body.width(&b.text)?;
                    if side + gap + label <= text.width() {
                        let x = text.center().x - ((side + gap + label) / 2) as i32;
                        let area = Rect::new(x, text.center().y - side as i32 / 2, side, side);
                        if self.icons.draw(c, name, area, density, tint)? {
                            text.set_x(x + (side + gap) as i32);
                            text.set_width(label + 1);
                            centered = false;
                        }
                    }
                }
                f.body.draw(c, &b.text, text, tint, centered)?;
            }
        }
        c.present();
        Ok(buttons)
    }
}

fn font_path() -> Result<PathBuf, String> {
    let out = Command::new("fc-match")
        .args(["-f", "%{file}", "sans:lang=zh-cn"])
        .output()
        .map_err(|e| e.to_string())?;
    let path = PathBuf::from(String::from_utf8_lossy(&out.stdout).trim());
    if !out.status.success() || !path.is_file() {
        return Err("找不到可用字体，请安装 fonts-noto-cjk 和 fontconfig".into());
    }
    Ok(path)
}

pub fn preview(mut page: u8) -> Result<(), String> {
    let sdl = sdl2::init()?;
    sdl2::hint::set("SDL_TOUCH_MOUSE_EVENTS", "0");
    let video = sdl.video()?;
    let mut window = video
        .window("YOUYOU 远程 · 离线界面预览（点击切换）", 1280, 720)
        .fullscreen_desktop()
        .resizable()
        .allow_highdpi()
        .position_centered()
        .build()
        .map_err(|e| e.to_string())?;
    set_window_icon(&mut window)?;
    let mut canvas = window.into_canvas().build().map_err(|e| e.to_string())?;
    let mut events = sdl.event_pump()?;
    let mut fullscreen_button = fullscreen::Control::default();
    let creator = canvas.texture_creator();
    let mut icons = Icons::new(&creator);
    let mut controllers = Controllers::new(&sdl);
    loop {
        youyou_player::preview(&mut canvas, page)?;
        fullscreen_button.draw(&mut canvas, &mut icons)?;
        canvas.present();
        loop {
            let event = events.wait_event();
            if let Some(pad) = controllers.handle(&event) {
                match pad {
                    PadAction::Back => return Ok(()),
                    PadAction::Confirm | PadAction::Right | PadAction::Down => {
                        page = (page + 1) % 8
                    }
                    PadAction::Left | PadAction::Up => page = (page + 7) % 8,
                    PadAction::Menu => continue,
                }
                break;
            }
            if fullscreen_button.handle(&mut canvas, &event)? {
                break;
            }
            match event {
                Event::Quit { .. }
                | Event::Window {
                    win_event: WindowEvent::Close,
                    ..
                }
                | Event::KeyDown {
                    keycode: Some(Keycode::ESCAPE),
                    ..
                } => return Ok(()),
                Event::MouseButtonUp {
                    mouse_btn: MouseButton::Left,
                    ..
                }
                | Event::FingerUp { .. }
                | Event::KeyDown {
                    keycode: Some(Keycode::RIGHT | Keycode::SPACE),
                    repeat: false,
                    ..
                } => {
                    page = (page + 1) % 8;
                    break;
                }
                Event::KeyDown {
                    keycode: Some(Keycode::LEFT),
                    repeat: false,
                    ..
                } => {
                    page = (page + 7) % 8;
                    break;
                }
                Event::Window {
                    win_event: WindowEvent::Exposed | WindowEvent::SizeChanged(..),
                    ..
                } => break,
                _ => (),
            }
        }
    }
}

fn pick_apk() -> Result<Option<PathBuf>, String> {
    let out = Command::new("kdialog")
        .args(["--title", "导入 UU 远程 TV 3.5.0", "--getopenfilename"])
        .arg(std::env::var_os("HOME").unwrap_or_default())
        .arg("*.apk|Android APK")
        .output()
        .map_err(|e| {
            format!("打开 KDE 文件选择器失败（{e}）。也可用 youyou-linux import-apk 文件路径")
        })?;
    if !out.status.success() {
        return Ok(None);
    }
    use std::os::unix::ffi::OsStringExt;
    let mut bytes = out.stdout;
    if bytes.last() == Some(&b'\n') {
        bytes.pop();
    }
    if bytes.is_empty() {
        return Ok(None);
    }
    Ok(Some(PathBuf::from(std::ffi::OsString::from_vec(bytes))))
}

fn read_digits(title: &str, secret: bool, max: usize) -> Result<Option<String>, String> {
    let out = Command::new("kdialog")
        .args([
            "--title",
            "YOUYOU 远程",
            if secret { "--password" } else { "--inputbox" },
            title,
        ])
        .output()
        .map_err(|e| e.to_string())?;
    if !out.status.success() {
        return Ok(None);
    }
    let digits = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if digits.is_empty() || digits.len() > max || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return Err("请输入有效数字".into());
    }
    Ok(Some(digits))
}

pub fn run() -> Result<(), String> {
    let sdl = sdl2::init()?;
    sdl2::hint::set("SDL_TOUCH_MOUSE_EVENTS", "1");
    let video = sdl.video()?;
    let ttf = sdl2::ttf::init().map_err(|e| e.to_string())?;
    let bundled = asset_dir().join("fonts/NotoSansSC-Regular.otf");
    let path = if bundled.is_file() {
        bundled
    } else {
        font_path()?
    };
    let mut window = video
        .window("YOUYOU 远程", 1100, 620)
        .fullscreen_desktop()
        .resizable()
        .allow_highdpi()
        .position_centered()
        .build()
        .map_err(|e| e.to_string())?;
    set_window_icon(&mut window)?;
    let mut canvas = window.into_canvas().build().map_err(|e| e.to_string())?;
    let (width, height) = canvas.output_size()?;
    let mut fonts = Fonts::load(&ttf, &path, geometry(width, height).2)?;
    let mut events = Some(sdl.event_pump()?);
    let mut controllers = Controllers::new(&sdl);
    let mut core = match youyou_account::open() {
        Ok(a) => Some(Core::open(a)?),
        Err(e) if e.kind() == ErrorKind::NotFound => None,
        Err(e) => return Err(e.to_string()),
    };
    if let Some(c) = &core {
        c.command("startup", json!({}))?;
    }
    let creator = canvas.texture_creator();
    let mut model = Model {
        state: State::default(),
        icons: Icons::new(&creator),
        setup: core.is_none(),
        fetching: false,
        notice: String::new(),
        focus: 0,
        pad_name: controllers.name(),
        pressed: None,
        country: "86".into(),
        mobile: String::new(),
        code: String::new(),
    };
    let mut buttons = model.paint(&mut canvas, &mut fonts)?;
    let mut download: Option<mpsc::Receiver<Result<(), String>>> = None;
    let mut next_poll = Instant::now();
    loop {
        let mut dirty = false;
        if let Some(rx) = &download {
            match rx.try_recv() {
                Ok(result) => {
                    download = None;
                    model.fetching = false;
                    dirty = true;
                    match result.and_then(|_| crate::core::restore()) {
                        Ok(c) => {
                            core = Some(c);
                            model.setup = false;
                            model.notice.clear();
                        }
                        Err(e) => model.notice = e,
                    }
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    download = None;
                    model.fetching = false;
                    model.notice = "导入任务已退出，可重新尝试".into();
                    dirty = true;
                }
                Err(mpsc::TryRecvError::Empty) => (),
            }
        }
        if let Some(c) = core.as_mut() {
            if Instant::now() >= next_poll {
                next_poll = Instant::now() + Duration::from_millis(125);
                let state = c.state()?;
                if state != model.state {
                    model.state = state;
                    dirty = true;
                    model.pressed = None;
                }
                if model.state.authenticated {
                    model.mobile.clear();
                    model.code.clear();
                    drop(events.take());
                    drop(controllers);
                    c.new_stream();
                    let result = youyou_player::run_authenticated(c, &sdl, &mut canvas);
                    if matches!(result, Ok(youyou_player::Exit::Closed)) {
                        break;
                    }
                    sdl2::hint::set("SDL_TOUCH_MOUSE_EVENTS", "1");
                    canvas
                        .window_mut()
                        .set_title("YOUYOU 远程")
                        .map_err(|e| e.to_string())?;

                    events = Some(sdl.event_pump()?);
                    controllers = Controllers::new(&sdl);
                    model.pad_name = controllers.name();
                    if let Err(e) = result {
                        return Err(e);
                    }
                    model.state = c.state()?;
                    model.notice.clear();
                    dirty = true;
                }
            }
        }
        if dirty {
            buttons = model.paint(&mut canvas, &mut fonts)?;
        }
        let Some(event) = events.as_mut().unwrap().wait_event_timeout(110) else {
            continue;
        };
        let hit = |x: i32, y: i32| buttons.iter().position(|b| b.area.contains_point((x, y)));
        let mut act = None;
        let pad = controllers.handle(&event);
        if matches!(
            event,
            Event::ControllerDeviceAdded { .. }
                | Event::ControllerDeviceRemoved { .. }
                | Event::ControllerDeviceRemapped { .. }
        ) {
            model.pad_name = controllers.name();
            buttons = model.paint(&mut canvas, &mut fonts)?;
            continue;
        }
        if let Some(pad) = pad {
            match pad {
                PadAction::Left => model.focus = move_focus(&buttons, model.focus, -1, 0),
                PadAction::Right => model.focus = move_focus(&buttons, model.focus, 1, 0),
                PadAction::Up => model.focus = move_focus(&buttons, model.focus, 0, -1),
                PadAction::Down => model.focus = move_focus(&buttons, model.focus, 0, 1),
                PadAction::Confirm => act = Some(buttons[model.focus].action.clone()),
                PadAction::Back if model.state.login_mode == "sms" => act = Some(Action::Login),
                PadAction::Back if model.state.stage == "login" => act = Some(Action::Cancel),
                PadAction::Back => {
                    model.focus = buttons
                        .iter()
                        .position(|b| b.action == Action::Quit)
                        .unwrap_or(0)
                }
                PadAction::Menu => continue,
            }
        } else {
            match event {
                Event::Quit { .. }
                | Event::Window {
                    win_event: WindowEvent::Close,
                    ..
                } => break,
                Event::Window {
                    win_event: WindowEvent::FocusLost,
                    ..
                } => model.pressed = None,
                Event::KeyDown {
                    keycode: Some(key),
                    repeat: false,
                    ..
                } => match key {
                    Keycode::ESCAPE => break,
                    Keycode::F11 => act = Some(Action::Fullscreen),
                    Keycode::RETURN | Keycode::SPACE => {
                        act = Some(buttons[model.focus].action.clone())
                    }
                    Keycode::TAB => model.focus = (model.focus + 1) % buttons.len(),
                    Keycode::UP => model.focus = move_focus(&buttons, model.focus, 0, -1),
                    Keycode::DOWN => model.focus = move_focus(&buttons, model.focus, 0, 1),
                    Keycode::LEFT => model.focus = move_focus(&buttons, model.focus, -1, 0),
                    Keycode::RIGHT => model.focus = move_focus(&buttons, model.focus, 1, 0),
                    _ => continue,
                },
                Event::MouseButtonDown {
                    mouse_btn: MouseButton::Left,
                    x,
                    y,
                    ..
                } => {
                    if let Some(i) = hit(x, y) {
                        model.focus = i;
                        model.pressed = Some(buttons[i].action.clone());
                    }
                }
                Event::MouseButtonUp {
                    mouse_btn: MouseButton::Left,
                    x,
                    y,
                    ..
                } => {
                    let previous = model.pressed.take();
                    if let Some(i) = hit(x, y) {
                        if previous.as_ref() == Some(&buttons[i].action) {
                            act = previous;
                        }
                    }
                }
                Event::Window {
                    win_event:
                        WindowEvent::Exposed | WindowEvent::SizeChanged(..) | WindowEvent::Shown,
                    ..
                } => (),
                _ => continue,
            }
        }
        if let Some(action) = act {
            if action == Action::ShowNotice {
                let result = Command::new("kdialog")
                    .args([
                        "--title",
                        "YOUYOU 远程 · 状态详情",
                        "--msgbox",
                        model.note(),
                    ])
                    .status();
                if let Err(e) = result {
                    model.notice = format!("无法打开详情窗口（{e}）。{}", model.note());
                }
                buttons = model.paint(&mut canvas, &mut fonts)?;
                continue;
            }
            model.notice.clear();
            match action {
                Action::Quit => break,
                Action::Fullscreen => fullscreen::toggle(&mut canvas)?,
                Action::Import | Action::Download => {
                    let selected = if action == Action::Import {
                        match pick_apk() {
                            Ok(Some(path)) => Some(path),
                            Ok(None) => continue,
                            Err(e) => {
                                model.notice = e;
                                buttons = model.paint(&mut canvas, &mut fonts)?;
                                continue;
                            }
                        }
                    } else {
                        None
                    };
                    model.fetching = true;
                    model.notice = if selected.is_some() {
                        "正在校验 APK 并提取…"
                    } else {
                        "正在从网易官网下载并校验 TV 客户端…"
                    }
                    .into();
                    let (tx, rx) = mpsc::channel();
                    std::thread::spawn(move || {
                        let result = match selected {
                            Some(p) => youyou_account::import_apk(&p),
                            None => youyou_account::download_official(),
                        };
                        let _ = tx.send(result.map_err(|e| e.to_string()));
                    });
                    download = Some(rx);
                }
                action => {
                    if let Some(c) = &core {
                        match action {
                            Action::Login => {
                                model.code.clear();
                                c.command("login", json!({}))?;
                            }
                            Action::Sms => c.command("sms-open", json!({}))?,
                            Action::Country | Action::Phone | Action::Code => {
                                let (prompt, secret, max, field) = match action {
                                    Action::Country => {
                                        ("国家或地区区号（不含 +）", false, 4, &mut model.country)
                                    }
                                    Action::Phone => {
                                        ("接收 UU 验证码的手机号", false, 15, &mut model.mobile)
                                    }
                                    _ => ("短信验证码", true, 6, &mut model.code),
                                };
                                match read_digits(prompt, secret, max) {
                                    Ok(Some(v)) => *field = v,
                                    Ok(None) => (),
                                    Err(e) => model.notice = e,
                                }
                            }
                            Action::SendCode | Action::Verify => {
                                c.command(if action==Action::SendCode{"sms-code"}else{"sms-login"},json!({"countryCode":model.country,"mobile":model.mobile,"code":model.code}))?;
                            }
                            Action::Cancel => c.command("cancel", json!({}))?,
                            _ => (),
                        }
                    }
                }
            }
        }
        buttons = model.paint(&mut canvas, &mut fonts)?;
    }
    Ok(())
}
