use sdl2::{
    controller::{Axis, Button, GameController},
    keyboard::Scancode,
    mouse::MouseButton,
    rect::Rect,
};
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};

/// Match SDL window coordinates to the image, including HiDPI and letterboxing.
pub(crate) fn image_point(
    view: Rect,
    output: (u32, u32),
    normalized: (f32, f32),
    clamp: bool,
) -> Option<(f32, f32)> {
    let x = (normalized.0 * output.0 as f32 - view.x() as f32) / view.width() as f32;
    let y = (normalized.1 * output.1 as f32 - view.y() as f32) / view.height() as f32;
    if !x.is_finite()
        || !y.is_finite()
        || (!clamp && (!(0.0..=1.0).contains(&x) || !(0.0..=1.0).contains(&y)))
    {
        return None;
    }
    Some((x.clamp(0., 1.), y.clamp(0., 1.)))
}

pub(crate) fn button(b: MouseButton) -> Option<u8> {
    match b {
        MouseButton::Left => Some(1),
        MouseButton::Right => Some(2),
        MouseButton::Middle => Some(3),
        _ => None,
    }
}

/// UU expects Windows virtual keys. SDL scancodes identify physical keys
/// independently of the local text input method, as game shortcuts require.
pub(crate) fn vk(s: Scancode) -> Option<i32> {
    use Scancode::*;
    let n = s as i32;
    Some(match s {
        _ if (A as i32..=Z as i32).contains(&n) => 0x41 + n - A as i32,
        _ if (Num1 as i32..=Num9 as i32).contains(&n) => 0x31 + n - Num1 as i32,
        _ if (F1 as i32..=F12 as i32).contains(&n) => 0x70 + n - F1 as i32,
        _ if (F13 as i32..=F24 as i32).contains(&n) => 0x7c + n - F13 as i32,
        _ if (Kp1 as i32..=Kp9 as i32).contains(&n) => 0x61 + n - Kp1 as i32,
        Num0 => 0x30,
        Kp0 => 0x60,
        Return | KpEnter => 13,
        Escape => 27,
        Backspace => 8,
        Tab => 9,
        Space => 32,
        LShift => 0xa0,
        RShift => 0xa1,
        LCtrl => 0xa2,
        RCtrl => 0xa3,
        LAlt => 0xa4,
        RAlt => 0xa5,
        LGui => 0x5b,
        RGui => 0x5c,
        CapsLock => 20,
        PrintScreen => 44,
        ScrollLock => 145,
        Pause => 19,
        Insert => 45,
        Home => 36,
        PageUp => 33,
        Delete => 46,
        End => 35,
        PageDown => 34,
        Right => 39,
        Left => 37,
        Down => 40,
        Up => 38,
        Application => 93,
        NumLockClear => 144,
        KpDivide => 111,
        KpMultiply => 106,
        KpMinus => 109,
        KpPlus => 107,
        KpPeriod => 110,
        Semicolon => 186,
        Equals => 187,
        Comma => 188,
        Minus => 189,
        Period => 190,
        Slash => 191,
        Grave => 192,
        LeftBracket => 219,
        Backslash => 220,
        RightBracket => 221,
        Apostrophe => 222,
        NonUsBackslash => 226,
        _ => return None,
    })
}

#[derive(Default)]
pub(crate) struct InputState {
    keys: BTreeSet<i32>,
    pub buttons: BTreeSet<u8>,
    fingers: BTreeMap<(i64, i64), (u8, f32, f32)>,
}

