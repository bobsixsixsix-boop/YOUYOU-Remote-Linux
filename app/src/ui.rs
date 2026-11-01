//! Flat 2D drawing for the lobby and the stream menu, in the player's GLES
//! context. Everything is one tinted quad: rounded rectangles and outlines from
//! a distance field, and text (ab_glyph with the system CJK font), the
//! project's SVG icons (resvg) and the login QR code as alpha masks.
//! Immediate mode: pages are redrawn from state on every change, widgets
//! report clicks from the input fed in since the last frame. Layout is in dp
//! (SDL window units); `scale` turns dp into drawable pixels.
use ab_glyph::{point, Font, FontVec, Glyph, PxScale, ScaleFont};
use glow::HasContext;
use sdl2::controller::{Axis, Button};
use sdl2::event::Event;
use sdl2::keyboard::Scancode;
use sdl2::mouse::MouseButton;
use std::collections::HashMap;
use std::ffi::c_void;
use std::hash::{DefaultHasher, Hash, Hasher};
use std::time::Instant;

pub type Rgba = [f32; 4];
pub const fn rgb(hex: u32) -> Rgba {
    [((hex >> 16) & 255) as f32 / 255.0, ((hex >> 8) & 255) as f32 / 255.0, (hex & 255) as f32 / 255.0, 1.0]
}
pub const fn alpha(c: Rgba, a: f32) -> Rgba {
    [c[0], c[1], c[2], a]
}
pub const BG: Rgba = rgb(0xF3F5F8);
pub const SURFACE: Rgba = rgb(0xFFFFFF);
pub const PRESSED: Rgba = rgb(0xE9EDF2);
pub const LINE: Rgba = rgb(0xDDE3EA);
pub const INK: Rgba = rgb(0x111B2B);
pub const MUTED: Rgba = rgb(0x5D6B7E);
pub const ACCENT: Rgba = rgb(0x0B66DD);
pub const ACCENT_DOWN: Rgba = rgb(0x0954B8);
pub const PALE: Rgba = rgb(0xE7F0FC);
pub const GREEN: Rgba = rgb(0x16A05A);
pub const RED: Rgba = rgb(0xCC3A2F);
pub const RED_PALE: Rgba = rgb(0xFBECEA);
pub const BLACK: Rgba = rgb(0x000000);
pub const WHITE: Rgba = rgb(0xFFFFFF);
/// Corner radius of buttons, fields and cards: small on purpose.
pub const RADIUS: f32 = 6.0;
/// The Y700 panel: 3040x1904 on 8.8 inches. The compositor reports it as 168x268 mm
/// (a 12.4 inch screen), so the density cannot be read at run time.
const PANEL_PPI: f32 = 408.0;

const SVG: &[(&str, &[u8])] = &[
    ("account", include_bytes!("../../assets/icons/account.svg")),
    ("back", include_bytes!("../../assets/icons/back.svg")),
    ("backspace", include_bytes!("../../assets/icons/backspace.svg")),
    ("device", include_bytes!("../../assets/icons/device.svg")),
    ("exit", include_bytes!("../../assets/icons/exit.svg")),
    ("refresh", include_bytes!("../../assets/icons/refresh.svg")),
    ("settings", include_bytes!("../../assets/icons/settings.svg")),
];
const MARK: &[u8] = include_bytes!("../../assets/youyou-mark-192.rgba");
const MARK_SIDE: u32 = 192;

const VERTEX: &str = "#version 300 es
uniform vec4 rect; // drawable pixels, y down
uniform vec2 screen;
out vec2 local;
out vec2 uv;
void main() {
    uv = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    local = uv * rect.zw;
    vec2 p = (rect.xy + local) / screen * 2.0 - 1.0;
    gl_Position = vec4(p.x, -p.y, 0.0, 1.0);
}";
const FRAGMENT: &str = "#version 300 es
precision highp float;
uniform vec4 rect;
uniform vec4 color;
uniform float radius;
uniform float stroke; // 0 fills
uniform int mode;     // 0 solid, 1 alpha mask, 2 RGBA image
uniform sampler2D image;
in vec2 local;
in vec2 uv;
out vec4 frag;
void main() {
    vec2 h = rect.zw * 0.5;
    vec2 q = abs(local - h) - h + radius;
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    float a = clamp(0.5 - d, 0.0, 1.0);
    if (stroke > 0.0) a *= clamp(0.5 + d + stroke, 0.0, 1.0);
    vec4 c = color;
    if (mode == 1) c.a *= texture(image, uv).r;
    if (mode == 2) c = vec4(texture(image, uv).rgb, texture(image, uv).a * color.a);
    frag = vec4(c.rgb, c.a * a);
}";

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

