use sdl2::{
    pixels::{Color, PixelFormatEnum},
    rect::Rect,
    render::{BlendMode, ScaleMode, Texture, TextureCreator, WindowCanvas},
    video::WindowContext,
};
use std::{collections::HashMap, path::PathBuf};

pub fn asset_dir() -> PathBuf {
    std::env::var_os("YOUYOU_ASSET_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/usr/share/youyou-linux"))
}

/// Each SDL renderer owns its SVG raster textures. Remember failed loads too,
/// so an incomplete installation does not repeatedly hit the disk during play.
pub struct Icons<'a> {
    root: PathBuf,
    creator: &'a TextureCreator<WindowContext>,
    textures: HashMap<(&'static str, u32), Option<Texture<'a>>>,
}

impl<'a> Icons<'a> {
    pub fn new(creator: &'a TextureCreator<WindowContext>) -> Self {
        Self {
            root: asset_dir().join("ui-svg"),
            creator,
            textures: HashMap::new(),
        }
    }

    pub fn draw(
        &mut self,
        canvas: &mut WindowCanvas,
        name: &'static str,
        bounds: Rect,
        scale: f32,
        tint: Color,
    ) -> Result<bool, String> {
        let pixels = (bounds.width().max(bounds.height()) as f32 * scale).ceil() as u32;
        let sizes: &[u32] = if name == "device" {
            &[32, 64, 128, 384]
        } else {
            &[32, 64, 128]
        };
        let index = sizes
            .iter()
            .position(|n| *n >= pixels)
            .unwrap_or(sizes.len() - 1);
        for &side in sizes[index..].iter().chain(sizes[..index].iter().rev()) {
            let texture = self.textures.entry((name, side)).or_insert_with(|| {
                let rgba = std::fs::read(self.root.join(format!("{name}-{side}.rgba"))).ok()?;
                if rgba.len() != (side * side * 4) as usize {
                    return None;
                }
                let mut t = self
                    .creator
                    .create_texture_static(PixelFormatEnum::RGBA32, side, side)
                    .ok()?;
                t.update(None, &rgba, side as usize * 4).ok()?;
                t.set_blend_mode(BlendMode::Blend);
                t.set_scale_mode(ScaleMode::Linear);
                Some(t)
            });
            if let Some(t) = texture {
                t.set_color_mod(tint.r, tint.g, tint.b);
                canvas.copy(t, None, bounds)?;
                return Ok(true);
            }
        }
        Ok(false)
    }
}
