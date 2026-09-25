use ruffle_core::events::{
    KeyDescriptor, KeyLocation, LogicalKey, MouseButton, NamedKey, PhysicalKey, PlayerEvent,
};
use ruffle_core::tag_utils::SwfMovie;
use ruffle_core::{FloatDuration, Player, PlayerBuilder};
use std::ffi::c_char;
use std::sync::{Arc, Mutex};
use swf::{Compression, Tag};

mod vita_renderer;
use vita_renderer::{renderer_stats_snapshot, VitaRenderer};

static BRIDGE_VERSION: &[u8] = b"FlashVita Ruffle bridge 0.1 / Ruffle 0.6.0\0";

#[repr(C)]
pub struct FlashVitaRuffleProbe {
    version: u8,
    compression: u8,
    has_avm1: u8,
    has_avm2: u8,
    frame_count: u16,
    reserved: u16,
    tag_count: u32,
    stage_width: i32,
    stage_height: i32,
    frame_rate: f32,
}

#[repr(C)]
pub struct FlashVitaRendererStats {
    frames: u64,
    colored_draws: u64,
    textured_draws: u64,
    bitmap_uploads: u64,
    lines: u64,
    gradient_skips: u64,
    missing_bitmaps: u64,
    mask_ops: u64,
    blends: u64,
    stage3d: u64,
}

struct HeadlessPlayer {
    player: Arc<Mutex<Player>>,
}

fn key_descriptor(key: i32) -> Option<KeyDescriptor> {
    let (physical_key, logical_key, key_location) = match key {
        1 => (PhysicalKey::Space, LogicalKey::Character(' '), KeyLocation::Standard),
        2 => (PhysicalKey::Enter, LogicalKey::Named(NamedKey::Enter), KeyLocation::Standard),
        3 => (PhysicalKey::Escape, LogicalKey::Named(NamedKey::Escape), KeyLocation::Standard),
        4 => (PhysicalKey::ArrowUp, LogicalKey::Named(NamedKey::ArrowUp), KeyLocation::Standard),
        5 => (PhysicalKey::ArrowDown, LogicalKey::Named(NamedKey::ArrowDown), KeyLocation::Standard),
        6 => (PhysicalKey::ArrowLeft, LogicalKey::Named(NamedKey::ArrowLeft), KeyLocation::Standard),
        7 => (PhysicalKey::ArrowRight, LogicalKey::Named(NamedKey::ArrowRight), KeyLocation::Standard),
        8 => (PhysicalKey::KeyZ, LogicalKey::Character('z'), KeyLocation::Standard),
        9 => (PhysicalKey::KeyX, LogicalKey::Character('x'), KeyLocation::Standard),
        10 => (PhysicalKey::KeyC, LogicalKey::Character('c'), KeyLocation::Standard),
        11 => (PhysicalKey::KeyA, LogicalKey::Character('a'), KeyLocation::Standard),
        12 => (PhysicalKey::KeyS, LogicalKey::Character('s'), KeyLocation::Standard),
        13 => (PhysicalKey::KeyD, LogicalKey::Character('d'), KeyLocation::Standard),
        14 => (PhysicalKey::KeyQ, LogicalKey::Character('q'), KeyLocation::Standard),
        15 => (PhysicalKey::KeyW, LogicalKey::Character('w'), KeyLocation::Standard),
        16 => (PhysicalKey::KeyE, LogicalKey::Character('e'), KeyLocation::Standard),
        17 => (PhysicalKey::ShiftLeft, LogicalKey::Named(NamedKey::Shift), KeyLocation::Left),
        18 => (PhysicalKey::ControlLeft, LogicalKey::Named(NamedKey::Control), KeyLocation::Left),
        _ => return None,
    };
    Some(KeyDescriptor {
        physical_key,
        logical_key,
        key_location,
    })
}

fn compression_id(compression: Compression) -> u8 {
    match compression {
        Compression::None => 0,
        Compression::Zlib => 1,
        Compression::Lzma => 2,
    }
}

fn probe_bytes(data: &[u8]) -> Result<FlashVitaRuffleProbe, ()> {
    let swf_buf = swf::decompress_swf(data).map_err(|_| ())?;
    let parsed = swf::parse_swf(&swf_buf).map_err(|_| ())?;
    let mut has_avm1 = false;
    let mut has_avm2 = false;

    for tag in &parsed.tags {
        match tag {
            Tag::DoAction(_) | Tag::DoInitAction { .. } => has_avm1 = true,
            Tag::DoAbc(_) | Tag::DoAbc2(_) => has_avm2 = true,
            _ => {}
        }
    }

    let stage = parsed.header.stage_size();
    Ok(FlashVitaRuffleProbe {
        version: parsed.header.version(),
        compression: compression_id(parsed.header.compression()),
        has_avm1: u8::from(has_avm1),
        has_avm2: u8::from(has_avm2),
        frame_count: parsed.header.num_frames(),
        reserved: 0,
        tag_count: parsed.tags.len().try_into().unwrap_or(u32::MAX),
        stage_width: stage.width().to_pixels().round() as i32,
        stage_height: stage.height().to_pixels().round() as i32,
        frame_rate: parsed.header.frame_rate().to_f32(),
    })
}

