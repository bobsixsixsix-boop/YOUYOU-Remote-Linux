use sdl2::{
    pixels::Color,
    rect::Rect,
    render::{ScaleMode, WindowCanvas},
    ttf::{Font, Sdl2TtfContext},
};
use std::path::Path;

pub(super) struct Face<'a> {
    font: Font<'a, 'static>,
    scale: f32,
}

impl Face<'_> {
    fn logical(&self, px: u32) -> u32 {
        (px as f32 / self.scale).round() as u32
    }
    pub(super) fn width(&self, text: &str) -> Result<u32, String> {
        if text.is_empty() {
            return Ok(0);
        }
        self.font
            .size_of(text)
            .map(|(w, _)| self.logical(w))
            .map_err(|e| e.to_string())
    }
    fn fit(&self, text: &str, width: u32) -> Result<String, String> {
        let clean: String = text.chars().filter(|c| !c.is_control()).collect();
        if self.width(&clean)? <= width {
            return Ok(clean);
        }
        let mut output = String::new();
        let available = width.saturating_sub(self.width("…")?);
        for ch in clean.chars() {
            output.push(ch);
            if self.width(&output)? > available {
                output.pop();
                break;
            }
        }
        output.push('…');
        Ok(output)
    }
    /// Keep texture pixels close to drawable pixels. Destination and hit-test coordinates stay logical.
    pub(super) fn draw(
        &self,
        c: &mut WindowCanvas,
        text: &str,
        bounds: Rect,
        tint: Color,
        centered: bool,
    ) -> Result<(), String> {
        let text = self.fit(text, bounds.width())?;
        if text.trim().is_empty() {
            return Ok(());
        }
        let surface = self
            .font
            .render(&text)
            .blended(tint)
            .map_err(|e| e.to_string())?;
        let creator = c.texture_creator();
        let mut texture = creator
            .create_texture_from_surface(&surface)
            .map_err(|e| e.to_string())?;
        texture.set_scale_mode(ScaleMode::Linear);
        let (w, h) = (
            self.logical(surface.width()).max(1),
            self.logical(surface.height()).max(1),
        );
        let x = if centered {
            bounds.center().x - w as i32 / 2
        } else {
            bounds.x()
        };
        c.copy(
            &texture,
            None,
            Rect::new(x, bounds.center().y - h as i32 / 2, w, h),
        )
    }
    /// Preserve explicit line breaks. Prefer a space between words; CJK can wrap between glyphs.
    pub(super) fn lines(&self, text: &str, width: u32) -> Result<Vec<String>, String> {
        let mut lines = Vec::new();
        for paragraph in text.lines() {
            let mut line = String::new();
            for ch in paragraph.chars().filter(|c| !c.is_control()) {
                if line.is_empty() && ch.is_whitespace() {
                    continue;
                }
                line.push(ch);
                if line.len() > ch.len_utf8() && self.width(&line)? > width {
                    line.pop();
                    if ch.is_ascii_alphanumeric() {
                        if let Some(space) = line.rfind(char::is_whitespace) {
                            let rest = line.split_off(space);
                            lines.push(line);
                            line = rest.trim_start().into();
                        } else {
                            lines.push(std::mem::take(&mut line));
                        }
                    } else {
                        lines.push(std::mem::take(&mut line));
                    }
                    if !ch.is_whitespace() {
                        line.push(ch);
                    }
                }
            }
            lines.push(line);
        }
        Ok(lines)
    }
}

pub(super) struct Fonts<'a> {
    pub(super) title: Face<'a>,
    pub(super) body: Face<'a>,
    pub(super) small: Face<'a>,
    sizes: [u16; 3],
    ctx: &'a Sdl2TtfContext,
    path: std::path::PathBuf,
}
impl<'a> Fonts<'a> {
    fn sizes(scale: f32) -> [u16; 3] {
        [34.0, 25.0, 21.0].map(|s| (s * scale).round().clamp(8., 256.) as u16)
    }
    pub(super) fn load(ctx: &'a Sdl2TtfContext, path: &Path, scale: f32) -> Result<Self, String> {
        let sizes = Self::sizes(scale);
        let bold = path.with_file_name("NotoSansSC-Bold.otf");
        let face = |path: &Path, size: u16, nominal: f32| -> Result<Face<'a>, String> {
            Ok(Face {
                font: ctx.load_font(path, size)?,
                scale: f32::from(size) / nominal,
            })
        };
        Ok(Self {
            title: face(if bold.is_file() { &bold } else { path }, sizes[0], 34.)?,
            body: face(path, sizes[1], 25.)?,
            small: face(path, sizes[2], 21.)?,
            sizes,
            ctx,
            path: path.to_path_buf(),
        })
    }
    pub(super) fn refresh(&mut self, scale: f32) -> Result<(), String> {
        if self.sizes != Self::sizes(scale) {
            *self = Self::load(self.ctx, &self.path, scale)?;
        }
        Ok(())
    }
}
