# FlashVita porting status

## Goal

Build a native PS Vita SWF player with a DaedalusX64-vitaGL-inspired frontend, Ruffle AVM1/AVM2 runtime, per-game controller mappings, touch/analog mouse support, audio and saves.

## M0 - Project/bootstrap - DONE

- Local repository created at `/Users/robin994/Documents/Code/PSVita/FlashVita`.
- VitaSDK toolchain detected at `/usr/local/vitasdk`.
- `imgui_vita`, `imgui`, `vitaGL`, `vitashark` and required Vita stubs detected and linked.
- Native ELF/FSELF/VPK pipeline working.
- Hardware bring-up found a pre-main prefetch abort in `__libc_init_array`: the custom Makefile was missing Vita's required linker relocation retention flag (`-Wl,-q`).
- Fixed `LDFLAGS` to retain relocation records. This is required for ASLR/runtime relocation of C++ constructor pointers and other absolute references.
- No commit or push performed.

## M1 - Native player shell - DONE

- Dear ImGui + imgui_vita + vitaGL frontend.
- Library screen inspired by the DaedalusX64-vitaGL selector flow.
- SWF discovery under `ux0:data/FlashVita/games/`.
- FWS/CWS/ZWS header recognition.
- SWF version and declared uncompressed size shown in details.
- Settings screen with theme, UI scale, VSync and library options.
- Per-game controller mapping screen.
- Vita face buttons, shoulders, Start/Select and D-pad represented in profiles.
- Left stick/front touch/rear touch mouse options represented in profiles.
- Player backend isolated behind `FlashPlayer`.
- UI rendering is confirmed working on real PS Vita hardware using the hardware-proven `vitaGL-fresh` + `imgui-vita-fresh` pair.

## M2 - Ruffle runtime bring-up - IN PROGRESS

Target the smallest useful execution path before full rendering:

Completed parser bring-up:

- FWS loading.
- CWS/zlib decompression.
- Stage RECT parsing and pixel dimensions.
- Frame rate and frame count parsing.
- SWF tag-stream scan.
- `ShowFrame`, `DoAction`, `DoInitAction`, `FileAttributes` and `DoABC` detection.
- Basic AVM1 action-record counting.
- AVM1/AVM2 classification surfaced in the Player screen.
- Parser failures are shown in the Player screen instead of crashing.
- Host tests generate both synthetic FWS and CWS samples and pass.

Ruffle integration implemented in source:

- Ruffle v0.6.0 pinned at `third_party/ruffle`.
- Rust `staticlib` bridge under `rust/ruffle_bridge`.
- Ruffle `swf::decompress_swf` / `parse_swf` probe exposed through a C ABI.
- Ruffle `SwfMovie::from_data` + `PlayerBuilder` headless construction prepared.
- `Player::tick` and `Player::render` are driven from the Vita frame loop.
- AVM1 and AVM2 remain Ruffle-owned; the previous plan to write a custom AVM1 VM is abandoned.
- Current Vita-Rust target configured as `armv7-sony-vita-newlibeabihf`.
- Rust nightly 1.100.0 and cargo-vita 0.2.2 verified locally.
- Full Ruffle v0.6.0 static library now cross-compiles successfully for Vita with `build-std=std,panic_abort`.
- Final Vita link requires `libpthread`; `-lpthread` is now included in the native link because Rust `std`, futures and image decoding use pthread/TLS primitives.
- First Ruffle-enabled FSELF/VPK build completed successfully on 2026-09-12.
- Ruffle-enabled VPK size is about 3.9 MiB, versus about 693 KiB for the C++ fallback build.

Real-hardware validation:

- Ruffle-enabled VPK boots successfully on real PS Vita.
- Three different user-supplied SWFs load and run successfully through the real Ruffle Player.
- Ruffle timeline execution and the first vitaGL rendering path are therefore hardware-validated.
- Rendering is functional but not yet pixel-faithful; M3 is now the active compatibility focus.

## M3 - Ruffle -> vitaGL renderer - IN PROGRESS

