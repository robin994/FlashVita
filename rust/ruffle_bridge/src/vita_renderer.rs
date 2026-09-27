use ruffle_render::backend::{
    BitmapCacheEntry, Context3D, Context3DProfile, PixelBenderOutput, PixelBenderTarget,
    RenderBackend, ShapeHandle, ShapeHandleImpl, ViewportDimensions,
};
use ruffle_render::bitmap::{
    Bitmap, BitmapHandle, BitmapHandleImpl, BitmapSource, PixelRegion, PixelSnapping, RgbaBufRead,
    SyncHandle,
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
use std::ffi::c_void;
use std::fmt::{Debug, Formatter};
use std::num::NonZeroU32;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;
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
    fn flashvita_vitagl_draw_colored_triangles(
        vertices: *const VitaVertex,
        vertex_count: usize,
        indices: *const u32,
        index_count: usize,
    );
    fn flashvita_vitagl_draw_colored_line_strip(vertices: *const VitaVertex, vertex_count: usize);
    fn flashvita_vitagl_create_texture(data: *const u8, width: u32, height: u32) -> u32;
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
    fn flashvita_vitagl_draw_textured_triangles(
        texture: u32,
        vertices: *const VitaTexVertex,
        vertex_count: usize,
        indices: *const u32,
        index_count: usize,
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
    bitmaps: Vec<(u16, BitmapHandle)>,
    gradients: Vec<Option<VitaGradientTexture>>,
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
}

impl BitmapHandleImpl for VitaBitmapHandle {}

impl Drop for VitaBitmapHandle {
    fn drop(&mut self) {
        if self.texture != 0 {
            unsafe { flashvita_vitagl_delete_texture(self.texture) };
        }
    }
}

#[derive(Default)]
struct FrameShapeBatch {
    jobs: Vec<FrameTransformJob>,
    color_vertices: Vec<VitaVertex>,
    tex_vertices: Vec<VitaTexVertex>,
    transformed_draws: usize,
}

impl FrameShapeBatch {
    fn clear(&mut self) {
        self.jobs.clear();
        self.color_vertices.clear();
        self.tex_vertices.clear();
        self.transformed_draws = 0;
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
        for draw in &shape.mesh.draws {
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
    }
}

pub struct VitaRenderer {
    dimensions: ViewportDimensions,
    tessellator: ShapeTessellator,
    quality: StageQuality,
    color_scratch: Vec<VitaVertex>,
    tex_scratch: Vec<VitaTexVertex>,
    color_index_scratch: Vec<u32>,
    bitmap_upload_scratch: Vec<u8>,
    frame_batch: FrameShapeBatch,
}

impl VitaRenderer {
    pub fn new(dimensions: ViewportDimensions) -> Self {
        Self {
            dimensions,
            tessellator: ShapeTessellator::new(),
            quality: StageQuality::High,
            color_scratch: Vec::new(),
            tex_scratch: Vec::new(),
            color_index_scratch: Vec::new(),
            bitmap_upload_scratch: Vec::new(),
            frame_batch: FrameShapeBatch::default(),
        }
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

fn create_gradient_texture(gradient: &TessGradient) -> Option<VitaGradientTexture> {
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

    let texture = unsafe { flashvita_vitagl_create_texture(pixels.as_ptr(), width, height) };
    if texture == 0 {
        return None;
    }
    STAT_BITMAP_UPLOADS.fetch_add(1, Ordering::Relaxed);
    STAT_BITMAP_UPLOADED_BYTES.fetch_add(pixels.len() as u64, Ordering::Relaxed);
    let wrap_mode = match gradient.repeat_mode {
        swf::GradientSpread::Pad => 0,
        swf::GradientSpread::Repeat => 1,
        swf::GradientSpread::Reflect => 2,
    };
    Some(VitaGradientTexture { texture, wrap_mode })
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
    color_batch_indices: &'a mut Vec<u32>,
    color_batch_start: Option<usize>,
    color_batch_end: usize,
}

impl<'a> VitaCommandHandler<'a> {
    fn new(
        color_scratch: &'a mut Vec<VitaVertex>,
        tex_scratch: &'a mut Vec<VitaTexVertex>,
        color_batch_indices: &'a mut Vec<u32>,
        prepared_color: &'a [VitaVertex],
        prepared_tex: &'a [VitaTexVertex],
    ) -> Self {
        Self {
            mask_depth: 0,
            drawing_mask: false,
            color_scratch,
            tex_scratch,
            color_batch_indices,
            prepared_color,
            prepared_tex,
            prepared_color_cursor: 0,
            prepared_tex_cursor: 0,
            color_batch_start: None,
            color_batch_end: 0,
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
                flashvita_vitagl_draw_colored_triangles(
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
    fn queue_prepared_color_draw(
        &mut self,
        draw: &ruffle_render::tessellator::Draw,
        start: usize,
        end: usize,
        index_count: usize,
    ) {
        if self.color_batch_start.is_some() && start != self.color_batch_end {
            self.flush_color_batch();
        }
        let batch_start = *self.color_batch_start.get_or_insert(start);
        let base = (start - batch_start) as u32;
        self.color_batch_indices.reserve(index_count);
        self.color_batch_indices.extend(
            draw.indices[..index_count]
                .iter()
                .map(|index| base + *index),
        );
        self.color_batch_end = end;
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
        for draw in &shape.mesh.draws {
            match &draw.draw_type {
                DrawType::Color => {
                    let index_count = self.index_count(draw);
                    let count = draw.vertices.len();
                    let start = self.prepared_color_cursor;
                    let end = start.saturating_add(count);
                    if end <= self.prepared_color.len() {
                        self.prepared_color_cursor = end;
                        self.queue_prepared_color_draw(draw, start, end, index_count);
                    } else {
                        self.flush_color_batch();
                        self.prepare_color_vertices(&draw.vertices, transform);
                        STAT_COLOR_SUBMISSIONS.fetch_add(1, Ordering::Relaxed);
                        unsafe {
                            flashvita_vitagl_draw_colored_triangles(
                                self.color_scratch.as_ptr(),
                                self.color_scratch.len(),
                                draw.indices.as_ptr(),
                                index_count,
                            );
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
                    if end <= self.prepared_tex.len() {
                        self.prepared_tex_cursor = end;
                        let vertices = &self.prepared_tex[start..end];
                        unsafe {
                            flashvita_vitagl_draw_textured_triangles(
                                texture.texture,
                                vertices.as_ptr(),
                                vertices.len(),
                                draw.indices.as_ptr(),
                                index_count,
                                1,
                                texture.wrap_mode,
                            );
                        }
                    } else {
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
                            flashvita_vitagl_draw_textured_triangles(
                                texture.texture,
                                self.tex_scratch.as_ptr(),
                                self.tex_scratch.len(),
                                draw.indices.as_ptr(),
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
                    if end <= self.prepared_tex.len() {
                        self.prepared_tex_cursor = end;
                        let vertices = &self.prepared_tex[start..end];
                        unsafe {
                            flashvita_vitagl_draw_textured_triangles(
                                texture.texture,
                                vertices.as_ptr(),
                                vertices.len(),
                                draw.indices.as_ptr(),
                                index_count,
                                u8::from(bitmap.is_smoothed),
                                u8::from(bitmap.is_repeating),
                            );
                        }
                    } else {
                        let tint = texture_tint(transform);
                        self.prepare_bitmap_vertices(
                            &draw.vertices,
                            transform,
                            bitmap.matrix,
                            tint,
                        );
                        unsafe {
                            flashvita_vitagl_draw_textured_triangles(
                                texture.texture,
                                self.tex_scratch.as_ptr(),
                                self.tex_scratch.len(),
                                draw.indices.as_ptr(),
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
        self.flush_color_batch();
        let points = [(0.0f32, 0.0f32), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)];
        let mut vertices = [vertex(0.0, 0.0, color); 4];
        for (i, (x, y)) in points.into_iter().enumerate() {
            let (x, y) = transform_point(matrix, x, y);
            vertices[i] = vertex(x, y, color);
        }
        let indices = [0u32, 1, 2, 0, 2, 3];
        STAT_COLOR_SUBMISSIONS.fetch_add(1, Ordering::Relaxed);
        unsafe {
            flashvita_vitagl_draw_colored_triangles(
                vertices.as_ptr(),
                vertices.len(),
                indices.as_ptr(),
                indices.len(),
            );
        }
    }

    fn draw_line_points(&mut self, color: Color, matrix: Matrix, rect: bool) {
        self.flush_color_batch();
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
        self.flush_color_batch();
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
        let v0 = region.y_min as f32 / texture.height as f32;
        let v1 = region.y_max as f32 / texture.height as f32;
        let src = [(0.0f32, 0.0f32, u0, v0), (1.0, 0.0, u1, v0), (1.0, 1.0, u1, v1), (0.0, 1.0, u0, v1)];
        let mut vertices = [tex_vertex(0.0, 0.0, 0.0, 0.0, tint); 4];
        for (i, (x, y, u, v)) in src.into_iter().enumerate() {
            let (x, y) = transform_point(matrix, x, y);
            vertices[i] = tex_vertex(x, y, u, v, tint);
        }
        let indices = [0u32, 1, 2, 0, 2, 3];
        unsafe {
            flashvita_vitagl_draw_textured_triangles(
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
        self.flush_color_batch();
        STAT_STAGE3D.fetch_add(1, Ordering::Relaxed);
    }

    fn render_shape(&mut self, shape: ShapeHandle, transform: Transform) {
        let any = shape.0.as_ref() as &dyn Any;
        if let Some(shape) = any.downcast_ref::<VitaShapeHandle>() {
            self.draw_shape_mesh(shape, &transform);
        }
    }

    fn render_alpha_mask(&mut self, maskee_commands: CommandList, _mask_commands: CommandList) {
        // Phase 1: render the content and ignore the alpha mask until the stencil path lands.
        self.flush_color_batch();
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
        maskee_commands.execute(self);
        self.flush_color_batch();
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
        self.flush_color_batch();
        unsafe { flashvita_vitagl_mask_push(self.mask_depth) };
        self.mask_depth += 1;
        self.drawing_mask = true;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn activate_mask(&mut self) {
        self.flush_color_batch();
        if self.mask_depth > 0 {
            unsafe { flashvita_vitagl_mask_activate(self.mask_depth) };
        }
        self.drawing_mask = false;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn deactivate_mask(&mut self) {
        self.flush_color_batch();
        if self.mask_depth > 0 {
            unsafe { flashvita_vitagl_mask_deactivate(self.mask_depth) };
        }
        self.drawing_mask = true;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn pop_mask(&mut self) {
        self.flush_color_batch();
        if self.mask_depth > 0 {
            self.mask_depth -= 1;
            unsafe { flashvita_vitagl_mask_pop(self.mask_depth) };
        }
        self.drawing_mask = false;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn blend(&mut self, commands: CommandList, _blend_mode: RenderBlendMode) {
        self.flush_color_batch();
        STAT_BLENDS.fetch_add(1, Ordering::Relaxed);
        commands.execute(self);
        self.flush_color_batch();
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
        let gradients = mesh.gradients.iter().map(create_gradient_texture).collect();
        ShapeHandle(Arc::new(VitaShapeHandle { mesh, bitmaps, gradients }))
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
        let gradients = mesh.gradients.iter().map(create_gradient_texture).collect();
        ShapeHandle(Arc::new(VitaShapeHandle { mesh, bitmaps, gradients }))
    }

    fn render_offscreen(
        &mut self,
        _handle: BitmapHandle,
        _commands: CommandList,
        _quality: StageQuality,
        _bounds: PixelRegion,
    ) -> Option<Box<dyn SyncHandle>> {
        None
    }

    fn submit_frame(
        &mut self,
        clear: Color,
        commands: CommandList,
        _cache_entries: Vec<BitmapCacheEntry>,
    ) {
        STAT_FRAMES.fetch_add(1, Ordering::Relaxed);
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
            &self.frame_batch.color_vertices,
            &self.frame_batch.tex_vertices,
        );
        commands.execute(&mut handler);
        handler.flush_color_batch();
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
        })))
    }

    fn register_bitmap(&mut self, bitmap: Bitmap<'_>) -> Result<BitmapHandle, Error> {
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
        let (width, height, pixels) = premultiplied_rgba(bitmap);
        if width != texture.width || height != texture.height {
            return Err(Error::Unimplemented("FlashVita bitmap resize is not supported yet".into()));
        }
        region.clamp(width, height);
        if region.is_empty() {
            return Ok(());
        }

        let changed_area = region.width() as u64 * region.height() as u64;
        let whole_area = width as u64 * height as u64;
        if texture.initialized.load(Ordering::Relaxed) && changed_area < whole_area / 2 {
            pack_rgba_region(&pixels, width, region, &mut self.bitmap_upload_scratch);
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
            unsafe {
                flashvita_vitagl_update_texture(texture.texture, pixels.as_ptr(), width, height)
            };
            STAT_BITMAP_UPLOADED_BYTES.fetch_add(pixels.len() as u64, Ordering::Relaxed);
            texture.initialized.store(true, Ordering::Relaxed);
        }
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
        _handle: Box<dyn SyncHandle>,
        _with_rgba: RgbaBufRead,
    ) -> Result<(), Error> {
        Err(Error::Unimplemented("Offscreen sync is not available on FlashVita".into()))
    }
}
