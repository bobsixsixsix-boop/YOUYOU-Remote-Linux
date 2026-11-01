use serde::Deserialize;
use serde_json::{json, Value};
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::ptr::NonNull;
use youyou_player::{BorrowedAvFrame, Format, Frame, FrameData, GpuVideo, PlayerStatus, Session};

#[repr(C)]
struct NativeFrame {
    data: *const u8,
    size: usize,
    serial: u64,
    width: i32,
    height: i32,
    format: i32,
}

extern "C" {
    fn yy_open(key: *const c_char, client_id: *const c_char) -> *mut c_void;
    fn yy_error() -> *const c_char;
    fn yy_close(core: *mut c_void);
    fn yy_command(core: *mut c_void, name: *const c_char, args: *const c_char) -> c_int;
    fn yy_snapshot(core: *mut c_void) -> *const c_char;
    fn yy_frame(core: *mut c_void, after: u64, frame: *mut NativeFrame) -> c_int;
    fn yy_test_media() -> c_int;
}
pub fn test_media() -> Result<(), String> {
    if unsafe { yy_test_media() } == 0 {
        Ok(())
    } else {
        Err(error())
    }
}

fn error() -> String {
    let msg = unsafe { yy_error() };
    if msg.is_null() {
        return "UU 协议库调用失败".into();
    }
    unsafe { CStr::from_ptr(msg) }
        .to_string_lossy()
        .into_owned()
}

#[derive(Deserialize, Default, PartialEq, Clone)]
#[serde(default, rename_all = "camelCase")]
pub struct State {
    pub stage: String,
    pub detail: String,
    pub qr_url: String,
    pub authenticated: bool,
    pub streaming: bool,
    pub control_channel_ready: bool,
    pub login_mode: String,
}

pub struct Core {
    ptr: NonNull<c_void>,
    serial: u64,
    _account: youyou_account::Account,
}

impl Core {
    pub fn open(account: youyou_account::Account) -> Result<Self, String> {
        // The upstream library uses relative paths for its session and configuration.
        std::env::set_current_dir(account.state_dir()).map_err(|e| e.to_string())?;
        let key = CString::new(account.key()).map_err(|e| e.to_string())?;
        let id = CString::new(account.client_id()).map_err(|e| e.to_string())?;
        let ptr = NonNull::new(unsafe { yy_open(key.as_ptr(), id.as_ptr()) }).ok_or_else(error)?;
        Ok(Self {
            ptr,
            serial: 0,
            _account: account,
        })
    }

    pub fn command(&self, name: &str, args: Value) -> Result<(), String> {
        let name = CString::new(name).map_err(|e| e.to_string())?;
        let args = CString::new(args.to_string()).map_err(|e| e.to_string())?;
        if unsafe { yy_command(self.ptr.as_ptr(), name.as_ptr(), args.as_ptr()) } < 0 {
            return Err(error());
        }
        Ok(())
    }

    pub fn state(&self) -> Result<State, String> {
        let ptr = unsafe { yy_snapshot(self.ptr.as_ptr()) };
        if ptr.is_null() {
            return Err(error());
        }
        serde_json::from_slice(unsafe { CStr::from_ptr(ptr) }.to_bytes())
            .map_err(|_| "无法读取协议库状态".into())
    }

    pub fn new_stream(&mut self) {
        self.serial = 0;
    }
}

impl Session for Core {
    fn frame(&mut self) -> Result<Option<Frame<'_>>, String> {
        let mut raw = std::mem::MaybeUninit::<NativeFrame>::uninit();
        match unsafe { yy_frame(self.ptr.as_ptr(), self.serial, raw.as_mut_ptr()) } {
            0 => return Ok(None),
            1 => (),
            _ => return Err(error()),
        }
        let raw = unsafe { raw.assume_init() };
        if raw.data.is_null()
            || !(1..=8192).contains(&raw.width)
            || !(1..=8192).contains(&raw.height)
        {
            return Err("无效的视频帧尺寸".into());
        }
        if raw.format == 3 {
            if raw.size != 0 {
                return Err("硬件视频帧不能含有像素数据长度".into());
            }
            self.serial = raw.serial;
            // yy_frame retains this AVFrame until its next call or yy_close.
            // Tie the opaque handle to this Core borrow, never to a byte slice.
            let frame = unsafe { BorrowedAvFrame::from_raw(raw.data.cast(), self) }
                .ok_or_else(|| "空的硬件视频帧".to_string())?;
            return Ok(Some(Frame {
                width: raw.width as u32,
                height: raw.height as u32,
                data: FrameData::Vaapi(frame),
            }));
        }
        let samples = raw.width as usize * raw.height as usize;
        let (format, size) = match raw.format {
            0 => (Format::Rgba, samples * 4),
            1 => (Format::Nv12Bt601, samples + samples / 2),
            2 => (Format::Nv12Bt709, samples + samples / 2),
            _ => return Err("未知的视频像素格式".into()),
        };
        if raw.size != size {
            return Err("视频帧长度与像素格式不一致".into());
        }
        // yy_frame retains the native picture until its next call or yy_close.
        // The mutable borrow of Core prevents either while SDL uploads this slice.
        let pixels = unsafe { std::slice::from_raw_parts(raw.data, raw.size) };
        self.serial = raw.serial;
        Ok(Some(Frame {
            width: raw.width as u32,
            height: raw.height as u32,
            data: FrameData::Pixels {
                format,
                bytes: pixels,
            },
        }))
    }
    fn input(&mut self, data: Value) -> Result<(), String> {
        self.command("input", data)
    }
    fn gpu_video(&mut self, canvas: &sdl2::render::WindowCanvas) -> Option<GpuVideo> {
        if !youyou_player::gpu_video_requested() {
            return None;
        }
        // play_loop drops its renderer-local presenter before this Core and the
        // caller-owned canvas can be destroyed. It never leaves the SDL thread.
        unsafe { GpuVideo::open(self.ptr.as_ptr(), canvas) }
    }
    fn status(&mut self) -> Result<PlayerStatus, String> {
        let s = self.state()?;
        Ok(PlayerStatus {
            title: if s.detail.is_empty() {
                "YOUYOU 远程".into()
            } else {
                s.detail
            },
            authenticated: s.authenticated,
            streaming: s.streaming,
            can_control: s.control_channel_ready,
        })
    }
    fn menu(
        &mut self,
        canvas: &sdl2::render::WindowCanvas,
    ) -> Result<Option<Box<dyn youyou_player::Menu>>, String> {
        unsafe { youyou_player::attach_upstream(self.ptr.as_ptr(), canvas) }
    }
}

impl Drop for Core {
    fn drop(&mut self) {
        unsafe { yy_close(self.ptr.as_ptr()) };
    }
}

pub fn restore() -> Result<Core, String> {
    let core = Core::open(youyou_account::open().map_err(|e| e.to_string())?)?;
    core.command("startup", json!({}))?;
    Ok(core)
}
