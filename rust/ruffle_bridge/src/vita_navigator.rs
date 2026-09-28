use async_channel::{Receiver, Sender};
use encoding_rs::Encoding;
use indexmap::IndexMap;
use ruffle_core::backend::log::LogBackend;
use ruffle_core::backend::navigator::{
    create_specific_fetch_error, ErrorResponse, NavigationMethod, NavigatorBackend, OwnedFuture,
    Request, SuccessResponse,
};
use ruffle_core::loader::Error;
use ruffle_core::socket::{ConnectionState, SocketAction, SocketHandle};
use std::borrow::Cow;
use std::cell::{Cell, RefCell};
use std::ffi::{c_char, CString};
use std::ffi::c_void;
use std::future::Future;
use std::io::{self, Read};
use std::pin::Pin;
use std::rc::Rc;
use std::sync::atomic::{AtomicU32, Ordering};
use std::task::{Context, Poll, Waker};
use std::time::Duration;
use url::{ParseError, Url};

unsafe extern "C" {
    fn flashvita_vita_file_size(path: *const c_char) -> i64;
    fn flashvita_vita_file_read(path: *const c_char, dst: *mut u8, capacity: usize) -> i32;
    fn flashvita_vita_file_open(path: *const c_char) -> i32;
    fn flashvita_vita_file_read_fd(fd: i32, dst: *mut u8, capacity: usize) -> i32;
    fn flashvita_vita_file_close(fd: i32) -> i32;
    fn flashvita_vita_remove_file(path: *const c_char) -> i32;
    fn flashvita_vita_mkdirs(path: *const c_char) -> i32;
    fn flashvita_vita_http_fetch_start(
        url: *const c_char,
        method: i32,
        body: *const u8,
        body_len: usize,
        content_type: *const c_char,
        destination: *const c_char,
    ) -> *mut c_void;
    fn flashvita_vita_http_fetch_poll(handle: *mut c_void, http_status: *mut i32) -> i32;
    fn flashvita_vita_http_fetch_destroy(handle: *mut c_void);
    fn flashvita_vita_log_line(line: *const c_char);
    fn flashvita_vita_logging_enabled() -> i32;
}

struct NativeHttpFuture {
    handle: *mut c_void,
}

impl NativeHttpFuture {
    fn start(
        url: &CString,
        method: i32,
        body: *const u8,
        body_len: usize,
        content_type: &CString,
        destination: &CString,
    ) -> Option<Self> {
        let handle = unsafe {
            flashvita_vita_http_fetch_start(
                url.as_ptr(),
                method,
                body,
                body_len,
                content_type.as_ptr(),
                destination.as_ptr(),
            )
        };
        (!handle.is_null()).then_some(Self { handle })
    }
}

impl Future for NativeHttpFuture {
    type Output = (i32, i32);

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let mut status = 0i32;
        let result = unsafe { flashvita_vita_http_fetch_poll(self.handle, &mut status) };
        if result == 0 {
            // The Vita executor is frame-driven; request another poll without
            // blocking the main Ruffle thread on sceHttp.
            cx.waker().wake_by_ref();
            Poll::Pending
        } else {
            let handle = std::mem::replace(&mut self.handle, std::ptr::null_mut());
            unsafe { flashvita_vita_http_fetch_destroy(handle) };
            Poll::Ready((result, status))
        }
    }
}

impl Drop for NativeHttpFuture {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { flashvita_vita_http_fetch_destroy(self.handle) };
            self.handle = std::ptr::null_mut();
        }
    }
}

pub(crate) fn logging_enabled() -> bool {
    unsafe { flashvita_vita_logging_enabled() != 0 }
}

pub(crate) fn log_line(line: &str) {
    if !logging_enabled() {
        return;
    }
    if let Ok(line) = CString::new(line) {
        unsafe { flashvita_vita_log_line(line.as_ptr()) };
    }
}

pub struct VitaLogBackend {
    trace_lines: AtomicU32,
}

impl VitaLogBackend {
    pub fn new() -> Self {
        Self {
            trace_lines: AtomicU32::new(0),
        }
    }