impl Rect {
    pub const fn new(x: f32, y: f32, w: f32, h: f32) -> Self {
        Self { x, y, w, h }
    }
    pub fn contains(&self, (x, y): (f32, f32)) -> bool {
        x >= self.x && x < self.x + self.w && y >= self.y && y < self.y + self.h
    }
    pub fn grow(&self, d: f32) -> Self {
        Self::new(self.x - d, self.y - d, self.w + 2.0 * d, self.h + 2.0 * d)
    }
    pub fn center(&self) -> (f32, f32) {
        (self.x + self.w / 2.0, self.y + self.h / 2.0)
    }
    fn scaled(&self, s: f32) -> Self {
        let (cx, cy) = self.center();
        Self::new(cx - self.w * s / 2.0, cy - self.h * s / 2.0, self.w * s, self.h * s)
    }
}

#[derive(Clone, Copy, PartialEq)]
pub enum Kind {
    Primary,
    Plain,
    Danger,
    Text,
}

#[derive(Default)]
pub struct Hit {
    pub clicked: bool,
    pub pressed: bool,
    pub focused: bool,
}

struct Tex {
    id: glow::Texture,
    w: u32,
    h: u32,
    ascent: f32,
    used: u64,
}

struct Tween {
    from: f32,
    to: f32,
    start: Instant,
    ms: f32,
}

struct Uniforms {
    rect: Option<glow::UniformLocation>,
    screen: Option<glow::UniformLocation>,
    color: Option<glow::UniformLocation>,
    radius: Option<glow::UniformLocation>,
    stroke: Option<glow::UniformLocation>,
    mode: Option<glow::UniformLocation>,
}

