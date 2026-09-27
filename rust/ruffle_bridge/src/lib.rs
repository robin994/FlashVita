use ruffle_core::backend::ui::{
    DialogResultFuture, FileFilter, FontDefinition, FullscreenError, LanguageIdentifier,
    MouseCursor, MultiDialogResultFuture, NullUiBackend, UiBackend,
};
use ruffle_core::events::{
    ImeEvent, KeyDescriptor, KeyLocation, LogicalKey, MouseButton, NamedKey, PhysicalKey,
    PlayerEvent, TextControlCode,
};
use ruffle_core::font::FontQuery;
use ruffle_core::tag_utils::SwfMovie;
use ruffle_core::{FloatDuration, Player, PlayerBuilder};
#[cfg(target_os = "vita")]
use ruffle_core::PlayerMode;
use std::cell::RefCell;
use std::ffi::{c_char, CStr};
#[cfg(target_os = "vita")]
use std::ffi::c_void;
#[cfg(target_os = "vita")]
use std::io::Read;
#[cfg(target_os = "vita")]
use std::time::Duration;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use swf::Compression;
use url::Url;

mod vita_navigator;
mod vita_renderer;
#[cfg(target_os = "vita")]
mod vita_audio;
#[cfg(target_os = "vita")]
mod vita_allocator;
use vita_navigator::{VitaLogBackend, VitaNavigatorBackend, VitaNavigatorState};
use vita_renderer::{renderer_stats_snapshot, VitaRenderer};
#[cfg(target_os = "vita")]
use vita_audio::VitaAudioBackend;

#[cfg(target_os = "vita")]
#[global_allocator]
static VITA_GLOBAL_ALLOCATOR: vita_allocator::VitaGlobalAllocator =
    vita_allocator::VitaGlobalAllocator;

static BRIDGE_VERSION: &[u8] = b"FlashVita Ruffle bridge 0.1 / Ruffle 0.6.0\0";

#[cfg(target_os = "vita")]
unsafe extern "C" {
    fn flashvita_vita_memblock_alloc(bytes: usize, uid_out: *mut i32) -> *mut c_void;
    fn flashvita_vita_perf_logging_enabled() -> i32;
    fn flashvita_vita_memblock_free(uid: i32, base: *mut c_void) -> i32;
}

pub(crate) fn perf_logging_enabled() -> bool {
    #[cfg(target_os = "vita")]
    {
        unsafe { flashvita_vita_perf_logging_enabled() != 0 }
    }
    #[cfg(not(target_os = "vita"))]
    {
        false
    }
}

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
    bitmap_partial_uploads: u64,
    bitmap_uploaded_bytes: u64,
    lines: u64,
    gradient_skips: u64,
    missing_bitmaps: u64,
    mask_ops: u64,
    blends: u64,
    stage3d: u64,
    transformed_vertices: u64,
    parallel_draws: u64,
    parallel_batches: u64,
    parallel_jobs: u64,
    color_submissions: u64,
    prepass_us: u64,
    submit_us: u64,
}

#[repr(C)]
pub struct FlashVitaTextInputInfo {
    multiline: u8,
    password: u8,
    reserved: [u8; 2],
    max_length: u32,
    initial_text_bytes: u32,
}

#[derive(Default)]
struct VitaUiState {
    keyboard_requested: AtomicBool,
}

struct VitaUiBackend {
    inner: NullUiBackend,
    state: Arc<VitaUiState>,
}

impl VitaUiBackend {
    fn new(state: Arc<VitaUiState>) -> Self {
        Self {
            inner: NullUiBackend::new(),
            state,
        }
    }
}

impl UiBackend for VitaUiBackend {
    fn mouse_visible(&self) -> bool {
        self.inner.mouse_visible()
    }

    fn set_mouse_visible(&mut self, visible: bool) {
        self.inner.set_mouse_visible(visible);
    }

    fn set_mouse_cursor(&mut self, cursor: MouseCursor) {
        self.inner.set_mouse_cursor(cursor);
    }

