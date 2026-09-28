use ruffle_render::backend::{
    BitmapCacheEntry, Context3D, Context3DProfile, PixelBenderOutput, PixelBenderTarget,
    RenderBackend, ShapeHandle, ShapeHandleImpl, ViewportDimensions,
};
use ruffle_render::bitmap::{
    Bitmap, BitmapFormat, BitmapHandle, BitmapHandleImpl, BitmapSource, PixelRegion, PixelSnapping,
    RgbaBufRead, SyncHandle,
};
use ruffle_render::commands::{Command, CommandHandler, CommandList, RenderBlendMode};
use ruffle_render::error::Error;
use ruffle_render::matrix::Matrix;
use ruffle_render::pixel_bender::{PixelBenderShader, PixelBenderShaderHandle};
use ruffle_render::pixel_bender_support::PixelBenderShaderArgument;
use ruffle_render::quality::StageQuality;
use ruffle_render::shape_utils::{DistilledShape, GradientType};
use ruffle_render::tessellator::{DrawType, Gradient as TessGradient, Mesh, ShapeTessellator};
use ruffle_render::transform::Transform;
use std::any::Any;
use std::borrow::Cow;
use std::collections::HashMap;
use std::ffi::c_void;
use std::fmt::{Debug, Formatter};
use std::num::NonZeroU32;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Weak};
use std::time::Instant;
use swf::{Color, ColorTransform};

#[repr(C)]
#[derive(Clone, Copy)]
struct VitaVertex {
    x: f32,
    y: f32,
    r: u8,
    g: u8,
    b: u8,
    a: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct VitaTexVertex {
    x: f32,
    y: f32,
    u: f32,
    v: f32,
    r: u8,
    g: u8,
    b: u8,
    a: u8,
}

unsafe extern "C" {
    fn flashvita_vita_parallel_for(
        count: u32,
        min_grain: u32,
        callback: unsafe extern "C" fn(*mut c_void, u32, u32),
        user: *mut c_void,
    ) -> i32;
    fn flashvita_vitagl_begin_flash_frame(r: u8, g: u8, b: u8, a: u8);
    fn flashvita_vitagl_begin_offscreen(
        texture: u32,
        width: u32,
        height: u32,
        x_min: u32,
        y_min: u32,
        x_max: u32,
        y_max: u32,
        r: u8,
        g: u8,
        b: u8,
        a: u8,
    ) -> i32;
    fn flashvita_vitagl_end_offscreen();
    fn flashvita_vitagl_read_texture_region(
        texture: u32,
        texture_width: u32,
        texture_height: u32,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        destination: *mut u8,
    ) -> i32;
    fn flashvita_vitagl_draw_colored_triangles(
        vertices: *const VitaVertex,
        vertex_count: usize,
        indices: *const u32,
        index_count: usize,
    );
    fn flashvita_vitagl_draw_colored_triangles_u16(
        vertices: *const VitaVertex,
        vertex_count: usize,
        indices: *const u16,
        index_count: usize,
    );
    fn flashvita_vitagl_draw_colored_line_strip(vertices: *const VitaVertex, vertex_count: usize);
    fn flashvita_vitagl_create_texture(data: *const u8, width: u32, height: u32) -> u32;
    fn flashvita_vitagl_create_texture_zero_copy(data: *mut u8, width: u32, height: u32) -> u32;
    fn flashvita_vitagl_update_zero_copy_texture(
        texture: u32,
        data: *const u8,
        source_width: u32,
        source_height: u32,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
    );
    fn flashvita_vitagl_update_texture(texture: u32, data: *const u8, width: u32, height: u32);
    fn flashvita_vitagl_update_texture_region(
        texture: u32,
        data: *const u8,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
    );
    fn flashvita_vitagl_delete_texture(texture: u32);
    fn flashvita_vitagl_delete_external_texture(texture: u32, external_data: *const u8);
    fn flashvita_vita_vgl_ram_owns(ptr: *const c_void) -> i32;
    fn flashvita_vitagl_draw_textured_triangles(
        texture: u32,
        vertices: *const VitaTexVertex,
        vertex_count: usize,
        indices: *const u32,
        index_count: usize,
        smoothing: u8,
        wrap_mode: u8,
    );
    fn flashvita_vitagl_draw_textured_triangles_u16(
        texture: u32,
        vertices: *const VitaTexVertex,
        vertex_count: usize,
        indices: *const u16,
        index_count: usize,
        smoothing: u8,
        wrap_mode: u8,
    );
    fn flashvita_vitagl_gpu_transform_available() -> i32;
    fn flashvita_vitagl_create_gpu_mesh(
        vertices: *const c_void,
        vertex_bytes: usize,
        indices: *const u16,
        index_count: usize,
    ) -> u64;
    fn flashvita_vitagl_delete_gpu_mesh(handle: u64);
    fn flashvita_vitagl_draw_gpu_colored(
        handle: u64,
        index_count: usize,
        transform: *const VitaGpuTransform,
    );
    fn flashvita_vitagl_draw_gpu_textured(
        handle: u64,
        index_count: usize,
        texture: u32,
        transform: *const VitaGpuTransform,
        smoothing: u8,
        wrap_mode: u8,
    );
    fn flashvita_vitagl_mask_push(previous_depth: u32);
    fn flashvita_vitagl_mask_activate(depth: u32);
    fn flashvita_vitagl_mask_deactivate(depth: u32);
    fn flashvita_vitagl_mask_pop(depth: u32);
}

const NATIVE_PARALLEL_VERTEX_THRESHOLD: usize = 256;
const FRAME_TRANSFORM_CHUNK_VERTICES: usize = 768;

#[derive(Clone, Copy)]
struct VitaMatrix2D {
    a: f32,
    b: f32,
    c: f32,
    d: f32,
    tx: f32,
    ty: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct VitaGpuTransform {
    a: f32,
    b: f32,
    c: f32,
    d: f32,
    tx: f32,
    ty: f32,
    mult: [f32; 4],
    add: [f32; 4],
}

impl From<&Transform> for VitaGpuTransform {
    fn from(transform: &Transform) -> Self {
        let matrix: VitaMatrix2D = transform.matrix.into();
        Self {
            a: matrix.a,
            b: matrix.b,
            c: matrix.c,
            d: matrix.d,
            tx: matrix.tx,
            ty: matrix.ty,
            mult: transform.color_transform.mult_rgba_normalized(),
            add: transform.color_transform.add_rgba_normalized(),
        }
    }
}

impl From<Matrix> for VitaMatrix2D {
    #[inline]
    fn from(matrix: Matrix) -> Self {
        Self {
            a: matrix.a,
            b: matrix.b,
            c: matrix.c,
            d: matrix.d,
            tx: matrix.tx.to_pixels() as f32,
            ty: matrix.ty.to_pixels() as f32,
        }
    }
}

struct ColorTransformJob {
    source: *const ruffle_render::tessellator::Vertex,
    destination: *mut VitaVertex,
    matrix: VitaMatrix2D,
    color_transform: ColorTransform,
    color_identity: bool,
}

struct GradientTransformJob {
    source: *const ruffle_render::tessellator::Vertex,
    destination: *mut VitaTexVertex,
    matrix: VitaMatrix2D,
    texture_matrix: [[f32; 3]; 3],
    linear: bool,
    tint: Color,
}

struct BitmapTransformJob {
    source: *const ruffle_render::tessellator::Vertex,
    destination: *mut VitaTexVertex,
    matrix: VitaMatrix2D,
    texture_matrix: [[f32; 3]; 3],
    tint: Color,
}

unsafe extern "C" fn transform_color_chunk(user: *mut c_void, begin: u32, end: u32) {
    let job = unsafe { &*(user.cast::<ColorTransformJob>()) };
    if job.color_identity {
        for index in begin as usize..end as usize {
            let source = unsafe { &*job.source.add(index) };
            let (x, y) = transform_point_fast(job.matrix, source.x, source.y);
            unsafe {
                job.destination.add(index).write(vertex(x, y, source.color));
            }
        }
    } else {
        for index in begin as usize..end as usize {
            let source = unsafe { &*job.source.add(index) };
            let (x, y) = transform_point_fast(job.matrix, source.x, source.y);
            let color = &job.color_transform * source.color;
            unsafe {
                job.destination.add(index).write(vertex(x, y, color));
            }
        }
    }
}

unsafe extern "C" fn transform_gradient_chunk(user: *mut c_void, begin: u32, end: u32) {
    let job = unsafe { &*(user.cast::<GradientTransformJob>()) };
    for index in begin as usize..end as usize {
        let source = unsafe { &*job.source.add(index) };
        let (x, y) = transform_point_fast(job.matrix, source.x, source.y);
        let (u, v) = texture_uv(&job.texture_matrix, source.x, source.y);
        unsafe {
            job.destination.add(index).write(tex_vertex(
                x,
                y,
                u,
                if job.linear { 0.5 } else { v },
                job.tint,
            ));
        }
    }
}

unsafe extern "C" fn transform_bitmap_chunk(user: *mut c_void, begin: u32, end: u32) {
    let job = unsafe { &*(user.cast::<BitmapTransformJob>()) };
    for index in begin as usize..end as usize {
        let source = unsafe { &*job.source.add(index) };
        let (x, y) = transform_point_fast(job.matrix, source.x, source.y);
        let (u, v) = texture_uv(&job.texture_matrix, source.x, source.y);
        unsafe {
            job.destination
                .add(index)
                .write(tex_vertex(x, y, u, v, job.tint));
        }
    }
}

enum FrameTransformJob {
    Color {
        source: *const ruffle_render::tessellator::Vertex,
        count: usize,
        destination_offset: usize,
        matrix: VitaMatrix2D,
        color_transform: ColorTransform,
        color_identity: bool,
    },
    Gradient {
        source: *const ruffle_render::tessellator::Vertex,
        count: usize,
        destination_offset: usize,
        matrix: VitaMatrix2D,
        texture_matrix: [[f32; 3]; 3],
        linear: bool,
        tint: Color,
    },
    Bitmap {
        source: *const ruffle_render::tessellator::Vertex,
        count: usize,
        destination_offset: usize,
        matrix: VitaMatrix2D,
        texture_matrix: [[f32; 3]; 3],
        tint: Color,
    },
}

struct FrameTransformContext {
    jobs: *const FrameTransformJob,
    job_count: usize,
    color_vertices: *mut VitaVertex,
    tex_vertices: *mut VitaTexVertex,
    next_job: AtomicUsize,
}

unsafe extern "C" fn transform_frame_jobs_worker(
    user: *mut c_void,
    _begin: u32,
    _end: u32,
) {
    let context = unsafe { &*(user.cast::<FrameTransformContext>()) };
    loop {
        let index = context.next_job.fetch_add(1, Ordering::Relaxed);
        if index >= context.job_count {
            break;
        }

        match unsafe { &*context.jobs.add(index) } {
            FrameTransformJob::Color {
                source,
                count,
                destination_offset,
                matrix,
                color_transform,
                color_identity,
            } => {
                if *color_identity {
                    for vertex_index in 0..*count {
                        let source = unsafe { &*source.add(vertex_index) };
                        let (x, y) = transform_point_fast(*matrix, source.x, source.y);
                        unsafe {
                            context
                                .color_vertices
                                .add(destination_offset + vertex_index)
                                .write(vertex(x, y, source.color));
                        }
                    }
                } else {
                    for vertex_index in 0..*count {
                        let source = unsafe { &*source.add(vertex_index) };
                        let (x, y) = transform_point_fast(*matrix, source.x, source.y);
                        let color = color_transform * source.color;
                        unsafe {
                            context
                                .color_vertices
                                .add(destination_offset + vertex_index)
                                .write(vertex(x, y, color));
                        }
                    }
                }
            }
            FrameTransformJob::Gradient {
                source,
                count,
                destination_offset,
                matrix,
                texture_matrix,
                linear,
                tint,
            } => {
                for vertex_index in 0..*count {
                    let source = unsafe { &*source.add(vertex_index) };
                    let (x, y) = transform_point_fast(*matrix, source.x, source.y);
                    let (u, v) = texture_uv(texture_matrix, source.x, source.y);
                    unsafe {
                        context
                            .tex_vertices
                            .add(destination_offset + vertex_index)
                            .write(tex_vertex(
                                x,
                                y,
                                u,
                                if *linear { 0.5 } else { v },
                                *tint,
                            ));
                    }
                }
            }
            FrameTransformJob::Bitmap {
                source,
                count,
                destination_offset,
                matrix,
                texture_matrix,
                tint,
            } => {
                for vertex_index in 0..*count {
                    let source = unsafe { &*source.add(vertex_index) };
                    let (x, y) = transform_point_fast(*matrix, source.x, source.y);
                    let (u, v) = texture_uv(texture_matrix, source.x, source.y);
                    unsafe {
                        context
                            .tex_vertices
                            .add(destination_offset + vertex_index)
                            .write(tex_vertex(x, y, u, v, *tint));
                    }
                }
            }
        }
    }
}

static STAT_FRAMES: AtomicU64 = AtomicU64::new(0);
static STAT_COLORED_DRAWS: AtomicU64 = AtomicU64::new(0);
static STAT_TEXTURED_DRAWS: AtomicU64 = AtomicU64::new(0);
static STAT_BITMAP_UPLOADS: AtomicU64 = AtomicU64::new(0);
static STAT_BITMAP_PARTIAL_UPLOADS: AtomicU64 = AtomicU64::new(0);
static STAT_BITMAP_UPLOADED_BYTES: AtomicU64 = AtomicU64::new(0);
static STAT_LINES: AtomicU64 = AtomicU64::new(0);
static STAT_GRADIENT_SKIPS: AtomicU64 = AtomicU64::new(0);
static STAT_MISSING_BITMAPS: AtomicU64 = AtomicU64::new(0);
static STAT_MASK_OPS: AtomicU64 = AtomicU64::new(0);
static STAT_BLENDS: AtomicU64 = AtomicU64::new(0);
static STAT_STAGE3D: AtomicU64 = AtomicU64::new(0);
static STAT_TRANSFORMED_VERTICES: AtomicU64 = AtomicU64::new(0);
static STAT_PARALLEL_DRAWS: AtomicU64 = AtomicU64::new(0);
static STAT_PARALLEL_BATCHES: AtomicU64 = AtomicU64::new(0);
static STAT_PARALLEL_JOBS: AtomicU64 = AtomicU64::new(0);
static STAT_COLOR_SUBMISSIONS: AtomicU64 = AtomicU64::new(0);
static STAT_PREPASS_US: AtomicU64 = AtomicU64::new(0);
static STAT_SUBMIT_US: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Copy, Default)]
pub struct RendererStatsSnapshot {
    pub frames: u64,
    pub colored_draws: u64,
    pub textured_draws: u64,
    pub bitmap_uploads: u64,
    pub bitmap_partial_uploads: u64,
    pub bitmap_uploaded_bytes: u64,
    pub lines: u64,
    pub gradient_skips: u64,
    pub missing_bitmaps: u64,
    pub mask_ops: u64,
    pub blends: u64,
    pub stage3d: u64,
    pub transformed_vertices: u64,
    pub parallel_draws: u64,
    pub parallel_batches: u64,
    pub parallel_jobs: u64,
    pub color_submissions: u64,
    pub prepass_us: u64,
    pub submit_us: u64,
}