pub struct Ui {
    gl: glow::Context,
    program: glow::Program,
    vertices: glow::VertexArray,
    at: Uniforms,
    fonts: [FontVec; 2],
    texts: HashMap<(String, u32, bool), Tex>,
    icons: HashMap<(&'static str, u32), Tex>,
    qr: Option<(String, Tex)>,
    mark: Option<Tex>,
    tweens: HashMap<u64, Tween>,
    frame: u64,
    /// Window size in dp.
    pub size: (f32, f32),
    /// Drawable pixels per dp.
    scale: f32,
    /// dp per SDL window unit, for mouse positions.
    unit: f32,
    // Input since the last frame. One pointer: the mouse or the first finger down.
    targets: Vec<(u64, Rect)>,
    focus: Option<u64>,
    keyboard: bool,
    finger: Option<i64>,
    down: bool,
    press: Option<(f32, f32)>,
    pos: (f32, f32),
    release: Option<(f32, f32)>,
    activate: bool,
    stick: (i16, i16),
    /// Esc or the pad's B this frame.
    pub back: bool,
    /// Digits typed this frame; '\x08' is backspace.
    pub typed: Vec<char>,
    /// Something changed since the last frame: draw again.
    pub dirty: bool,
    /// A tween is still running: draw again soon.
    pub animating: bool,
}

fn key(text: &str) -> u64 {
    let mut h = DefaultHasher::new();
    text.hash(&mut h);
    h.finish()
}

/// The fontconfig match for `pattern`, e.g. the CJK sans of the system.
fn system_font(pattern: &str) -> Result<FontVec, String> {
    let out = std::process::Command::new("fc-match").args(["-f", "%{file}\n%{index}", pattern]).output();
    let out = out.map_err(|e| format!("fc-match: {e}"))?;
    let text = String::from_utf8_lossy(&out.stdout);
    let (file, index) = text.split_once('\n').ok_or("fc-match 没有找到字体")?;
    let data = std::fs::read(file).map_err(|e| format!("{file}: {e}"))?;
    FontVec::try_from_vec_and_index(data, index.trim().parse().unwrap_or(0)).map_err(|e| format!("{file}: {e}"))
}

/// Glyphs on one line with the baseline at the font's ascent, and the width.
fn layout(font: &FontVec, text: &str, px: f32) -> (Vec<Glyph>, f32) {
    let f = font.as_scaled(PxScale::from(px));
    let (mut x, mut last, mut glyphs) = (0.0, None, Vec::new());
    for c in text.chars() {
        let id = f.glyph_id(c);
        if let Some(prev) = last {
            x += f.kern(prev, id);
        }
        glyphs.push(id.with_scale_and_position(px, point(x, f.ascent())));
        x += f.h_advance(id);
        last = Some(id);
    }
    (glyphs, x)
}

fn rasterize(font: &FontVec, text: &str, px: f32) -> (Vec<u8>, u32, u32) {
    let (glyphs, width) = layout(font, text, px);
    let f = font.as_scaled(PxScale::from(px));
    let (w, h) = (width.ceil() as u32 + 2, (f.ascent() - f.descent()).ceil() as u32 + 1);
    let mut pixels = vec![0u8; (w * h) as usize];
    for g in glyphs {
        let Some(o) = font.outline_glyph(g) else { continue };
        let b = o.px_bounds();
        o.draw(|gx, gy, c| {
            let (x, y) = (b.min.x as i32 + gx as i32, b.min.y as i32 + gy as i32);
            if x >= 0 && y >= 0 && (x as u32) < w && (y as u32) < h {
                let p = &mut pixels[(y as u32 * w + x as u32) as usize];
                *p = (*p).max((c * 255.0).round() as u8);
            }
        });
    }
    (pixels, w, h)
}

fn rasterize_svg(data: &[u8], px: u32) -> Option<Vec<u8>> {
    let tree = resvg::usvg::Tree::from_data(data, &resvg::usvg::Options::default()).ok()?;
    let mut pixmap = resvg::tiny_skia::Pixmap::new(px, px)?;
    let s = px as f32 / tree.size().width();
    resvg::render(&tree, resvg::tiny_skia::Transform::from_scale(s, s), &mut pixmap.as_mut());
    Some(pixmap.data().chunks_exact(4).map(|p| p[3]).collect())
}

impl Ui {
    /// Needs the window's GLES context to be current.
    pub fn new(video: &sdl2::VideoSubsystem) -> Result<Self, String> {
        let fonts = [system_font("sans-serif:lang=zh-cn")?, system_font("sans-serif:lang=zh-cn:weight=medium")?];
        unsafe {
            let gl = glow::Context::from_loader_function(|name| video.gl_get_proc_address(name) as *const c_void);
            let program = gl.create_program()?;
            for (kind, source) in [(glow::VERTEX_SHADER, VERTEX), (glow::FRAGMENT_SHADER, FRAGMENT)] {
                let shader = gl.create_shader(kind)?;
                gl.shader_source(shader, source);
                gl.compile_shader(shader);
                if !gl.get_shader_compile_status(shader) {
                    return Err(format!("界面着色器编译失败: {}", gl.get_shader_info_log(shader)));
                }
                gl.attach_shader(program, shader);
                gl.delete_shader(shader);
            }
            gl.link_program(program);
            if !gl.get_program_link_status(program) {
                return Err(format!("界面着色器链接失败: {}", gl.get_program_info_log(program)));
            }
            let at = |name| gl.get_uniform_location(program, name);
            let at = Uniforms {
                rect: at("rect"),
                screen: at("screen"),
                color: at("color"),
                radius: at("radius"),
                stroke: at("stroke"),
                mode: at("mode"),
            };
            Ok(Self {
                vertices: gl.create_vertex_array()?,
                gl,
                program,
                at,
                fonts,
                texts: HashMap::new(),
                icons: HashMap::new(),
                qr: None,
                mark: None,
                tweens: HashMap::new(),
                frame: 0,
                size: (1.0, 1.0),
                scale: 1.0,
                unit: 1.0,
                targets: Vec::new(),
                focus: None,
                keyboard: false,
                finger: None,
                down: false,
                press: None,
                pos: (0.0, 0.0),
                release: None,
                activate: false,
                stick: (0, 0),
                back: false,
                typed: Vec::new(),
                dirty: true,
                animating: false,
            })
        }
    }

