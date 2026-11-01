//! Opaque VAAPI frames and the optional renderer-thread native presenter.
use sdl2::{rect::Rect, render::WindowCanvas, sys::SDL_Renderer};
use std::{
    ffi::{c_char, c_void, CStr},
    marker::PhantomData,
    ptr::NonNull,
    rc::Rc,
};

/// A hardware frame borrowed from a session. This is never pixel memory.
/// The session cannot be reused while its opaque frame is still borrowed.
///
/// ```compile_fail
/// use std::{ffi::c_void, ptr::NonNull};
/// use youyou_player::BorrowedAvFrame;
/// let mut owner = ();
/// let frame = unsafe {
///     BorrowedAvFrame::from_raw(NonNull::<c_void>::dangling().as_ptr(), &mut owner)
/// };
/// let second_borrow = &mut owner;
/// drop(frame);
/// ```
pub struct BorrowedAvFrame<'a> {
    ptr: NonNull<c_void>,
    _borrow: PhantomData<&'a c_void>,
    _thread: PhantomData<Rc<()>>,
}

impl<'a> BorrowedAvFrame<'a> {
    /// # Safety
    /// `ptr` must be a valid VAAPI AVFrame retained by `owner` for this entire
    /// borrow. It may only be consumed by the renderer thread's native presenter.
    pub unsafe fn from_raw<T>(ptr: *const c_void, _owner: &'a mut T) -> Option<Self> {
        NonNull::new(ptr.cast_mut()).map(|ptr| Self {
            ptr,
            _borrow: PhantomData,
            _thread: PhantomData,
        })
    }
}

/// Experimental rendering is opt-in; an explicit software decoder wins.
pub fn gpu_video_requested() -> bool {
    std::env::var("YOUYOU_GPU_VIDEO").as_deref() == Ok("1")
        && std::env::var("UU_DECODER").as_deref() != Ok("software")
}

extern "C" {
    fn yy_gpu_open(core: *mut c_void, renderer: *mut SDL_Renderer) -> *mut c_void;
    fn yy_gpu_close(presenter: *mut c_void);
    fn yy_gpu_upload(presenter: *mut c_void, avframe: *const c_void) -> i32;
    fn yy_gpu_draw(presenter: *mut c_void, x: i32, y: i32, width: i32, height: i32) -> i32;
    fn yy_gpu_presented(presenter: *mut c_void) -> i32;
    fn yy_gpu_reset(presenter: *mut c_void) -> i32;
    fn yy_error() -> *const c_char;
}

fn native_error() -> Option<String> {
    let message = unsafe { yy_error() };
    if message.is_null() {
        return None;
    }
    let message = unsafe { CStr::from_ptr(message) }.to_string_lossy();
    (!message.is_empty()).then(|| message.into_owned())
}

fn note_fallback(stage: &str) {
    if let Some(message) = native_error() {
        eprintln!("YOUYOU_GPU_VIDEO fallback stage={stage} reason={message}");
    }
}

fn check(result: i32) -> Result<i32, String> {
    if result >= 0 {
        return Ok(result);
    }
    Err(native_error().unwrap_or_else(|| "GPU 视频显示调用失败".into()))
}

#[derive(Debug, PartialEq, Eq)]
pub enum Upload {
    Imported,
    Busy,
    Fallback,
}

/// Owns renderer-local resources. It cannot cross threads and must be dropped
/// before either its renderer or native session is destroyed.
///
/// ```compile_fail
/// fn require_send_sync<T: Send + Sync>() {}
/// require_send_sync::<youyou_player::GpuVideo>();
/// ```
pub struct GpuVideo {
    ptr: NonNull<c_void>,
    active: bool,
    _thread: PhantomData<Rc<()>>,
}

impl GpuVideo {
    /// # Safety
    /// `core` must be this renderer thread's live native session. The caller must
    /// keep both `core` and `canvas` alive until the returned presenter is dropped.
    /// All calls, including Drop, must run on this same renderer thread.
    pub unsafe fn open(core: *mut c_void, canvas: &WindowCanvas) -> Option<Self> {
        let ptr = NonNull::new(yy_gpu_open(core, canvas.raw()));
        if ptr.is_none() {
            note_fallback("open");
        }
        ptr.map(|ptr| Self {
            ptr,
            active: true,
            _thread: PhantomData,
        })
    }

    pub fn upload(&mut self, frame: &BorrowedAvFrame<'_>) -> Result<Upload, String> {
        if !self.active {
            return Ok(Upload::Fallback);
        }
        match check(unsafe { yy_gpu_upload(self.ptr.as_ptr(), frame.ptr.as_ptr()) })? {
            1 => Ok(Upload::Imported),
            2 => Ok(Upload::Busy),
            0 => {
                note_fallback("upload");
                self.active = false;
                Ok(Upload::Fallback)
            }
            _ => Err("未知的 GPU 视频上传状态".into()),
        }
    }

    pub fn draw(&mut self, view: Rect) -> Result<bool, String> {
        if !self.active {
            return Ok(false);
        }
        // Zero can also mean that reset removed the last image. Do not treat
        // that as a permanently failed presenter; upload/presented explicitly
        // report whether native hardware output has been disabled.
        check(unsafe {
            yy_gpu_draw(
                self.ptr.as_ptr(),
                view.x(),
                view.y(),
                view.width() as i32,
                view.height() as i32,
            )
        })
        .map(|result| result == 1)
    }

    /// Call only after a GPU draw and SDL_RenderPresent, before another upload.
    pub fn presented(&mut self) -> Result<bool, String> {
        self.active = check(unsafe { yy_gpu_presented(self.ptr.as_ptr()) })? == 1;
        if !self.active {
            note_fallback("presented");
        }
        Ok(self.active)
    }

    pub fn reset(&mut self) -> Result<(), String> {
        check(unsafe { yy_gpu_reset(self.ptr.as_ptr()) }).map(|_| ())
    }
}

impl Drop for GpuVideo {
    fn drop(&mut self) {
        unsafe { yy_gpu_close(self.ptr.as_ptr()) };
    }
}