pub fn renderer_stats_snapshot() -> RendererStatsSnapshot {
    RendererStatsSnapshot {
        frames: STAT_FRAMES.load(Ordering::Relaxed),
        colored_draws: STAT_COLORED_DRAWS.load(Ordering::Relaxed),
        textured_draws: STAT_TEXTURED_DRAWS.load(Ordering::Relaxed),
        bitmap_uploads: STAT_BITMAP_UPLOADS.load(Ordering::Relaxed),
        bitmap_partial_uploads: STAT_BITMAP_PARTIAL_UPLOADS.load(Ordering::Relaxed),
        bitmap_uploaded_bytes: STAT_BITMAP_UPLOADED_BYTES.load(Ordering::Relaxed),
        lines: STAT_LINES.load(Ordering::Relaxed),
        gradient_skips: STAT_GRADIENT_SKIPS.load(Ordering::Relaxed),
        missing_bitmaps: STAT_MISSING_BITMAPS.load(Ordering::Relaxed),
        mask_ops: STAT_MASK_OPS.load(Ordering::Relaxed),
        blends: STAT_BLENDS.load(Ordering::Relaxed),
        stage3d: STAT_STAGE3D.load(Ordering::Relaxed),
        transformed_vertices: STAT_TRANSFORMED_VERTICES.load(Ordering::Relaxed),
        parallel_draws: STAT_PARALLEL_DRAWS.load(Ordering::Relaxed),
        parallel_batches: STAT_PARALLEL_BATCHES.load(Ordering::Relaxed),
        parallel_jobs: STAT_PARALLEL_JOBS.load(Ordering::Relaxed),
        color_submissions: STAT_COLOR_SUBMISSIONS.load(Ordering::Relaxed),
        prepass_us: STAT_PREPASS_US.load(Ordering::Relaxed),
        submit_us: STAT_SUBMIT_US.load(Ordering::Relaxed),
    }
}

struct VitaShapeHandle {
    mesh: Mesh,
    indices_u16: Vec<Option<Vec<u16>>>,
    gpu_draws: Vec<Option<VitaGpuMesh>>,
    bitmaps: Vec<(u16, BitmapHandle)>,
    gradients: Vec<Option<Arc<VitaGradientTexture>>>,
}

impl Debug for VitaShapeHandle {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("VitaShapeHandle")
            .field("draws", &self.mesh.draws.len())
            .finish()
    }
}

impl ShapeHandleImpl for VitaShapeHandle {}

#[derive(Debug)]
struct VitaGpuMesh {
    handle: u64,
    index_count: usize,
}

impl Drop for VitaGpuMesh {
    fn drop(&mut self) {
        if self.handle != 0 {
            unsafe { flashvita_vitagl_delete_gpu_mesh(self.handle) };
        }
    }
}

#[derive(Debug)]
struct VitaGradientTexture {
    texture: u32,
    wrap_mode: u8,
}

impl Drop for VitaGradientTexture {
    fn drop(&mut self) {
        if self.texture != 0 {
            unsafe { flashvita_vitagl_delete_texture(self.texture) };
        }
    }
}

#[derive(Debug)]
struct VitaBitmapHandle {
    texture: u32,
    width: u32,
    height: u32,
    initialized: AtomicBool,
    offscreen_y_flipped: AtomicBool,
    external_data: *const u8,
}

impl BitmapHandleImpl for VitaBitmapHandle {}

#[derive(Debug)]
struct VitaSyncHandle {
    bitmap: BitmapHandle,
    bounds: PixelRegion,
}

impl SyncHandle for VitaSyncHandle {}

impl Drop for VitaBitmapHandle {
    fn drop(&mut self) {
        if self.texture != 0 {
            unsafe {
                if self.external_data.is_null() {
                    flashvita_vitagl_delete_texture(self.texture);
                } else {
                    flashvita_vitagl_delete_external_texture(self.texture, self.external_data);
                }
            }
        }
    }
}

#[derive(Default)]
struct FrameShapeBatch {
    jobs: Vec<FrameTransformJob>,
    color_vertices: Vec<VitaVertex>,
    tex_vertices: Vec<VitaTexVertex>,
    transformed_draws: usize,
    frame_id: u64,
    transform_cache: HashMap<ShapeTransformKey, CachedShapeVertices>,
    pending_cache_writes: Vec<PendingShapeCacheWrite>,
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
struct ShapeTransformKey {
    shape: usize,
    matrix: [u32; 6],
    mult: [u32; 4],
    add: [i16; 4],
}

struct CachedShapeVertices {
    color: Vec<VitaVertex>,
    tex: Vec<VitaTexVertex>,
    last_used: u64,
}

struct PendingShapeCacheWrite {
    key: ShapeTransformKey,
    color_start: usize,
    color_end: usize,
    tex_start: usize,
    tex_end: usize,
}

fn shape_transform_key(shape: &VitaShapeHandle, transform: &Transform) -> ShapeTransformKey {
    let matrix: VitaMatrix2D = transform.matrix.into();
    let mult = transform.color_transform.mult_rgba_normalized().map(f32::to_bits);
    ShapeTransformKey {
        shape: shape as *const VitaShapeHandle as usize,
        matrix: [
            matrix.a.to_bits(),
            matrix.b.to_bits(),
            matrix.c.to_bits(),
            matrix.d.to_bits(),
            matrix.tx.to_bits(),
            matrix.ty.to_bits(),
        ],
        mult,
        add: [
            transform.color_transform.r_add,
            transform.color_transform.g_add,
            transform.color_transform.b_add,
            transform.color_transform.a_add,
        ],
    }
}

impl FrameShapeBatch {
    fn clear(&mut self) {
        self.jobs.clear();
        self.color_vertices.clear();
        self.tex_vertices.clear();
        self.transformed_draws = 0;
        self.pending_cache_writes.clear();
    }