    fn upload(&self, data: &[u8], w: u32, h: u32, rgba: bool, nearest: bool) -> Tex {
        let gl = &self.gl;
        unsafe {
            let id = gl.create_texture().expect("GL texture");
            gl.active_texture(glow::TEXTURE0);
            gl.bind_texture(glow::TEXTURE_2D, Some(id));
            gl.pixel_store_i32(glow::UNPACK_ALIGNMENT, 1);
            let (inner, format) = if rgba { (glow::RGBA8, glow::RGBA) } else { (glow::R8, glow::RED) };
            gl.tex_image_2d(glow::TEXTURE_2D, 0, inner as i32, w as i32, h as i32, 0, format, glow::UNSIGNED_BYTE,
                glow::PixelUnpackData::Slice(Some(data)));
            let filter = if nearest { glow::NEAREST } else { glow::LINEAR } as i32;
            gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MIN_FILTER, filter);
            gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MAG_FILTER, filter);
            gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_S, glow::CLAMP_TO_EDGE as i32);
            gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_T, glow::CLAMP_TO_EDGE as i32);
            Tex { id, w, h, ascent: 0.0, used: self.frame }
        }
    }

    /// One quad in drawable pixels.
    fn quad(&self, r: [f32; 4], color: Rgba, radius: f32, stroke: f32, mode: i32, tex: Option<glow::Texture>) {
        let gl = &self.gl;
        unsafe {
            gl.uniform_4_f32(self.at.rect.as_ref(), r[0], r[1], r[2], r[3]);
            gl.uniform_4_f32(self.at.color.as_ref(), color[0], color[1], color[2], color[3]);
            gl.uniform_1_f32(self.at.radius.as_ref(), radius.min(r[2].min(r[3]) / 2.0));
            gl.uniform_1_f32(self.at.stroke.as_ref(), stroke);
            gl.uniform_1_i32(self.at.mode.as_ref(), mode);
            if tex.is_some() {
                gl.active_texture(glow::TEXTURE0);
                gl.bind_texture(glow::TEXTURE_2D, tex);
            }
            gl.draw_arrays(glow::TRIANGLE_STRIP, 0, 4);
        }
    }

    fn px(&self, r: Rect) -> [f32; 4] {
        let s = self.scale;
        [(r.x * s).round(), (r.y * s).round(), (r.w * s).round(), (r.h * s).round()]
    }

    /// Starts a frame over the whole drawable; `window` is the SDL window size.
    pub fn begin(&mut self, window: (u32, u32), drawable: (u32, u32)) {
        // Android's dp, 1/160 inch: pages are as big on the glass as the official app's.
        // KDE's own 1.9 scale left 16 dp text 1.9 mm tall on this panel (owner: unreadable).
        let compositor = drawable.0 as f32 / window.0.max(1) as f32;
        self.scale = (PANEL_PPI / 160.0).max(compositor);
        self.unit = compositor / self.scale;
        self.size = (drawable.0 as f32 / self.scale, drawable.1 as f32 / self.scale);
        self.frame += 1;
        self.dirty = false;
        self.animating = false;
        self.targets.clear();
        let gl = &self.gl;
        unsafe {
            gl.viewport(0, 0, drawable.0 as i32, drawable.1 as i32);
            gl.enable(glow::BLEND);
            gl.blend_func_separate(glow::SRC_ALPHA, glow::ONE_MINUS_SRC_ALPHA, glow::ONE, glow::ONE_MINUS_SRC_ALPHA);
            gl.use_program(Some(self.program));
            gl.bind_vertex_array(Some(self.vertices));
            gl.uniform_2_f32(self.at.screen.as_ref(), drawable.0 as f32, drawable.1 as f32);
        }
    }

    pub fn end(&mut self) {
        if self.keyboard && !self.targets.iter().any(|t| Some(t.0) == self.focus) {
            self.focus = self.targets.first().map(|t| t.0);
            self.dirty |= self.focus.is_some();
        }
        self.release = None;
        self.activate = false;
        self.back = false;
        self.typed.clear();
        if !self.down {
            self.press = None;
        }
        // Text that was not drawn this frame or the last goes (stats change every frame).
        let frame = self.frame;
        let gl = &self.gl;
        self.texts.retain(|_, t| {
            let keep = t.used + 1 >= frame;
            if !keep {
                unsafe { gl.delete_texture(t.id) };
            }
            keep
        });
        unsafe { gl.disable(glow::BLEND) };
    }

    pub fn clear(&self, color: Rgba) {
        unsafe {
            self.gl.clear_color(color[0], color[1], color[2], 1.0);
            self.gl.clear(glow::COLOR_BUFFER_BIT);
        }
    }

    pub fn rect(&self, r: Rect, radius: f32, color: Rgba) {
        self.quad(self.px(r), color, radius * self.scale, 0.0, 0, None);
    }

    pub fn outline(&self, r: Rect, radius: f32, width: f32, color: Rgba) {
        self.quad(self.px(r), color, radius * self.scale, (width * self.scale).max(1.0), 0, None);
    }

    /// Eases towards `target` (quint-out, close to the cubic-bezier(0.23, 1, 0.32, 1)
    /// ease-out) whenever the target changes.
    pub fn tween(&mut self, name: &str, target: f32, ms: f32) -> f32 {
        let now = Instant::now();
        let value = |t: &Tween| {
            let k = (now - t.start).as_secs_f32() * 1000.0 / t.ms;
            if k >= 1.0 { t.to } else { t.from + (t.to - t.from) * (1.0 - (1.0 - k).powi(5)) }
        };
        let t = self.tweens.entry(key(name)).or_insert(Tween { from: target, to: target, start: now, ms });
        if t.to != target {
            *t = Tween { from: value(t), to: target, start: now, ms };
        }
        let v = value(t);
        self.animating |= v != t.to;
        v
    }

    pub fn measure(&self, text: &str, size: f32, bold: bool) -> f32 {
        layout(&self.fonts[bold as usize], text, (size * self.scale).round()).1 / self.scale
    }

    /// `text` cut to `width` with an ellipsis.
    pub fn clip(&self, text: &str, width: f32, size: f32, bold: bool) -> String {
        if self.measure(text, size, bold) <= width {
            return text.into();
        }
        let mut t: String = text.into();
        while !t.is_empty() && self.measure(&format!("{t}…"), size, bold) > width {
            t.pop();
        }
        t + "…"
    }

    /// One line, vertically centred on `cy` (on the ideographic em box, so CJK and
    /// digits sit in the middle); `align` 0 puts its left edge at `x`, 0.5 its
    /// centre, 1 its right edge. Returns the width.
    pub fn text(&mut self, text: &str, x: f32, cy: f32, size: f32, bold: bool, color: Rgba, align: f32) -> f32 {
        if text.is_empty() {
            return 0.0;
        }
        let px = (size * self.scale).round() as u32;
        let k = (text.to_string(), px, bold);
        if !self.texts.contains_key(&k) {
            let font = &self.fonts[bold as usize];
            let (data, w, h) = rasterize(font, text, px as f32);
            let mut t = self.upload(&data, w, h, false, false);
            t.ascent = font.as_scaled(PxScale::from(px as f32)).ascent();
            self.texts.insert(k.clone(), t);
        }
        let t = self.texts.get_mut(&k).expect("cached");
        t.used = self.frame;
        let (id, w, h, ascent) = (t.id, t.w as f32, t.h as f32, t.ascent);
        let left = (x * self.scale - (w - 2.0) * align).round();
        let top = (cy * self.scale + 0.38 * px as f32 - ascent).round();
        self.quad([left, top, w, h], color, 0.0, 0.0, 1, Some(id));
        (w - 2.0) / self.scale
    }

    /// Wrapped text from the top at `y` within `width` (breaks anywhere, as CJK
    /// allows); returns the height used.
    pub fn paragraph(&mut self, text: &str, x: f32, y: f32, width: f32, size: f32, color: Rgba, align: f32) -> f32 {
        let mut lines = Vec::new();
        for part in text.split('\n') {
            let mut line = String::new();
            for c in part.chars() {
                line.push(c);
                // Closing punctuation hangs past the edge rather than starting a line.
                if line.chars().count() > 1 && !"，。、；：！？）」』”".contains(c) && self.measure(&line, size, false) > width {
                    line.pop();
                    lines.push(std::mem::replace(&mut line, c.to_string()));
                }
            }
            lines.push(line);
        }
        let height = size * 1.6;
        for (i, line) in lines.iter().enumerate() {
            self.text(line, x + width * align, y + height * (i as f32 + 0.5), size, false, color, align);
        }
        height * lines.len() as f32
    }

    pub fn icon(&mut self, name: &'static str, cx: f32, cy: f32, size: f32, color: Rgba) {
        let px = (size * self.scale).round() as u32;
        if !self.icons.contains_key(&(name, px)) {
            let data = SVG.iter().find(|i| i.0 == name).and_then(|i| rasterize_svg(i.1, px));
            let Some(data) = data else { return };
            let t = self.upload(&data, px, px, false, false);
            self.icons.insert((name, px), t);
        }
        let id = self.icons[&(name, px)].id;
        let half = px as f32 / 2.0;
        self.quad([(cx * self.scale - half).round(), (cy * self.scale - half).round(), px as f32, px as f32], color, 0.0, 0.0, 1,
            Some(id));
    }

    /// The project mark, `size` dp square.
    pub fn mark(&mut self, x: f32, y: f32, size: f32) {
        if self.mark.is_none() {
            self.mark = Some(self.upload(MARK, MARK_SIDE, MARK_SIDE, true, false));
        }
        let id = self.mark.as_ref().map(|t| t.id);
        self.quad(self.px(Rect::new(x, y, size, size)), WHITE, 0.0, 0.0, 2, id);
    }

    /// `text` as a QR code, whole pixels per module, centred in `r`.
    pub fn qr(&mut self, text: &str, r: Rect) {
        if self.qr.as_ref().is_none_or(|q| q.0 != text) {
            let Ok(code) = qrcode::QrCode::new(text.as_bytes()) else { return };
            let n = code.width() as u32;
            let data: Vec<u8> = code.to_colors().into_iter().map(|c| if c == qrcode::Color::Dark { 255 } else { 0 }).collect();
            if let Some((_, old)) = self.qr.take() {
                unsafe { self.gl.delete_texture(old.id) };
            }
            self.qr = Some((text.into(), self.upload(&data, n, n, false, true)));
        }
        let Some((_, t)) = &self.qr else { return };
        let side = ((r.w.min(r.h) * self.scale) / t.w as f32).floor() * t.w as f32;
        let (cx, cy) = r.center();
        let (x, y) = ((cx * self.scale - side / 2.0).round(), (cy * self.scale - side / 2.0).round());
        self.quad([x, y, side, side], INK, 0.0, 0.0, 1, Some(t.id));
    }

    // ---- input ----

    /// Pointer, key and pad events. Returns whether the UI used the event.
    pub fn feed(&mut self, e: &Event) -> bool {
        let (w, h) = self.size;
        match *e {
            // Mouse events synthesized from touch carry SDL_TOUCH_MOUSEID.
            Event::MouseButtonDown { which, mouse_btn: MouseButton::Left, x, y, .. } if which != u32::MAX => {
                self.pointer_down((x as f32 * self.unit, y as f32 * self.unit))
            }
            Event::MouseMotion { which, x, y, .. } if which != u32::MAX => self.pos = (x as f32 * self.unit, y as f32 * self.unit),
            Event::MouseButtonUp { which, mouse_btn: MouseButton::Left, x, y, .. } if which != u32::MAX => {
                self.pointer_up((x as f32 * self.unit, y as f32 * self.unit))
            }
            Event::FingerDown { finger_id, x, y, .. } if self.finger.is_none() && !self.down => {
                self.finger = Some(finger_id);
                self.pointer_down((x * w, y * h));
            }
            Event::FingerMotion { finger_id, x, y, .. } if self.finger == Some(finger_id) => self.pos = (x * w, y * h),
            Event::FingerUp { finger_id, x, y, .. } if self.finger == Some(finger_id) => {
                self.finger = None;
                self.pointer_up((x * w, y * h));
            }
            Event::KeyDown { scancode: Some(s), .. } => match s {
                Scancode::Up => self.nav(0.0, -1.0),
                Scancode::Down => self.nav(0.0, 1.0),
                Scancode::Left => self.nav(-1.0, 0.0),
                Scancode::Right => self.nav(1.0, 0.0),
                Scancode::Return | Scancode::KpEnter | Scancode::Space => self.press_focused(),
                Scancode::Escape | Scancode::AcBack => self.back = true,
                Scancode::Backspace => self.typed.push('\x08'),
                _ => match digit(s) {
                    Some(d) => self.typed.push(d),
                    None => return false,
                },
            },
            Event::ControllerButtonDown { button, .. } => match button {
                Button::DPadUp => self.nav(0.0, -1.0),
                Button::DPadDown => self.nav(0.0, 1.0),
                Button::DPadLeft => self.nav(-1.0, 0.0),
                Button::DPadRight => self.nav(1.0, 0.0),
                Button::A => self.press_focused(),
                Button::B => self.back = true,
                _ => return false,
            },
            // The left stick moves focus once per push past half way.
            Event::ControllerAxisMotion { axis: axis @ (Axis::LeftX | Axis::LeftY), value, .. } => {
                let old = if axis == Axis::LeftX { &mut self.stick.0 } else { &mut self.stick.1 };
                let (was, now) = ((*old / 16384).signum(), (value / 16384).signum());
                *old = value;
                if now == 0 || now == was {
                    return false;
                }
                let d = now as f32;
                if axis == Axis::LeftX { self.nav(d, 0.0) } else { self.nav(0.0, d) }
            }
            _ => return false,
        }
        self.dirty = true;
        true
    }

    fn pointer_down(&mut self, p: (f32, f32)) {
        (self.press, self.pos, self.down, self.keyboard) = (Some(p), p, true, false);
    }

    fn pointer_up(&mut self, p: (f32, f32)) {
        (self.pos, self.release, self.down) = (p, Some(p), false);
    }

    /// Moves focus to the nearest target in the direction (the first press only shows it).
    fn nav(&mut self, dx: f32, dy: f32) {
        let from = self.targets.iter().find(|t| Some(t.0) == self.focus).map(|t| t.1);
        let Some(from) = from.filter(|_| self.keyboard) else {
            self.keyboard = true;
            self.focus = self.focus.filter(|f| self.targets.iter().any(|t| t.0 == *f)).or(self.targets.first().map(|t| t.0));
            return;
        };
        let (ox, oy) = from.center();
        let best = self.targets.iter().filter_map(|&(id, r)| {
            let (x, y) = r.center();
            let (x, y) = (x - ox, y - oy);
            let forward = x * dx + y * dy;
            (forward > 1.0).then_some((forward + 3.0 * (x * dy - y * dx).abs(), id))
        });
        if let Some((_, id)) = best.min_by(|a, b| a.0.total_cmp(&b.0)) {
            self.focus = Some(id);
        }
    }

    fn press_focused(&mut self) {
        if self.keyboard && self.focus.is_some() {
            self.activate = true;
        } else {
            self.nav(0.0, 0.0);
        }
    }

    /// A click anywhere in `r` that is not a focus target (e.g. the picture around a drawer).
    pub fn tap(&self, r: Rect) -> bool {
        self.release.is_some_and(|p| r.contains(p)) && self.press.is_some_and(|p| r.contains(p))
    }

    /// Registers a focus target and reports what the input did to it.
    pub fn hit(&mut self, name: &str, r: Rect) -> Hit {
        let id = key(name);
        self.targets.push((id, r));
        let hit = Hit {
            clicked: self.tap(r) || (self.activate && self.focus == Some(id)),
            pressed: self.down && self.press.is_some_and(|p| r.contains(p)) && r.contains(self.pos),
            focused: self.keyboard && self.focus == Some(id),
        };
        self.dirty |= hit.clicked;
        hit
    }

    fn ring(&self, r: Rect) {
        self.outline(r.grow(4.0), RADIUS + 4.0, 2.0, ACCENT);
    }

    /// Label (and icon) centred; the type size follows the height.
    pub fn button(&mut self, name: &str, r: Rect, label: &str, icon: Option<&'static str>, kind: Kind, enabled: bool) -> bool {
        let hit = if enabled { self.hit(name, r) } else { Hit::default() };
        let b = r.scaled(self.tween(name, if hit.pressed { 0.97 } else { 1.0 }, 160.0));
        let (fill, ink) = match (kind, enabled, hit.pressed) {
            (_, false, _) => (PRESSED, MUTED),
            (Kind::Primary, _, p) => (if p { ACCENT_DOWN } else { ACCENT }, WHITE),
            (Kind::Danger, _, p) => (if p { PRESSED } else { RED_PALE }, RED),
            (Kind::Plain, _, p) => (if p { PRESSED } else { SURFACE }, INK),
            (Kind::Text, _, p) => (if p { PRESSED } else { alpha(SURFACE, 0.0) }, ACCENT),
        };
        self.rect(b, RADIUS, fill);
        if kind == Kind::Plain && enabled {
            self.outline(b, RADIUS, 1.0, LINE);
        }
        let size = (r.h * 0.34).clamp(16.0, 26.0);
        let (iw, gap) = (size * 1.25, if label.is_empty() { 0.0 } else { size * 0.45 });
        let width = self.measure(label, size, true) + icon.map_or(0.0, |_| iw + gap);
        let (mut x, cy) = (r.x + (r.w - width) / 2.0, r.y + r.h / 2.0);
        if let Some(i) = icon {
            self.icon(i, x + iw / 2.0, cy, iw, ink);
            x += iw + gap;
        }
        self.text(label, x, cy, size, true, ink, 0.0);
        if hit.focused {
            self.ring(r);
        }
        hit.clicked && enabled
    }

    /// One option of a choice; selected ones are tinted.
    pub fn chip(&mut self, name: &str, r: Rect, label: &str, selected: bool) -> bool {
        let hit = self.hit(name, r);
        let fill = if selected { PALE } else if hit.pressed { PRESSED } else { SURFACE };
        self.rect(r, RADIUS, fill);
        self.outline(r, RADIUS, if selected { 1.5 } else { 1.0 }, if selected { ACCENT } else { LINE });
        self.text(label, r.x + r.w / 2.0, r.y + r.h / 2.0, 16.0, true, if selected { ACCENT } else { INK }, 0.5);
        if hit.focused {
            self.ring(r);
        }
        hit.clicked
    }

    /// A row of chips wrapping within `width`; the one clicked and the height used.
    pub fn chips(&mut self, name: &str, x: f32, y: f32, width: f32, labels: &[String], selected: Option<usize>) -> (Option<usize>, f32) {
        let (mut cx, mut cy, mut picked) = (x, y, None);
        for (i, label) in labels.iter().enumerate() {
            let w = self.measure(label, 16.0, true) + 40.0;
            if cx > x && cx + w > x + width {
                (cx, cy) = (x, cy + 56.0);
            }
            if self.chip(&format!("{name}/{i}"), Rect::new(cx, cy, w, 46.0), label, selected == Some(i)) {
                picked = Some(i);
            }
            cx += w + 10.0;
        }
        (picked, cy + 46.0 - y)
    }

    /// A form value: label on the left, value (or placeholder) on the right.
    pub fn field(&mut self, name: &str, r: Rect, label: &str, value: &str, placeholder: &str, active: bool) -> bool {
        let hit = self.hit(name, r);
        self.rect(r, RADIUS, if hit.pressed { PRESSED } else { SURFACE });
        self.outline(r, RADIUS, if active { 2.0 } else { 1.0 }, if active { ACCENT } else { LINE });
        let cy = r.y + r.h / 2.0;
        self.text(label, r.x + 18.0, cy, 16.0, false, MUTED, 0.0);
        let right = r.x + r.w - 18.0;
        if value.is_empty() {
            self.text(placeholder, right - if active { 8.0 } else { 0.0 }, cy, 18.0, false, LINE_TEXT, 1.0);
        } else {
            self.text(value, right - if active { 8.0 } else { 0.0 }, cy, 20.0, true, INK, 1.0);
        }
        if active {
            self.rect(Rect::new(right - 3.0, cy - 13.0, 2.0, 26.0), 0.0, ACCENT);
        }
        if hit.focused {
            self.ring(r);
        }
        hit.clicked
    }

    /// 1–9, clear, 0, backspace in `r`: the key pressed ('C' clears, '\x08' deletes).
    pub fn keypad(&mut self, name: &str, r: Rect) -> Option<char> {
        const KEYS: [char; 12] = ['1', '2', '3', '4', '5', '6', '7', '8', '9', 'C', '0', '\x08'];
        let (gap, mut out) = (10.0, None);
        let (w, h) = ((r.w - 2.0 * gap) / 3.0, (r.h - 3.0 * gap) / 4.0);
        for (i, k) in KEYS.into_iter().enumerate() {
            let b = Rect::new(r.x + (i % 3) as f32 * (w + gap), r.y + (i / 3) as f32 * (h + gap), w, h);
            let label = match k {
                'C' => "清空".to_string(),
                '\x08' => String::new(),
                d => d.to_string(),
            };
            if self.button(&format!("{name}/{i}"), b, &label, (k == '\x08').then_some("backspace"), Kind::Plain, true) {
                out = Some(k);
            }
        }
        out
    }
}

