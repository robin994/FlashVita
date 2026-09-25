use ruffle_render::backend::{
    BitmapCacheEntry, Context3D, Context3DProfile, PixelBenderOutput, PixelBenderTarget,
    RenderBackend, ShapeHandle, ShapeHandleImpl, ViewportDimensions,
};
use ruffle_render::bitmap::{
    Bitmap, BitmapHandle, BitmapHandleImpl, BitmapSource, PixelRegion, PixelSnapping, RgbaBufRead,
    SyncHandle,
};
use ruffle_render::commands::{CommandHandler, CommandList, RenderBlendMode};
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
use std::fmt::{Debug, Formatter};
use std::num::NonZeroU32;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use swf::Color;

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

static STAT_FRAMES: AtomicU64 = AtomicU64::new(0);
static STAT_COLORED_DRAWS: AtomicU64 = AtomicU64::new(0);
static STAT_TEXTURED_DRAWS: AtomicU64 = AtomicU64::new(0);
static STAT_BITMAP_UPLOADS: AtomicU64 = AtomicU64::new(0);
static STAT_LINES: AtomicU64 = AtomicU64::new(0);
static STAT_GRADIENT_SKIPS: AtomicU64 = AtomicU64::new(0);
static STAT_MISSING_BITMAPS: AtomicU64 = AtomicU64::new(0);
static STAT_MASK_OPS: AtomicU64 = AtomicU64::new(0);
static STAT_BLENDS: AtomicU64 = AtomicU64::new(0);
static STAT_STAGE3D: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Copy, Default)]
pub struct RendererStatsSnapshot {
    pub frames: u64,
    pub colored_draws: u64,
    pub textured_draws: u64,
    pub bitmap_uploads: u64,
    pub lines: u64,
    pub gradient_skips: u64,
    pub missing_bitmaps: u64,
    pub mask_ops: u64,
    pub blends: u64,
    pub stage3d: u64,
}