unsafe fn input_slice<'a>(data: *const u8, len: usize) -> Option<&'a [u8]> {
    if data.is_null() || len == 0 {
        return None;
    }
    Some(std::slice::from_raw_parts(data, len))
}

#[no_mangle]
pub extern "C" fn flashvita_ruffle_bridge_version() -> *const c_char {
    BRIDGE_VERSION.as_ptr().cast()
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_renderer_stats(out: *mut FlashVitaRendererStats) -> i32 {
    if out.is_null() {
        return -1;
    }
    let stats = renderer_stats_snapshot();
    out.write(FlashVitaRendererStats {
        frames: stats.frames,
        colored_draws: stats.colored_draws,
        textured_draws: stats.textured_draws,
        bitmap_uploads: stats.bitmap_uploads,
        lines: stats.lines,
        gradient_skips: stats.gradient_skips,
        missing_bitmaps: stats.missing_bitmaps,
        mask_ops: stats.mask_ops,
        blends: stats.blends,
        stage3d: stats.stage3d,
    });
    0
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_probe(
    data: *const u8,
    len: usize,
    out: *mut FlashVitaRuffleProbe,
) -> i32 {
    let Some(data) = input_slice(data, len) else {
        return -1;
    };
    if out.is_null() {
        return -2;
    }
    let Ok(probe) = probe_bytes(data) else {
        return -3;
    };
    out.write(probe);
    0
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_headless_create(
    data: *const u8,
    len: usize,
    out: *mut FlashVitaRuffleProbe,
) -> *mut HeadlessPlayer {
    let Some(data) = input_slice(data, len) else {
        return std::ptr::null_mut();
    };
    let Ok(probe) = probe_bytes(data) else {
        return std::ptr::null_mut();
    };
    let Ok(movie) = SwfMovie::from_data(data, "file:///FlashVita/game.swf".into(), None, None) else {
        return std::ptr::null_mut();
    };

    let player = PlayerBuilder::new()
        .with_movie(movie)
        .with_autoplay(true)
        .with_viewport_dimensions(960, 544, 1.0)
        .with_renderer(VitaRenderer::new(ruffle_core::ViewportDimensions {
            width: 960,
            height: 544,
            scale_factor: 1.0,
        }))
        .build();

    if !out.is_null() {
        out.write(probe);
    }

    Box::into_raw(Box::new(HeadlessPlayer { player }))
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_headless_tick(
    handle: *mut HeadlessPlayer,
    dt_ms: f64,
) -> i32 {
    let Some(handle) = handle.as_mut() else {
        return -1;
    };
    let Ok(mut player) = handle.player.lock() else {
        return -2;
    };
    player.tick(FloatDuration::from_millis(dt_ms.max(0.0)));
    // Keep rendering coupled to the tick on Vita. Gating on needs_render()
    // caused severe pacing regressions on hardware with some SWFs.
    player.render();
    1
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_key_event(
    handle: *mut HeadlessPlayer,
    key: i32,
    down: u8,
) -> i32 {
    let Some(handle) = handle.as_mut() else {
        return -1;
    };
    let Some(key) = key_descriptor(key) else {
        return -2;
    };
    let Ok(mut player) = handle.player.lock() else {
        return -3;
    };
    let event = if down != 0 {
        PlayerEvent::KeyDown { key }
    } else {
        PlayerEvent::KeyUp { key }
    };
    player.handle_event(event);
    0
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_mouse_move(
    handle: *mut HeadlessPlayer,
    x: f64,
    y: f64,
) -> i32 {
    let Some(handle) = handle.as_mut() else {
        return -1;
    };
    let Ok(mut player) = handle.player.lock() else {
        return -2;
    };
    player.handle_event(PlayerEvent::MouseMove { x, y });
    0
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_mouse_button(
    handle: *mut HeadlessPlayer,
    x: f64,
    y: f64,
    down: u8,
) -> i32 {
    let Some(handle) = handle.as_mut() else {
        return -1;
    };
    let Ok(mut player) = handle.player.lock() else {
        return -2;
    };
    let event = if down != 0 {
        PlayerEvent::MouseDown {
            x,
            y,
            button: MouseButton::Left,
            index: None,
        }
    } else {
        PlayerEvent::MouseUp {
            x,
            y,
            button: MouseButton::Left,
        }
    };
    player.handle_event(event);
    0
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_headless_destroy(handle: *mut HeadlessPlayer) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}
