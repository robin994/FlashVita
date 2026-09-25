# Third-party notices

## DaedalusX64-vitaGL

FlashVita's Vita frontend structure and UI interaction model are adapted from DaedalusX64-vitaGL by Rinnegatamante and contributors:

https://github.com/Rinnegatamante/DaedalusX64-vitaGL

The referenced DaedalusX64-vitaGL source is distributed under the GNU General Public License version 2. FlashVita preserves GPLv2 licensing for this derivative frontend work. The full license text is in `LICENSE`.

The FlashVita codebase intentionally does not import the N64 emulation core (CPU, RSP, RDP, ROM, save-state, HLE graphics/audio or dynarec subsystems). The adapted concepts are limited to the PS Vita frontend architecture and UI flow: vitaGL + imgui-vita initialization, full-screen immediate-mode menu organization, library/detail navigation, settings, and controller-oriented interaction.

## Dear ImGui / imgui-vita / vitaGL

FlashVita links against the VitaSDK-provided builds of Dear ImGui/imgui-vita and vitaGL. Their respective upstream licenses remain applicable to those libraries.

## Ruffle

FlashVita vendors Ruffle 0.6.0 as the Flash runtime source under `third_party/ruffle` and links selected Ruffle crates through the `rust/ruffle_bridge` static library when `ENABLE_RUFFLE=1`.

Ruffle is developed by Ruffle LLC and contributors and is dual-licensed under MIT or Apache-2.0. FlashVita's GPLv2 distribution obligations remain applicable to the combined application.