    fn log_limited(&self, kind: &str, message: &str) {
        if !logging_enabled() {
            return;
        }
        const TRACE_LIMIT: u32 = 256;
        let index = self.trace_lines.fetch_add(1, Ordering::Relaxed);
        if index < TRACE_LIMIT {
            let clipped: String = message.chars().take(768).collect();
            log_line(&format!(
                "avm {} {}",
                kind,
                clipped.replace('\r', " ").replace('\n', " ")
            ));
        } else if index == TRACE_LIMIT {
            log_line("avm trace_suppressed limit=256");
        }
    }
}

impl LogBackend for VitaLogBackend {
    fn avm_trace(&self, message: &str) {
        self.log_limited("trace", message);
    }

    fn avm_warning(&self, message: &str) {
        self.log_limited("warning", message);
    }
}

fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf29ce484222325u64;
    for byte in bytes {
        hash ^= *byte as u64;
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash
}

fn sanitize_segment(segment: &str) -> String {
    let mut out = String::with_capacity(segment.len().min(72));
    for ch in segment.chars() {
        if ch.is_ascii_alphanumeric() || matches!(ch, '.' | '-' | '_') {
            out.push(ch);
        } else {
            out.push('_');
        }
        if out.len() >= 64 {
            break;
        }
    }
    if out.is_empty() {
        out.push('_');
    }
    if segment.len() > 64 {
        out.push_str(&format!("_{:08x}", fnv1a64(segment.as_bytes()) as u32));
    }
    out
}

fn safe_relative_path(url: &Url) -> Option<Vec<String>> {
    let mut parts = Vec::new();
    for segment in url.path_segments()? {
        if segment.is_empty() || segment == "." {
            continue;
        }
        if segment == ".." {
            return None;
        }
        parts.push(sanitize_segment(segment));
    }
    Some(parts)
}

pub(crate) fn read_native_file(path: &str) -> Result<Vec<u8>, String> {
    let c_path = CString::new(path).map_err(|_| "path contains NUL".to_string())?;
    let size = unsafe { flashvita_vita_file_size(c_path.as_ptr()) };
    if size < 0 {
        return Err("file not found".to_string());
    }
    if size > 256 * 1024 * 1024 {
        return Err("cached response is too large".to_string());
    }

    let mut bytes = vec![0u8; size as usize];
    if bytes.is_empty() {
        return Ok(bytes);
    }
    let read = unsafe {
        flashvita_vita_file_read(c_path.as_ptr(), bytes.as_mut_ptr(), bytes.len())
    };
    if read < 0 || read as usize != bytes.len() {
        return Err(format!("native file read failed ({read})"));
    }
    Ok(bytes)
}

pub(crate) struct NativeFileReader {
    fd: i32,
}

impl NativeFileReader {
    pub(crate) fn open(path: &str) -> Result<Self, String> {
        let c_path = CString::new(path).map_err(|_| "path contains NUL".to_string())?;
        let fd = unsafe { flashvita_vita_file_open(c_path.as_ptr()) };
        if fd < 0 {
            return Err(format!("native file open failed ({fd})"));
        }
        Ok(Self { fd })
    }
}

impl Read for NativeFileReader {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        let read = unsafe { flashvita_vita_file_read_fd(self.fd, buf.as_mut_ptr(), buf.len()) };
        if read < 0 {
            return Err(io::Error::other(format!(
                "native file read failed ({read})"
            )));
        }
        Ok(read as usize)
    }
}

impl Drop for NativeFileReader {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe {
                flashvita_vita_file_close(self.fd);
            }
            self.fd = -1;
        }
    }
}

pub(crate) fn native_file_size(path: &str) -> Result<usize, String> {
    let c_path = CString::new(path).map_err(|_| "path contains NUL".to_string())?;
    let size = unsafe { flashvita_vita_file_size(c_path.as_ptr()) };
    if size < 0 {
        return Err("file not found".to_string());
    }
    usize::try_from(size).map_err(|_| "file is too large".to_string())
}

