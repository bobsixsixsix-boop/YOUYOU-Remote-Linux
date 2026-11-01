//! Zero-copy display: the two planes of each decoder NV12 DMA-BUF become R8
//! and GR88 EGLImages, and the shader converts YUV to RGB. Importing NV12 as
//! one image fails on freedreno (Adreno 840, Mesa 26): Mesa imports it per
//! plane as R8_G8B8_420 and the 960x540 chroma plane is refused (EGL_BAD_ALLOC).
//! Doing the matrix ourselves also keeps colours independent of driver hints.
use crate::bridge::RawFrame;
use glow::HasContext;
use sdl2::rect::Rect;
use std::collections::HashMap;
use std::ffi::c_void;
use std::ptr::null_mut;

type Image = *mut c_void;
type GetDisplay = unsafe extern "C" fn() -> *mut c_void;
type GetError = unsafe extern "C" fn() -> i32;
type CreateImage = unsafe extern "C" fn(*mut c_void, *mut c_void, u32, *mut c_void, *const i32) -> Image;
type DestroyImage = unsafe extern "C" fn(*mut c_void, Image) -> u32;
type TargetTexture = unsafe extern "C" fn(u32, Image);

const EGL_LINUX_DMA_BUF_EXT: u32 = 0x3270;
const DRM_FORMAT_R8: i32 = 0x2020_3852;
const DRM_FORMAT_GR88: i32 = 0x3838_5247;

const VERTEX: &str = "#version 300 es
out vec2 uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}";
// shortcut: BT.709 limited range, what UU hosts send; read the stream's VUI
// (V4L2 colorspace) if a host ever sends something else.
const FRAGMENT: &str = "#version 300 es
precision highp float;
uniform sampler2D luma;
uniform sampler2D chroma;
in vec2 uv;
out vec4 color;
void main() {
    float y = (texture(luma, uv).r - 16.0 / 255.0) * (255.0 / 219.0);
    vec2 c = (texture(chroma, uv).rg - 128.0 / 255.0) * (255.0 / 224.0);
    color = vec4(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x, 1.0);
}";

/// Largest rectangle with the picture's aspect ratio centred in `area`.
pub fn fit(picture: (u32, u32), area: (u32, u32)) -> Rect {
    let (pw, ph, aw, ah) = (picture.0 as u64, picture.1 as u64, area.0.max(1) as u64, area.1.max(1) as u64);
    let (w, h) = if pw == 0 || ph == 0 {
        (aw, ah)
    } else if aw * ph <= ah * pw {
        (aw, (aw * ph / pw).max(1))
    } else {
        ((ah * pw / ph).max(1), ah)
    };
    Rect::new(((aw - w) / 2) as i32, ((ah - h) / 2) as i32, w as u32, h as u32)
}

/// One decoder buffer imported as luma and chroma textures.
struct Planes {
    images: [Image; 2],
    textures: [glow::Texture; 2],
}

pub struct Renderer {
    pub gl: glow::Context,
    program: glow::Program,
    vertices: glow::VertexArray,
    display: *mut c_void,
    get_error: GetError,
    create: CreateImage,
    destroy: DestroyImage,
    target: TargetTexture,
    epoch: u64,
    // Decoder buffers are reused, so each one is imported once per decoder setup.
    imported: HashMap<u32, Planes>,
}

impl Renderer {
    /// Needs the window's GLES context to be current.
    pub fn new(video: &sdl2::VideoSubsystem) -> Result<Self, String> {
        let load = |name: &str| video.gl_get_proc_address(name) as *const c_void;
        let function = |name: &str| {
            let p = load(name);
            if p.is_null() { Err(format!("显卡驱动缺少 {name}")) } else { Ok(p) }
        };
        unsafe {
            let gl = glow::Context::from_loader_function(load);
            eprintln!("GL: {} | {}", gl.get_parameter_string(glow::RENDERER), gl.get_parameter_string(glow::VERSION));
            let program = gl.create_program()?;
            for (kind, source) in [(glow::VERTEX_SHADER, VERTEX), (glow::FRAGMENT_SHADER, FRAGMENT)] {
                let shader = gl.create_shader(kind)?;
                gl.shader_source(shader, source);
                gl.compile_shader(shader);
                if !gl.get_shader_compile_status(shader) {
                    return Err(format!("着色器编译失败: {}", gl.get_shader_info_log(shader)));
                }
                gl.attach_shader(program, shader);
                gl.delete_shader(shader);
            }
            gl.link_program(program);
            if !gl.get_program_link_status(program) {
                return Err(format!("着色器链接失败: {}", gl.get_program_info_log(program)));
            }
            gl.use_program(Some(program));
            gl.uniform_1_i32(gl.get_uniform_location(program, "chroma").as_ref(), 1);
            let get_display: GetDisplay = std::mem::transmute(function("eglGetCurrentDisplay")?);
            Ok(Self {
                vertices: gl.create_vertex_array()?,
                gl,
                program,
                display: get_display(),
                get_error: std::mem::transmute(function("eglGetError")?),
                create: std::mem::transmute(function("eglCreateImageKHR")?),
                destroy: std::mem::transmute(function("eglDestroyImageKHR")?),
                target: std::mem::transmute(function("glEGLImageTargetTexture2DOES")?),
                epoch: 0,
                imported: HashMap::new(),
            })
        }
    }

