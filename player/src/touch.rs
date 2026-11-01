use crate::input::{image_point, InputState};
use sdl2::{event::Event, rect::Rect};
use serde_json::Value;
use std::collections::BTreeSet;

type Contact = (i64, i64);

#[derive(Clone, Copy, PartialEq, Debug)]
enum Phase {
    Down,
    Motion,
    Up,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct Touch {
    id: Contact,
    phase: Phase,
    point: (f32, f32),
    time: u32, // SDL ticks in milliseconds
}

impl Touch {
    pub(crate) fn from_event(event: &Event) -> Option<Self> {
        let (&id, &finger, &x, &y, &time, phase) = match event {
            Event::FingerDown {
                touch_id,
                finger_id,
                x,
                y,
                timestamp,
                ..
            } => (touch_id, finger_id, x, y, timestamp, Phase::Down),
            Event::FingerMotion {
                touch_id,
                finger_id,
                x,
                y,
                timestamp,
                ..
            } => (touch_id, finger_id, x, y, timestamp, Phase::Motion),
            Event::FingerUp {
                touch_id,
                finger_id,
                x,
                y,
                timestamp,
                ..
            } => (touch_id, finger_id, x, y, timestamp, Phase::Up),
            _ => return None,
        };
        Some(Self {
            id: (id, finger),
            phase,
            point: (x, y),
            time,
        })
    }

    pub(crate) fn packets(
        self,
        keys: &mut InputState,
        view: Rect,
        output: (u32, u32),
    ) -> [Option<Value>; 2] {
        let down = self.phase == Phase::Down;
        let point = image_point(view, output, self.point, !down);
        if let Some(mouse) = &mut keys.desktop_mouse {
            return match self.phase {
                Phase::Down => mouse.press(self.id, point),
                Phase::Motion => [mouse.motion(self.id, point), None],
                Phase::Up => mouse.lift(self.id, point),
            };
        }
        let packet = if self.phase == Phase::Up {
            keys.finger(self.id, None, false)
        } else {
            point.and_then(|p| keys.finger(self.id, Some(p), down))
        };
        [packet, None]
    }
}

struct Candidate {
    samples: Vec<Touch>,
    window: (u32, u32),
}

impl Candidate {
    fn distance(&self, t: Touch) -> (f32, f32) {
        let origin = self.samples[0].point;
        let direction = if origin.0 < 0.5 { 1.0 } else { -1.0 };
        (
            (t.point.0 - origin.0) * self.window.0 as f32 * direction,
            ((t.point.1 - origin.1) * self.window.1 as f32).abs(),
        )
    }

    fn expired(&self, now: u32) -> bool {
        let first = self.samples[0];
        let last = *self.samples.last().unwrap();
        let age = now.wrapping_sub(first.time);
        // A stationary press belongs to the host. Allow more time for a swipe
        // already moving inward, but return a stalled drag to the host as well.
        age >= 1000
            || (age >= 200 && self.distance(last).0 < 5.0)
            || now.wrapping_sub(last.time) >= 350
    }
}

pub(crate) enum Route {
    Pass,
    Hold,
    OpenMenu,
    // Forward these earlier samples before processing the current event normally.
    Replay(Vec<Touch>),
}

#[derive(Default)]
pub(crate) struct EdgeSwipe {
    contacts: BTreeSet<Contact>,
    pending: Option<Candidate>,
    swallowed: Option<Contact>,
}

impl EdgeSwipe {
    pub(crate) fn cancel_pending(&mut self) {
        self.pending = None;
    }

    pub(crate) fn expired(&mut self, now: u32) -> Option<Vec<Touch>> {
        if self.pending.as_ref()?.expired(now) {
            self.pending.take().map(|candidate| candidate.samples)
        } else {
            None
        }
    }