    fn reserve_color_vertices(&mut self, count: usize) -> usize {
        let offset = self.color_vertices.len();
        self.color_vertices.reserve(count);
        unsafe {
            self.color_vertices.set_len(offset + count);
        }
        offset
    }

    fn reserve_tex_vertices(&mut self, count: usize) -> usize {
        let offset = self.tex_vertices.len();
        self.tex_vertices.reserve(count);
        unsafe {
            self.tex_vertices.set_len(offset + count);
        }
        offset
    }

    fn add_color_draw(
        &mut self,
        vertices: &[ruffle_render::tessellator::Vertex],
        transform: &Transform,
    ) {
        if vertices.is_empty() {
            return;
        }
        let destination_offset = self.reserve_color_vertices(vertices.len());
        STAT_TRANSFORMED_VERTICES.fetch_add(vertices.len() as u64, Ordering::Relaxed);
        self.transformed_draws += 1;

        for chunk_start in (0..vertices.len()).step_by(FRAME_TRANSFORM_CHUNK_VERTICES) {
            let count = (vertices.len() - chunk_start).min(FRAME_TRANSFORM_CHUNK_VERTICES);
            self.jobs.push(FrameTransformJob::Color {
                source: vertices.as_ptr().wrapping_add(chunk_start),
                count,
                destination_offset: destination_offset + chunk_start,
                matrix: transform.matrix.into(),
                color_transform: transform.color_transform,
                color_identity: transform.color_transform == ColorTransform::IDENTITY,
            });
        }
    }

    fn add_gradient_draw(
        &mut self,
        vertices: &[ruffle_render::tessellator::Vertex],
        transform: &Transform,
        texture_matrix: [[f32; 3]; 3],
        linear: bool,
        tint: Color,
    ) {
        if vertices.is_empty() {
            return;
        }
        let destination_offset = self.reserve_tex_vertices(vertices.len());
        STAT_TRANSFORMED_VERTICES.fetch_add(vertices.len() as u64, Ordering::Relaxed);
        self.transformed_draws += 1;

        for chunk_start in (0..vertices.len()).step_by(FRAME_TRANSFORM_CHUNK_VERTICES) {
            let count = (vertices.len() - chunk_start).min(FRAME_TRANSFORM_CHUNK_VERTICES);
            self.jobs.push(FrameTransformJob::Gradient {
                source: vertices.as_ptr().wrapping_add(chunk_start),
                count,
                destination_offset: destination_offset + chunk_start,
                matrix: transform.matrix.into(),
                texture_matrix,
                linear,
                tint,
            });
        }
    }

    fn add_bitmap_draw(
        &mut self,
        vertices: &[ruffle_render::tessellator::Vertex],
        transform: &Transform,
        texture_matrix: [[f32; 3]; 3],
        tint: Color,
    ) {
        if vertices.is_empty() {
            return;
        }
        let destination_offset = self.reserve_tex_vertices(vertices.len());
        STAT_TRANSFORMED_VERTICES.fetch_add(vertices.len() as u64, Ordering::Relaxed);
        self.transformed_draws += 1;

        for chunk_start in (0..vertices.len()).step_by(FRAME_TRANSFORM_CHUNK_VERTICES) {
            let count = (vertices.len() - chunk_start).min(FRAME_TRANSFORM_CHUNK_VERTICES);
            self.jobs.push(FrameTransformJob::Bitmap {
                source: vertices.as_ptr().wrapping_add(chunk_start),
                count,
                destination_offset: destination_offset + chunk_start,
                matrix: transform.matrix.into(),
                texture_matrix,
                tint,
            });
        }
    }

    fn collect_shape(&mut self, shape: &VitaShapeHandle, transform: &Transform) {
        const TRANSFORM_CACHE_MIN_VERTICES: usize = 128;
        let cacheable = shape
            .mesh
            .draws
            .iter()
            .enumerate()
            .filter(|(index, _)| shape.gpu_draws[*index].is_none())
            .map(|(_, draw)| draw.vertices.len())
            .sum::<usize>()
            >= TRANSFORM_CACHE_MIN_VERTICES;
        let key = cacheable.then(|| shape_transform_key(shape, transform));
        if let Some(key) = key {
            if let Some(cached) = self.transform_cache.get_mut(&key) {
                cached.last_used = self.frame_id;
                self.color_vertices.extend_from_slice(&cached.color);
                self.tex_vertices.extend_from_slice(&cached.tex);
                return;
            }
        }

        let color_start = self.color_vertices.len();
        let tex_start = self.tex_vertices.len();
        for (draw_index, draw) in shape.mesh.draws.iter().enumerate() {
            if shape.gpu_draws[draw_index].is_some() {
                continue;
            }
            match &draw.draw_type {
                DrawType::Color => self.add_color_draw(&draw.vertices, transform),
                DrawType::Gradient { matrix, gradient } => {
                    let Some(Some(texture)) = shape.gradients.get(*gradient) else {
                        continue;
                    };
                    self.add_gradient_draw(
                        &draw.vertices,
                        transform,
                        *matrix,
                        matches!(
                            shape.mesh.gradients[*gradient].gradient_type,
                            GradientType::Linear
                        ),
                        texture_tint(transform),
                    );
                    let _ = texture;
                }
                DrawType::Bitmap(bitmap) => {
                    let Some((_, handle)) = shape
                        .bitmaps
                        .iter()
                        .find(|(id, _)| *id == bitmap.bitmap_id)
                    else {
                        continue;
                    };
                    if texture_from_handle(handle).is_none() {
                        continue;
                    }
                    self.add_bitmap_draw(
                        &draw.vertices,
                        transform,
                        bitmap.matrix,
                        texture_tint(transform),
                    );
                }
            }
        }
        if let Some(key) = key {
            self.pending_cache_writes.push(PendingShapeCacheWrite {
                key,
                color_start,
                color_end: self.color_vertices.len(),
                tex_start,
                tex_end: self.tex_vertices.len(),
            });
        }
    }

    fn collect_commands(&mut self, commands: &CommandList) {
        for command in &commands.commands {
            match command {
                Command::RenderShape { shape, transform } => {
                    let any = shape.0.as_ref() as &dyn Any;
                    if let Some(shape) = any.downcast_ref::<VitaShapeHandle>() {
                        self.collect_shape(shape, transform);
                    }
                }
                Command::RenderAlphaMask {
                    maskee_commands,
                    mask_commands: _,
                } => {
                    // The Vita backend currently renders only the maskee branch.
                    self.collect_commands(maskee_commands);
                }
                Command::Blend(commands, _) => self.collect_commands(commands),
                _ => {}
            }
        }
    }

    fn prepare(&mut self, commands: &CommandList) {
        self.clear();
        self.frame_id = self.frame_id.wrapping_add(1);
        let min_frame = self.frame_id.saturating_sub(2);
        self.transform_cache
            .retain(|_, entry| entry.last_used >= min_frame);
        self.collect_commands(commands);
    }

    fn execute(&mut self) {
        if self.jobs.is_empty() {
            return;
        }

        let mut context = FrameTransformContext {
            jobs: self.jobs.as_ptr(),
            job_count: self.jobs.len(),
            color_vertices: self.color_vertices.as_mut_ptr(),
            tex_vertices: self.tex_vertices.as_mut_ptr(),
            next_job: AtomicUsize::new(0),
        };

        let parallel = self.jobs.len() >= 3
            && unsafe {
                flashvita_vita_parallel_for(
                    3,
                    1,
                    transform_frame_jobs_worker,
                    (&mut context as *mut FrameTransformContext).cast(),
                )
            } > 0;

        if parallel {
            STAT_PARALLEL_BATCHES.fetch_add(1, Ordering::Relaxed);
            STAT_PARALLEL_DRAWS.fetch_add(self.transformed_draws as u64, Ordering::Relaxed);
            STAT_PARALLEL_JOBS.fetch_add(self.jobs.len() as u64, Ordering::Relaxed);
        } else {
            unsafe {
                transform_frame_jobs_worker(
                    (&mut context as *mut FrameTransformContext).cast(),
                    0,
                    1,
                );
            }
        }

        for pending in self.pending_cache_writes.drain(..) {
            self.transform_cache.insert(
                pending.key,
                CachedShapeVertices {
                    color: self.color_vertices[pending.color_start..pending.color_end].to_vec(),
                    tex: self.tex_vertices[pending.tex_start..pending.tex_end].to_vec(),
                    last_used: self.frame_id,
                },
            );
        }
    }
}

pub struct VitaRenderer {
    dimensions: ViewportDimensions,
    tessellator: ShapeTessellator,
    quality: StageQuality,
    color_scratch: Vec<VitaVertex>,
    tex_scratch: Vec<VitaTexVertex>,
    color_index_scratch: Vec<u16>,
    tex_index_scratch: Vec<u16>,
    bitmap_upload_scratch: Vec<u8>,
    frame_batch: FrameShapeBatch,
    gradient_cache: HashMap<u64, Weak<VitaGradientTexture>>,
    zero_copy_bitmapdata: bool,
}

impl VitaRenderer {
    pub fn new(dimensions: ViewportDimensions, zero_copy_bitmapdata: bool) -> Self {
        Self {
            dimensions,
            tessellator: ShapeTessellator::new(),
            quality: StageQuality::High,
            color_scratch: Vec::new(),
            tex_scratch: Vec::new(),
            color_index_scratch: Vec::new(),
            tex_index_scratch: Vec::new(),
            bitmap_upload_scratch: Vec::new(),
            frame_batch: FrameShapeBatch::default(),
            gradient_cache: HashMap::new(),
            zero_copy_bitmapdata,
        }
    }