fn digit(s: Scancode) -> Option<char> {
    use Scancode::*;
    let d = match s {
        Num0 | Kp0 => 0,
        Num1 | Kp1 => 1,
        Num2 | Kp2 => 2,
        Num3 | Kp3 => 3,
        Num4 | Kp4 => 4,
        Num5 | Kp5 => 5,
        Num6 | Kp6 => 6,
        Num7 | Kp7 => 7,
        Num8 | Kp8 => 8,
        Num9 | Kp9 => 9,
        _ => return None,
    };
    char::from_digit(d, 10)
}

/// Placeholder text: lighter than MUTED, still readable.
pub const LINE_TEXT: Rgba = rgb(0x9AA5B4);

/// Applies a keypad or keyboard key to a digit string of at most `max` digits.
pub fn edit(value: &mut String, key: char, max: usize) {
    match key {
        '\x08' => {
            value.pop();
        }
        'C' => value.clear(),
        d if d.is_ascii_digit() && value.len() < max => value.push(d),
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn edits_digits() {
        let mut v = String::new();
        for k in ['1', '3', 'x', '8', '\x08', '9'] {
            edit(&mut v, k, 3);
        }
        assert_eq!(v, "139");
        edit(&mut v, '7', 3);
        assert_eq!(v, "139");
        edit(&mut v, 'C', 3);
        assert!(v.is_empty());
    }
    #[test]
    fn rect_geometry() {
        let r = Rect::new(10.0, 20.0, 100.0, 40.0);
        assert!(r.contains((10.0, 20.0)) && !r.contains((110.0, 30.0)));
        assert_eq!(r.grow(4.0), Rect::new(6.0, 16.0, 108.0, 48.0));
        assert_eq!(r.scaled(0.5), Rect::new(35.0, 30.0, 50.0, 20.0));
    }
}