const FETCH_INTERVAL_PUMPS: u64 = 4;

struct FetchGate {
    permit: Cell<bool>,
}

impl FetchGate {
    fn new() -> Self {
        Self {
            permit: Cell::new(true),
        }
    }

    fn set_permit(&self, permit: bool) {
        self.permit.set(permit);
    }

    fn try_take(&self) -> bool {
        self.permit.replace(false)
    }
}

struct FetchPermit {
    gate: Rc<FetchGate>,
}

impl Future for FetchPermit {
    type Output = ();

    fn poll(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.gate.try_take() {
            Poll::Ready(())
        } else {
            Poll::Pending
        }
    }
}

pub struct VitaNavigatorState {
    futures: Vec<OwnedFuture<(), Error>>,
    fetch_gate: Rc<FetchGate>,
    pump_count: u64,
}

impl VitaNavigatorState {
    pub fn new() -> Self {
        Self {
            futures: Vec::new(),
            fetch_gate: Rc::new(FetchGate::new()),
            pump_count: 0,
        }
    }

    pub fn pump(&mut self) {
        self.pump_count = self.pump_count.wrapping_add(1);
        self.fetch_gate.set_permit(
            self.pump_count == 1 || self.pump_count % FETCH_INTERVAL_PUMPS == 0,
        );

        let waker = Waker::noop();
        let mut context = Context::from_waker(waker);
        let mut index = 0;
        while index < self.futures.len() {
            let result = self.futures[index].as_mut().poll(&mut context);
            match result {
                Poll::Ready(Ok(())) => {
                    drop(self.futures.swap_remove(index));
                }
                Poll::Ready(Err(error)) => {
                    log_line(&format!("navigator future error: {error}"));
                    drop(self.futures.swap_remove(index));
                }
                Poll::Pending => index += 1,
            }
        }
    }

    pub fn shutdown(&mut self) {
        self.futures.clear();
        self.futures.shrink_to_fit();
        self.fetch_gate.set_permit(false);
        self.pump_count = 0;
    }
}

struct VitaResponse {
    url: String,
    body: Option<Vec<u8>>,
    cursor: usize,
    status: u16,
    redirected: bool,
}

impl SuccessResponse for VitaResponse {
    fn url(&self) -> Cow<'_, str> {
        Cow::Borrowed(&self.url)
    }

    fn set_url(&mut self, url: String) {
        self.url = url;
    }

    fn body(mut self: Box<Self>) -> OwnedFuture<Vec<u8>, Error> {
        let body = self.body.take().unwrap_or_default();
        Box::pin(async move { Ok(body) })
    }

    fn text_encoding(&self) -> Option<&'static Encoding> {
        None
    }

    fn status(&self) -> u16 {
        self.status
    }

    fn redirected(&self) -> bool {
        self.redirected
    }

    fn next_chunk(&mut self) -> OwnedFuture<Option<Vec<u8>>, Error> {
        const CHUNK: usize = 16 * 1024;
        let Some(body) = self.body.as_ref() else {
            return Box::pin(async { Ok(None) });
        };
        if self.cursor >= body.len() {
            return Box::pin(async { Ok(None) });
        }
        let end = (self.cursor + CHUNK).min(body.len());
        let chunk = body[self.cursor..end].to_vec();
        self.cursor = end;
        Box::pin(async move { Ok(Some(chunk)) })
    }

    fn expected_length(&self) -> Result<Option<u64>, Error> {
        Ok(self.body.as_ref().map(|body| body.len() as u64))
    }
}

#[derive(Clone)]
pub struct VitaNavigatorBackend {
    base_url: Url,
    cache_root: String,
    state: Rc<RefCell<VitaNavigatorState>>,
    fetch_gate: Rc<FetchGate>,
}

impl VitaNavigatorBackend {
    pub fn new(
        movie_url: &str,
        cache_root: String,
        state: Rc<RefCell<VitaNavigatorState>>,
    ) -> Result<Self, ParseError> {
        let base_url = Url::parse(movie_url)?;
        let fetch_gate = state.borrow().fetch_gate.clone();
        if let Ok(cache) = CString::new(cache_root.as_str()) {
            unsafe {
                flashvita_vita_mkdirs(cache.as_ptr());
            }
        }
        Ok(Self {
            base_url,
            cache_root,
            state,
            fetch_gate,
        })
    }

