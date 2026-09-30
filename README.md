# FlashVita

FlashVita is an experimental native Flash/SWF player for PlayStation Vita.

> **Alpha software:** the current public build is `v0.15.31-alpha.1`. Compatibility is incomplete, performance varies significantly between SWFs, and crashes or rendering bugs are still possible. Please report reproducible issues with the SWF name, FlashVita version and a runtime log when available.

FlashVita embeds Ruffle 0.6.0 for SWF parsing, AVM1/AVM2 execution and the Flash timeline, with a Vita-specific frontend, renderer, audio backend, storage layer, input handling and network adapter. The current renderer uses a native SceGxm path for the hot draw path while keeping vitaGL for integration/fallback paths that are still being replaced.

## Current features

- Native PS Vita UI built with Dear ImGui + imgui_vita + vitaGL.
- SWF library scan from `ux0:data/FlashVita/games/`.
- Basic SWF header inspection for FWS, CWS and ZWS files.
- Per-game Vita button mappings saved under `ux0:data/FlashVita/profiles/`.
- Configurable analog-stick and touch mouse emulation flags.
- Global settings persisted to `ux0:data/FlashVita/config.ini`.
- AVM1/ActionScript 1-2 and AVM2/ActionScript 3 handled by Ruffle.
- Ruffle 0.6.0 vendored under `third_party/ruffle`.
- C ABI bridge between the Vita C++ frontend and the Rust Ruffle player.
- Vita-native SceGxm rendering hot path with asynchronous render submission and vitaGL fallback/integration paths.
- `cacheAsBitmap`/offscreen rendering, bitmap dirty-region updates and optional zero-copy BitmapData backing.
- Vita controls, analog mouse and touch translated to Ruffle `PlayerEvent` input events.
- Native Vita audio backend and file/network adapters for the Ruffle runtime.
- Per-game external asset directory and HTTP cache for SWFs that load additional files.

## Installing FlashVita

1. Download the latest **Alpha** VPK from the GitHub Releases page.
2. Copy `FlashVita.vpk` to your Vita using VitaShell USB or FTP.
3. Install the VPK from VitaShell.
4. Launch FlashVita once. It creates its data folders under `ux0:data/FlashVita/`.

The VPK includes the extended-memory application attribute used by FlashVita. For normal users, install the VPK rather than replacing only `eboot.bin`.

FlashVita does **not** include Flash games. Use SWF files and external assets that you are legally allowed to use.

## Installing SWF games

For a standalone SWF, copy the file into:

```text
ux0:data/FlashVita/games/
```

Example:

```text
ux0:data/FlashVita/games/MyGame.swf
```

Then open FlashVita and use **Refresh** if the game is not already visible in the library.

The SWF filename becomes the game's storage ID in most cases. For `MyGame.swf`, per-game files are stored under:

```text
ux0:data/FlashVita/gamefiles/MyGame/
```

### Games that load external files

Some SWFs are only a launcher and load images, audio, XML, binary data or additional SWFs at runtime. Put those files under the matching `gamefiles` directory while preserving the paths expected by the game.

For example, if `MyGame.swf` requests `data/config.xml`:

```text
ux0:data/FlashVita/
├── games/
│   └── MyGame.swf
└── gamefiles/
    └── MyGame/
        └── data/
            └── config.xml
```

FlashVita also treats local `gamefiles/<game>/data/...` files as overrides for remote HTTP/HTTPS requests whose URL contains a `/data/` path. This is useful for games whose original servers are no longer available.

Files downloaded by a game are cached automatically under:

```text
ux0:data/FlashVita/gamefiles/<game>/remote/<host>/...
```

### Overriding a game's base URL

FlashVita automatically writes an informational `origin.txt` file for each game. Do not use that file as a manual setting.

If a SWF requires a specific original web origin, create:

```text
ux0:data/FlashVita/gamefiles/<game>/origin.override.txt
```

and put a single base URL in it, for example:

```text
https://example.com/games/MyGame.swf
```

Relative URL requests made by the SWF will then resolve from that origin.

### Example: game with local data files

```text
ux0:data/FlashVita/
├── games/
│   └── ExampleGame.swf
└── gamefiles/
    └── ExampleGame/
        ├── data/
        │   ├── level1.bin
        │   └── music.ogg
        └── origin.override.txt   # optional
```

If a title still shows a black screen, hangs while loading, or reports missing assets, check whether the original SWF expected additional files or an online server.

## Runtime controls

The frontend exposes mappings for Cross, Circle, Square, Triangle, L, R, Start, Select and the D-pad. When the Ruffle bridge is enabled these mappings are emitted as Ruffle keyboard events. Left stick and front/rear touch feed Ruffle mouse move/button events.

The current default keyboard profile is:

- Cross -> O
- Circle -> X
- Square -> P
- Triangle -> C
- L -> A
- R -> S
- Start -> Enter
- Select -> Backspace
- D-pad -> Arrow keys

Mappings are editable per game from FlashVita and are saved under `ux0:data/FlashVita/profiles/`.

## File layout on Vita

```text
ux0:data/FlashVita/
├── games/
├── gamefiles/
├── profiles/
├── saves/
└── config.ini
```

`gamefiles/` contains per-game external assets, downloaded-file caches and optional origin overrides.

## Alpha compatibility notes

- Flash compatibility is not complete. AVM1 and AVM2 titles can behave differently depending on the APIs they use.
- SWFs that rely on browser JavaScript, unsupported plugins, sockets or desktop-only AIR APIs may not work.
- Network-loaded games may require their original service to still exist, a local asset mirror, or `origin.override.txt`.
- Rendering is under active development. Complex masks, filters, text, video and cached/offscreen content may still expose visual issues.
- Performance is game-dependent. Some titles are CPU-bound in ActionScript; others are GPU/render-bound.
- Save behavior depends on what storage API the SWF uses and should not yet be considered universally compatible.
- Keep a backup of important data under `ux0:data/FlashVita/` when testing new alpha builds.

## Building FlashVita

Requires VitaSDK with vitaGL and imgui-vita installed. Initialize the Ruffle
submodule and install a current nightly Rust compiler with `rust-src`:

```sh
git submodule update --init third_party/ruffle
rustup toolchain install nightly --component rust-src
```

```sh
make ENABLE_RUFFLE=1 -j8 verify
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

See `PORTING_STATUS.md` for the active milestone plan and `docs/VITA_NATIVE_PLAN.md` for the Vita-native performance plan.

## DaedalusX64-vitaGL provenance

The UI structure and Vita frontend approach are based on and adapted from Rinnegatamante's DaedalusX64-vitaGL project. FlashVita does not include the N64 CPU/RSP/RDP emulator core. See `THIRD_PARTY_NOTICES.md` and `LICENSE` for licensing information.