- Custom `RenderBackend` skeleton implemented without `wgpu`.
- Ruffle/Lyon CPU `ShapeTessellator` retained for vector shapes.
- Solid-color shape meshes are converted into vitaGL triangle draws.
- Ruffle `DrawRect` is converted to a vitaGL quad.
- Stage coordinate transform path targets the Vita 960x544 surface.
- Ruffle bitmap uploads now create real vitaGL textures and standalone bitmap display objects render as textured quads.
- Bitmap fills inside tessellated vector shapes are resolved through `BitmapSource` and rendered with Ruffle's generated UV matrix.
- Vector shape colors now apply Ruffle `ColorTransform`; textured content applies the multiplicative RGBA component as vertex modulation.
- `DrawLine` and `DrawLineRect` are no longer dropped and use the vitaGL line path.
- Dynamic bitmap updates currently use a correctness-first full texture re-upload; dirty-region uploads are a later optimization.
- Renderer counters are exported through the Rust/C ABI and logged on first frame/every 600 ticks for colored draws, textured draws, uploads, lines, skipped gradients, missing bitmaps, masks and blends.
- Ruffle rendering now follows the upstream `needs_render()` signal instead of redrawing unconditionally at the Vita refresh rate.
- Fullscreen gameplay skips ImGui frame/render work entirely and only swaps buffers when Ruffle produced a new frame.
- Per-draw temporary vertex buffers are reused instead of allocating a fresh `Vec` for every shape draw.
- vitaGL client-state, texture binding, filter and wrap state are cached across Flash draws within a frame to reduce fixed-function state churn.
- Stencil rendering uses Ruffle's `mask_index_count`, so stroke geometry is no longer incorrectly written into Flash masks.
- Runtime profiling now logs 300-tick windows as `ruffle_perf` with rendered-frame count and average/max tick+render time against the 16.667 ms 60 Hz budget.
- Renderer diagnostics now count `stage3d` submissions explicitly to distinguish actual Stage3D content from vector pseudo-3D SWFs.
- Standard Ruffle masks now use the vitaGL stencil buffer with the native `PushMask -> ActivateMask -> DeactivateMask -> PopMask` sequence.
- Linear gradients are rendered from a 256-sample texture ramp; radial/focal gradients use compact generated 64x64 textures and Ruffle's gradient UV matrix.
- Gradient pad/repeat/reflect modes map to clamp/repeat/mirrored-repeat where applicable.
- Alpha masks and nontrivial blend modes still retain simplified phase-1 behavior.
- Stage3D and Pixel Bender explicitly return unsupported.
- Remaining: alpha masks, nontrivial blend modes, additive textured ColorTransform, exact radial/focal spread outside the normalized gradient square, text/font fidelity, dirty-region texture updates and cache/batching work.

## M4 - Input bridge - IMPLEMENTED, NEEDS RUFFLE HARDWARE VALIDATION

- Saved Vita mappings translate to Ruffle key-down/key-up events.
- Left analog drives a software mouse cursor with deadzone and profile sensitivity.
- Front/rear touch map from Vita 1920x1088 touch coordinates to 960x544 Flash coordinates.
- Touch down/up maps to the primary Ruffle mouse button.
- Per-game profiles remain the source of keyboard mappings and mouse options.
- Starting a Ruffle game now hides the ImGui frontend automatically so Flash receives unobstructed input.
- `L+R+START` toggles the frontend while a game is running; the chord is suppressed from Ruffle so it cannot leave Flash keys latched.
- v0.8 pauses Ruffle while the frontend is visible, releases held Flash inputs on entry, and restores a clean vitaGL state before ImGui. This avoids cross-contamination between ImGui and the Flash stencil/texture state.
- The experimental `needs_render()` frame gating was removed after a severe hardware pacing regression; Ruffle renders once per gameplay tick again, using measured wall-clock delta time.

## M5 - Audio and persistence - NOT STARTED

- Flash PCM/audio stream output through `sceAudioOut` or a lightweight Vita backend.
- `SharedObject` persistence under `ux0:data/FlashVita/saves/`.
- Pause/resume handling.

## M6 - Compatibility/performance

- Filters and blend modes.
- More complete text/font behavior.
- Streaming/network APIs where practical.
- Ruffle AVM2 compatibility profiling on Vita.
- vitaGL batching and cache profiling on real hardware.

## Known limitations

- The default `ENABLE_RUFFLE=0` VPK remains available as a UI/parser fallback, but the active hardware-tested build is now Ruffle-enabled.
- Standard stencil masks and primary gradient types are implemented but still need real-hardware fidelity validation; alpha masks, filters, complex blend modes and some text behavior remain incomplete.
- Textured ColorTransform currently applies multiplicative RGBA only; additive channel offsets still need a shader-capable path or CPU fallback.
- Audio still uses Ruffle's null backend; persistent SharedObject storage is not connected.
- Networking/video/Stage3D/Pixel Bender are not implemented for Vita yet.