    fn local_path_for_url(&self, url: &Url) -> Option<String> {
        if url.scheme() == "file" {
            let mut parts = safe_relative_path(url)?;
            if parts.first().map(|part| part.as_str()) == Some("FlashVita") {
                parts.remove(0);
            }
            // The synthetic movie filename is not part of resource URLs after
            // Url::join, but strip it if an SWF asks for its own file URL.
            if parts.len() == 1 && parts[0].ends_with(".swf") {
                return None;
            }
            let mut path = self.cache_root.clone();
            for part in parts {
                path.push('/');
                path.push_str(&part);
            }
            return Some(path);
        }

        if url.scheme() != "http" && url.scheme() != "https" {
            return None;
        }

        let host = sanitize_segment(url.host_str().unwrap_or("unknown-host"));
        let mut path = format!("{}/remote/{}", self.cache_root, host);
        let parts = safe_relative_path(url)?;
        if parts.is_empty() {
            path.push_str("/index.bin");
        } else {
            for part in parts {
                path.push('/');
                path.push_str(&part);
            }
        }
        if url.path().ends_with('/') {
            path.push_str("/index.bin");
        }
        if let Some(query) = url.query() {
            path.push_str(&format!(".q{:016x}", fnv1a64(query.as_bytes())));
        }
        Some(path)
    }

    fn local_override_for_remote_url(&self, url: &Url) -> Option<String> {
        if url.scheme() != "http" && url.scheme() != "https" {
            return None;
        }
        let parts = safe_relative_path(url)?;
        let data_index = parts
            .iter()
            .position(|part| part.eq_ignore_ascii_case("data"))?;

        let mut path = self.cache_root.clone();
        for part in &parts[data_index..] {
            path.push('/');
            path.push_str(part);
        }
        Some(path)
    }

