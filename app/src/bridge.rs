//! Safe wrappers over core/bridge.h (libyouyou_core.so).
use serde_json::Value;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::io;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RawFrame {
    pub fd: c_int,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub uv_offset: u32,
    pub index: u32,
    pub epoch: u64,
    pub serial: u64,
    pub ready_us: i64,
}

extern "C" {
    fn yy_open(key: *const c_char, client_id: *const c_char) -> c_int;
    fn yy_close();
    fn yy_error() -> *const c_char;
    fn yy_command(name: *const c_char, json: *const c_char) -> c_int;
    fn yy_snapshot() -> *mut c_char;
    fn yy_free(text: *mut c_char);
    fn yy_frame_acquire(out: *mut RawFrame) -> c_int;
    fn yy_frame_next(out: *mut RawFrame) -> c_int;
    fn yy_frame_release(frame: *const RawFrame);
    fn yy_frame_wait(serial: u64, timeout_ms: c_int) -> u64;
    fn yy_debug_video(data: *const c_void, size: usize, hevc: c_int);
}

fn error() -> io::Error {
    let text = unsafe { yy_error() };
    let text = if text.is_null() { "native core failed".into() } else { unsafe { CStr::from_ptr(text) }.to_string_lossy() };
    io::Error::other(text.into_owned())
}

/// The upstream client. One per process; closing it ends any stream. It owns
/// the account so the state directory stays locked until the client is gone.
pub struct Core {
    _account: youyou_account::Account,
}

impl Core {
    /// The caller has already moved into the account's state directory:
    /// upstream keeps config.ini, session.json and connection.log in the working directory.
    pub fn open(account: youyou_account::Account) -> io::Result<Self> {
        let key = CString::new(account.key()).map_err(io::Error::other)?;
        let id = CString::new(account.client_id()).map_err(io::Error::other)?;
        if unsafe { yy_open(key.as_ptr(), id.as_ptr()) } < 0 {
            return Err(error());
        }
        Ok(Self { _account: account })
    }
    pub fn command(&self, name: &str, data: &Value) -> io::Result<()> {
        let name = CString::new(name).map_err(io::Error::other)?;
        let json = CString::new(data.to_string()).map_err(io::Error::other)?;
        if unsafe { yy_command(name.as_ptr(), json.as_ptr()) } < 0 {
            return Err(error());
        }
        Ok(())
    }
    pub fn snapshot(&self) -> Value {
        let text = unsafe { yy_snapshot() };
        if text.is_null() {
            return Value::Null;
        }
        let value = serde_json::from_slice(unsafe { CStr::from_ptr(text) }.to_bytes()).unwrap_or(Value::Null);
        unsafe { yy_free(text) };
        value
    }
}

impl Drop for Core {
    fn drop(&mut self) {
        unsafe { yy_close() }
    }
}

/// A decoded picture on loan from the decoder; dropping it gives the buffer back.
pub struct Frame(pub RawFrame);

impl Frame {
    /// The newest picture nobody has taken yet.
    pub fn acquire() -> Option<Self> {
        let mut raw = RawFrame::default();
        (unsafe { yy_frame_acquire(&mut raw) } == 1).then_some(Self(raw))
    }
    /// The oldest picture nobody has taken yet, and how many were waiting, it included.
    pub fn next() -> Option<(Self, i32)> {
        let mut raw = RawFrame::default();
        let waiting = unsafe { yy_frame_next(&mut raw) };
        (waiting > 0).then_some((Self(raw), waiting))
    }
}

impl Drop for Frame {
    fn drop(&mut self) {
        unsafe {
            libc::close(self.0.fd);
            yy_frame_release(&self.0);
        }
    }
}

/// Blocks until a picture newer than `serial` is ready; its serial, or 0 on timeout.
pub fn wait(serial: u64, timeout_ms: i32) -> u64 {
    unsafe { yy_frame_wait(serial, timeout_ms) }
}

pub fn debug_video(unit: &[u8], hevc: bool) {
    unsafe { yy_debug_video(unit.as_ptr().cast(), unit.len(), hevc.into()) }
}
