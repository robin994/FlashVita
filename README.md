# FlashVita

FlashVita is an experimental native Flash/SWF player for PlayStation Vita.

The native Vita shell is hardware-verified. Runtime integration is now based on Ruffle 0.6.0: FlashVita keeps its C++/vitaGL frontend while a Rust static library embeds Ruffle's SWF parser, AVM1/AVM2 player and timeline engine.

## Current features

- Native PS Vita UI built with Dear ImGui + imgui_vita + vitaGL.
- DaedalusX64-vitaGL-inspired library/settings/control flow, adapted for SWF games.
- SWF library scan from `ux0:data/FlashVita/games/`.
- Basic SWF header inspection for FWS, CWS and ZWS files.
- Per-game Vita button mappings saved under `ux0:data/FlashVita/profiles/`.
- Configurable analog-stick and touch mouse emulation flags.
- Global settings persisted to `ux0:data/FlashVita/config.ini`.
- Runtime/player interface isolated from the UI, with AVM1/AVM2 handled by Ruffle.
- Ruffle 0.6.0 vendored under `third_party/ruffle`.
- C ABI bridge between the Vita C++ frontend and the Rust Ruffle player.
- Ruffle headless `PlayerBuilder` path with null desktop services and a custom Vita renderer hook.
- First Ruffle -> vitaGL renderer path using Ruffle/Lyon CPU tessellation for solid-color vector triangles.
- Vita controls, analog mouse and touch translated to Ruffle `PlayerEvent` input events.
- Native Vita audio backend and file/network adapters for the Ruffle runtime.

## Runtime controls

The frontend exposes mappings for Cross, Circle, Square, Triangle, L, R, Start, Select and the D-pad. When the Ruffle bridge is enabled these mappings are emitted as Ruffle keyboard events. Left stick and front/rear touch feed Ruffle mouse move/button events.

The default keyboard profile is:

- Cross -> Space
- Circle -> X
- Square -> Z
- Triangle -> C
- L -> A
- R -> S
- Start -> Enter
- Select -> Escape
- D-pad -> Arrow keys

## File layout on Vita

```text
ux0:data/FlashVita/
├── games/
├── profiles/
├── saves/
└── config.ini
```

Copy `.swf` games into `ux0:data/FlashVita/games/` and press **Refresh** in the library.

## Building FlashVita

Requires VitaSDK with vitaGL and imgui-vita installed. Initialize the Ruffle
submodule and install a current nightly Rust compiler with `rust-src`:

```sh
git submodule update --init third_party/ruffle
rustup toolchain install nightly --component rust-src
```

```sh
make -j4
make verify
```

The VPK is generated at:

```text
build/ruffle/FlashVita.vpk
```

The default build includes Ruffle. Ruffle 0.6.0 uses Rust edition 2024; PS Vita
is a Tier-3 Rust target with `std`. `cargo-vita` is used by the optional toolchain check:

```sh
cargo +nightly install cargo-vita
make ruffle-check
```

## Building the frontend shell

The shell build omits Ruffle and needs only the Vita C++ toolchain. It is useful
for UI and parser regression work:

```sh
make ENABLE_RUFFLE=0 verify
```

Its VPK is written to `build/shell/FlashVita.vpk`. Each build mode has
its own object and package directory, so switching `ENABLE_RUFFLE` does not reuse the
other mode's output. Use `BUILD_ROOT=/path/to/output` to change the output root.
`VITAGL_DIR`, `IMGUI_VITA_DIR`, and `RUFFLE_TARGET_DIR` can be overridden for local toolchains.
Cargo output defaults to ignored `build/cargo-target` to avoid changing the
previously tracked Rust build artifacts in the repository.

Run the host parser test with `make -f tests/HostTest.mk test`.

The Settings screen can enable detailed performance profiling. It is off by default and
requires runtime logs to be enabled. Profiling writes 30-frame timing windows to
`ux0:data/FlashVita/runtime.log`, including renderer upload counts and bytes.

FlashVita links a prebuilt vitaGL archive from `VITAGL_DIR`. vitaGL make flags are
chosen when that archive is built, not when FlashVita is built. Keep the archive's
source revision and build command alongside it when comparing performance.

At startup FlashVita requests 444 MHz ARM, 222 MHz bus, 222 MHz GPU and
166 MHz GPU crossbar clocks through VitaSDK. The requested and effective clocks
and API results are written to `runtime.log` when logging is enabled. Higher
clocks increase power use; device measurements should confirm their effect.

## Runtime roadmap

Ruffle is the runtime for both AVM1/ActionScript 1-2 and AVM2/ActionScript 3. FlashVita does not attempt to reimplement either VM. The Vita-specific work is concentrated in rendering, input, audio, storage, networking policy and performance.

See `PORTING_STATUS.md` for the active milestone plan.

## DaedalusX64-vitaGL provenance

The UI structure and Vita frontend approach are based on and adapted from Rinnegatamante's DaedalusX64-vitaGL project. FlashVita does not include the N64 CPU/RSP/RDP emulator core. See `THIRD_PARTY_NOTICES.md` and `LICENSE` for licensing information.