    fn import(&self, f: &RawFrame) -> Result<Planes, String> {
        let plane = |fourcc, width: u32, height: u32, offset: u32| {
            let a = [0x3057, width as i32, 0x3056, height as i32, 0x3271, fourcc, // EGL_WIDTH, EGL_HEIGHT, EGL_LINUX_DRM_FOURCC_EXT
                0x3272, f.fd, 0x3273, offset as i32, 0x3274, f.stride as i32, // plane 0 fd, offset, pitch
                0x3443, 0, 0x3444, 0, 0x3038]; // modifier DRM_FORMAT_MOD_LINEAR (lo, hi); EGL_NONE
            let image = unsafe { (self.create)(self.display, null_mut(), EGL_LINUX_DMA_BUF_EXT, null_mut(), a.as_ptr()) };
            if image.is_null() {
                return Err(format!("EGL 导入解码缓冲失败 0x{:x}", unsafe { (self.get_error)() }));
            }
            Ok(image)
        };
        let luma = plane(DRM_FORMAT_R8, f.width, f.height, 0)?;
        let chroma = match plane(DRM_FORMAT_GR88, f.width.div_ceil(2), f.height.div_ceil(2), f.uv_offset) {
            Ok(image) => image,
            Err(e) => {
                unsafe { (self.destroy)(self.display, luma) };
                return Err(e);
            }
        };
        let gl = &self.gl;
        let mut textures = [None; 2];
        for (texture, image) in textures.iter_mut().zip([luma, chroma]) {
            unsafe {
                let t = gl.create_texture()?;
                gl.bind_texture(glow::TEXTURE_2D, Some(t));
                gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MIN_FILTER, glow::LINEAR as i32);
                gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MAG_FILTER, glow::LINEAR as i32);
                gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_S, glow::CLAMP_TO_EDGE as i32);
                gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_T, glow::CLAMP_TO_EDGE as i32);
                (self.target)(glow::TEXTURE_2D, image);
                *texture = Some(t);
            }
        }
        Ok(Planes { images: [luma, chroma], textures: textures.map(Option::unwrap) })
    }

    fn forget(&mut self) {
        for (_, p) in self.imported.drain() {
            unsafe {
                p.textures.into_iter().for_each(|t| self.gl.delete_texture(t));
                p.images.into_iter().for_each(|i| {
                    (self.destroy)(self.display, i);
                });
            }
        }
    }

    /// Black screen with the picture letterboxed in it; `view` is in drawable
    /// pixels from the top-left, as input mapping uses it.
    pub fn draw(&mut self, frame: Option<&RawFrame>, view: Rect, drawable: (u32, u32)) -> Result<(), String> {
        if let Some(f) = frame {
            if f.epoch != self.epoch {
                self.forget();
                self.epoch = f.epoch;
            }
            if !self.imported.contains_key(&f.index) {
                let planes = self.import(f)?;
                self.imported.insert(f.index, planes);
            }
        }
        let gl = &self.gl;
        unsafe {
            gl.viewport(0, 0, drawable.0 as i32, drawable.1 as i32);
            gl.clear_color(0.0, 0.0, 0.0, 1.0);
            gl.clear(glow::COLOR_BUFFER_BIT);
            let Some(planes) = frame.and_then(|f| self.imported.get(&f.index)) else { return Ok(()) };
            gl.viewport(view.x(), drawable.1 as i32 - view.bottom(), view.width() as i32, view.height() as i32);
            gl.use_program(Some(self.program));
            gl.bind_vertex_array(Some(self.vertices));
            for (unit, texture) in [glow::TEXTURE0, glow::TEXTURE1].into_iter().zip(planes.textures) {
                gl.active_texture(unit);
                gl.bind_texture(glow::TEXTURE_2D, Some(texture));
            }
            gl.draw_arrays(glow::TRIANGLE_STRIP, 0, 4);
        }
        Ok(())
    }

    /// RGB of the back buffer at the centres of seven vertical bars across
    /// `view` at 30% height: SMPTE bars check colour matrix and range.
    pub fn probe(&self, view: Rect, drawable_height: u32) -> Vec<[u8; 3]> {
        let y = drawable_height as i32 - (view.y() + view.height() as i32 * 3 / 10);
        (0..7)
            .map(|i| {
                let x = view.x() + view.width() as i32 * (2 * i + 1) / 14;
                let mut px = [0u8; 4];
                unsafe {
                    self.gl.read_pixels(x, y, 1, 1, glow::RGBA, glow::UNSIGNED_BYTE, glow::PixelPackData::Slice(Some(&mut px)));
                }
                [px[0], px[1], px[2]]
            })
            .collect()
    }
}

impl Drop for Renderer {
    fn drop(&mut self) {
        self.forget();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn letterboxes_both_ways() {
        assert_eq!(fit((1920, 1080), (2560, 1600)), Rect::new(0, 80, 2560, 1440));
        assert_eq!(fit((1600, 2560), (2560, 1600)), Rect::new(780, 0, 1000, 1600));
        assert_eq!(fit((2560, 1600), (2560, 1600)), Rect::new(0, 0, 2560, 1600));
        assert_eq!(fit((0, 0), (800, 600)), Rect::new(0, 0, 800, 600));
    }
}