    fn clipboard_content(&mut self) -> String {
        self.inner.clipboard_content()
    }

    fn set_clipboard_content(&mut self, content: String) {
        self.inner.set_clipboard_content(content);
    }

    fn set_fullscreen(&mut self, is_full: bool) -> Result<(), FullscreenError> {
        self.inner.set_fullscreen(is_full)
    }

    fn display_root_movie_download_failed_message(
        &self,
        invalid_swf: bool,
        fetched_error: String,
    ) {
        self.inner
            .display_root_movie_download_failed_message(invalid_swf, fetched_error);
    }

    fn message(&self, message: &str) {
        self.inner.message(message);
    }

    fn open_virtual_keyboard(&self) {
        self.state.keyboard_requested.store(true, Ordering::Release);
    }

    fn close_virtual_keyboard(&self) {
        self.state.keyboard_requested.store(false, Ordering::Release);
    }

    fn language(&self) -> LanguageIdentifier {
        self.inner.language()
    }

    fn display_unsupported_video(&self, url: Url) {
        self.inner.display_unsupported_video(url);
    }

    fn load_device_font(&self, query: &FontQuery, register: &mut dyn FnMut(FontDefinition)) {
        self.inner.load_device_font(query, register);
    }

    fn sort_device_fonts(
        &self,
        query: &FontQuery,
        register: &mut dyn FnMut(FontDefinition),
    ) -> Vec<FontQuery> {
        self.inner.sort_device_fonts(query, register)
    }

    fn display_file_open_dialog(&mut self, filters: Vec<FileFilter>) -> Option<DialogResultFuture> {
        self.inner.display_file_open_dialog(filters)
    }

    fn display_file_open_dialog_multiple(
        &mut self,
        filters: Vec<FileFilter>,
    ) -> Option<MultiDialogResultFuture> {
        self.inner.display_file_open_dialog_multiple(filters)
    }

    fn display_file_save_dialog(
        &mut self,
        file_name: String,
        title: String,
    ) -> Option<DialogResultFuture> {
        self.inner.display_file_save_dialog(file_name, title)
    }

    fn close_file_dialog(&mut self) {
        self.inner.close_file_dialog();
    }
}

