//! Linux desktop front end. The native session owns the decoders and SDL audio.
mod controllers;
pub mod fullscreen;
mod gpu;
pub mod icons;
mod input;
mod menu;
mod touch;
pub use controllers::{Controllers, PadAction};
pub use gpu::{gpu_video_requested, BorrowedAvFrame, GpuVideo, Upload as GpuUpload};
pub use menu::{attach_upstream, menu_self_test, preview, Menu};
use touch::{EdgeSwipe, Route, Touch};

use input::{absolute_pointer, button, image_point, pad_state, vk, InputState};
use menu::{MenuPointers, Pointer, PointerRoute, Press};
use sdl2::{
    event::{Event, WindowEvent},
    keyboard::Scancode,
    mouse::MouseWheelDirection,
    pixels::{Color, PixelFormatEnum},
    rect::Rect,
    render::{ScaleMode, Texture, TextureAccess, TextureCreator, WindowCanvas},
    video::WindowContext,
};
use serde_json::{json, Value};
use std::time::{Duration, Instant};

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Format {
    Rgba,
    Nv12Bt601,
    Nv12Bt709,
}

pub struct Frame<'a> {
    pub width: u32,
    pub height: u32,
    pub data: FrameData<'a>,
}

pub enum FrameData<'a> {
    Pixels { format: Format, bytes: &'a [u8] },
    Vaapi(BorrowedAvFrame<'a>),
}

pub struct PlayerStatus {
    pub title: String,
    pub streaming: bool,
    pub can_control: bool,
    pub authenticated: bool,
}

impl PlayerStatus {
    fn window_title(&self) -> String {
        // The Y700 edge script checks this state before the session's free-form detail.
        let page = if self.streaming {
            "串流中"
        } else {
            "设备"
        };
        format!("YOUYOU · {page} | {}   |   F8 菜单 · F11 全屏", self.title)
    }
}

pub trait Session {
    fn menu(&mut self, _: &WindowCanvas) -> Result<Option<Box<dyn Menu>>, String> {
        Ok(None)
    }
    fn gpu_video(&mut self, _: &WindowCanvas) -> Option<GpuVideo> {
        None
    }
    /// Return each decoded frame at most once. Audio is played by the session.
    fn frame(&mut self) -> Result<Option<Frame<'_>>, String>;
    fn input(&mut self, data: Value) -> Result<(), String>;
    /// Current description for the window title, or Err for a terminal failure.
    fn status(&mut self) -> Result<PlayerStatus, String>;
}

fn fit(output: (u32, u32), video: (u32, u32)) -> Rect {
    let (w, h) = (u64::from(output.0.max(1)), u64::from(output.1.max(1)));
    let (vw, vh) = (u64::from(video.0.max(1)), u64::from(video.1.max(1)));
    let (rw, rh) = if w * vh < h * vw {
        (w, (w * vh / vw).max(1))
    } else {
        ((h * vw / vh).max(1), h)
    };
    Rect::new(
        ((w - rw) / 2) as i32,
        ((h - rh) / 2) as i32,
        rw as u32,
        rh as u32,
    )
}

struct VideoTexture<'a> {
    texture: Texture<'a>,
    format: Format,
}
#[derive(Copy, Clone, PartialEq, Eq)]
enum VideoSource {
    None,
    Cpu,
    Gpu,
}