    pub(crate) fn route(&mut self, t: Touch, window: (u32, u32), enabled: bool) -> Route {
        let first_contact = self.contacts.is_empty();
        match t.phase {
            Phase::Down => {
                self.contacts.insert(t.id);
            }
            Phase::Up => {
                self.contacts.remove(&t.id);
            }
            Phase::Motion => {}
        }
        // Keep the initiating finger local through its release, even if another
        // input closes the menu before this finger is lifted.
        if self.swallowed == Some(t.id) {
            if t.phase == Phase::Up {
                self.swallowed = None;
            }
            return Route::Hold;
        }
        if !enabled {
            self.cancel_pending();
            return Route::Pass;
        }
        if let Some(candidate) = &mut self.pending {
            let (inward, vertical) = candidate.distance(t);
            let incompatible = t.id != candidate.samples[0].id
                || t.phase == Phase::Down
                || candidate.window != window
                || candidate.expired(t.time)
                || inward < -8.0
                || (vertical >= 20.0 && vertical > inward * 1.4);
            if !incompatible {
                let threshold = (window.0 as f32 * 0.2).min(44.0);
                if inward >= threshold && inward >= vertical * 1.1 {
                    self.pending = None;
                    self.swallowed = (t.phase != Phase::Up).then_some(t.id);
                    return Route::OpenMenu;
                }
                if t.phase != Phase::Up {
                    candidate.samples.push(t);
                    return Route::Hold;
                }
            }
            return Route::Replay(self.pending.take().unwrap().samples);
        }
        if first_contact && t.phase == Phase::Down && window.0 > 0 && window.1 > 0 {
            // Accept the bezel itself as well as a thumb landing slightly inside.
            // Keep the band proportional on narrow windows so content stays direct.
            let edge = (window.0 as f32 * 0.08).min(96.0);
            let x = t.point.0 * window.0 as f32;
            if (0.0..=edge).contains(&x) || (0.0..=edge).contains(&(window.0 as f32 - x)) {
                self.pending = Some(Candidate {
                    samples: vec![t],
                    window,
                });
                return Route::Hold;
            }
        }
        Route::Pass
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use Phase::*;

    const WINDOW: (u32, u32) = (1280, 800);
    fn sample(id: i64, phase: Phase, x: u32, y: u32, time: u32) -> Touch {
        Touch {
            id: (1, id),
            phase,
            point: (x as f32 / 1280.0, y as f32 / 800.0),
            time,
        }
    }

    fn desktop_packets(keys: &mut InputState, id: i64, phase: Phase, x: u32, y: u32) -> Vec<Value> {
        sample(id, phase, x, y, 100)
            .packets(keys, Rect::new(128, 80, 1024, 640), WINDOW)
            .into_iter()
            .flatten()
            .collect()
    }

    #[test]
    fn desktop_drag_moves_to_contact_before_press_and_releases_at_lift() {
        let mut keys = InputState::default();
        keys.set_desktop_mouse(true);
        let down = desktop_packets(&mut keys, 5, Down, 640, 400);
        assert_eq!(
            down,
            vec![
                serde_json::json!({"action":"mouse_move_absolute","abs_x":0.5,"abs_y":0.5,"source":"touch_button_position"}),
                serde_json::json!({"action":"mouse_press","button":1,"source":"phone_mouse"})
            ]
        );
        assert_eq!(
            desktop_packets(&mut keys, 5, Motion, 896, 560),
            vec![crate::input::absolute_pointer((0.75, 0.75))]
        );
        assert_eq!(
            desktop_packets(&mut keys, 5, Up, 1279, 799),
            vec![
                serde_json::json!({"action":"mouse_move_absolute","abs_x":1.0,"abs_y":1.0,"source":"touch_button_position"}),
                serde_json::json!({"action":"mouse_release","button":1,"source":"phone_mouse"})
            ]
        );
        assert!(keys.release().is_empty());
    }

    #[test]
    fn black_bars_and_extra_fingers_do_not_pick_up_a_mouse_drag() {
        let mut keys = InputState::default();
        keys.set_desktop_mouse(true);
        for (id, phase, x, y) in [
            (1, Down, 48, 80),
            (1, Motion, 640, 400),
            (2, Down, 640, 400),
            (1, Up, 640, 400),
            (2, Motion, 680, 400),
            (2, Up, 680, 400),
        ] {
            assert!(desktop_packets(&mut keys, id, phase, x, y).is_empty());
        }
        assert_eq!(desktop_packets(&mut keys, 1, Down, 640, 400).len(), 2);
        assert!(desktop_packets(&mut keys, 2, Down, 720, 400).is_empty());
        assert!(desktop_packets(&mut keys, 2, Motion, 750, 400).is_empty());
        assert_eq!(desktop_packets(&mut keys, 1, Up, 640, 400).len(), 2);
        assert!(desktop_packets(&mut keys, 3, Down, 720, 400).is_empty());
        assert!(desktop_packets(&mut keys, 2, Up, 750, 400).is_empty());
        assert!(desktop_packets(&mut keys, 3, Up, 720, 400).is_empty());
        assert_eq!(desktop_packets(&mut keys, 4, Down, 640, 400).len(), 2);
    }

    #[test]
    fn switching_or_losing_focus_releases_only_the_owner_that_pressed() {
        let mut keys = InputState::default();
        assert_eq!(desktop_packets(&mut keys, 4, Down, 640, 400)[0]["type"], 1);
        assert_eq!(keys.set_desktop_mouse(true)[0]["type"], 4);
        keys.mouse(1, true);
        desktop_packets(&mut keys, 6, Down, 640, 400);
        let finger_up =
            serde_json::json!({"action":"mouse_release","button":1,"source":"phone_mouse"});
        assert_eq!(keys.set_desktop_mouse(false), vec![finger_up.clone()]);
        assert!(keys.buttons.contains(&1));
        assert!(desktop_packets(&mut keys, 6, Up, 640, 400).is_empty());
        keys.set_desktop_mouse(true);
        desktop_packets(&mut keys, 7, Down, 640, 400);
        assert_eq!(
            keys.release(),
            vec![
                finger_up,
                serde_json::json!({"action":"mouse_release","button":1})
            ]
        );
        assert!(keys.release().is_empty());
    }

    #[test]
    fn both_edges_claim_only_an_inward_swipe_and_swallow_its_tail() {
        for (start, end) in [(25, 99), (1254, 1180)] {
            let mut edge = EdgeSwipe::default();
            assert!(matches!(
                edge.route(sample(1, Down, start, 480, 20), WINDOW, true),
                Route::Hold
            ));
            assert!(matches!(
                edge.route(sample(1, Motion, end, 486, 90), WINDOW, true),
                Route::OpenMenu
            ));
            assert!(matches!(
                edge.route(sample(1, Motion, end, 490, 105), WINDOW, false),
                Route::Hold
            ));
            assert!(matches!(
                edge.route(sample(1, Up, end, 490, 120), WINDOW, true),
                Route::Hold
            ));
            assert!(matches!(
                edge.route(sample(1, Down, 600, 200, 140), WINDOW, true),
                Route::Pass
            ));
        }
    }

    #[test]
    fn fast_release_can_finish_a_swipe_but_not_a_tap() {
        let mut edge = EdgeSwipe::default();
        edge.route(sample(1, Down, 18, 200, 100), WINDOW, true);
        let Route::Replay(samples) = edge.route(sample(1, Up, 20, 202, 125), WINDOW, true) else {
            panic!("edge tap lost its press");
        };
        assert_eq!(samples.len(), 1);
        assert_eq!(samples[0].phase, Down);
        edge.route(sample(1, Down, 18, 200, 200), WINDOW, true);
        assert!(matches!(
            edge.route(sample(1, Up, 120, 205, 235), WINDOW, true),
            Route::OpenMenu
        ));
    }

    #[test]
    fn bezel_or_thumb_width_in_accepts_a_short_diagonal_swipe() {
        for (start, end) in [(0, 46), (88, 134), (1192, 1146), (1280, 1234)] {
            let mut edge = EdgeSwipe::default();
            assert!(matches!(
                edge.route(sample(1, Down, start, 350, 10), WINDOW, true),
                Route::Hold
            ));
            assert!(matches!(
                edge.route(sample(1, Motion, end, 388, 80), WINDOW, true),
                Route::OpenMenu
            ));
        }
        let mut edge = EdgeSwipe::default();
        assert!(matches!(
            edge.route(sample(1, Down, 105, 350, 40), WINDOW, true),
            Route::Pass
        ));
    }

    #[test]
    fn an_inward_swipe_can_move_slowly_without_turning_into_a_long_press() {
        let mut edge = EdgeSwipe::default();
        for (phase, x, t) in [
            (Down, 80, 10),
            (Motion, 88, 170),
            (Motion, 99, 475),
            (Motion, 112, 750),
        ] {
            assert!(matches!(
                edge.route(sample(1, phase, x, 330, t), WINDOW, true),
                Route::Hold
            ));
        }
        assert!(matches!(
            edge.route(sample(1, Up, 127, 333, 900), WINDOW, true),
            Route::OpenMenu
        ));
    }

    #[test]
    fn stationary_press_times_out_but_moving_swipe_can_pass_200_ms() {
        let mut edge = EdgeSwipe::default();
        edge.route(sample(1, Down, 22, 300, 10), WINDOW, true);
        assert!(edge.expired(209).is_none());
        assert_eq!(edge.expired(210).unwrap()[0].phase, Down);
        assert!(matches!(
            edge.route(sample(1, Up, 22, 300, 900), WINDOW, true),
            Route::Pass
        ));
        edge.route(sample(1, Down, 22, 300, 1000), WINDOW, true);
        edge.route(sample(1, Motion, 42, 302, 1100), WINDOW, true);
        assert!(edge.expired(1240).is_none());
        assert!(matches!(
            edge.route(sample(1, Motion, 90, 306, 1300), WINDOW, true),
            Route::OpenMenu
        ));
    }

    #[test]
    fn second_finger_replays_the_full_drag_before_multitouch_begins() {
        let mut edge = EdgeSwipe::default();
        let samples = [sample(1, Down, 25, 320, 10), sample(1, Motion, 30, 320, 18)];
        for s in samples {
            assert!(matches!(edge.route(s, WINDOW, true), Route::Hold));
        }
        let second = sample(2, Down, 500, 440, 30);
        let Route::Replay(buffer) = edge.route(second, WINDOW, true) else {
            panic!("dropped multitouch");
        };
        let mut keys = InputState::default();
        let view = Rect::new(0, 0, 1280, 800);
        let packets: Vec<_> = buffer
            .into_iter()
            .chain([second])
            .flat_map(|s| s.packets(&mut keys, view, WINDOW).into_iter().flatten())
            .collect();
        assert_eq!(
            packets
                .iter()
                .map(|p| p["type"].as_i64().unwrap())
                .collect::<Vec<_>>(),
            [1, 2, 2]
        );
        assert_eq!(packets[2]["points"].as_array().unwrap().len(), 2);
        assert!(matches!(
            edge.route(sample(1, Motion, 160, 322, 45), WINDOW, true),
            Route::Pass
        ));
    }

    #[test]
    fn center_vertical_and_existing_multitouch_are_remote_gestures() {
        let mut edge = EdgeSwipe::default();
        assert!(matches!(
            edge.route(sample(1, Down, 500, 300, 0), WINDOW, true),
            Route::Pass
        ));
        assert!(matches!(
            edge.route(sample(2, Down, 20, 250, 5), WINDOW, true),
            Route::Pass
        ));
        edge.route(sample(2, Up, 20, 250, 10), WINDOW, true);
        edge.route(sample(1, Up, 500, 300, 15), WINDOW, true);
        edge.route(sample(1, Down, 20, 250, 20), WINDOW, true);
        assert!(matches!(
            edge.route(sample(1, Motion, 23, 273, 40), WINDOW, true),
            Route::Replay(_)
        ));
        assert!(matches!(
            edge.route(sample(1, Motion, 160, 267, 50), WINDOW, true),
            Route::Pass
        ));
    }
}