    fn render_commands_to_texture(
        &mut self,
        handle: &BitmapHandle,
        commands: CommandList,
        clear: Color,
        bounds: PixelRegion,
    ) -> bool {
        let Some(texture) = texture_from_handle(handle) else {
            return false;
        };
        if texture.texture == 0 || bounds.is_empty() {
            return false;
        }

        self.frame_batch.prepare(&commands);
        self.frame_batch.execute();
        let begin_result = unsafe {
            flashvita_vitagl_begin_offscreen(
                texture.texture,
                texture.width,
                texture.height,
                bounds.x_min,
                bounds.y_min,
                bounds.x_max,
                bounds.y_max,
                clear.r,
                clear.g,
                clear.b,
                clear.a,
            )
        };
        if begin_result < 0 {
            return false;
        }

        let mut handler = VitaCommandHandler::new(
            &mut self.color_scratch,
            &mut self.tex_scratch,
            &mut self.color_index_scratch,
            &mut self.tex_index_scratch,
            &self.frame_batch.color_vertices,
            &self.frame_batch.tex_vertices,
        );
        commands.execute(&mut handler);
        handler.flush_batches();
        unsafe { flashvita_vitagl_end_offscreen() };
        texture.initialized.store(true, Ordering::Relaxed);
        // CPU-uploaded bitmaps treat v=0 as Flash's top row. An OpenGL FBO
        // writes that same logical top row at v=1, so remember that this
        // texture needs inverted V coordinates when sampled later.
        texture.offscreen_y_flipped.store(true, Ordering::Relaxed);
        true
    }
}

fn transform_point(matrix: Matrix, x: f32, y: f32) -> (f32, f32) {
    transform_point_fast(matrix.into(), x, y)
}

#[inline(always)]
fn transform_point_fast(matrix: VitaMatrix2D, x: f32, y: f32) -> (f32, f32) {
    (
        matrix.a * x + matrix.c * y + matrix.tx,
        matrix.b * x + matrix.d * y + matrix.ty,
    )
}

fn vertex(x: f32, y: f32, color: Color) -> VitaVertex {
    VitaVertex {
        x,
        y,
        r: color.r,
        g: color.g,
        b: color.b,
        a: color.a,
    }
}

fn tex_vertex(x: f32, y: f32, u: f32, v: f32, color: Color) -> VitaTexVertex {
    VitaTexVertex {
        x,
        y,
        u,
        v,
        r: color.r,
        g: color.g,
        b: color.b,
        a: color.a,
    }
}

fn texture_tint(transform: &Transform) -> Color {
    let mult = transform.color_transform.mult_rgba_normalized();
    let alpha = mult[3].clamp(0.0, 1.0);
    Color {
        r: (mult[0].clamp(0.0, 1.0) * alpha * 255.0).round() as u8,
        g: (mult[1].clamp(0.0, 1.0) * alpha * 255.0).round() as u8,
        b: (mult[2].clamp(0.0, 1.0) * alpha * 255.0).round() as u8,
        a: (alpha * 255.0).round() as u8,
    }
}

fn premultiplied_rgba<'a>(bitmap: Bitmap<'a>) -> (u32, u32, Cow<'a, [u8]>) {
    let width = bitmap.width();
    let height = bitmap.height();
    let bitmap = bitmap.to_rgba();
    (width, height, bitmap.into_buf())
}

fn pack_rgba_region(source: &[u8], source_width: u32, region: PixelRegion, out: &mut Vec<u8>) {
    let row_bytes = region.width() as usize * 4;
    let source_stride = source_width as usize * 4;
    out.clear();
    out.reserve(row_bytes * region.height() as usize);
    for y in region.y_min..region.y_max {
        let start = y as usize * source_stride + region.x_min as usize * 4;
        out.extend_from_slice(&source[start..start + row_bytes]);
    }
}

fn pack_bitmap_region_rgba(bitmap: &Bitmap<'_>, region: PixelRegion, out: &mut Vec<u8>) {
    match bitmap.format() {
        BitmapFormat::Rgba => pack_rgba_region(bitmap.data(), bitmap.width(), region, out),
        BitmapFormat::Rgb => {
            let source = bitmap.data();
            let source_stride = bitmap.width() as usize * 3;
            out.clear();
            out.reserve(region.width() as usize * region.height() as usize * 4);
            for y in region.y_min..region.y_max {
                let row = &source[y as usize * source_stride..(y as usize + 1) * source_stride];
                for x in region.x_min..region.x_max {
                    let offset = x as usize * 3;
                    out.extend_from_slice(&[row[offset], row[offset + 1], row[offset + 2], 255]);
                }
            }
        }
        BitmapFormat::Yuv420p | BitmapFormat::Yuva420p => {
            // Video formats need chroma conversion. Keep the generic path for
            // them; BitmapData updates (the hot Flash path) are already RGBA.
            let rgba = bitmap.reborrow().to_rgba();
            pack_rgba_region(rgba.data(), bitmap.width(), region, out);
        }
    }
}

#[cfg(test)]
mod bitmap_region_tests {
    use super::*;

    #[test]
    fn packs_only_selected_rows_and_columns() {
        let source: Vec<u8> = (0..4 * 3 * 4).collect();
        let mut packed = vec![255; 64];
        pack_rgba_region(&source, 4, PixelRegion::for_region(1, 1, 2, 2), &mut packed);
        assert_eq!(packed, [&source[20..28], &source[36..44]].concat());
    }
}

fn texture_from_handle(handle: &BitmapHandle) -> Option<&VitaBitmapHandle> {
    let any = handle.0.as_ref() as &dyn Any;
    any.downcast_ref::<VitaBitmapHandle>()
}

fn compact_mesh_indices(mesh: &Mesh) -> Vec<Option<Vec<u16>>> {
    mesh.draws
        .iter()
        .map(|draw| {
            draw.indices
                .iter()
                .copied()
                .map(u16::try_from)
                .collect::<Result<Vec<_>, _>>()
                .ok()
        })
        .collect()
}

fn create_gpu_draws(mesh: &Mesh, indices_u16: &[Option<Vec<u16>>]) -> Vec<Option<VitaGpuMesh>> {
    const GPU_TRANSFORM_MIN_VERTICES: usize = 512;
    const GPU_TRANSFORM_MAX_DRAWS: usize = 8;

    let vertex_count = mesh.draws.iter().map(|draw| draw.vertices.len()).sum::<usize>();
    let eligible = vertex_count >= GPU_TRANSFORM_MIN_VERTICES
        && mesh.draws.len() <= GPU_TRANSFORM_MAX_DRAWS
        && unsafe { flashvita_vitagl_gpu_transform_available() } > 0;
    if !eligible {
        return (0..mesh.draws.len()).map(|_| None).collect();
    }

    mesh.draws
        .iter()
        .zip(indices_u16)
        .map(|(draw, compact)| {
            let indices = compact.as_deref()?;
            let index_count = indices.len();
            if index_count < 3 {
                return None;
            }

            let handle = match &draw.draw_type {
                DrawType::Color => {
                    let vertices = draw
                        .vertices
                        .iter()
                        .map(|source| vertex(source.x, source.y, source.color))
                        .collect::<Vec<_>>();
                    unsafe {
                        flashvita_vitagl_create_gpu_mesh(
                            vertices.as_ptr().cast(),
                            std::mem::size_of_val(vertices.as_slice()),
                            indices.as_ptr(),
                            index_count,
                        )
                    }
                }
                DrawType::Gradient { matrix, gradient } => {
                    let linear = matches!(
                        mesh.gradients[*gradient].gradient_type,
                        GradientType::Linear
                    );
                    let vertices = draw
                        .vertices
                        .iter()
                        .map(|source| {
                            let (u, v) = texture_uv(matrix, source.x, source.y);
                            tex_vertex(
                                source.x,
                                source.y,
                                u,
                                if linear { 0.5 } else { v },
                                Color::WHITE,
                            )
                        })
                        .collect::<Vec<_>>();
                    unsafe {
                        flashvita_vitagl_create_gpu_mesh(
                            vertices.as_ptr().cast(),
                            std::mem::size_of_val(vertices.as_slice()),
                            indices.as_ptr(),
                            index_count,
                        )
                    }
                }
                DrawType::Bitmap(bitmap) => {
                    let vertices = draw
                        .vertices
                        .iter()
                        .map(|source| {
                            let (u, v) = texture_uv(&bitmap.matrix, source.x, source.y);
                            tex_vertex(source.x, source.y, u, v, Color::WHITE)
                        })
                        .collect::<Vec<_>>();
                    unsafe {
                        flashvita_vitagl_create_gpu_mesh(
                            vertices.as_ptr().cast(),
                            std::mem::size_of_val(vertices.as_slice()),
                            indices.as_ptr(),
                            index_count,
                        )
                    }
                }
            };

            (handle != 0).then_some(VitaGpuMesh { handle, index_count })
        })
        .collect()
}

unsafe fn draw_textured_compact_or_u32(
    texture: u32,
    vertices: &[VitaTexVertex],
    compact: Option<&[u16]>,
    original: &[u32],
    index_count: usize,
    smoothing: u8,
    wrap_mode: u8,
) {
    if let Some(indices) = compact {
        flashvita_vitagl_draw_textured_triangles_u16(
            texture,
            vertices.as_ptr(),
            vertices.len(),
            indices.as_ptr(),
            index_count,
            smoothing,
            wrap_mode,
        );
    } else {
        flashvita_vitagl_draw_textured_triangles(
            texture,
            vertices.as_ptr(),
            vertices.len(),
            original.as_ptr(),
            index_count,
            smoothing,
            wrap_mode,
        );
    }
}

fn texture_uv(matrix: &[[f32; 3]; 3], x: f32, y: f32) -> (f32, f32) {
    (
        matrix[0][0] * x + matrix[1][0] * y + matrix[2][0],
        matrix[0][1] * x + matrix[1][1] * y + matrix[2][1],
    )
}

fn srgb_to_linear(v: f32) -> f32 {
    if v <= 0.04045 {
        v / 12.92
    } else {
        ((v + 0.055) / 1.055).powf(2.4)
    }
}

fn linear_to_srgb(v: f32) -> f32 {
    if v <= 0.0031308 {
        v * 12.92
    } else {
        1.055 * v.powf(1.0 / 2.4) - 0.055
    }
}