fn cpu_fallback(texture: Option<&VideoTexture<'_>>, size: &mut (u32, u32)) -> VideoSource {
    if let Some(texture) = texture {
        let info = texture.texture.query();
        *size = (info.width, info.height);
        VideoSource::Cpu
    } else {
        VideoSource::None
    }
}
fn upload<'a>(
    creator: &'a TextureCreator<WindowContext>,
    texture: &mut Option<VideoTexture<'a>>,
    frame: &Frame<'_>,
    access: TextureAccess,
) -> Result<(), String> {
    let FrameData::Pixels {
        format: pixel_format,
        bytes,
    } = &frame.data
    else {
        return Err("硬件视频帧不能作为像素上传".into());
    };
    if frame.width == 0 || frame.height == 0 || frame.width > 16384 || frame.height > 16384 {
        return Err(format!("无效画面尺寸 {} × {}", frame.width, frame.height));
    }
    let w = frame.width as usize;
    let h = frame.height as usize;
    let (format, pitch, size) = match pixel_format {
        Format::Rgba => (PixelFormatEnum::RGBA32, w * 4, w * h * 4),
        Format::Nv12Bt601 | Format::Nv12Bt709 => {
            if w % 2 != 0 || h % 2 != 0 {
                return Err("NV12 尺寸必须为偶数".into());
            }
            use sdl2::sys::SDL_YUV_CONVERSION_MODE::*;
            let mode = if *pixel_format == Format::Nv12Bt709 {
                SDL_YUV_CONVERSION_BT709
            } else {
                SDL_YUV_CONVERSION_BT601
            };
            // SDL2 has no safe binding for this process-global conversion mode.
            // All rendering and texture uploads are performed on this thread.
            unsafe {
                sdl2::sys::SDL_SetYUVConversionMode(mode);
            }
            (PixelFormatEnum::NV12, w, w * h * 3 / 2)
        }
    };
    if bytes.len() != size {
        return Err("解码画面数据长度不符".into());
    }
    let fresh = texture.as_ref().map_or(true, |t| {
        let q = t.texture.query();
        t.format != *pixel_format
            || (q.format, q.width, q.height) != (format, frame.width, frame.height)
    });
    if fresh {
        // Frames arrive in externally owned memory and are never locked via
        // SDL_LockTexture. A static texture does not need SDL's CPU shadow copy.
        let mut next = creator
            .create_texture(format, access, frame.width, frame.height)
            .map_err(|e| e.to_string())?;
        next.set_scale_mode(ScaleMode::Linear);
        *texture = Some(VideoTexture {
            texture: next,
            format: *pixel_format,
        });
    }
    texture
        .as_mut()
        .unwrap()
        .texture
        .update(None, bytes, pitch)
        .map_err(|e| e.to_string())
}

fn paint(
    canvas: &mut WindowCanvas,
    texture: Option<&VideoTexture>,
    view: Rect,
) -> Result<(), String> {
    canvas.set_draw_color(Color::RGB(10, 12, 16));
    canvas.clear();
    if let Some(t) = texture {
        canvas.copy(&t.texture, None, view)?;
    }
    Ok(())
}
fn pointer(session: &mut impl Session, point: (f32, f32)) -> Result<(), String> {
    session.input(absolute_pointer(point))
}

#[derive(PartialEq, Eq, Debug)]
pub enum Exit {
    Closed,
    LoggedOut,
}
pub fn run_authenticated(
    session: &mut impl Session,
    sdl: &sdl2::Sdl,
    canvas: &mut WindowCanvas,
) -> Result<Exit, String> {
    // SDL's synthetic mouse events would otherwise duplicate real finger taps.
    sdl2::hint::set("SDL_TOUCH_MOUSE_EVENTS", "0");
    sdl2::hint::set("SDL_MOUSE_TOUCH_EVENTS", "0");
    // The login UI uses SDL logical mouse coordinates. The player needs raw
    // window coordinates to map touches into each negotiated stream viewport.
    canvas.set_logical_size(0, 0).map_err(|e| e.to_string())?;
    play_loop(session, sdl, canvas)
}

fn send_touches(
    session: &mut impl Session,
    keys: &mut InputState,
    view: Rect,
    output: (u32, u32),
    samples: impl IntoIterator<Item = Touch>,
) -> Result<(), String> {
    for sample in samples {
        for packet in sample.packets(keys, view, output).into_iter().flatten() {
            session.input(packet)?;
        }
    }
    Ok(())
}

fn sync_touch_mode(
    session: &mut impl Session,
    keys: &mut InputState,
    edge: &mut EdgeSwipe,
    flags: i32,
) -> Result<(), String> {
    let desktop = flags & 8 != 0;
    if keys.desktop_mouse.is_some() != desktop {
        edge.cancel_pending();
        for release in keys.set_desktop_mouse(desktop) {
            session.input(release)?;
        }
    }
    Ok(())
}