impl InputState {
    pub fn key(&mut self, key: i32, pressed: bool) -> Option<Value> {
        let changed = if pressed {
            self.keys.insert(key)
        } else {
            self.keys.remove(&key)
        };
        changed.then(|| json!({"action":if pressed {"kbd_press"} else {"kbd_release"}, "key":key}))
    }
    pub fn mouse(&mut self, button: u8, pressed: bool) -> Option<Value> {
        let changed = if pressed {
            self.buttons.insert(button)
        } else {
            self.buttons.remove(&button)
        };
        changed.then(
            || json!({"action":if pressed {"mouse_press"} else {"mouse_release"}, "button":button}),
        )
    }
    pub fn finger(
        &mut self,
        id: (i64, i64),
        point: Option<(f32, f32)>,
        down: bool,
    ) -> Option<Value> {
        let was_empty = self.fingers.is_empty();
        if let Some((x, y)) = point {
            if let Some(f) = self.fingers.get_mut(&id) {
                f.1 = x;
                f.2 = y;
            } else if down {
                let slot = (0..10).find(|n| self.fingers.values().all(|f| f.0 != *n))?;
                self.fingers.insert(id, (slot, x, y));
            } else {
                return None;
            }
        } else {
            self.fingers.remove(&id)?;
        }
        // Match the upstream native-touch lifecycle. END carries no points.
        let kind = if self.fingers.is_empty() {
            3
        } else if was_empty {
            1
        } else {
            2
        };
        let mut contacts: Vec<_> = self.fingers.values().collect();
        contacts.sort_by_key(|f| f.0);
        let points: Vec<_> = contacts
            .iter()
            .map(|(id, x, y)| json!({"id":id,"x":x,"y":y}))
            .collect();
        Some(json!({"action":"touch_native","type":kind,"points":points}))
    }
    pub fn release(&mut self) -> Vec<Value> {
        let mut events = Vec::new();
        for key in std::mem::take(&mut self.keys) {
            events.push(json!({"action":"kbd_release","key":key}));
        }
        for button in std::mem::take(&mut self.buttons) {
            events.push(json!({"action":"mouse_release","button":button}));
        }
        if !self.fingers.is_empty() {
            self.fingers.clear();
            events.push(json!({"action":"touch_native","type":4,"points":[]}));
        }
        events
    }
}

pub(crate) fn pad_state(pad: &GameController) -> Value {
    let mut buttons = 0u32;
    for (button, bit) in [
        (Button::DPadUp, 1),
        (Button::DPadDown, 2),
        (Button::DPadLeft, 4),
        (Button::DPadRight, 8),
        (Button::Start, 16),
        (Button::Back, 32),
        (Button::LeftStick, 64),
        (Button::RightStick, 128),
        (Button::LeftShoulder, 256),
        (Button::RightShoulder, 512),
        (Button::A, 4096),
        (Button::B, 8192),
        (Button::X, 16384),
        (Button::Y, 32768),
    ] {
        if pad.button(button) {
            buttons |= bit;
        }
    }
    let trigger = |a| (i32::from(pad.axis(a)).max(0) * 255 + 16383) / 32767;
    json!({"action":"gpd_update","index":0,"buttons":buttons,
        "left_trigger":trigger(Axis::TriggerLeft),"right_trigger":trigger(Axis::TriggerRight),
        "left_thumb_x":pad.axis(Axis::LeftX),"left_thumb_y":pad.axis(Axis::LeftY).saturating_neg(),
        "right_thumb_x":pad.axis(Axis::RightX),"right_thumb_y":pad.axis(Axis::RightY).saturating_neg()})
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn portrait_video_rejects_new_contacts_in_bars_and_clamps_drags() {
        let r = Rect::new(0, 300, 800, 450);
        assert_eq!(
            image_point(r, (800, 1050), (0.5, 0.5), false),
            Some((0.5, 0.5))
        );
        assert_eq!(image_point(r, (800, 1050), (0.5, 0.), false), None);
        assert_eq!(
            image_point(r, (800, 1050), (0.5, 0.), true),
            Some((0.5, 0.))
        );
    }
    #[test]
    fn multitouch_keeps_stable_slots_and_cancels_on_focus_loss() {
        let mut s = InputState::default();
        assert_eq!(s.finger((1, 9), Some((0.1, 0.1)), true).unwrap()["type"], 1);
        assert_eq!(
            s.finger((1, 2), Some((0.8, 0.8)), true).unwrap()["points"][1]["id"],
            1
        );
        let update = s.finger((1, 9), None, false).unwrap();
        assert_eq!(update["points"][0]["id"], 1);
        assert_eq!(update["type"], 2);
        assert_eq!(s.release()[0]["type"], 4);
        assert!(s.finger((1, 2), None, false).is_none());
    }
    #[test]
    fn held_inputs_release_once() {
        let mut s = InputState::default();
        s.key(87, true);
        assert!(s.key(87, true).is_none());
        s.mouse(1, true);
        assert_eq!(s.release().len(), 2);
        assert!(s.release().is_empty());
        assert_eq!(vk(Scancode::W), Some(87));
        assert_eq!(vk(Scancode::F12), Some(123));
    }
}
