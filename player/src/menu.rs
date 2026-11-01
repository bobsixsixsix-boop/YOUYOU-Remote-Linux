use sdl2::{render::WindowCanvas, sys::SDL_Renderer};
use std::{
    collections::BTreeMap,
    ffi::{c_char, c_void, CStr, CString},
    ptr::NonNull,
};

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(super) enum Pointer {
    Mouse(u32),
    Finger(i64, i64),
}

pub(super) enum Press {
    Down,
    Move,
    Up,
}

#[derive(Debug, PartialEq, Eq)]
pub(super) enum PointerRoute {
    Remote,
    Consume,
    Menu(i32),
}

#[cfg(test)]
mod pointer_tests {
    use super::*;
    use PointerRoute::{Consume, Menu as Action, Remote};
    use Press::{Down, Move, Up};

    #[test]
    fn only_a_single_local_press_completes_a_menu_click() {
        let mut input = MenuPointers::default();
        let first = Pointer::Finger(1, 10);
        let second = Pointer::Mouse(2);
        assert_eq!(input.route(first, Down, true), Action(8));
        assert_eq!(input.route(first, Move, true), Action(10));
        assert_eq!(input.route(second, Down, true), Action(14));
        assert_eq!(input.route(first, Up, true), Consume);
        assert_eq!(input.route(second, Up, true), Consume);
        assert_eq!(input.route(second, Down, true), Action(8));
        assert_eq!(input.route(second, Up, true), Action(9));
    }

    #[test]
    fn a_menu_press_stays_local_through_a_shortcut_close() {
        let mut input = MenuPointers::default();
        let mouse = Pointer::Mouse(0);
        assert_eq!(input.route(mouse, Down, true), Action(8));
        assert!(input.cancel());
        assert_eq!(input.route(mouse, Move, false), Consume);
        assert_eq!(input.route(mouse, Up, false), Consume);
        assert_eq!(input.route(mouse, Move, false), Remote);
        assert_eq!(input.route(mouse, Down, false), Remote);
        assert_eq!(input.route(mouse, Up, false), Remote);
    }
}

/// A menu click requires one pointer from down to up. A pointer taken by the
/// menu stays local even if a shortcut closes the drawer before its release.
#[derive(Default)]
pub(super) struct MenuPointers {
    // The value records where each down began, even if capture changes later.
    held: BTreeMap<Pointer, bool>,
    primary: Option<Pointer>,
}

impl MenuPointers {
    pub fn cancel(&mut self) -> bool {
        self.primary.take().is_some()
    }

    pub fn route(&mut self, id: Pointer, phase: Press, captured: bool) -> PointerRoute {
        use PointerRoute::*;
        match phase {
            Press::Down => {
                if self.held.contains_key(&id) {
                    return Consume;
                }
                self.held.insert(id, captured);
                if !captured {
                    Remote
                } else if self.held.len() == 1 {
                    self.primary = Some(id);
                    Menu(8)
                } else if self.cancel() {
                    Menu(14) // Cancel the native press when a second pointer joins.
                } else {
                    Consume
                }
            }
            Press::Move => {
                if captured && self.primary == Some(id) {
                    Menu(10)
                } else if captured || self.held.get(&id) == Some(&true) {
                    Consume
                } else {
                    Remote
                }
            }
            Press::Up => {
                let local = self.held.remove(&id) == Some(true);
                if self.primary == Some(id) {
                    self.primary = None;
                    if captured {
                        Menu(9)
                    } else {
                        Consume
                    }
                } else if captured || local {
                    Consume
                } else {
                    Remote
                }
            }
        }
    }
}

pub trait Menu {
    fn report_present_fps(&mut self, _fps: f64) {}
    fn tick(&mut self, action: i32, x: i32, y: i32, text: &str) -> Result<i32, String>;
    fn draw(&mut self) -> Result<(), String>;
    fn disconnect(&mut self) -> Result<(), String> {
        Ok(())
    }
}
extern "C" {
    fn yy_ui_open(
        core: *mut c_void,
        renderer: *mut SDL_Renderer,
        assets: *const c_char,
    ) -> *mut c_void;
    fn yy_ui_close(p: *mut c_void);
    fn yy_ui_disconnect(p: *mut c_void) -> i32;
    fn yy_ui_error() -> *const c_char;
    fn yy_ui_tick(p: *mut c_void, action: i32, x: i32, y: i32, text: *const c_char) -> i32;
    fn yy_ui_draw(p: *mut c_void) -> i32;
    fn yy_ui_present_fps(p: *mut c_void, fps: f64);
    fn yy_preview(renderer: *mut SDL_Renderer, assets: *const c_char, page: i32) -> i32;
    fn yy_ui_selftest(renderer: *mut SDL_Renderer, assets: *const c_char) -> i32;
}
fn assets() -> Result<CString, String> {
    use std::os::unix::ffi::OsStrExt;
    CString::new(crate::icons::asset_dir().as_os_str().as_bytes()).map_err(|e| e.to_string())
}
fn error() -> String {
    unsafe { CStr::from_ptr(yy_ui_error()) }
        .to_string_lossy()
        .into_owned()
}
fn check(v: i32) -> Result<i32, String> {
    if v < 0 {
        Err(error())
    } else {
        Ok(v)
    }
}
struct NativeMenu(NonNull<c_void>);
impl Drop for NativeMenu {
    fn drop(&mut self) {
        unsafe { yy_ui_close(self.0.as_ptr()) };
    }
}
impl Menu for NativeMenu {
    fn report_present_fps(&mut self, fps: f64) {
        unsafe { yy_ui_present_fps(self.0.as_ptr(), fps) };
    }
    fn tick(&mut self, action: i32, x: i32, y: i32, text: &str) -> Result<i32, String> {
        let value = CString::new(text).map_err(|e| e.to_string())?;
        check(unsafe { yy_ui_tick(self.0.as_ptr(), action, x, y, value.as_ptr()) })
    }
    fn draw(&mut self) -> Result<(), String> {
        check(unsafe { yy_ui_draw(self.0.as_ptr()) }).map(|_| ())
    }
    fn disconnect(&mut self) -> Result<(), String> {
        check(unsafe { yy_ui_disconnect(self.0.as_ptr()) }).map(|_| ())
    }
}
/// # Safety
/// The core must be the live native client belonging to this session. Its
/// lifetime and the canvas lifetime must extend until the menu is dropped.
pub unsafe fn attach_upstream(
    core: *mut c_void,
    canvas: &WindowCanvas,
) -> Result<Option<Box<dyn Menu>>, String> {
    let p = yy_ui_open(core, canvas.raw(), assets()?.as_ptr());
    Ok(Some(Box::new(NativeMenu(
        NonNull::new(p).ok_or_else(error)?,
    ))))
}
pub fn preview(canvas: &mut WindowCanvas, page: u8) -> Result<(), String> {
    if page > 7 {
        return Err("未知预览页面".into());
    }
    canvas.set_draw_color(sdl2::pixels::Color::RGB(20, 22, 26));
    canvas.clear();
    check(unsafe { yy_preview(canvas.raw(), assets()?.as_ptr(), page.into()) }).map(|_| ())
}
pub fn menu_self_test() -> Result<(), String> {
    let sdl = sdl2::init()?;
    let video = sdl.video()?;
    let window = video
        .window("YOUYOU · 菜单自检", 1280, 720)
        .hidden()
        .build()
        .map_err(|e| e.to_string())?;
    let canvas = window.into_canvas().build().map_err(|e| e.to_string())?;
    check(unsafe { yy_ui_selftest(canvas.raw(), assets()?.as_ptr()) }).map(|_| ())
}