fn play_loop(
    session: &mut impl Session,
    sdl: &sdl2::Sdl,
    canvas: &mut WindowCanvas,
) -> Result<Exit, String> {
    let mut menu = session.menu(canvas)?;
    let mut gpu = session.gpu_video(canvas);
    let mut video_source = VideoSource::None;
    let mut fullscreen_button = fullscreen::Control::default();
    let mut menu_flags = 0;
    let mut menu_pointers = MenuPointers::default();
    let mut edge_swipe = EdgeSwipe::default();
    let timer = sdl.timer()?;
    let mut menu_paint = Instant::now();
    let mut exit = Exit::Closed;
    let creator = canvas.texture_creator();
    let mut icons = icons::Icons::new(&creator);
    let mut texture = None;
    let mut spare_texture = None;
    let renderer = canvas.info();
    let mut rotate_textures =
        renderer.flags & sdl2::sys::SDL_RendererFlags::SDL_RENDERER_ACCELERATED as u32 != 0;
    let access = if rotate_textures && renderer.texture_formats.contains(&PixelFormatEnum::NV12) {
        TextureAccess::Static
    } else {
        TextureAccess::Streaming
    };
    let mut events = sdl.event_pump()?;
    let mut controllers = Controllers::new(sdl);
    if let Some(ui) = &mut menu {
        ui.tick(15, 0, 0, controllers.name().as_deref().unwrap_or(""))?;
    }
    let mut last_pad = None;
    let mut pad_started = false;
    let mut pad_poll = Instant::now();
    let mut pad_sent = Instant::now();
    let mut keys = InputState::default();
    let mut focused = true;
    let mut size = (1280, 720);
    let mut redraw = true;
    let mut title = String::new();
    let mut next_status = Instant::now();
    let mut streaming = false;
    let mut can_control = false;
    let mut rate_start = Instant::now();
    let mut last_frame = Instant::now();
    let mut next_check = last_frame;
    let mut presented_frames = 0u32;
    let result = (|| {
        'play: loop {
            if Instant::now() >= next_status {
                let status = session.status()?;
                if !status.authenticated {
                    exit = Exit::LoggedOut;
                    break 'play;
                }
                if can_control && (!status.can_control || !status.streaming) {
                    for data in keys.release() {
                        session.input(data)?;
                    }
                    if pad_started {
                        session.input(json!({"action":"gpd_disconnect","index":0}))?;
                    }
                    pad_started = false;
                    last_pad = None;
                }
                let next_control = status.can_control && status.streaming;
                if streaming != status.streaming {
                    edge_swipe = EdgeSwipe::default();
                    if menu_pointers.cancel() {
                        if let Some(ui) = &mut menu {
                            ui.tick(14, 0, 0, "")?;
                        }
                    }
                    menu_pointers = MenuPointers::default();
                } else if can_control != next_control {
                    edge_swipe.cancel_pending();
                }
                can_control = next_control;
                if streaming && !status.streaming {
                    texture = None;
                    spare_texture = None;
                    if let Some(gpu) = &mut gpu {
                        gpu.reset()?;
                    }
                    video_source = VideoSource::None;
                    redraw = true;
                }
                if streaming != status.streaming {
                    rate_start = Instant::now();
                    last_frame = rate_start;
                    next_check = last_frame;
                    presented_frames = 0;
                    if let Some(ui) = &mut menu {
                        ui.report_present_fps(-1.0);
                    }
                }
                streaming = status.streaming;
                let new = status.window_title();
                if new != title {
                    canvas
                        .window_mut()
                        .set_title(&new)
                        .map_err(|e| e.to_string())?;
                    title = new;
                }
                next_status = Instant::now() + Duration::from_millis(300);
            }
            let mut new_video_frame = false;
            if streaming {
                if let Some(frame) = session.frame()? {
                    let arrived = Instant::now();
                    // Poll shortly before the next expected frame. Input wakes SDL
                    // early; the ceiling also accommodates sudden source-rate changes.
                    let lead = arrived
                        .duration_since(last_frame)
                        .saturating_sub(Duration::from_millis(2))
                        .min(Duration::from_millis(8));
                    next_check = arrived + lead;
                    last_frame = arrived;
                    match &frame.data {
                        FrameData::Pixels { .. } => {
                            if rotate_textures {
                                // CPU uploads use their own pair of SDL textures.
                                // These are never the presenter's imported images.
                                std::mem::swap(&mut texture, &mut spare_texture);
                            }
                            if let Err(err) = upload(&creator, &mut texture, &frame, access) {
                                if !rotate_textures || spare_texture.is_none() {
                                    return Err(err);
                                }
                                rotate_textures = false;
                                texture = spare_texture.take();
                                upload(&creator, &mut texture, &frame, access)?;
                            }
                            if video_source == VideoSource::Gpu {
                                if let Some(gpu) = &mut gpu {
                                    gpu.reset()?;
                                }
                            }
                            video_source = VideoSource::Cpu;
                            size = (frame.width, frame.height);
                            new_video_frame = true;
                            redraw = true;
                        }
                        FrameData::Vaapi(native) => {
                            let gpu = gpu.as_mut().ok_or("硬件视频帧没有可用的显示器")?;
                            match gpu.upload(native)? {
                                gpu::Upload::Imported => {
                                    video_source = VideoSource::Gpu;
                                    size = (frame.width, frame.height);
                                    new_video_frame = true;
                                    redraw = true;
                                }
                                gpu::Upload::Busy => {} // Keep the last imported frame and size.
                                gpu::Upload::Fallback => {
                                    // Native failure has safely retired its images and
                                    // disabled new hardware output. Retain any CPU image
                                    // until the decoder supplies the next CPU frame.
                                    video_source = cpu_fallback(texture.as_ref(), &mut size);
                                    redraw = true;
                                }
                            }
                        }
                    }
                }
            }
            let output = canvas.output_size()?;
            let logical = canvas.window().size();
            let mut view = fit(output, size);
            if let Some(ui) = &mut menu {
                let flags = ui.tick(0, 0, 0, "")?;
                redraw |= flags != menu_flags
                    || (flags & 4 != 0 && menu_paint.elapsed() >= Duration::from_millis(100));
                menu_flags = flags;
                sync_touch_mode(session, &mut keys, &mut edge_swipe, flags)?;
                if flags & 1 != 0 {
                    edge_swipe.cancel_pending();
                }
            }
            if redraw {
                paint(
                    canvas,
                    if video_source == VideoSource::Cpu {
                        texture.as_ref()
                    } else {
                        None
                    },
                    view,
                )?;
                let mut gpu_drawn = false;
                if video_source == VideoSource::Gpu {
                    gpu_drawn = gpu.as_mut().ok_or("GPU 显示器已失效")?.draw(view)?;
                    if !gpu_drawn {
                        video_source = cpu_fallback(texture.as_ref(), &mut size);
                        view = fit(output, size);
                        paint(canvas, texture.as_ref(), view)?;
                        new_video_frame = false;
                    }
                }
                if let Some(ui) = &mut menu {
                    ui.draw()?;
                }
                if !streaming {
                    fullscreen_button.draw(canvas, &mut icons)?;
                }
                canvas.present();
                if new_video_frame {
                    presented_frames += 1;
                }
                menu_paint = Instant::now();
                redraw = false;
                if gpu_drawn && !gpu.as_mut().unwrap().presented()? {
                    video_source = cpu_fallback(texture.as_ref(), &mut size);
                    view = fit(output, size);
                    redraw = true;
                }
            }
            let elapsed = rate_start.elapsed();
            if streaming && elapsed >= Duration::from_secs(1) {
                if let Some(ui) = &mut menu {
                    ui.report_present_fps(f64::from(presented_frames) / elapsed.as_secs_f64());
                    // Refresh the visible statistics even when the source stops sending frames.
                    redraw |= last_frame.elapsed() >= Duration::from_millis(150);
                }
                rate_start = Instant::now();
                presented_frames = 0;
            }
            let local = |x: i32, y: i32| {
                (
                    x as f32 / logical.0.max(1) as f32,
                    y as f32 / logical.1.max(1) as f32,
                )
            };
            // A display swap can take an entire refresh period. Check for the
            // next decoded picture immediately afterwards, even with the drawer
            // open. Only sleep after a poll actually finds the video queue empty.
            let first = if new_video_frame {
                None
            } else {
                let timeout = if !streaming {
                    60
                } else if last_frame.elapsed() >= Duration::from_millis(150) {
                    16
                } else {
                    next_check
                        .saturating_duration_since(Instant::now())
                        .as_millis()
                        .max(2) as u32
                };
                events.wait_event_timeout(timeout)
            };
            for event in first.into_iter().chain(events.poll_iter()) {
                if event
                    .get_window_id()
                    .is_some_and(|id| id != canvas.window().id())
                {
                    continue;
                }
                // SDL's mouse-to-touch and touch-to-mouse hints can change with
                // the platform. Ignore emulated events even after a menu closes.
                match &event {
                    Event::FingerDown { touch_id: -1, .. }
                    | Event::FingerUp { touch_id: -1, .. }
                    | Event::FingerMotion { touch_id: -1, .. }
                    | Event::MouseMotion {
                        which: u32::MAX, ..
                    }
                    | Event::MouseButtonDown {
                        which: u32::MAX, ..
                    }
                    | Event::MouseButtonUp {
                        which: u32::MAX, ..
                    }
                    | Event::MouseWheel {
                        which: u32::MAX, ..
                    } => continue,
                    _ => {}
                }
                let previous_pad = controllers.id();
                let navigation = controllers.handle(&event);
                if controllers.id() != previous_pad {
                    if pad_started {
                        session.input(json!({"action":"gpd_disconnect","index":0}))?;
                    }
                    pad_started = false;
                    last_pad = None;
                    if let Some(ui) = &mut menu {
                        ui.tick(15, 0, 0, controllers.name().as_deref().unwrap_or(""))?;
                    }
                    redraw = true;
                }
                if fullscreen_button.handle_with_visibility(canvas, &event, !streaming)? {
                    redraw = true;
                    continue;
                }
                let touch = Touch::from_event(&event);
                let mut open_by_swipe = false;
                if let Some(sample) = touch.filter(|_| focused && streaming && menu.is_some()) {
                    // An earlier menu event in this same SDL batch may have changed the mode.
                    sync_touch_mode(session, &mut keys, &mut edge_swipe, menu_flags)?;
                    let eligible = menu_flags & 1 == 0;
                    match edge_swipe.route(sample, logical, eligible) {
                        Route::Hold => continue,
                        Route::OpenMenu => open_by_swipe = true,
                        Route::Replay(samples)
                            if can_control && video_source != VideoSource::None =>
                        {
                            send_touches(session, &mut keys, view, output, samples)?;
                        }
                        _ => {}
                    }
                }
                if let Some(ui) = &mut menu {
                    if matches!(
                        &event,
                        Event::Window {
                            win_event: WindowEvent::FocusLost,
                            ..
                        }
                    ) {
                        menu_pointers = MenuPointers::default();
                        ui.tick(14, 0, 0, "")?;
                    }
                    let captured = menu_flags & 1 != 0;
                    let pos = |p: (f32, f32)| {
                        // The native page owns the transform used to draw and hit-test it.
                        (
                            (p.0 * output.0 as f32).floor() as i32,
                            (p.1 * output.1 as f32).floor() as i32,
                        )
                    };
                    let pointer = match &event {
                        Event::MouseButtonDown {
                            which,
                            x,
                            y,
                            mouse_btn: sdl2::mouse::MouseButton::Left,
                            ..
                        } => Some((Pointer::Mouse(*which), Press::Down, local(*x, *y))),
                        Event::MouseButtonUp {
                            which,
                            x,
                            y,
                            mouse_btn: sdl2::mouse::MouseButton::Left,
                            ..
                        } => Some((Pointer::Mouse(*which), Press::Up, local(*x, *y))),
                        Event::MouseMotion { which, x, y, .. } => {
                            Some((Pointer::Mouse(*which), Press::Move, local(*x, *y)))
                        }
                        Event::FingerDown {
                            touch_id,
                            finger_id,
                            x,
                            y,
                            ..
                        } => Some((
                            Pointer::Finger(*touch_id, *finger_id),
                            Press::Down,
                            (*x, *y),
                        )),
                        Event::FingerUp {
                            touch_id,
                            finger_id,
                            x,
                            y,
                            ..
                        } => Some((Pointer::Finger(*touch_id, *finger_id), Press::Up, (*x, *y))),
                        Event::FingerMotion {
                            touch_id,
                            finger_id,
                            x,
                            y,
                            ..
                        } => Some((
                            Pointer::Finger(*touch_id, *finger_id),
                            Press::Move,
                            (*x, *y),
                        )),
                        _ => None,
                    };
                    if let Some((id, phase, point)) = pointer.filter(|_| focused) {
                        match menu_pointers.route(id, phase, captured) {
                            PointerRoute::Consume => continue,
                            PointerRoute::Menu(a) => {
                                let (x, y) = pos(point);
                                menu_flags = ui.tick(a, x, y, "")?;
                                redraw = true;
                                continue;
                            }
                            PointerRoute::Remote => {}
                        }
                    }
                    let mut xy = (0, 0);
                    let mut txt = "";
                    let action = match &event {
                        _ if open_by_swipe => Some(1),
                        Event::KeyDown {
                            scancode: Some(Scancode::F8),
                            repeat: false,
                            ..
                        } => Some(1),
                        _ if navigation == Some(PadAction::Menu) => Some(1),
                        Event::MouseWheel { y, .. } if captured => {
                            xy.1 = *y;
                            Some(13)
                        }
                        Event::TextInput { text, .. } if captured => {
                            txt = text;
                            Some(11)
                        }
                        Event::KeyDown {
                            scancode: Some(s),
                            repeat: false,
                            ..
                        } if captured => match s {
                            Scancode::Return | Scancode::KpEnter => Some(2),
                            Scancode::Escape => Some(3),
                            Scancode::Up => Some(4),
                            Scancode::Down => Some(5),
                            Scancode::Left => Some(6),
                            Scancode::Right => Some(7),
                            Scancode::Backspace => Some(12),
                            Scancode::Tab => Some(17),
                            Scancode::Num0 | Scancode::Kp0 => Some(16),
                            key if (Scancode::Num1 as i32..=Scancode::Num9 as i32)
                                .contains(&(*key as i32)) =>
                            {
                                xy.0 = *key as i32 - Scancode::Num1 as i32 + 1;
                                Some(16)
                            }
                            key if (Scancode::Kp1 as i32..=Scancode::Kp9 as i32)
                                .contains(&(*key as i32)) =>
                            {
                                xy.0 = *key as i32 - Scancode::Kp1 as i32 + 1;
                                Some(16)
                            }
                            _ => None,
                        },
                        _ if captured => match navigation {
                            Some(PadAction::Confirm) => Some(2),
                            Some(PadAction::Back) => Some(3),
                            Some(PadAction::Up) => Some(4),
                            Some(PadAction::Down) => Some(5),
                            Some(PadAction::Left) => Some(6),
                            Some(PadAction::Right) => Some(7),
                            _ => None,
                        },
                        _ => None,
                    };
                    if let Some(a) = action.filter(|_| focused) {
                        if a == 1 {
                            if menu_pointers.cancel() {
                                ui.tick(14, 0, 0, "")?;
                            }
                            edge_swipe.cancel_pending();
                            for d in keys.release() {
                                session.input(d)?;
                            }
                            if pad_started {
                                session.input(json!({"action":"gpd_disconnect","index":0}))?;
                                pad_started = false;
                                last_pad = None;
                            }
                        }
                        menu_flags = ui.tick(a, xy.0, xy.1, txt)?;
                        redraw = true;
                        continue;
                    }
                }
                match event {
                    Event::Quit { .. }
                    | Event::Window {
                        win_event: WindowEvent::Close,
                        ..
                    } => break 'play,
                    Event::Window {
                        win_event: WindowEvent::FocusLost,
                        ..
                    } => {
                        for data in keys.release() {
                            session.input(data)?;
                        }
                        if pad_started {
                            session.input(json!({"action":"gpd_disconnect","index":0}))?;
                            pad_started = false;
                            last_pad = None;
                        }
                        focused = false;
                        edge_swipe = EdgeSwipe::default();
                    }
                    Event::Window {
                        win_event: WindowEvent::FocusGained,
                        ..
                    } => {
                        focused = true;
                        redraw = true;
                    }
                    Event::Window { .. } => redraw = true,
                    _ if !focused
                        || !can_control
                        || video_source == VideoSource::None
                        || menu_flags & 1 != 0 => {}
                    Event::KeyDown {
                        scancode: Some(s),
                        repeat: false,
                        ..
                    } => {
                        if let Some(data) = vk(s).and_then(|k| keys.key(k, true)) {
                            session.input(data)?;
                        }
                    }
                    Event::KeyUp {
                        scancode: Some(s), ..
                    } => {
                        if let Some(data) = vk(s).and_then(|k| keys.key(k, false)) {
                            session.input(data)?;
                        }
                    }
                    Event::MouseMotion { which, x, y, .. } if which != u32::MAX => {
                        if let Some(p) =
                            image_point(view, output, local(x, y), !keys.buttons.is_empty())
                        {
                            pointer(session, p)?;
                        }
                    }
                    Event::MouseButtonDown {
                        which,
                        mouse_btn,
                        x,
                        y,
                        ..
                    } if which != u32::MAX => {
                        if let (Some(b), Some(p)) = (
                            button(mouse_btn),
                            image_point(view, output, local(x, y), false),
                        ) {
                            pointer(session, p)?;
                            if let Some(data) = keys.mouse(b, true) {
                                session.input(data)?;
                            }
                        }
                    }
                    Event::MouseButtonUp {
                        which, mouse_btn, ..
                    } if which != u32::MAX => {
                        if let Some(data) = button(mouse_btn).and_then(|b| keys.mouse(b, false)) {
                            session.input(data)?;
                        }
                    }
                    Event::MouseWheel {
                        precise_x,
                        precise_y,
                        direction,
                        ..
                    } => {
                        let scale = if direction == MouseWheelDirection::Flipped {
                            -120.0
                        } else {
                            120.0
                        };
                        session.input(json!({"action":"mouse_scroll","delta_x":(scale*precise_x).round() as i32,
                            "delta_y":(scale*precise_y).round() as i32}))?;
                    }
                    _ => send_touches(session, &mut keys, view, output, touch)?,
                }
            }
            if let Some(samples) = edge_swipe.expired(timer.ticks()) {
                if focused
                    && can_control
                    && video_source != VideoSource::None
                    && menu_flags & 1 == 0
                {
                    send_touches(session, &mut keys, view, output, samples)?;
                }
            }
            if focused
                && can_control
                && video_source != VideoSource::None
                && (menu.is_none() || menu_flags & 3 == 2)
                && pad_poll.elapsed() >= Duration::from_millis(8)
            {
                pad_poll = Instant::now();
                if let Some(p) = controllers.pad() {
                    let packet = pad_state(p);
                    if !pad_started {
                        session.input(json!({"action":"gpd_connect","index":0}))?;
                        pad_started = true;
                    }
                    if last_pad.as_ref() != Some(&packet)
                        || pad_sent.elapsed() >= Duration::from_millis(500)
                    {
                        session.input(packet.clone())?;
                        last_pad = Some(packet);
                        pad_sent = Instant::now();
                    }
                }
            }
        }
        Ok(exit)
    })();
    if streaming {
        let _ = canvas.window_mut().set_title("YOUYOU 远程");
    }
    for data in keys.release() {
        let _ = session.input(data);
    }
    if pad_started {
        let _ = session.input(json!({"action":"gpd_disconnect","index":0}));
    }
    if let Some(ui) = &mut menu {
        let disconnected = ui.disconnect();
        if result.is_ok() {
            disconnected?;
        }
    }
    result
}

