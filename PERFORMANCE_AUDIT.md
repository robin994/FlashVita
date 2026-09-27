# FlashVita: native Vita performance audit

Audit date: 2026-09-27. Base revision: `5b385c3dc8f0ef892be9198de94dc7d6201691ff`
(`origin/main` after `git pull --ff-only`). Working branch: `codex/vita-performance-cleanup`.

## What the code runs

FlashVita is a native PS Vita executable. Its C++ frontend uses VitaSDK and
vitaGL; the full player links a Rust Ruffle bridge and renders through vitaGL.
The default `make` target now builds this full player. `ENABLE_RUFFLE=0` builds
only the frontend and SWF parser for regression work.

The project links a prebuilt `libvitaGL.a`. Its build command and flags are not
recorded in this repository, so the exact flags of the linked archive are
unverified. FlashVita's `Makefile` cannot retroactively change them. In the
[current upstream vitaGL Makefile](https://github.com/Rinnegatamante/vitaGL/blob/master/Makefile),
`USE_SCRATCH_MEMORY=1` and `NO_DEBUG=1` are recognized; the pictured
`HAVE_GLSL_SUPPORT=1` and `CIRCULAR_VERTEX_POOL=2` are not Makefile options.
`NO_DEBUG=1` removes GL error handling, so it should be considered only after
visual comparison and error-free hardware runs. FlashVita currently submits
client arrays rather than VBOs, so a scratch-memory benefit for its draw path
must be measured rather than assumed.

## Findings and changes

| Finding at base revision | Change |
| --- | --- |
| Both `ENABLE_RUFFLE` modes reused `build/FlashVita.vpk`; switching mode could leave a stale package. | Separate `build/ruffle` and `build/shell` outputs; the full player is the default. |
| The default Rust target directory contains tracked build artifacts. | New builds write under ignored `build/cargo-target` by default. |
| Ruffle rebuilt complete bitmap textures for every dirty-region update. | Pack small dirty regions into a reusable buffer and call `glTexSubImage2D`; keep full upload for first update and regions covering at least half the bitmap. |
| Frame and AVM profiling logged frequently in the ordinary run path. | Optional profiling is off by default and records 30-frame windows when enabled with runtime logs. |
| Flash renderer GL callbacks and player lifecycle shared one large C++ file. | Move GL callbacks and state cache into `src/player/vitagl_bridge.cpp`; remove an unused Rust draw path. |
| Host parser test used C++14 and had no implementation of Vita file reading. | Build the host test with C++17 and a test-only file reader. |
| Documentation described old renderer and audio behavior. | Update build, renderer, and runtime documentation. |

## Validation

- `make BUILD_ROOT=/tmp/flashvita-implementation-check RUFFLE_TARGET_DIR=/tmp/flashvita-cargo-target -j4 verify`: passed, full Vita VPK.
- `make BUILD_ROOT=/tmp/flashvita-implementation-check ENABLE_RUFFLE=0 -j4 verify`: passed, shell Vita VPK.
- `make -f tests/HostTest.mk test`: passed.
- `cargo +nightly test --locked --release --manifest-path rust/ruffle_bridge/Cargo.toml bitmap_region_tests`: one test passed.
- `git diff --check`: passed.

The first audit VPK had SHA-256
`78676eb85bf85d215546ea63b29e536e75718e6d2b6d361ea5ae15d23b4ff254`.
The current ignored `build/ruffle/FlashVita.vpk` includes the subsequent
pointer and clock changes and has SHA-256
`39d3f7e801fc71b958785ac0a109a49f485f8829ac1fe15a9134512ea0294a42`.

`cargo +nightly fmt --all --check` still reports existing formatting drift in
the bridge and vendored Ruffle code. Applying it wholesale would rewrite files
outside this performance change.

## Pacman hardware investigation

The user reported about 3 FPS in `pacman.swf` on Vita. A live log from the
installed VPK above confirms the second 300-frame window averaged 375,985 us
per loop (about 2.7 FPS): Ruffle update averaged 341,660 us and render 29,936
us. The sampled frame spent 152,704 us in mouse state updates, made 628 mouse
pick tests, and dispatched zero mouse events. This identifies idle pointer hit
testing as a major cost for this title. The SWF declares 21 FPS. The diagnostic
input is kept in ignored `build/diagnostics/pacman.swf`; the baseline log is
`build/diagnostics/pacman-baseline.log`.

The candidate keeps Ruffle's pointer outside the stage when the Vita pointer
is inactive. Stick movement, touch, or a click reactivates mouse handling;
three seconds after the last pointer activity, it sends MouseLeave and stops
idle hit testing. Profiling stays disabled by default, but now uses 30-frame
windows when explicitly enabled so low-FPS runs can be measured promptly.
The candidate VPK is `build/diagnostics/FlashVita-pacman-idle.vpk`, SHA-256
`9df53928545b96e56cb6383b6c6c1d96aa69bdc4ddbe30bad5674f74248036bb`.

The idle-pointer candidate was installed on the same Vita, with its eboot
SHA-256 `0d12f66c75c9cdd944c1fb2b42b3190a292cdf227b907997a98da5d8ab45a5d8`.
The live capture in `build/diagnostics/pacman-optimized-live.log` confirms zero
mouse pick tests and about 50–77 us of mouse work in sampled ticks. After
startup, 30-frame windows averaged 54.5, 56.6 and 93.1 ms per Ruffle tick.
The corresponding render averages were 34.2, 34.3 and 34.1 ms; the update
averages were 20.2, 22.3 and 59.0 ms. The heavy sampled tick executed 1,782
AVM1 actions, while other samples queued 316–632 method callbacks and executed
no AVM1 bytecode. The pointer fix is confirmed, but full-speed Pacman is not.
The current log ended at 150 ticks, so these windows do not establish stable
sustained gameplay performance or visual/input correctness.
A later startup log was intentionally interrupted by the user for another
test before any gameplay timing was recorded; it is excluded from the comparison.

FlashVita did not request game clocks. A further candidate now requests
444 MHz ARM, 222 MHz bus, 222 MHz GPU and 166 MHz GPU crossbar at startup,
and logs the actual frequencies and return codes. This uses the public VitaSDK
power API. The full VPK is `build/ruffle/FlashVita.vpk`
(SHA-256 `39d3f7e801fc71b958785ac0a109a49f485f8829ac1fe15a9134512ea0294a42`).
Its eboot is installed on the Vita as `ux0:/app/FLASHVITA/eboot.bin`, with
SHA-256 `80483300854e780ccbae9496e5289f2a44b18c444d1ffe03cd0f8ebe231df362`
verified by downloading the installed file. The previous idle-pointer build
is saved as `eboot.bin.flashvita-idle-candidate`. The clock request raises
power use. The Cubefield run below verifies that the requested clocks were
applied; their isolated performance effect still requires a same-scene
comparison with and without the request. Other SWFs still need visual and
audio validation.

## Cubefield hardware investigation

The user reports about 1 FPS in `cubefield.swf`. The live capture with the
clock-request build is `build/diagnostics/cubefield-clocks-live.log`; the
diagnostic SWF is kept only in ignored `build/diagnostics/cubefield.swf`
(SHA-256 `079cdc40c0cc39589dbd571ac6e0856a78f0c72007e01c25bba2cfa2a432d828`).
It is a CWS version 8 AVM1 movie, declares 24 FPS and 72 timeline frames.
The Vita reported effective ARM/bus/GPU/Xbar clocks of 444/222/222/166 MHz,
with all four clock requests returning success.

Before gameplay, 30-frame windows averaged about 17 ms per application loop,
with only 1.3–1.5 ms in Ruffle update and 2.2–2.3 ms in render. Gameplay then
raised the sampled queued AVM1 work to 17,437 actions in 237.5 ms. The later
30-frame window averaged 321.8 ms per loop; its sampled tick executed 15,350
AVM1 actions in 328.9 ms and spent 267.8 ms in garbage collection. Rendering
also rose to 55.5 ms on average in the corresponding Ruffle window, with
about 811,000 additional transformed vertices and 4,453 additional colored
draws over 30 frames. The log stopped at frame 210; it confirms about 3.1
application cycles/s in that last complete window, not the user's later visual
estimate of 1 FPS.

Older local device captures from v0.15.11/v0.15.15 show the same progressive
collapse and later windows near 1 second per cycle, with the AVM1 update
dominating. They are historical evidence, not a same-build benchmark. The
current data rules out failed clock requests, idle-pointer hit testing and
texture uploads as the main cause. The next useful profiling target is the
AVM1 instruction mix and allocation/GC growth during gameplay; reducing only
draw calls would not close the roughly 24x gap to the SWF's 41.7 ms frame
budget in the worst historical windows. Avoid game-specific frame skipping or
silently dropping script actions because either changes game behavior.

After the diagnostic runs, the Vita's original `config.ini` was restored;
`enable_perf_logs` is again absent and therefore defaults to off. A copy of
the profiling-enabled configuration remains on the device as
`config.ini.profile-enabled-20260927` for recovery.