fn gradient_color(gradient: &TessGradient, t: f32) -> Color {
    let Some(first) = gradient.records.first() else {
        return Color::TRANSPARENT;
    };
    if gradient.records.len() == 1 {
        return first.color;
    }

    let t = t.clamp(0.0, 1.0);
    let ratio = t * 255.0;
    let mut left = first;
    let mut right = gradient.records.last().unwrap_or(first);
    for pair in gradient.records.windows(2) {
        if ratio >= pair[0].ratio as f32 && ratio <= pair[1].ratio as f32 {
            left = &pair[0];
            right = &pair[1];
            break;
        }
    }

    if ratio <= left.ratio as f32 {
        return left.color;
    }
    if ratio >= right.ratio as f32 {
        return right.color;
    }

    let span = (right.ratio - left.ratio).max(1) as f32;
    let mix = ((ratio - left.ratio as f32) / span).clamp(0.0, 1.0);
    let lerp = |a: u8, b: u8, linear_rgb: bool| -> u8 {
        let a = a as f32 / 255.0;
        let b = b as f32 / 255.0;
        let value = if linear_rgb {
            linear_to_srgb(srgb_to_linear(a) + (srgb_to_linear(b) - srgb_to_linear(a)) * mix)
        } else {
            a + (b - a) * mix
        };
        (value.clamp(0.0, 1.0) * 255.0).round() as u8
    };
    let linear_rgb = matches!(gradient.interpolation, swf::GradientInterpolation::LinearRgb);
    Color {
        r: lerp(left.color.r, right.color.r, linear_rgb),
        g: lerp(left.color.g, right.color.g, linear_rgb),
        b: lerp(left.color.b, right.color.b, linear_rgb),
        a: lerp(left.color.a, right.color.a, false),
    }
}

fn apply_gradient_spread(t: f32, spread: swf::GradientSpread) -> f32 {
    match spread {
        swf::GradientSpread::Pad => t.clamp(0.0, 1.0),
        swf::GradientSpread::Repeat => t.rem_euclid(1.0),
        swf::GradientSpread::Reflect => {
            let t = t.abs();
            let whole = t.floor() as i32;
            let frac = t.fract();
            if whole & 1 == 0 { frac } else { 1.0 - frac }
        }
    }
}

fn gradient_cache_key(width: u32, height: u32, wrap_mode: u8, pixels: &[u8]) -> u64 {
    let mut hash = 0xcbf29ce484222325u64;
    for byte in width
        .to_le_bytes()
        .into_iter()
        .chain(height.to_le_bytes())
        .chain([wrap_mode])
        .chain(pixels.iter().copied())
    {
        hash ^= byte as u64;
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash
}

fn create_gradient_texture(
    gradient: &TessGradient,
    cache: &mut HashMap<u64, Weak<VitaGradientTexture>>,
) -> Option<Arc<VitaGradientTexture>> {
    if cache.len() > 256 {
        cache.retain(|_, texture| texture.strong_count() != 0);
    }
    let (width, height) = match gradient.gradient_type {
        GradientType::Linear => (256u32, 1u32),
        GradientType::Radial | GradientType::Focal => (64u32, 64u32),
    };
    let mut pixels = vec![0u8; (width * height * 4) as usize];
    let focal = gradient.focal_point.to_f32().clamp(-0.98, 0.98);

    for y in 0..height {
        for x in 0..width {
            let u = if width > 1 { x as f32 / (width - 1) as f32 } else { 0.5 };
            let v = if height > 1 { y as f32 / (height - 1) as f32 } else { 0.5 };
            let t = match gradient.gradient_type {
                GradientType::Linear => u,
                GradientType::Radial => {
                    let dx = u * 2.0 - 1.0;
                    let dy = v * 2.0 - 1.0;
                    (dx * dx + dy * dy).sqrt()
                }
                GradientType::Focal => {
                    let ux = u * 2.0 - 1.0;
                    let uy = v * 2.0 - 1.0;
                    let mut dx = focal - ux;
                    let mut dy = -uy;
                    let len = (dx * dx + dy * dy).sqrt().max(1.0e-6);
                    dx /= len;
                    dy /= len;
                    let denom = (1.0 - focal * focal * dy * dy).max(0.0).sqrt() + focal * dx;
                    if denom.abs() > 1.0e-6 { len / denom } else { 1.0 }
                }
            };
            let color = gradient_color(gradient, apply_gradient_spread(t, gradient.repeat_mode));
            let offset = ((y * width + x) * 4) as usize;
            let alpha = color.a as u16;
            pixels[offset] = ((color.r as u16 * alpha + 127) / 255) as u8;
            pixels[offset + 1] = ((color.g as u16 * alpha + 127) / 255) as u8;
            pixels[offset + 2] = ((color.b as u16 * alpha + 127) / 255) as u8;
            pixels[offset + 3] = color.a;
        }
    }

    let wrap_mode = match gradient.repeat_mode {
        swf::GradientSpread::Pad => 0,
        swf::GradientSpread::Repeat => 1,
        swf::GradientSpread::Reflect => 2,
    };
    let key = gradient_cache_key(width, height, wrap_mode, &pixels);
    if let Some(texture) = cache.get(&key).and_then(Weak::upgrade) {
        return Some(texture);
    }

    let texture = unsafe { flashvita_vitagl_create_texture(pixels.as_ptr(), width, height) };
    if texture == 0 {
        return None;
    }
    STAT_BITMAP_UPLOADS.fetch_add(1, Ordering::Relaxed);
    STAT_BITMAP_UPLOADED_BYTES.fetch_add(pixels.len() as u64, Ordering::Relaxed);
    let texture = Arc::new(VitaGradientTexture { texture, wrap_mode });
    cache.insert(key, Arc::downgrade(&texture));
    Some(texture)
}

struct VitaCommandHandler<'a> {
    mask_depth: u32,
    drawing_mask: bool,
    color_scratch: &'a mut Vec<VitaVertex>,
    tex_scratch: &'a mut Vec<VitaTexVertex>,
    prepared_color: &'a [VitaVertex],
    prepared_tex: &'a [VitaTexVertex],
    prepared_color_cursor: usize,
    prepared_tex_cursor: usize,
    color_batch_indices: &'a mut Vec<u16>,
    tex_batch_indices: &'a mut Vec<u16>,
    color_batch_start: Option<usize>,
    color_batch_end: usize,
    tex_batch_start: Option<usize>,
    tex_batch_end: usize,
    tex_batch_texture: u32,
    tex_batch_smoothing: u8,
    tex_batch_wrap: u8,
}

impl<'a> VitaCommandHandler<'a> {
    fn new(
        color_scratch: &'a mut Vec<VitaVertex>,
        tex_scratch: &'a mut Vec<VitaTexVertex>,
        color_batch_indices: &'a mut Vec<u16>,
        tex_batch_indices: &'a mut Vec<u16>,
        prepared_color: &'a [VitaVertex],
        prepared_tex: &'a [VitaTexVertex],
    ) -> Self {
        Self {
            mask_depth: 0,
            drawing_mask: false,
            color_scratch,
            tex_scratch,
            color_batch_indices,
            tex_batch_indices,
            prepared_color,
            prepared_tex,
            prepared_color_cursor: 0,
            prepared_tex_cursor: 0,
            color_batch_start: None,
            color_batch_end: 0,
            tex_batch_start: None,
            tex_batch_end: 0,
            tex_batch_texture: 0,
            tex_batch_smoothing: 0,
            tex_batch_wrap: 0,
        }
    }

    #[inline]
    fn flush_color_batch(&mut self) {
        let Some(start) = self.color_batch_start.take() else {
            return;
        };
        let end = self.color_batch_end;
        if end > start && !self.color_batch_indices.is_empty() {
            let vertices = &self.prepared_color[start..end];
            STAT_COLOR_SUBMISSIONS.fetch_add(1, Ordering::Relaxed);
            unsafe {
                flashvita_vitagl_draw_colored_triangles_u16(
                    vertices.as_ptr(),
                    vertices.len(),
                    self.color_batch_indices.as_ptr(),
                    self.color_batch_indices.len(),
                );
            }
        }
        self.color_batch_indices.clear();
        self.color_batch_end = 0;
    }

    #[inline]
    fn flush_texture_batch(&mut self) {
        let Some(start) = self.tex_batch_start.take() else {
            return;
        };
        let end = self.tex_batch_end;
        if end > start && !self.tex_batch_indices.is_empty() && self.tex_batch_texture != 0 {
            let vertices = &self.prepared_tex[start..end];
            unsafe {
                flashvita_vitagl_draw_textured_triangles_u16(
                    self.tex_batch_texture,
                    vertices.as_ptr(),
                    vertices.len(),
                    self.tex_batch_indices.as_ptr(),
                    self.tex_batch_indices.len(),
                    self.tex_batch_smoothing,
                    self.tex_batch_wrap,
                );
            }
        }
        self.tex_batch_indices.clear();
        self.tex_batch_end = 0;
        self.tex_batch_texture = 0;
    }

    #[inline]
    fn flush_batches(&mut self) {
        self.flush_color_batch();
        self.flush_texture_batch();
    }

    #[inline]
    fn queue_prepared_color_draw(
        &mut self,
        indices: &[u16],
        start: usize,
        end: usize,
        index_count: usize,
    ) {
        if self.color_batch_start.is_some() && start != self.color_batch_end {
            self.flush_color_batch();
        }
        if let Some(batch_start) = self.color_batch_start {
            if end.saturating_sub(batch_start) > (u16::MAX as usize + 1) {
                self.flush_color_batch();
            }
        }
        let batch_start = *self.color_batch_start.get_or_insert(start);
        let base = (start - batch_start) as u16;
        self.color_batch_indices.reserve(index_count);
        self.color_batch_indices.extend(
            indices[..index_count]
                .iter()
                .map(|index| base.wrapping_add(*index)),
        );
        self.color_batch_end = end;
    }

    #[inline]
    fn queue_prepared_texture_draw(
        &mut self,
        texture: u32,
        smoothing: u8,
        wrap_mode: u8,
        indices: &[u16],
        start: usize,
        end: usize,
        index_count: usize,
    ) {
        let state_mismatch = self.tex_batch_start.is_some()
            && (texture != self.tex_batch_texture
                || smoothing != self.tex_batch_smoothing
                || wrap_mode != self.tex_batch_wrap
                || start != self.tex_batch_end);
        if state_mismatch {
            self.flush_texture_batch();
        }
        if let Some(batch_start) = self.tex_batch_start {
            if end.saturating_sub(batch_start) > (u16::MAX as usize + 1) {
                self.flush_texture_batch();
            }
        }
        let batch_start = *self.tex_batch_start.get_or_insert(start);
        if self.tex_batch_texture == 0 {
            self.tex_batch_texture = texture;
            self.tex_batch_smoothing = smoothing;
            self.tex_batch_wrap = wrap_mode;
        }
        let base = (start - batch_start) as u16;
        self.tex_batch_indices.reserve(index_count);
        self.tex_batch_indices.extend(
            indices[..index_count]
                .iter()
                .map(|index| base.wrapping_add(*index)),
        );
        self.tex_batch_end = end;
    }

    #[inline]
    fn index_count(&self, draw: &ruffle_render::tessellator::Draw) -> usize {
        if self.drawing_mask {
            draw.mask_index_count as usize
        } else {
            draw.indices.len()
        }
    }