/// Inject presses, focus loss and a close request into an SDL window.
/// No transport or desktop input device is involved.
fn check_sdl_input(sdl: &sdl2::Sdl) -> Result<(), String> {
    use sdl2::keyboard::Mod;
    use sdl2::mouse::MouseButton;
    struct Probe {
        events: Vec<Value>,
        frame: Option<Frame<'static>>,
        ready: bool,
    }
    impl Session for Probe {
        fn frame(&mut self) -> Result<Option<Frame<'_>>, String> {
            Ok(self.frame.take())
        }
        fn input(&mut self, data: Value) -> Result<(), String> {
            self.events.push(data);
            Ok(())
        }
        fn status(&mut self) -> Result<PlayerStatus, String> {
            Ok(PlayerStatus {
                title: "输入自检".into(),
                streaming: true,
                can_control: self.ready,
                authenticated: true,
            })
        }
    }
    let video = sdl.video()?;
    let event = sdl.event()?;
    for ready in [true, false] {
        let win = video
            .window("YOUYOU · 输入自检", 160, 120)
            .hidden()
            .build()
            .map_err(|e| e.to_string())?;
        let window_id = win.id();
        let mut canvas = win.into_canvas().build().map_err(|e| e.to_string())?;
        let mut probe = Probe {
            events: vec![],
            frame: Some(Frame {
                width: 4,
                height: 2,
                data: FrameData::Pixels {
                    format: Format::Rgba,
                    bytes: &[128; 4 * 2 * 4],
                },
            }),
            ready,
        };
        let key = |id| Event::KeyDown {
            timestamp: 0,
            window_id: id,
            keycode: None,
            scancode: Some(Scancode::W),
            keymod: Mod::NOMOD,
            repeat: false,
        };
        for e in [
            Event::Window {
                timestamp: 0,
                window_id,
                win_event: WindowEvent::FocusGained,
            },
            key(window_id.wrapping_add(1)), // An old lobby window cannot forward keys.
            key(window_id),
            Event::MouseButtonDown {
                timestamp: 0,
                window_id,
                which: 0,
                mouse_btn: MouseButton::Left,
                clicks: 1,
                x: 80,
                y: 5,
            }, // black bar
            Event::MouseButtonDown {
                timestamp: 0,
                window_id,
                which: 0,
                mouse_btn: MouseButton::Left,
                clicks: 1,
                x: 80,
                y: 60,
            },
            Event::Window {
                timestamp: 0,
                window_id,
                win_event: WindowEvent::FocusLost,
            },
            key(window_id), // A press after focus loss must not enter the stream.
            Event::Window {
                timestamp: 0,
                window_id,
                win_event: WindowEvent::Close,
            },
        ] {
            event.push_event(e)?;
        }
        play_loop(&mut probe, sdl, &mut canvas)?;
        let expected = if ready {
            vec![
                json!({"action":"kbd_press","key":87}),
                json!({"action":"mouse_move_absolute","abs_x":0.5,"abs_y":0.5}),
                json!({"action":"mouse_press","button":1}),
                json!({"action":"kbd_release","key":87}),
                json!({"action":"mouse_release","button":1}),
            ]
        } else {
            vec![]
        };
        if probe.events != expected {
            return Err(format!("SDL 输入自检失败 {:?}", probe.events));
        }
    }
    Ok(())
}