pub fn renderer_stats_snapshot() -> RendererStatsSnapshot {
    RendererStatsSnapshot {
        frames: STAT_FRAMES.load(Ordering::Relaxed),
        colored_draws: STAT_COLORED_DRAWS.load(Ordering::Relaxed),
        textured_draws: STAT_TEXTURED_DRAWS.load(Ordering::Relaxed),
        bitmap_uploads: STAT_BITMAP_UPLOADS.load(Ordering::Relaxed),
        lines: STAT_LINES.load(Ordering::Relaxed),
        gradient_skips: STAT_GRADIENT_SKIPS.load(Ordering::Relaxed),
        missing_bitmaps: STAT_MISSING_BITMAPS.load(Ordering::Relaxed),
        mask_ops: STAT_MASK_OPS.load(Ordering::Relaxed),
        blends: STAT_BLENDS.load(Ordering::Relaxed),
        stage3d: STAT_STAGE3D.load(Ordering::Relaxed),
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
}

impl BitmapHandleImpl for VitaBitmapHandle {}

impl Drop for VitaBitmapHandle {
    fn drop(&mut self) {
        if self.texture != 0 {
            unsafe { flashvita_vitagl_delete_texture(self.texture) };
        }
    }
}

pub struct VitaRenderer {
    dimensions: ViewportDimensions,
    tessellator: ShapeTessellator,
    quality: StageQuality,
}

impl VitaRenderer {
    pub fn new(dimensions: ViewportDimensions) -> Self {
        Self {
            dimensions,
            tessellator: ShapeTessellator::new(),
            quality: StageQuality::High,
        }
    }
}

fn transform_point(matrix: Matrix, x: f32, y: f32) -> (f32, f32) {
    let tx = matrix.tx.to_pixels() as f32;
    let ty = matrix.ty.to_pixels() as f32;
    (
        matrix.a * x + matrix.c * y + tx,
        matrix.b * x + matrix.d * y + ty,
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

fn bitmap_tint(transform: &Transform) -> Color {
    let mult = transform.color_transform.mult_rgba_normalized();
    Color {
        r: (mult[0].clamp(0.0, 1.0) * 255.0).round() as u8,
        g: (mult[1].clamp(0.0, 1.0) * 255.0).round() as u8,
        b: (mult[2].clamp(0.0, 1.0) * 255.0).round() as u8,
        a: (mult[3].clamp(0.0, 1.0) * 255.0).round() as u8,
    }
}

fn straight_rgba(bitmap: Bitmap<'_>) -> (u32, u32, Vec<u8>) {
    let width = bitmap.width();
    let height = bitmap.height();
    let bitmap = bitmap.to_rgba();
    let mut pixels = bitmap.data().to_vec();

    // Ruffle stores RGBA bitmaps premultiplied. vitaGL's phase-1 renderer uses
    // straight-alpha blending for both vector and bitmap content, so normalize once at upload.
    for pixel in pixels.chunks_exact_mut(4) {
        let alpha = pixel[3] as u32;
        if alpha == 0 {
            pixel[0] = 0;
            pixel[1] = 0;
            pixel[2] = 0;
        } else if alpha < 255 {
            for channel in &mut pixel[0..3] {
                let value = (*channel as u32 * 255 + alpha / 2) / alpha;
                *channel = value.min(255) as u8;
            }
        }
    }
    (width, height, pixels)
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
            pixels[offset] = color.r;
            pixels[offset + 1] = color.g;
            pixels[offset + 2] = color.b;
            pixels[offset + 3] = color.a;
        }
    }

    let texture = unsafe { flashvita_vitagl_create_texture(pixels.as_ptr(), width, height) };
    if texture == 0 {
        return None;
    }
    STAT_BITMAP_UPLOADS.fetch_add(1, Ordering::Relaxed);
    let wrap_mode = match gradient.repeat_mode {
        swf::GradientSpread::Pad => 0,
        swf::GradientSpread::Repeat => 1,
        swf::GradientSpread::Reflect => 2,
    };
    Some(VitaGradientTexture { texture, wrap_mode })
}

struct VitaCommandHandler {
    mask_depth: u32,
    drawing_mask: bool,
    color_scratch: Vec<VitaVertex>,
    tex_scratch: Vec<VitaTexVertex>,
}

impl VitaCommandHandler {
    fn new() -> Self {
        Self {
            mask_depth: 0,
            drawing_mask: false,
            color_scratch: Vec::new(),
            tex_scratch: Vec::new(),
        }
    }

    #[inline]
    fn index_count(&self, draw: &ruffle_render::tessellator::Draw) -> usize {
        if self.drawing_mask {
            draw.mask_index_count as usize
        } else {
            draw.indices.len()
        }
    }
}

impl VitaCommandHandler {
    fn draw_mesh(&mut self, mesh: &Mesh, transform: &Transform) {
        for draw in &mesh.draws {
            match &draw.draw_type {
                DrawType::Color => {
                    self.color_scratch.clear();
                    self.color_scratch.reserve(draw.vertices.len().saturating_sub(self.color_scratch.capacity()));
                    for source in &draw.vertices {
                        let (x, y) = transform_point(transform.matrix, source.x, source.y);
                        let color = &transform.color_transform * source.color;
                        self.color_scratch.push(vertex(x, y, color));
                    }
                    let index_count = self.index_count(draw);
                    unsafe {
                        flashvita_vitagl_draw_colored_triangles(
                            self.color_scratch.as_ptr(),
                            self.color_scratch.len(),
                            draw.indices.as_ptr(),
                            index_count,
                        );
                    }
                    STAT_COLORED_DRAWS.fetch_add(1, Ordering::Relaxed);
                }
                DrawType::Gradient { .. } => {
                    STAT_GRADIENT_SKIPS.fetch_add(1, Ordering::Relaxed);
                }
                DrawType::Bitmap(_) => {
                    // Bitmap fills need the shape's character-id -> texture mapping;
                    // handled by draw_shape_mesh below.
                }
            }
        }
    }

    fn draw_shape_mesh(&mut self, shape: &VitaShapeHandle, transform: &Transform) {
        for draw in &shape.mesh.draws {
            match &draw.draw_type {
                DrawType::Color => {
                    self.color_scratch.clear();
                    self.color_scratch.reserve(draw.vertices.len().saturating_sub(self.color_scratch.capacity()));
                    for source in &draw.vertices {
                        let (x, y) = transform_point(transform.matrix, source.x, source.y);
                        let color = &transform.color_transform * source.color;
                        self.color_scratch.push(vertex(x, y, color));
                    }
                    let index_count = self.index_count(draw);
                    unsafe {
                        flashvita_vitagl_draw_colored_triangles(
                            self.color_scratch.as_ptr(),
                            self.color_scratch.len(),
                            draw.indices.as_ptr(),
                            index_count,
                        );
                    }
                    STAT_COLORED_DRAWS.fetch_add(1, Ordering::Relaxed);
                }
                DrawType::Gradient { matrix, gradient } => {
                    let Some(Some(texture)) = shape.gradients.get(*gradient) else {
                        STAT_GRADIENT_SKIPS.fetch_add(1, Ordering::Relaxed);
                        continue;
                    };
                    let tint = bitmap_tint(transform);
                    self.tex_scratch.clear();
                    self.tex_scratch.reserve(draw.vertices.len().saturating_sub(self.tex_scratch.capacity()));
                    for source in &draw.vertices {
                        let (x, y) = transform_point(transform.matrix, source.x, source.y);
                        let (u, v) = texture_uv(matrix, source.x, source.y);
                        let v = if matches!(shape.mesh.gradients[*gradient].gradient_type, GradientType::Linear) {
                            0.5
                        } else {
                            v
                        };
                        self.tex_scratch.push(tex_vertex(x, y, u, v, tint));
                    }
                    let index_count = self.index_count(draw);
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
                    let tint = bitmap_tint(transform);
                    self.tex_scratch.clear();
                    self.tex_scratch.reserve(draw.vertices.len().saturating_sub(self.tex_scratch.capacity()));
                    for source in &draw.vertices {
                        let (x, y) = transform_point(transform.matrix, source.x, source.y);
                        let (u, v) = texture_uv(&bitmap.matrix, source.x, source.y);
                        self.tex_scratch.push(tex_vertex(x, y, u, v, tint));
                    }
                    let index_count = self.index_count(draw);
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
                    STAT_TEXTURED_DRAWS.fetch_add(1, Ordering::Relaxed);
                }
            }
        }
    }

    fn draw_unit_quad(&mut self, color: Color, matrix: Matrix) {
        let points = [(0.0f32, 0.0f32), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)];
        let mut vertices = [vertex(0.0, 0.0, color); 4];
        for (i, (x, y)) in points.into_iter().enumerate() {
            let (x, y) = transform_point(matrix, x, y);
            vertices[i] = vertex(x, y, color);
        }
        let indices = [0u32, 1, 2, 0, 2, 3];
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

impl CommandHandler for VitaCommandHandler {
    fn render_bitmap(
        &mut self,
        bitmap: BitmapHandle,
        transform: Transform,
        smoothing: bool,
        pixel_snapping: PixelSnapping,
        region: PixelRegion,
    ) {
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
        let tint = bitmap_tint(&transform);
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
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
        maskee_commands.execute(self);
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
        unsafe { flashvita_vitagl_mask_push(self.mask_depth) };
        self.mask_depth += 1;
        self.drawing_mask = true;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn activate_mask(&mut self) {
        if self.mask_depth > 0 {
            unsafe { flashvita_vitagl_mask_activate(self.mask_depth) };
        }
        self.drawing_mask = false;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn deactivate_mask(&mut self) {
        if self.mask_depth > 0 {
            unsafe { flashvita_vitagl_mask_deactivate(self.mask_depth) };
        }
        self.drawing_mask = true;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn pop_mask(&mut self) {
        if self.mask_depth > 0 {
            self.mask_depth -= 1;
            unsafe { flashvita_vitagl_mask_pop(self.mask_depth) };
        }
        self.drawing_mask = false;
        STAT_MASK_OPS.fetch_add(1, Ordering::Relaxed);
    }

    fn blend(&mut self, commands: CommandList, _blend_mode: RenderBlendMode) {
        STAT_BLENDS.fetch_add(1, Ordering::Relaxed);
        commands.execute(self);
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
        unsafe { flashvita_vitagl_begin_flash_frame(clear.r, clear.g, clear.b, clear.a) };
        commands.execute(&mut VitaCommandHandler::new());
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
        })))
    }

    fn register_bitmap(&mut self, bitmap: Bitmap<'_>) -> Result<BitmapHandle, Error> {
        let (width, height, pixels) = straight_rgba(bitmap);
        let texture = unsafe { flashvita_vitagl_create_texture(pixels.as_ptr(), width, height) };
        if texture == 0 {
            return Err(Error::Unimplemented("vitaGL bitmap upload failed".into()));
        }
        STAT_BITMAP_UPLOADS.fetch_add(1, Ordering::Relaxed);
        Ok(BitmapHandle(Arc::new(VitaBitmapHandle {
            texture,
            width,
            height,
        })))
    }

    fn update_texture(
        &mut self,
        handle: &BitmapHandle,
        bitmap: Bitmap<'_>,
        _region: PixelRegion,
    ) -> Result<(), Error> {
        let Some(texture) = texture_from_handle(handle) else {
            return Err(Error::Unimplemented("invalid FlashVita bitmap handle".into()));
        };
        let (width, height, pixels) = straight_rgba(bitmap);
        if width != texture.width || height != texture.height {
            return Err(Error::Unimplemented("FlashVita bitmap resize is not supported yet".into()));
        }
        unsafe { flashvita_vitagl_update_texture(texture.texture, pixels.as_ptr(), width, height) };
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