    #[inline]
    fn prepare_color_vertices(
        &mut self,
        vertices: &[ruffle_render::tessellator::Vertex],
        transform: &Transform,
    ) {
        STAT_TRANSFORMED_VERTICES.fetch_add(vertices.len() as u64, Ordering::Relaxed);
        self.color_scratch.clear();
        self.color_scratch.reserve(vertices.len());

        unsafe {
            self.color_scratch.set_len(vertices.len());
        }

        let mut job = ColorTransformJob {
            source: vertices.as_ptr(),
            destination: self.color_scratch.as_mut_ptr(),
            matrix: transform.matrix.into(),
            color_transform: transform.color_transform,
            color_identity: transform.color_transform == ColorTransform::IDENTITY,
        };

        let parallel = vertices.len() >= NATIVE_PARALLEL_VERTEX_THRESHOLD
            && unsafe {
                flashvita_vita_parallel_for(
                    vertices.len() as u32,
                    NATIVE_PARALLEL_VERTEX_THRESHOLD as u32,
                    transform_color_chunk,
                    (&mut job as *mut ColorTransformJob).cast(),
                )
            } > 0;

        if parallel {
            STAT_PARALLEL_DRAWS.fetch_add(1, Ordering::Relaxed);
        } else {
            unsafe {
                transform_color_chunk(
                    (&mut job as *mut ColorTransformJob).cast(),
                    0,
                    vertices.len() as u32,
                );
            }
        }
    }

    #[inline]
    fn prepare_gradient_vertices(
        &mut self,
        vertices: &[ruffle_render::tessellator::Vertex],
        transform: &Transform,
        texture_matrix: [[f32; 3]; 3],
        linear: bool,
        tint: Color,
    ) {
        STAT_TRANSFORMED_VERTICES.fetch_add(vertices.len() as u64, Ordering::Relaxed);
        self.tex_scratch.clear();
        self.tex_scratch.reserve(vertices.len());

        unsafe {
            self.tex_scratch.set_len(vertices.len());
        }

        let mut job = GradientTransformJob {
            source: vertices.as_ptr(),
            destination: self.tex_scratch.as_mut_ptr(),
            matrix: transform.matrix.into(),
            texture_matrix,
            linear,
            tint,
        };

        let parallel = vertices.len() >= NATIVE_PARALLEL_VERTEX_THRESHOLD
            && unsafe {
                flashvita_vita_parallel_for(
                    vertices.len() as u32,
                    NATIVE_PARALLEL_VERTEX_THRESHOLD as u32,
                    transform_gradient_chunk,
                    (&mut job as *mut GradientTransformJob).cast(),
                )
            } > 0;

        if parallel {
            STAT_PARALLEL_DRAWS.fetch_add(1, Ordering::Relaxed);
        } else {
            unsafe {
                transform_gradient_chunk(
                    (&mut job as *mut GradientTransformJob).cast(),
                    0,
                    vertices.len() as u32,
                );
            }
        }
    }

    #[inline]
    fn prepare_bitmap_vertices(
        &mut self,
        vertices: &[ruffle_render::tessellator::Vertex],
        transform: &Transform,
        texture_matrix: [[f32; 3]; 3],
        tint: Color,
    ) {
        STAT_TRANSFORMED_VERTICES.fetch_add(vertices.len() as u64, Ordering::Relaxed);
        self.tex_scratch.clear();
        self.tex_scratch.reserve(vertices.len());

        unsafe {
            self.tex_scratch.set_len(vertices.len());
        }

        let mut job = BitmapTransformJob {
            source: vertices.as_ptr(),
            destination: self.tex_scratch.as_mut_ptr(),
            matrix: transform.matrix.into(),
            texture_matrix,
            tint,
        };

        let parallel = vertices.len() >= NATIVE_PARALLEL_VERTEX_THRESHOLD
            && unsafe {
                flashvita_vita_parallel_for(
                    vertices.len() as u32,
                    NATIVE_PARALLEL_VERTEX_THRESHOLD as u32,
                    transform_bitmap_chunk,
                    (&mut job as *mut BitmapTransformJob).cast(),
                )
            } > 0;

        if parallel {
            STAT_PARALLEL_DRAWS.fetch_add(1, Ordering::Relaxed);
        } else {
            unsafe {
                transform_bitmap_chunk(
                    (&mut job as *mut BitmapTransformJob).cast(),
                    0,
                    vertices.len() as u32,
                );
            }
        }
    }
}

impl VitaCommandHandler<'_> {
    fn draw_shape_mesh(&mut self, shape: &VitaShapeHandle, transform: &Transform) {
        for (draw_index, draw) in shape.mesh.draws.iter().enumerate() {
            let compact_indices = shape.indices_u16[draw_index].as_deref();
            if let Some(gpu) = shape.gpu_draws[draw_index].as_ref() {
                self.flush_batches();
                let gpu_transform = VitaGpuTransform::from(transform);
                let index_count = self.index_count(draw).min(gpu.index_count);
                match &draw.draw_type {
                    DrawType::Color => {
                        unsafe {
                            flashvita_vitagl_draw_gpu_colored(
                                gpu.handle,
                                index_count,
                                &gpu_transform,
                            );
                        }
                        STAT_COLOR_SUBMISSIONS.fetch_add(1, Ordering::Relaxed);
                        STAT_COLORED_DRAWS.fetch_add(1, Ordering::Relaxed);
                    }
                    DrawType::Gradient { gradient, .. } => {
                        let Some(Some(texture)) = shape.gradients.get(*gradient) else {
                            STAT_GRADIENT_SKIPS.fetch_add(1, Ordering::Relaxed);
                            continue;
                        };
                        unsafe {
                            flashvita_vitagl_draw_gpu_textured(
                                gpu.handle,
                                index_count,
                                texture.texture,
                                &gpu_transform,
                                1,
                                texture.wrap_mode,
                            );
                        }
                        STAT_TEXTURED_DRAWS.fetch_add(1, Ordering::Relaxed);
                    }
                    DrawType::Bitmap(bitmap) => {
                        let Some((_, handle)) = shape
                            .bitmaps
                            .iter()
                            .find(|(id, _)| *id == bitmap.bitmap_id)
                        else {
                            STAT_MISSING_BITMAPS.fetch_add(1, Ordering::Relaxed);
                            continue;
                        };
                        let Some(texture) = texture_from_handle(handle) else {
                            STAT_MISSING_BITMAPS.fetch_add(1, Ordering::Relaxed);
                            continue;
                        };
                        unsafe {
                            flashvita_vitagl_draw_gpu_textured(
                                gpu.handle,
                                index_count,
                                texture.texture,
                                &gpu_transform,
                                u8::from(bitmap.is_smoothed),
                                u8::from(bitmap.is_repeating),
                            );
                        }
                        STAT_TEXTURED_DRAWS.fetch_add(1, Ordering::Relaxed);
                    }
                }
                continue;
            }
            match &draw.draw_type {
                DrawType::Color => {
                    self.flush_texture_batch();
                    let index_count = self.index_count(draw);
                    let count = draw.vertices.len();
                    let start = self.prepared_color_cursor;
                    let end = start.saturating_add(count);
                    if end <= self.prepared_color.len() && compact_indices.is_some() {
                        self.prepared_color_cursor = end;
                        self.queue_prepared_color_draw(
                            compact_indices.unwrap(),
                            start,
                            end,
                            index_count,
                        );
                    } else if end <= self.prepared_color.len() {
                        self.prepared_color_cursor = end;
                        self.flush_color_batch();
                        let vertices = &self.prepared_color[start..end];
                        STAT_COLOR_SUBMISSIONS.fetch_add(1, Ordering::Relaxed);
                        unsafe {
                            flashvita_vitagl_draw_colored_triangles(
                                vertices.as_ptr(),
                                vertices.len(),
                                draw.indices.as_ptr(),
                                index_count,
                            );
                        }
                    } else {
                        self.flush_color_batch();
                        self.prepare_color_vertices(&draw.vertices, transform);
                        STAT_COLOR_SUBMISSIONS.fetch_add(1, Ordering::Relaxed);
                        unsafe {
                            if let Some(indices) = compact_indices {
                                flashvita_vitagl_draw_colored_triangles_u16(
                                    self.color_scratch.as_ptr(),
                                    self.color_scratch.len(),
                                    indices.as_ptr(),
                                    index_count,
                                );
                            } else {
                                flashvita_vitagl_draw_colored_triangles(
                                    self.color_scratch.as_ptr(),
                                    self.color_scratch.len(),
                                    draw.indices.as_ptr(),
                                    index_count,
                                );
                            }
                        }
                    }
                    STAT_COLORED_DRAWS.fetch_add(1, Ordering::Relaxed);
                }
                DrawType::Gradient { matrix, gradient } => {
                    self.flush_color_batch();
                    let Some(Some(texture)) = shape.gradients.get(*gradient) else {
                        STAT_GRADIENT_SKIPS.fetch_add(1, Ordering::Relaxed);
                        continue;
                    };
                    let index_count = self.index_count(draw);
                    let count = draw.vertices.len();
                    let start = self.prepared_tex_cursor;
                    let end = start.saturating_add(count);
                    if end <= self.prepared_tex.len() && compact_indices.is_some() {
                        self.prepared_tex_cursor = end;
                        self.queue_prepared_texture_draw(
                            texture.texture,
                            1,
                            texture.wrap_mode,
                            compact_indices.unwrap(),
                            start,
                            end,
                            index_count,
                        );
                    } else if end <= self.prepared_tex.len() {
                        self.prepared_tex_cursor = end;
                        self.flush_texture_batch();
                        let vertices = &self.prepared_tex[start..end];
                        unsafe {
                            draw_textured_compact_or_u32(
                                texture.texture,
                                vertices,
                                None,
                                &draw.indices,
                                index_count,
                                1,
                                texture.wrap_mode,
                            );
                        }
                    } else {
                        self.flush_texture_batch();
                        let tint = texture_tint(transform);
                        self.prepare_gradient_vertices(
                            &draw.vertices,
                            transform,
                            *matrix,
                            matches!(
                                shape.mesh.gradients[*gradient].gradient_type,
                                GradientType::Linear
                            ),
                            tint,
                        );
                        unsafe {
                            draw_textured_compact_or_u32(
                                texture.texture,
                                self.tex_scratch,
                                compact_indices,
                                &draw.indices,
                                index_count,
                                1,
                                texture.wrap_mode,
                            );
                        }
                    }
                    STAT_TEXTURED_DRAWS.fetch_add(1, Ordering::Relaxed);
                }
                DrawType::Bitmap(bitmap) => {
                    self.flush_color_batch();
                    let Some((_, handle)) = shape
                        .bitmaps
                        .iter()
                        .find(|(id, _)| *id == bitmap.bitmap_id)
                    else {
                        STAT_MISSING_BITMAPS.fetch_add(1, Ordering::Relaxed);
                        continue;
                    };
                    let Some(texture) = texture_from_handle(handle) else {
                        STAT_MISSING_BITMAPS.fetch_add(1, Ordering::Relaxed);
                        continue;
                    };
                    let index_count = self.index_count(draw);
                    let count = draw.vertices.len();
                    let start = self.prepared_tex_cursor;
                    let end = start.saturating_add(count);
                    if end <= self.prepared_tex.len() && compact_indices.is_some() {
                        self.prepared_tex_cursor = end;
                        self.queue_prepared_texture_draw(
                            texture.texture,
                            u8::from(bitmap.is_smoothed),
                            u8::from(bitmap.is_repeating),
                            compact_indices.unwrap(),
                            start,
                            end,
                            index_count,
                        );
                    } else if end <= self.prepared_tex.len() {
                        self.prepared_tex_cursor = end;
                        self.flush_texture_batch();
                        let vertices = &self.prepared_tex[start..end];
                        unsafe {
                            draw_textured_compact_or_u32(
                                texture.texture,
                                vertices,
                                None,
                                &draw.indices,
                                index_count,
                                u8::from(bitmap.is_smoothed),
                                u8::from(bitmap.is_repeating),
                            );
                        }
                    } else {
                        self.flush_texture_batch();
                        let tint = texture_tint(transform);
                        self.prepare_bitmap_vertices(
                            &draw.vertices,
                            transform,
                            bitmap.matrix,
                            tint,
                        );
                        unsafe {
                            draw_textured_compact_or_u32(
                                texture.texture,
                                self.tex_scratch,
                                compact_indices,
                                &draw.indices,
                                index_count,
                                u8::from(bitmap.is_smoothed),
                                u8::from(bitmap.is_repeating),
                            );
                        }
                    }
                    STAT_TEXTURED_DRAWS.fetch_add(1, Ordering::Relaxed);
                }
            }
        }
    }

    fn draw_unit_quad(&mut self, color: Color, matrix: Matrix) {
        self.flush_batches();
        let points = [(0.0f32, 0.0f32), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)];
        let mut vertices = [vertex(0.0, 0.0, color); 4];
        for (i, (x, y)) in points.into_iter().enumerate() {
            let (x, y) = transform_point(matrix, x, y);
            vertices[i] = vertex(x, y, color);
        }
        let indices = [0u16, 1, 2, 0, 2, 3];
        STAT_COLOR_SUBMISSIONS.fetch_add(1, Ordering::Relaxed);
        unsafe {
            flashvita_vitagl_draw_colored_triangles_u16(
                vertices.as_ptr(),
                vertices.len(),
                indices.as_ptr(),
                indices.len(),
            );
        }
    }

    fn draw_line_points(&mut self, color: Color, matrix: Matrix, rect: bool) {
        self.flush_batches();
        let source: &[(f32, f32)] = if rect {
            &[(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0), (0.0, 0.0)]
        } else {
            &[(0.0, 0.0), (1.0, 0.0)]
        };
        let mut vertices = Vec::with_capacity(source.len());
        for &(x, y) in source {
            let (x, y) = transform_point(matrix, x, y);
            vertices.push(vertex(x, y, color));
        }
        unsafe { flashvita_vitagl_draw_colored_line_strip(vertices.as_ptr(), vertices.len()) };
        STAT_LINES.fetch_add(1, Ordering::Relaxed);
    }
}