/// Exercise the production frame upload and presentation path. Use
/// SDL_VIDEODRIVER=dummy for headless runs; otherwise exercise the active driver.
pub fn self_test() -> Result<(), String> {
    let sdl = sdl2::init()?;
    let video = sdl.video()?;
    let win = video
        .window("YOUYOU · 显示自检", 12, 12)
        .hidden()
        .build()
        .map_err(|e| e.to_string())?;
    let mut canvas = win.into_canvas().build().map_err(|e| e.to_string())?;
    let creator = canvas.texture_creator();
    let mut tex = None;
    let size = canvas.output_size()?;
    let r = fit(size, (2, 2));
    for (fmt, buf, expected) in [
        (Format::Rgba, [37, 67, 117, 255].repeat(4), [37, 67, 117]),
        (
            Format::Nv12Bt601,
            vec![81, 81, 81, 81, 90, 240],
            [255, 0, 0],
        ),
        (
            Format::Nv12Bt709,
            vec![63, 63, 63, 63, 102, 240],
            [255, 0, 0],
        ),
    ] {
        upload(
            &creator,
            &mut tex,
            &Frame {
                width: 2,
                height: 2,
                data: FrameData::Pixels {
                    format: fmt,
                    bytes: &buf,
                },
            },
            TextureAccess::Streaming,
        )?;
        paint(&mut canvas, tex.as_ref(), r)?;
        let pixel = canvas.read_pixels(
            Rect::new(r.center().x(), r.center().y(), 1, 1),
            PixelFormatEnum::RGBA32,
        )?;
        if pixel.len() < 4
            || expected
                .iter()
                .zip(&pixel)
                .any(|(&a, &b)| u8::abs_diff(a, b) > 8)
        {
            return Err(format!("SDL {fmt:?} 颜色自检失败，得到 {pixel:?}"));
        }
    }
    check_sdl_input(&sdl)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn window_title_identifies_only_the_live_session() {
        let mut status = PlayerStatus {
            title: "YOUYOU · 串流中 | 来自会话的状态".into(),
            streaming: false,
            can_control: true,
            authenticated: true,
        };
        assert!(!status.window_title().starts_with("YOUYOU · 串流中 | "));
        status.streaming = true;
        status.can_control = false;
        assert!(status.window_title().starts_with("YOUYOU · 串流中 | "));
    }

    #[test]
    fn native_y700_landscape_and_portrait_letterbox() {
        assert_eq!(fit((3040, 1904), (1280, 720)), Rect::new(0, 97, 3040, 1710));
        assert_eq!(
            fit((1200, 1920), (1920, 1080)),
            Rect::new(0, 622, 1200, 675)
        );
    }
    #[test]
    fn headless_display_round_trip() {
        std::env::set_var("SDL_VIDEODRIVER", "dummy");
        self_test().unwrap();
    }
}