struct HeadlessPlayer {
    player: Arc<Mutex<Player>>,
    ui_state: Arc<VitaUiState>,
    navigator_state: Rc<RefCell<VitaNavigatorState>>,
    ssf2_compat_pending: bool,
    profile_ticks: u32,
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
        19 => (PhysicalKey::KeyO, LogicalKey::Character('o'), KeyLocation::Standard),
        20 => (PhysicalKey::KeyP, LogicalKey::Character('p'), KeyLocation::Standard),
        21 => (
            PhysicalKey::Backspace,
            LogicalKey::Named(NamedKey::Backspace),
            KeyLocation::Standard,
        ),
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

fn scan_tag_headers(data: &[u8]) -> (u32, bool, bool) {
    let mut offset = 0usize;
    let mut tag_count = 0u32;
    let mut has_avm1 = false;
    let mut has_avm2 = false;

    while offset + 2 <= data.len() {
        let header = u16::from_le_bytes([data[offset], data[offset + 1]]);
        offset += 2;
        let code = header >> 6;
        let mut length = (header & 0x3f) as usize;
        if length == 0x3f {
            if offset + 4 > data.len() {
                break;
            }
            length = u32::from_le_bytes([
                data[offset],
                data[offset + 1],
                data[offset + 2],
                data[offset + 3],
            ]) as usize;
            offset += 4;
        }

        tag_count = tag_count.saturating_add(1);
        match code {
            12 | 59 => has_avm1 = true, // DoAction / DoInitAction
            72 | 82 => has_avm2 = true, // DoABC / DoABC2
            0 => break,                 // End
            _ => {}
        }

        let Some(next) = offset.checked_add(length) else {
            break;
        };
        if next > data.len() {
            break;
        }
        offset = next;
    }

    (tag_count, has_avm1, has_avm2)
}

fn probe_swf_buf(swf_buf: &swf::SwfBuf) -> FlashVitaRuffleProbe {
    let (tag_count, has_avm1, tag_has_avm2) = scan_tag_headers(&swf_buf.data);
    let has_avm2 = swf_buf.header.is_action_script_3() || tag_has_avm2;
    let has_marker = |needle: &[u8]| {
        !needle.is_empty()
            && swf_buf
                .data
                .windows(needle.len())
                .any(|window| window == needle)
    };
    let is_air = has_marker(b"flash.filesystem")
        || has_marker(b"NativeApplication")
        || has_marker(b"NativeWindow");

    let stage = swf_buf.header.stage_size();
    FlashVitaRuffleProbe {
        version: swf_buf.header.version(),
        compression: compression_id(swf_buf.header.compression()),
        has_avm1: u8::from(has_avm1),
        has_avm2: u8::from(has_avm2),
        frame_count: swf_buf.header.num_frames(),
        // bit 0 is used by the Vita frontend to keep AIR movies in local mode.
        reserved: u16::from(is_air),
        tag_count,
        stage_width: stage.width().to_pixels().round() as i32,
        stage_height: stage.height().to_pixels().round() as i32,
        frame_rate: swf_buf.header.frame_rate().to_f32(),
    }
}

fn probe_bytes(data: &[u8]) -> Result<FlashVitaRuffleProbe, ()> {
    let swf_buf = swf::decompress_swf(data).map_err(|_| ())?;
    Ok(probe_swf_buf(&swf_buf))
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
        bitmap_partial_uploads: stats.bitmap_partial_uploads,
        bitmap_uploaded_bytes: stats.bitmap_uploaded_bytes,
        lines: stats.lines,
        gradient_skips: stats.gradient_skips,
        missing_bitmaps: stats.missing_bitmaps,
        mask_ops: stats.mask_ops,
        blends: stats.blends,
        stage3d: stats.stage3d,
        transformed_vertices: stats.transformed_vertices,
        parallel_draws: stats.parallel_draws,
        parallel_batches: stats.parallel_batches,
        parallel_jobs: stats.parallel_jobs,
        color_submissions: stats.color_submissions,
        prepass_us: stats.prepass_us,
        submit_us: stats.submit_us,
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
    swf_path: *const c_char,
    web_movie_url: *const c_char,
    air_movie_url: *const c_char,
    cache_root: *const c_char,
    out: *mut FlashVitaRuffleProbe,
) -> *mut HeadlessPlayer {
    if swf_path.is_null()
        || web_movie_url.is_null()
        || air_movie_url.is_null()
        || cache_root.is_null()
    {
        return std::ptr::null_mut();
    }
    let Ok(swf_path) = CStr::from_ptr(swf_path).to_str() else {
        return std::ptr::null_mut();
    };
    let Ok(web_movie_url) = CStr::from_ptr(web_movie_url).to_str() else {
        return std::ptr::null_mut();
    };
    let Ok(air_movie_url) = CStr::from_ptr(air_movie_url).to_str() else {
        return std::ptr::null_mut();
    };
    let Ok(cache_root) = CStr::from_ptr(cache_root).to_str() else {
        return std::ptr::null_mut();
    };

    let Ok(compressed_len) = vita_navigator::native_file_size(swf_path) else {
        return std::ptr::null_mut();
    };

    #[cfg(target_os = "vita")]
    let reserved_memblock = {
        const VITA_MEMBLOCK_THRESHOLD: usize = 8 * 1024 * 1024;
        let mut header = [0u8; 8];
        let declared_len = vita_navigator::NativeFileReader::open(swf_path)
            .and_then(|mut reader| {
                reader
                    .read_exact(&mut header)
                    .map_err(|error| error.to_string())?;
                Ok(u32::from_le_bytes([
                    header[4], header[5], header[6], header[7],
                ]) as usize)
            })
            .unwrap_or(0);

        if declared_len >= VITA_MEMBLOCK_THRESHOLD {
            let mut uid = -1;
            let base = flashvita_vita_memblock_alloc(declared_len, &mut uid).cast::<u8>();
            if !base.is_null() {
                vita_navigator::log_line(&format!(
                    "swf_memblock reserve_ok bytes={} uid={}",
                    declared_len, uid
                ));
                Some((uid, base, declared_len))
            } else {
                vita_navigator::log_line(&format!(
                    "swf_memblock reserve_failed bytes={} result={}",
                    declared_len, uid
                ));
                None
            }
        } else {
            None
        }
    };

    let Ok(reader) = vita_navigator::NativeFileReader::open(swf_path) else {
        #[cfg(target_os = "vita")]
        if let Some((uid, base, _)) = reserved_memblock {
            flashvita_vita_memblock_free(uid, base.cast());
        }
        return std::ptr::null_mut();
    };
    let Ok(swf_buf) = swf::decompress_swf(reader) else {
        #[cfg(target_os = "vita")]
        if let Some((uid, base, _)) = reserved_memblock {
            flashvita_vita_memblock_free(uid, base.cast());
        }
        return std::ptr::null_mut();
    };
    let probe = probe_swf_buf(&swf_buf);
    let is_air = (probe.reserved & 1) != 0;
    let movie_url = if is_air {
        air_movie_url
    } else {
        web_movie_url
    };
    #[cfg(target_os = "vita")]
    let movie = if let Some((uid, base, capacity)) = reserved_memblock {
        vita_navigator::log_line(&format!(
            "swf_memblock commit bytes={} capacity={} uid={}",
            swf_buf.data.len(),
            capacity,
            uid
        ));
        SwfMovie::from_swf_buf_with_reserved_vita_memblock(
            swf_buf,
            compressed_len,
            movie_url.to_owned(),
            None,
            None,
            uid,
            base,
            capacity,
        )
    } else {
        SwfMovie::from_swf_buf(
            swf_buf,
            compressed_len,
            movie_url.to_owned(),
            None,
            None,
        )
    };

    #[cfg(not(target_os = "vita"))]
    let movie = SwfMovie::from_swf_buf(
        swf_buf,
        compressed_len,
        movie_url.to_owned(),
        None,
        None,
    );

    let ui_state = Arc::new(VitaUiState::default());
    let navigator_state = Rc::new(RefCell::new(VitaNavigatorState::new()));
    let Ok(navigator) = VitaNavigatorBackend::new(
        movie_url,
        cache_root.to_owned(),
        navigator_state.clone(),
    ) else {
        return std::ptr::null_mut();
    };
    let mut player_builder = PlayerBuilder::new()
        .with_movie(movie)
        .with_autoplay(true)
        .with_viewport_dimensions(960, 544, 1.0)
        .with_ui(VitaUiBackend::new(ui_state.clone()))
        .with_log(VitaLogBackend::new())
        .with_navigator(navigator)
        .with_renderer(VitaRenderer::new(ruffle_core::ViewportDimensions {
            width: 960,
            height: 544,
            scale_factor: 1.0,
        }));
    #[cfg(target_os = "vita")]
    {
        // AVM1 titles use release semantics for the lowest interpreter overhead.
        // Keep debugger mode only for AVM2 titles where uncaught bootstrap errors
        // are otherwise extremely difficult to diagnose on hardware.
        if probe.has_avm2 != 0 {
            player_builder = player_builder.with_player_mode(PlayerMode::Debug);
            vita_navigator::log_line("diagnostic player_mode=debug avm=avm2");
        } else {
            player_builder = player_builder.with_player_mode(PlayerMode::Release);
            vita_navigator::log_line("diagnostic player_mode=release avm=avm1");
        }
        // Large desktop-era SWFs can legitimately spend far longer than the
        // desktop default 15 seconds in their first AVM2 bootstrap on Vita.
        // Keep a watchdog, but give slow initialization enough headroom.
        player_builder = player_builder.with_max_execution_duration(Duration::from_secs(120));
        vita_navigator::log_line("diagnostic avm_execution_timeout_s=120");
    }
    #[cfg(target_os = "vita")]
    if let Some(audio) = VitaAudioBackend::new() {
        player_builder = player_builder.with_audio(audio);
    }
    let player = player_builder.build();
    #[cfg(target_os = "vita")]
    if let Ok(mut player_guard) = player.lock() {
        // Vita has no stationary desktop pointer. Avoid hit-testing the whole
        // display list until analog/touch input or a mouse click arrives.
        player_guard.set_mouse_in_stage(false);
    }

    if !out.is_null() {
        out.write(probe);
    }

    Box::into_raw(Box::new(HeadlessPlayer {
        player,
        ui_state,
        navigator_state,
        ssf2_compat_pending: cache_root.ends_with("/SSF2"),
        profile_ticks: 0,
    }))
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_headless_update(
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
    handle.profile_ticks = handle.profile_ticks.wrapping_add(1);
    if handle.profile_ticks % 30 == 0 && perf_logging_enabled() {
        let p = player.vita_frame_profile();
        vita_navigator::log_line(&format!(
            "avm_frame preload_us={} avm2_us={} avm1_us={} avm1_actions={} get_member={} set_member={} get_var={} set_var={} call_fn={} call_method={} push={} queued_us={} queued_actions={} mouse_us={} mouse_drag_us={} mouse_state_us={} mouse_pick_tests={} mouse_events={} mouse_actions={} gc_us={} update_calls={} run_frame_us={} timers_us={} timer_actions={} sockets_us={} net_us={} stream_us={} stream_body_us={} stream_active={} stream_queued={} audio_tick_us={} audio_us={} local_us={} callbacks_us={}",
            p.preload_us,
            p.avm2_us,
            p.avm1_us,
            p.avm1_actions,
            p.avm1_get_member,
            p.avm1_set_member,
            p.avm1_get_variable,
            p.avm1_set_variable,
            p.avm1_call_function,
            p.avm1_call_method,
            p.avm1_push,
            p.queued_actions_us,
            p.queued_actions,
            p.mouse_us,
            p.mouse_drag_us,
            p.mouse_state_us,
            p.mouse_pick_tests,
            p.mouse_event_dispatches,
            p.mouse_actions,
            p.gc_us,
            p.update_calls,
            p.run_frame_total_us,
            p.timers_us,
            p.timer_actions,
            p.sockets_us,
            p.net_us,
            p.stream_us,
            p.stream_body_us,
            p.stream_active,
            p.stream_queued,
            p.audio_tick_us,
            p.audio_us,
            p.local_connections_us,
            p.callbacks_us,
        ));
        vita_navigator::log_line(&format!(
            "avm_q items={} normal={} init={} construct={} method={} notify={} push={} gm={} cm={} mcr={} pd_blocks={} pd_push={} pd_mt={}",
            p.queued_items,
            p.queued_normal,
            p.queued_initialize,
            p.queued_construct,
            p.queued_method,
            p.queued_notify,
            p.queued_push,
            p.queued_get_member,
            p.queued_call_method,
            p.queued_movieclip_refs,
            p.queued_predecode_blocks,
            p.queued_predecode_pushes,
            p.queued_predecode_parallel,
        ));
    }
    if handle.ssf2_compat_pending
        && player.set_avm2_static_bool(
            "com.mcleodgaming.ssf2.util.ResourceManager",
            "multimode",
            false,
        )
    {
        vita_navigator::log_line("compat ssf2 multimode=false");
        handle.ssf2_compat_pending = false;
    }
    drop(player);
    handle.navigator_state.borrow_mut().pump();
    1
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_headless_tick(
    handle: *mut HeadlessPlayer,
    dt_ms: f64,
) -> i32 {
    let update = flashvita_ruffle_headless_update(handle, dt_ms);
    if update < 0 {
        return update;
    }
    flashvita_ruffle_headless_render(handle)
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_headless_render(handle: *mut HeadlessPlayer) -> i32 {
    let Some(handle) = handle.as_mut() else {
        return -1;
    };
    let Ok(mut player) = handle.player.lock() else {
        return -2;
    };
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
    player.set_mouse_in_stage(true);
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
    player.set_mouse_in_stage(true);
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
pub unsafe extern "C" fn flashvita_ruffle_mouse_leave(handle: *mut HeadlessPlayer) -> i32 {
    let Some(handle) = handle.as_mut() else {
        return -1;
    };
    let Ok(mut player) = handle.player.lock() else {
        return -2;
    };
    player.set_mouse_in_stage(false);
    player.handle_event(PlayerEvent::MouseLeave);
    0
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_text_input_info(
    handle: *mut HeadlessPlayer,
    out: *mut FlashVitaTextInputInfo,
    initial_text: *mut u8,
    initial_text_capacity: usize,
) -> i32 {
    let Some(handle) = handle.as_mut() else {
        return -1;
    };
    if out.is_null() {
        return -2;
    }
    if !handle
        .ui_state
        .keyboard_requested
        .load(Ordering::Acquire)
    {
        return 0;
    }

    let Ok(mut player) = handle.player.lock() else {
        return -3;
    };

    let mut text = String::new();
    let mut multiline = false;
    let mut password = false;
    let mut max_length = 0u32;
    let found = player.mutate_with_update_context(|context| {
        let Some(field) = context.focus_tracker.get_as_edit_text() else {
            return false;
        };
        if !field.is_editable() {
            return false;
        }
        text = field.text().to_utf8_lossy().into_owned();
        multiline = field.is_multiline();
        password = field.is_password();
        max_length = field.max_chars().max(0) as u32;
        true
    });

    if !found {
        handle
            .ui_state
            .keyboard_requested
            .store(false, Ordering::Release);
        return 0;
    }

    let mut copied = 0usize;
    if !initial_text.is_null() && initial_text_capacity > 0 {
        copied = text.len().min(initial_text_capacity - 1);
        while copied > 0 && !text.is_char_boundary(copied) {
            copied -= 1;
        }
        std::ptr::copy_nonoverlapping(text.as_ptr(), initial_text, copied);
        *initial_text.add(copied) = 0;
    }

    out.write(FlashVitaTextInputInfo {
        multiline: u8::from(multiline),
        password: u8::from(password),
        reserved: [0; 2],
        max_length,
        initial_text_bytes: copied.try_into().unwrap_or(u32::MAX),
    });
    1
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_virtual_keyboard_ack(
    handle: *mut HeadlessPlayer,
) -> i32 {
    let Some(handle) = handle.as_mut() else {
        return -1;
    };
    handle
        .ui_state
        .keyboard_requested
        .store(false, Ordering::Release);
    0
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_replace_focused_text(
    handle: *mut HeadlessPlayer,
    data: *const u8,
    len: usize,
) -> i32 {
    let Some(handle) = handle.as_mut() else {
        return -1;
    };
    let text = if len == 0 {
        String::new()
    } else {
        if data.is_null() {
            return -2;
        }
        let bytes = std::slice::from_raw_parts(data, len);
        let Ok(text) = std::str::from_utf8(bytes) else {
            return -3;
        };
        text.to_owned()
    };

    let Ok(mut player) = handle.player.lock() else {
        return -4;
    };
    player.handle_event(PlayerEvent::TextControl {
        code: TextControlCode::SelectAll,
    });
    player.handle_event(PlayerEvent::Ime(ImeEvent::Commit(text)));
    0
}

#[no_mangle]
pub unsafe extern "C" fn flashvita_ruffle_headless_destroy(handle: *mut HeadlessPlayer) {
    if handle.is_null() {
        return;
    }

    let boxed = Box::from_raw(handle);
    vita_navigator::log_line(&format!(
        "destroy begin player_strong={} navigator_strong={}",
        Arc::strong_count(&boxed.player),
        Rc::strong_count(&boxed.navigator_state)
    ));

    boxed.navigator_state.borrow_mut().shutdown();
    let HeadlessPlayer {
        player,
        ui_state,
        navigator_state,
        ssf2_compat_pending: _,
        profile_ticks: _,
    } = *boxed;

    drop(player);
    drop(ui_state);
    drop(navigator_state);
    vita_navigator::log_line("destroy complete");
}