impl CommandHandler for VitaCommandHandler<'_> {
    fn render_bitmap(
        &mut self,
        bitmap: BitmapHandle,
        transform: Transform,
        smoothing: bool,
        pixel_snapping: PixelSnapping,
        region: PixelRegion,
    ) {
        self.flush_batches();
        let Some(texture) = texture_from_handle(&bitmap) else {
            STAT_MISSING_BITMAPS.fetch_add(1, Ordering::Relaxed);
            return;
        };
        if texture.texture == 0 || region.is_empty() {
            return;
        }

        let mut matrix = transform.matrix;
        pixel_snapping.apply(&mut matrix);
        matrix *= Matrix::scale(region.width() as f32, region.height() as f32);
        let tint = texture_tint(&transform);
        let u0 = region.x_min as f32 / texture.width as f32;
        let u1 = region.x_max as f32 / texture.width as f32;
        let (v0, v1) = if texture.offscreen_y_flipped.load(Ordering::Relaxed) {
            (
                1.0 - region.y_min as f32 / texture.height as f32,
                1.0 - region.y_max as f32 / texture.height as f32,
            )
        } else {
            (
                region.y_min as f32 / texture.height as f32,
                region.y_max as f32 / texture.height as f32,
            )
        };
        let src = [(0.0f32, 0.0f32, u0, v0), (1.0, 0.0, u1, v0), (1.0, 1.0, u1, v1), (0.0, 1.0, u0, v1)];
        let mut vertices = [tex_vertex(0.0, 0.0, 0.0, 0.0, tint); 4];
        for (i, (x, y, u, v)) in src.into_iter().enumerate() {
            let (x, y) = transform_point(matrix, x, y);
            vertices[i] = tex_vertex(x, y, u, v, tint);
        }
        let indices = [0u16, 1, 2, 0, 2, 3];
        unsafe {
            flashvita_vitagl_draw_textured_triangles_u16(
                texture.texture,
                vertices.as_ptr(),
                vertices.len(),
                indices.as_ptr(),
                indices.len(),
                u8::from(smoothing),
                0,
            );
        }
        STAT_TEXTURED_DRAWS.fetch_add(1, Ordering::Relaxed);
    }

    fn render_stage3d(&mut self, _bitmap: BitmapHandle, _transform: Transform) {
        self.flush_batches();
        STAT_STAGE3D.fetch_add(1, Ordering::Relaxed);
    }

    fn render_shape(&mut self, shape: ShapeHandle, transform: Transform) {
        let any = shape.0.as_ref() as &dyn Any;
        if let Some(shape) = any.downcast_ref::<VitaShapeHandle>() {
            self.draw_shape_mesh(shape, &transform);
        }
    }

    fn render_alpha_mask(&mut self, maskee_commands: CommandList, mask_commands: CommandList) {
        // Alpha masks share the same nested stencil depth used by ordinary SWF masks.
        // Draw the mask once to increment stencil, render the maskee through it, then
        // replay the mask with the decrement op to restore the previous depth.
        self.flush_batches();
        self.push_mask();
        mask_commands.clone().execute(self);
        self.flush_batches();
        self.activate_mask();
        maskee_commands.execute(self);
        self.flush_batches();
        self.deactivate_mask();
        mask_commands.execute(self);
        self.flush_batches();
        self.pop_mask();
    }

    fn draw_rect(&mut self, color: Color, matrix: Matrix) {
        self.draw_unit_quad(color, matrix);
    }

    fn draw_line(&mut self, color: Color, matrix: Matrix) {
        self.draw_line_points(color, matrix, false);
    }

    fn draw_line_rect(&mut self, color: Color, matrix: Matrix) {
        self.draw_line_points(color, matrix, true);
    }

    fn push_mask(&mut self) {
        self.flush_batches();
        unsafe { flashvita_vitagl_mask_push(self.mask_depth) };
        self.mask_depth += 1;
        self.drawing_mask = true;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn activate_mask(&mut self) {
        self.flush_batches();
        if self.mask_depth > 0 {
            unsafe { flashvita_vitagl_mask_activate(self.mask_depth) };
        }
        self.drawing_mask = false;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn deactivate_mask(&mut self) {
        self.flush_batches();
        if self.mask_depth > 0 {
            unsafe { flashvita_vitagl_mask_deactivate(self.mask_depth) };
        }
        self.drawing_mask = true;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn pop_mask(&mut self) {
        self.flush_batches();
        if self.mask_depth > 0 {
            self.mask_depth -= 1;
            unsafe { flashvita_vitagl_mask_pop(self.mask_depth) };
        }
        self.drawing_mask = false;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn blend(&mut self, commands: CommandList, _blend_mode: RenderBlendMode) {
        self.flush_batches();
        STAT_BLENDS.fetch_add(1, Ordering::Relaxed);
        commands.execute(self);
        self.flush_batches();
    }
}

impl RenderBackend for VitaRenderer {
    fn viewport_dimensions(&self) -> ViewportDimensions {
        self.dimensions
    }

    fn set_viewport_dimensions(&mut self, dimensions: ViewportDimensions) {
        self.dimensions = dimensions;
    }

    fn register_shape(
        &mut self,
        shape: DistilledShape,
        bitmap_source: &dyn BitmapSource,
    ) -> ShapeHandle {
        let mesh = self.tessellator.tessellate_shape(shape, bitmap_source);
        let indices_u16 = compact_mesh_indices(&mesh);
        let gpu_draws = create_gpu_draws(&mesh, &indices_u16);
        let mut bitmaps: Vec<(u16, BitmapHandle)> = Vec::new();
        for draw in &mesh.draws {
            if let DrawType::Bitmap(bitmap) = &draw.draw_type {
                if !bitmaps.iter().any(|(id, _)| *id == bitmap.bitmap_id) {
                    if let Some(handle) = bitmap_source.bitmap_handle(bitmap.bitmap_id, self) {
                        bitmaps.push((bitmap.bitmap_id, handle));
                    }
                }
            }
        }
        let gradients = mesh
            .gradients
            .iter()
            .map(|gradient| create_gradient_texture(gradient, &mut self.gradient_cache))
            .collect();
        ShapeHandle(Arc::new(VitaShapeHandle {
            mesh,
            indices_u16,
            gpu_draws,
            bitmaps,
            gradients,
        }))
    }

    fn register_shape_with_scale(
        &mut self,
        shape: DistilledShape,
        bitmap_source: &dyn BitmapSource,
        scale: f32,
    ) -> ShapeHandle {
        let mesh = self
            .tessellator
            .tessellate_shape_with_scale(shape, bitmap_source, scale);
        let indices_u16 = compact_mesh_indices(&mesh);
        let gpu_draws = create_gpu_draws(&mesh, &indices_u16);
        let mut bitmaps: Vec<(u16, BitmapHandle)> = Vec::new();
        for draw in &mesh.draws {
            if let DrawType::Bitmap(bitmap) = &draw.draw_type {
                if !bitmaps.iter().any(|(id, _)| *id == bitmap.bitmap_id) {
                    if let Some(handle) = bitmap_source.bitmap_handle(bitmap.bitmap_id, self) {
                        bitmaps.push((bitmap.bitmap_id, handle));
                    }
                }
            }
        }
        let gradients = mesh
            .gradients
            .iter()
            .map(|gradient| create_gradient_texture(gradient, &mut self.gradient_cache))
            .collect();
        ShapeHandle(Arc::new(VitaShapeHandle {
            mesh,
            indices_u16,
            gpu_draws,
            bitmaps,
            gradients,
        }))
    }

    fn render_offscreen(
        &mut self,
        handle: BitmapHandle,
        commands: CommandList,
        _quality: StageQuality,
        bounds: PixelRegion,
    ) -> Option<Box<dyn SyncHandle>> {
        self.render_commands_to_texture(&handle, commands, Color::TRANSPARENT, bounds)
            .then(|| {
                Box::new(VitaSyncHandle {
                    bitmap: handle,
                    bounds,
                }) as Box<dyn SyncHandle>
            })
    }

    fn is_offscreen_supported(&self) -> bool {
        true
    }

    fn submit_frame(
        &mut self,
        clear: Color,
        commands: CommandList,
        cache_entries: Vec<BitmapCacheEntry>,
    ) {
        STAT_FRAMES.fetch_add(1, Ordering::Relaxed);
        for entry in cache_entries {
            if let Some(texture) = texture_from_handle(&entry.handle) {
                let bounds = PixelRegion::for_whole_size(texture.width, texture.height);
                let _ = self.render_commands_to_texture(
                    &entry.handle,
                    entry.commands,
                    entry.clear,
                    bounds,
                );
            }
        }
        let profiling = crate::perf_logging_enabled();
        let prepass_begin = profiling.then(Instant::now);
        self.frame_batch.prepare(&commands);
        self.frame_batch.execute();
        if let Some(begin) = prepass_begin {
            STAT_PREPASS_US.fetch_add(begin.elapsed().as_micros() as u64, Ordering::Relaxed);
        }

        let submit_begin = profiling.then(Instant::now);
        unsafe { flashvita_vitagl_begin_flash_frame(clear.r, clear.g, clear.b, clear.a) };
        let mut handler = VitaCommandHandler::new(
            &mut self.color_scratch,
            &mut self.tex_scratch,
            &mut self.color_index_scratch,
            &mut self.tex_index_scratch,
            &self.frame_batch.color_vertices,
            &self.frame_batch.tex_vertices,
        );
        commands.execute(&mut handler);
        handler.flush_batches();
        if let Some(begin) = submit_begin {
            STAT_SUBMIT_US.fetch_add(begin.elapsed().as_micros() as u64, Ordering::Relaxed);
        }
    }

    fn create_empty_texture(
        &mut self,
        width: NonZeroU32,
        height: NonZeroU32,
    ) -> Result<BitmapHandle, Error> {
        let texture = unsafe {
            flashvita_vitagl_create_texture(std::ptr::null(), width.get(), height.get())
        };
        if texture == 0 {
            return Err(Error::Unimplemented("vitaGL texture allocation failed".into()));
        }
        STAT_BITMAP_UPLOADS.fetch_add(1, Ordering::Relaxed);
        Ok(BitmapHandle(Arc::new(VitaBitmapHandle {
            texture,
            width: width.get(),
            height: height.get(),
            initialized: AtomicBool::new(false),
            offscreen_y_flipped: AtomicBool::new(false),
            external_data: std::ptr::null(),
        })))
    }

    fn register_bitmap(&mut self, bitmap: Bitmap<'_>) -> Result<BitmapHandle, Error> {
        if self.zero_copy_bitmapdata && bitmap.format() == BitmapFormat::Rgba {
            let width = bitmap.width();
            let height = bitmap.height();
            let data = bitmap.data();
            if unsafe { flashvita_vita_vgl_ram_owns(data.as_ptr().cast()) } > 0 {
                let texture = unsafe {
                    flashvita_vitagl_create_texture_zero_copy(
                        data.as_ptr().cast_mut(),
                        width,
                        height,
                    )
                };
                if texture != 0 {
                    return Ok(BitmapHandle(Arc::new(VitaBitmapHandle {
                        texture,
                        width,
                        height,
                        initialized: AtomicBool::new(true),
                        offscreen_y_flipped: AtomicBool::new(false),
                        external_data: data.as_ptr(),
                    })));
                }
            }
        }

        let (width, height, pixels) = premultiplied_rgba(bitmap);
        let texture = unsafe { flashvita_vitagl_create_texture(pixels.as_ptr(), width, height) };
        if texture == 0 {
            return Err(Error::Unimplemented("vitaGL bitmap upload failed".into()));
        }
        STAT_BITMAP_UPLOADS.fetch_add(1, Ordering::Relaxed);
        STAT_BITMAP_UPLOADED_BYTES.fetch_add(pixels.len() as u64, Ordering::Relaxed);
        Ok(BitmapHandle(Arc::new(VitaBitmapHandle {
            texture,
            width,
            height,
            initialized: AtomicBool::new(true),
            offscreen_y_flipped: AtomicBool::new(false),
            external_data: std::ptr::null(),
        })))
    }

    fn update_texture(
        &mut self,
        handle: &BitmapHandle,
        bitmap: Bitmap<'_>,
        mut region: PixelRegion,
    ) -> Result<(), Error> {
        let Some(texture) = texture_from_handle(handle) else {
            return Err(Error::Unimplemented("invalid FlashVita bitmap handle".into()));
        };
        let width = bitmap.width();
        let height = bitmap.height();
        if width != texture.width || height != texture.height {
            return Err(Error::Unimplemented("FlashVita bitmap resize is not supported yet".into()));
        }
        region.clamp(width, height);
        if region.is_empty() {
            return Ok(());
        }

        if !texture.external_data.is_null()
            && bitmap.format() == BitmapFormat::Rgba
            && texture.external_data == bitmap.data().as_ptr()
        {
            unsafe {
                flashvita_vitagl_update_zero_copy_texture(
                    texture.texture,
                    bitmap.data().as_ptr(),
                    width,
                    height,
                    region.x_min,
                    region.y_min,
                    region.width(),
                    region.height(),
                );
            }
            // The bridge double-buffers VGL_RAM storage and swaps the descriptor;
            // there is no glTexSubImage upload on this path.
            texture.offscreen_y_flipped.store(false, Ordering::Relaxed);
            return Ok(());
        }

        let changed_area = region.width() as u64 * region.height() as u64;
        let whole_area = width as u64 * height as u64;
        if texture.initialized.load(Ordering::Relaxed) && changed_area < whole_area / 2 {
            pack_bitmap_region_rgba(&bitmap, region, &mut self.bitmap_upload_scratch);
            unsafe {
                flashvita_vitagl_update_texture_region(
                    texture.texture,
                    self.bitmap_upload_scratch.as_ptr(),
                    region.x_min,
                    region.y_min,
                    region.width(),
                    region.height(),
                );
            }
            STAT_BITMAP_PARTIAL_UPLOADS.fetch_add(1, Ordering::Relaxed);
            STAT_BITMAP_UPLOADED_BYTES.fetch_add(
                self.bitmap_upload_scratch.len() as u64,
                Ordering::Relaxed,
            );
        } else {
            let (_, _, pixels) = premultiplied_rgba(bitmap);
            unsafe {
                flashvita_vitagl_update_texture(texture.texture, pixels.as_ptr(), width, height)
            };
            STAT_BITMAP_UPLOADED_BYTES.fetch_add(pixels.len() as u64, Ordering::Relaxed);
            texture.initialized.store(true, Ordering::Relaxed);
        }
        texture.offscreen_y_flipped.store(false, Ordering::Relaxed);
        STAT_BITMAP_UPLOADS.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }

    fn create_context3d(&mut self, _profile: Context3DProfile) -> Result<Box<dyn Context3D>, Error> {
        Err(Error::Unimplemented("Stage3D is not available on FlashVita".into()))
    }

    fn debug_info(&self) -> Cow<'static, str> {
        Cow::Borrowed("Renderer: FlashVita vitaGL phase 1")
    }

    fn name(&self) -> &'static str {
        "flashvita_vitagl"
    }

    fn set_quality(&mut self, quality: StageQuality) {
        self.quality = quality;
    }

    fn compile_pixelbender_shader(
        &mut self,
        _shader: PixelBenderShader,
    ) -> Result<PixelBenderShaderHandle, Error> {
        Err(Error::Unimplemented("Pixel Bender is not available on FlashVita".into()))
    }

    fn run_pixelbender_shader(
        &mut self,
        _handle: PixelBenderShaderHandle,
        _arguments: &[PixelBenderShaderArgument],
        _target: &PixelBenderTarget,
    ) -> Result<PixelBenderOutput, Error> {
        Err(Error::Unimplemented("Pixel Bender is not available on FlashVita".into()))
    }

    fn resolve_sync_handle(
        &mut self,
        handle: Box<dyn SyncHandle>,
        with_rgba: RgbaBufRead,
    ) -> Result<(), Error> {
        let handle = Box::<dyn Any>::downcast::<VitaSyncHandle>(handle)
            .map_err(|_| Error::Unimplemented("invalid FlashVita sync handle".into()))?;
        let Some(texture) = texture_from_handle(&handle.bitmap) else {
            return Err(Error::Unimplemented("invalid FlashVita bitmap sync target".into()));
        };
        let mut bounds = handle.bounds;
        bounds.clamp(texture.width, texture.height);
        if bounds.is_empty() {
            with_rgba(&[], 0);
            return Ok(());
        }

        let row_bytes = bounds.width().saturating_mul(4);
        let byte_count = row_bytes as usize * bounds.height() as usize;
        let mut pixels = vec![0u8; byte_count];
        let result = unsafe {
            flashvita_vitagl_read_texture_region(
                texture.texture,
                texture.width,
                texture.height,
                bounds.x_min,
                bounds.y_min,
                bounds.width(),
                bounds.height(),
                pixels.as_mut_ptr(),
            )
        };
        if result < 0 {
            return Err(Error::Unimplemented("FlashVita offscreen readback failed".into()));
        }
        with_rgba(&pixels, row_bytes);
        Ok(())
    }
}
