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

    pub(crate) fn packet(
        self,
        keys: &mut InputState,
        view: Rect,
        output: (u32, u32),
    ) -> Option<Value> {
        if self.phase == Phase::Up {
            return keys.finger(self.id, None, false);
        }
        let down = self.phase == Phase::Down;
        let point = image_point(view, output, self.point, !down)?;
        keys.finger(self.id, Some(point), down)
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
        age >= 650
            || (age >= 200 && self.distance(last).0 < 8.0)
            || now.wrapping_sub(last.time) >= 250
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
                || (vertical >= 12.0 && vertical > inward * 0.75);
            if !incompatible {
                let threshold = (window.0 as f32 * 0.2).min(64.0);
                if inward >= threshold && inward >= vertical * 1.5 {
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
            let edge = (window.0 as f32 * 0.06).min(40.0);
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
            .map(|s| s.packet(&mut keys, view, WINDOW).unwrap())
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
            edge.route(sample(1, Motion, 23, 267, 40), WINDOW, true),
            Route::Replay(_)
        ));
        assert!(matches!(
            edge.route(sample(1, Motion, 160, 267, 50), WINDOW, true),
            Route::Pass
        ));
    }
}