    async fn fetch_async(&self, request: Request) -> Result<Box<dyn SuccessResponse>, ErrorResponse> {
        log_line(&format!(
            "net fetch method={} url={}",
            request.method(),
            request.url()
        ));
        let resolved = self
            .resolve_url(request.url())
            .map_err(|error| create_specific_fetch_error("Invalid URL", request.url(), error))?;
        let resolved_string = resolved.to_string();
        let Some(cache_path) = self.local_path_for_url(&resolved) else {
            return Err(create_specific_fetch_error(
                "Unsupported URL scheme",
                resolved.as_str(),
                "",
            ));
        };

        let is_get = request.method() == NavigationMethod::Get;
        if is_get {
            if let Some(override_path) = self.local_override_for_remote_url(&resolved) {
                if let Ok(bytes) = read_native_file(&override_path) {
                    log_line(&format!(
                        "net local_override url={} path={} bytes={}",
                        resolved,
                        override_path,
                        bytes.len()
                    ));
                    return Ok(Box::new(VitaResponse {
                        url: resolved_string,
                        body: Some(bytes),
                        cursor: 0,
                        status: 200,
                        redirected: false,
                    }));
                }
            }

            if let Ok(bytes) = read_native_file(&cache_path) {
                log_line(&format!(
                    "net cache_hit url={} path={} bytes={}",
                    resolved,
                    cache_path,
                    bytes.len()
                ));
                return Ok(Box::new(VitaResponse {
                    url: resolved_string,
                    body: Some(bytes),
                    cursor: 0,
                    status: 200,
                    redirected: false,
                }));
            }
        }

        if resolved.scheme() == "file" {
            log_line(&format!(
                "net local_miss url={} path={}",
                resolved, cache_path
            ));
            return Err(create_specific_fetch_error(
                "Local gamefile not found",
                resolved.as_str(),
                cache_path,
            ));
        }

        let transient_path;
        let destination = if is_get {
            cache_path.as_str()
        } else {
            transient_path = format!(
                "{}/_tmp/post_{:016x}.bin",
                self.cache_root,
                fnv1a64(
                    format!(
                        "{}:{}",
                        resolved,
                        request
                            .body()
                            .as_ref()
                            .map(|(body, _)| fnv1a64(body))
                            .unwrap_or_default()
                    )
                    .as_bytes()
                )
            );
            transient_path.as_str()
        };

        let c_url = CString::new(resolved.as_str()).map_err(|_| {
            create_specific_fetch_error("Invalid URL", resolved.as_str(), "embedded NUL")
        })?;
        let c_destination = CString::new(destination).map_err(|_| {
            create_specific_fetch_error("Invalid cache path", resolved.as_str(), "embedded NUL")
        })?;

        let (body_ptr, body_len, content_type) = match request.body() {
            Some((body, mime)) => (body.as_ptr(), body.len(), mime.as_str()),
            None => (std::ptr::null(), 0, ""),
        };
        let c_content_type = CString::new(content_type).map_err(|_| {
            create_specific_fetch_error("Invalid content type", resolved.as_str(), "embedded NUL")
        })?;
        log_line(&format!(
            "net download_start method={} url={} path={}",
            request.method(),
            resolved,
            destination
        ));
        let Some(job) = NativeHttpFuture::start(
            &c_url,
            if is_get { 0 } else { 1 },
            body_ptr,
            body_len,
            &c_content_type,
            &c_destination,
        ) else {
            return Err(create_specific_fetch_error(
                "Network worker start failed",
                resolved.as_str(),
                "",
            ));
        };
        let (result, status) = job.await;
        if result < 0 {
            log_line(&format!(
                "net download_fail status={} url={}",
                status, resolved
            ));
            return Err(create_specific_fetch_error(
                "Network fetch failed",
                resolved.as_str(),
                status,
            ));
        }

        let bytes = read_native_file(destination).map_err(|error| {
            create_specific_fetch_error("Downloaded file unreadable", resolved.as_str(), error)
        })?;
        if !is_get {
            unsafe {
                flashvita_vita_remove_file(c_destination.as_ptr());
            }
        }

        log_line(&format!(
            "net download_ok status={} url={} bytes={} path={}",
            status,
            resolved,
            bytes.len(),
            destination
        ));
        Ok(Box::new(VitaResponse {
            url: resolved_string,
            body: Some(bytes),
            cursor: 0,
            status: status.clamp(0, u16::MAX as i32) as u16,
            redirected: false,
        }))
    }
}

impl NavigatorBackend for VitaNavigatorBackend {
    fn navigate_to_url(
        &self,
        url: &str,
        target: &str,
        _vars_method: Option<(NavigationMethod, IndexMap<String, String>)>,
    ) {
        log_line(&format!("navigate ignored target={} url={}", target, url));
    }

    fn fetch(&self, request: Request) -> OwnedFuture<Box<dyn SuccessResponse>, ErrorResponse> {
        let backend = self.clone();
        let gate = self.fetch_gate.clone();
        Box::pin(async move {
            FetchPermit { gate }.await;
            backend.fetch_async(request).await
        })
    }

    fn resolve_url(&self, url: &str) -> Result<Url, ParseError> {
        match Url::parse(url) {
            Ok(url) => Ok(self.pre_process_url(url)),
            Err(ParseError::RelativeUrlWithoutBase) => {
                self.base_url.join(url).map(|url| self.pre_process_url(url))
            }
            Err(error) => Err(error),
        }
    }

    fn spawn_future(&mut self, future: OwnedFuture<(), Error>) {
        self.state.borrow_mut().futures.push(future);
    }

    fn pre_process_url(&self, url: Url) -> Url {
        url
    }

    fn connect_socket(
        &mut self,
        _host: String,
        _port: u16,
        _timeout: Duration,
        handle: SocketHandle,
        _receiver: Receiver<Vec<u8>>,
        sender: Sender<SocketAction>,
    ) {
        let _ = sender.try_send(SocketAction::Connect(handle, ConnectionState::Failed));
    }
}
