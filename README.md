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
- Runtime/player interface isolated from the UI so an AVM1/AVM2 implementation can be integrated without redesigning the frontend.
- Ruffle 0.6.0 vendored under `third_party/ruffle`.
- C ABI bridge between the Vita C++ frontend and the Rust Ruffle player.
- Ruffle headless `PlayerBuilder` path with null desktop services and a custom Vita renderer hook.
- First Ruffle -> vitaGL renderer path using Ruffle/Lyon CPU tessellation for solid-color vector triangles.
- Vita controls, analog mouse and touch translated to Ruffle `PlayerEvent` input events.

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

## Building the native shell

Requires VitaSDK with vitaGL and imgui-vita installed.

```sh
make -j4
make verify
```

The VPK is generated at:

```text
build/FlashVita.vpk
```

This default build uses `ENABLE_RUFFLE=0`, so it does not require a Rust toolchain and is useful for frontend/regression work.

## Building with Ruffle

Ruffle 0.6.0 uses Rust edition 2024. PS Vita is a Tier-3 Rust target with `std`, so the Vita build needs a current nightly compiler and `rust-src`; `cargo-vita` is also used for Vita-Rust tooling.

One-time host setup:

```sh
rustup toolchain install nightly --component rust-src
cargo +nightly install cargo-vita
```

Then verify and build:

```sh
make ruffle-check
make ENABLE_RUFFLE=1 clean verify
```

The Ruffle static library target is `armv7-sony-vita-newlibeabihf` and is linked into the same native FlashVita VPK.

## Runtime roadmap

Ruffle is the runtime for both AVM1/ActionScript 1-2 and AVM2/ActionScript 3. FlashVita does not attempt to reimplement either VM. The Vita-specific work is concentrated in rendering, input, audio, storage, networking policy and performance.

See `PORTING_STATUS.md` for the active milestone plan.

## DaedalusX64-vitaGL provenance

The UI structure and Vita frontend approach are based on and adapted from Rinnegatamante's DaedalusX64-vitaGL project. FlashVita does not include the N64 CPU/RSP/RDP emulator core. See `THIRD_PARTY_NOTICES.md` and `LICENSE` for licensing information.
