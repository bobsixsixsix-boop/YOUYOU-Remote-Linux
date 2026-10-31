//! Desktop fullscreen follows the active output and keeps its refresh rate.
use crate::icons::Icons;
use sdl2::{
    event::{Event, WindowEvent},
    keyboard::Scancode,
    mouse::MouseButton,
    pixels::Color,
    rect::{Point, Rect},
    render::WindowCanvas,
    video::FullscreenType,
};

pub fn active(canvas: &WindowCanvas) -> bool {
    canvas.window().fullscreen_state() != FullscreenType::Off
}

pub fn icon_name(canvas: &WindowCanvas) -> &'static str {
    if active(canvas) {
        "fullscreen-exit"
    } else {
        "fullscreen"
    }
}

fn icon_bounds(button: Rect) -> Rect {
    let side = (button.width().min(button.height()) * 3 / 5).max(1);
    Rect::from_center(button.center(), side, side)
}

pub fn toggle(canvas: &mut WindowCanvas) -> Result<(), String> {
    let next = if active(canvas) {
        FullscreenType::Off
    } else {
        FullscreenType::Desktop
    };
    canvas.window_mut().set_fullscreen(next)
}

fn corner_rect(output: (u32, u32), logical: (u32, u32)) -> Rect {
    let pixels = |n: u32, l: u32| {
        (48 * u64::from(n) / u64::from(l.max(1))).clamp(1, u64::from(n.max(1))) as u32
    };
    let w = pixels(output.0, logical.0);
    Rect::new(
        output.0.saturating_sub(w) as i32,
        0,
        w,
        pixels(output.1, logical.1),
    )
}

#[derive(PartialEq, Eq, Clone, Copy)]
enum Contact {
    Mouse(u32),
    Finger(i64, i64),
}

#[derive(Default)]
pub struct Control {
    contact: Option<Contact>,
}

impl Control {
    pub fn draw(&self, canvas: &mut WindowCanvas, icons: &mut Icons) -> Result<(), String> {
        let r = corner_rect(canvas.output_size()?, canvas.window().size());
        let bg = if self.contact.is_some() {
            Color::RGB(222, 240, 255)
        } else {
            Color::WHITE
        };
        let ink = Color::RGB(11, 102, 221);
        canvas.set_draw_color(bg);
        canvas.fill_rect(r)?;
        if icons.draw(canvas, icon_name(canvas), icon_bounds(r), 1.0, ink)? {
            return Ok(());
        }
        canvas.set_draw_color(ink);
        let w = (r.width() * 2 / 5).max(1);
        let h = (r.height() / 3).max(1);
        let x = r.center().x() - w as i32 / 2;
        let y = r.center().y() - h as i32 / 2;
        if active(canvas) {
            canvas.draw_rect(Rect::new(x + 3, y - 3, w, h))?;
            let front = Rect::new(x - 2, y + 2, w, h);
            canvas.set_draw_color(bg);
            canvas.fill_rect(front)?;
            canvas.set_draw_color(ink);
            canvas.draw_rect(front)?;
        } else {
            let length = (h / 2).max(1);
            for (dx, dy) in [(0, 0), (1, 0), (0, 1), (1, 1)] {
                let cx = x + dx * (w as i32 - 2);
                let cy = y + dy * (h as i32 - 2);
                canvas.fill_rect(Rect::new(cx - dx * (length as i32 - 2), cy, length, 2))?;
                canvas.fill_rect(Rect::new(cx, cy - dy * (length as i32 - 2), 2, length))?;
            }
        }
        Ok(())
    }

    // Consume both ends of a local gesture. Releasing a finger outside the
    // button cancels it without sending any of that gesture to the remote PC.
    pub fn handle(&mut self, canvas: &mut WindowCanvas, event: &Event) -> Result<bool, String> {
        self.handle_with_visibility(canvas, event, true)
    }

    pub(crate) fn handle_with_visibility(
        &mut self,
        canvas: &mut WindowCanvas,
        event: &Event,
        button_visible: bool,
    ) -> Result<bool, String> {
        let (ww, wh) = canvas.window().size();
        let mouse = |x: i32, y: i32| (x as f32 / ww.max(1) as f32, y as f32 / wh.max(1) as f32);
        let (contact, down, pos) = match *event {
            Event::KeyDown {
                scancode: Some(Scancode::F11),
                repeat,
                ..
            } => {
                if !repeat {
                    toggle(canvas)?;
                }
                return Ok(true);
            }
            Event::KeyUp {
                scancode: Some(Scancode::F11),
                ..
            } => return Ok(true),
            _ if !button_visible => {
                self.contact = None;
                return Ok(false);
            }
            Event::MouseButtonDown {
                which,
                mouse_btn: MouseButton::Left,
                x,
                y,
                ..
            } if which != u32::MAX => (Contact::Mouse(which), true, mouse(x, y)),
            Event::MouseButtonUp {
                which,
                mouse_btn: MouseButton::Left,
                x,
                y,
                ..
            } if which != u32::MAX => (Contact::Mouse(which), false, mouse(x, y)),
            Event::FingerDown {
                touch_id,
                finger_id,
                x,
                y,
                ..
            } => (Contact::Finger(touch_id, finger_id), true, (x, y)),
            Event::FingerUp {
                touch_id,
                finger_id,
                x,
                y,
                ..
            } => (Contact::Finger(touch_id, finger_id), false, (x, y)),
            Event::MouseMotion { which, .. } => {
                return Ok(self.contact == Some(Contact::Mouse(which)))
            }
            Event::FingerMotion {
                touch_id,
                finger_id,
                ..
            } => return Ok(self.contact == Some(Contact::Finger(touch_id, finger_id))),
            Event::Window {
                win_event: WindowEvent::FocusLost,
                ..
            } => {
                self.contact = None;
                return Ok(false);
            }
            _ => return Ok(false),
        };
        let output = canvas.output_size()?;
        let inside = corner_rect(output, (ww, wh)).contains_point(Point::new(
            (pos.0 * output.0 as f32) as i32,
            (pos.1 * output.1 as f32) as i32,
        ));
        if down && inside && self.contact.is_none() {
            self.contact = Some(contact);
            return Ok(true);
        }
        if !down && self.contact == Some(contact) {
            self.contact = None;
            if inside {
                toggle(canvas)?;
            }
            return Ok(true);
        }
        Ok(false)
    }
}
