use sdl2::{
    controller::{Axis, Button, GameController},
    event::Event,
    GameControllerSubsystem, Sdl,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PadAction {
    Confirm,
    Back,
    Up,
    Down,
    Left,
    Right,
    Menu,
}

#[derive(Default)]
struct Navigation {
    horizontal: i8,
    vertical: i8,
    select: bool,
    start: bool,
}

impl Navigation {
    fn button(&mut self, b: Button, down: bool) -> Option<PadAction> {
        let chord = self.select && self.start;
        match b {
            Button::Back => self.select = down,
            Button::Start => self.start = down,
            _ => (),
        }
        if !down {
            return None;
        }
        Some(match b {
            Button::A => PadAction::Confirm,
            Button::B => PadAction::Back,
            Button::DPadLeft => PadAction::Left,
            Button::DPadRight => PadAction::Right,
            Button::DPadUp => PadAction::Up,
            Button::DPadDown => PadAction::Down,
            Button::Guide => PadAction::Menu,
            // The Y700's padkeys service reserves the G9's Legion/Guide
            // button. Select + Start can still open or close the local menu.
            Button::Back | Button::Start if !chord && self.select && self.start => PadAction::Menu,
            _ => return None,
        })
    }

    fn axis(&mut self, axis: Axis, value: i16) -> Option<PadAction> {
        let (latch, negative, positive) = match axis {
            Axis::LeftX => (&mut self.horizontal, PadAction::Left, PadAction::Right),
            Axis::LeftY => (&mut self.vertical, PadAction::Up, PadAction::Down),
            _ => return None,
        };
        // A held stick moves focus once. Its lower release threshold absorbs
        // small stick drift without requiring a perfect zero between steps.
        let edge = if value <= -16000 {
            -1
        } else if value >= 16000 {
            1
        } else if value.unsigned_abs() <= 8000 {
            0
        } else {
            return None;
        };
        if *latch == edge {
            return None;
        }
        *latch = edge;
        match edge {
            -1 => Some(negative),
            1 => Some(positive),
            _ => None,
        }
    }
}

/// Keep the same SDL controller open for events, menu navigation and game input.
/// Device-added events carry indices; button/axis/removal events carry instance IDs.
pub struct Controllers {
    api: Option<GameControllerSubsystem>,
    pad: Option<GameController>,
    navigation: Navigation,
}

impl Controllers {
    pub fn new(sdl: &Sdl) -> Self {
        let mut controllers = Self {
            api: sdl.game_controller().ok(),
            pad: None,
            navigation: Navigation::default(),
        };
        controllers.rescan();
        controllers
    }
    fn rescan(&mut self) {
        self.navigation = Navigation::default();
        self.pad = self
            .api
            .as_ref()
            .and_then(|api| (0..api.num_joysticks().ok()?).find_map(|index| api.open(index).ok()));
    }
    pub fn name(&self) -> Option<String> {
        self.pad.as_ref().map(|p| p.name())
    }
    pub(crate) fn pad(&self) -> Option<&GameController> {
        self.pad.as_ref()
    }
    pub(crate) fn id(&self) -> Option<u32> {
        self.pad.as_ref().map(|p| p.instance_id())
    }

    pub fn handle(&mut self, event: &Event) -> Option<PadAction> {
        match *event {
            Event::ControllerDeviceAdded { which, .. } if self.pad.is_none() => {
                self.pad = self.api.as_ref().and_then(|api| api.open(which).ok());
            }
            Event::ControllerDeviceRemoved { which, .. } if self.id() == Some(which) => {
                self.pad = None;
                self.rescan();
            }
            Event::ControllerButtonDown { which, button, .. } if self.id() == Some(which) => {
                return self.navigation.button(button, true);
            }
            Event::ControllerButtonUp { which, button, .. } if self.id() == Some(which) => {
                return self.navigation.button(button, false);
            }
            Event::ControllerAxisMotion {
                which, axis, value, ..
            } if self.id() == Some(which) => {
                return self.navigation.axis(axis, value);
            }
            Event::Window {
                win_event: sdl2::event::WindowEvent::FocusLost,
                ..
            } => {
                self.navigation = Navigation::default();
            }
            _ => (),
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn navigation_uses_edges_and_leaves_individual_start_and_back_alone() {
        let mut nav = Navigation::default();
        assert_eq!(nav.button(Button::A, true), Some(PadAction::Confirm));
        assert_eq!(nav.button(Button::B, true), Some(PadAction::Back));
        assert_eq!(nav.button(Button::Back, true), None);
        assert_eq!(nav.button(Button::Start, true), Some(PadAction::Menu));
        assert_eq!(nav.button(Button::Start, true), None);
        assert_eq!(nav.button(Button::Start, false), None);
        assert_eq!(nav.button(Button::Back, false), None);
        assert_eq!(nav.axis(Axis::LeftX, 19000), Some(PadAction::Right));
        assert_eq!(nav.axis(Axis::LeftX, 10000), None);
        assert_eq!(nav.axis(Axis::LeftX, 17000), None);
        assert_eq!(nav.axis(Axis::LeftX, -23000), Some(PadAction::Left));
        assert_eq!(nav.axis(Axis::LeftX, 400), None);
        assert_eq!(nav.axis(Axis::LeftX, -17000), Some(PadAction::Left));
        assert_eq!(nav.axis(Axis::LeftY, -20000), Some(PadAction::Up));
        assert_eq!(nav.axis(Axis::LeftY, 20000), Some(PadAction::Down));
        assert_eq!(nav.axis(Axis::RightX, 21000), None);
    }
}
