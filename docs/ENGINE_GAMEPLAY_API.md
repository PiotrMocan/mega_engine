# Engine Gameplay & Content API Reference

This document describes the reusable C++23 core engine APIs available for game projects without modifying core renderer internals or embedding project-specific gameplay logic into the engine.

See also [CONTENT_PIPELINE_CONTRACT.md](CONTENT_PIPELINE_CONTRACT.md) for the Blender/glTF export JSON schema (`asset_manifest.json`, `scene_layout.json`, `animation_manifest.json`).

---

## 1. Coordinate System & Units

- **Units**: metric meters (`1.0` = 1 meter), radians internally in C++ physics/controllers.
- **World / Runtime Axes**: right-handed, `+Y` up, `+Z` forward for authored characters and directional assets (`yaw = 0` points along `+Z`).
- **JSON Quaternions**: serialized as `[x, y, z, w]` arrays in content manifests (converted cleanly to/from `glm::quat(w, x, y, z)`).

---

## 2. Authoritative Runtime Simulation Pipeline (`GameplaySession`)

Headers: [`src/gameplay_session.hpp`](../src/gameplay_session.hpp), [`src/gameplay_framework.hpp`](../src/gameplay_framework.hpp)

`GameplaySession` provides deterministic ordering across 5 per-frame stages (`dt = clamp(delta_seconds, 0.0f, 0.1f)`) and supports opt-in project customization with zero per-frame heap allocations:

1. **Stage 1 — Input & AI** (`on_input_ai`)
2. **Stage 2 — Movement & Physics** (`AnimatedCharacterController` or `CharacterController`, `on_movement_physics`, `PhysicsWorld::step`, trigger & entity updates)
3. **Stage 3 — Transforms & Child Attachments** (`RuntimeTransformRegistry::sync_outputs` updating live `CollisionWorld` colliders + `Renderer` transforms, `on_post_transforms`)
4. **Stage 4 — Animation & IK** (`AnimatorRuntimeSystem::update` reading updated live world matrices, `on_post_animation`)
5. **Stage 5 — Camera, Spatial Audio & UI Overlay** (`ThirdPersonCamera` or `GameplayUpdateContext::set_camera_pose` / `set_camera_angles` overrides, `GameOverlayCanvas`, `Renderer::set_game_overlay_vertices`, `AudioEngine` spatial sync from `RuntimeTransformRegistry`)

### Project Customization Hooks

- `GameplaySession::set_player_setup(const GameplayPlayerSetup&)`:
  - `custom_player_node`: bind a scene node instead of spawning the default `"Runtime Player"` cube.
  - `disable_default_player_cube`: opt out of automatic cube creation.
  - `use_animated_character` & `animated_character_config`: switch on `AnimatedCharacterController`.
  - `spawn_position`, `spawn_yaw_radians`, `initial_input_context`.
- `GameplaySession::set_default_camera_look_enabled(bool)` and `GameplayUpdateContext::suppress_default_camera_look`: allow project modules to suppress built-in `ThirdPersonCamera` mouse/stick orbit during cutscenes, custom cameras, or alternate control modes.
- `GameplaySession::set_extension_callbacks(const GameplayExtensionCallbacks&)`:
  - Stateless C function pointers + `void* user_data` for `on_start`, `on_stop`, `on_input_ai`, `on_movement_physics`, `on_post_transforms`, `on_post_animation`, `on_camera_audio_overlay`, `on_save_project_state`, `on_load_project_state`, `on_query_debug_state`.
- `GameStateSnapshot::custom_project_json`: persisted under `"project_state"` in save files via `save_game_state` / `load_game_state`.

---

## 3. Authoritative Transform Hierarchy & Sockets (`RuntimeTransformRegistry`)

Header: [`src/runtime_transform_registry.hpp`](../src/runtime_transform_registry.hpp)

- Register runtime nodes (`register_node`, `register_node_pose`) and local sockets (`register_socket(owner, name, local_pose)`).
- Attach rigid children (`attach_child`, `attach_child_to_socket`) for articulated props, attachments, or mounted characters with cycle detection.
- Query world transforms (`world_matrix`, `world_pose`, `socket_world_matrix`, `socket_world_pose`) or call `sync_outputs(&collision_world, &renderer_sink)` once after transform updates.

Common socket naming conventions:
- Character limbs & IK contacts: `hand_l`, `hand_r`, `foot_l`, `foot_r`, plus arbitrary project-defined mount sockets.

---

## 4. Animated Character & Root Motion

Headers: [`src/animated_character.hpp`](../src/animated_character.hpp), [`src/skeletal_animation.hpp`](../src/skeletal_animation.hpp)

- `AnimatedCharacterController`:
  - Data-driven `CharacterClipBinding` slots for `idle`, `walk`, `run`, `stop`, `turn_left`, `turn_right`, `jump`, `fall`, `land`, plus explicit `root_motion_bone` and `set_override_clip`.
  - Root-motion deltas are extracted via `extract_root_motion_delta(skeleton, bone_idx, ...)` and resolved against `CollisionWorld::resolve_capsule`.
- `AnimatorRuntimeSystem::set_root_motion_bone(SceneNodeId, std::string_view)`: specifies the root-motion bone per animator node; root-motion translation is reconciled against `CollisionWorld` capsules during `update`.
- Renderer skeletal draws lock skinned assets to LOD0 with expanded bounds and skip static TLAS inclusion to prevent bone/LOD popping artifacts.

---

## 5. Physics & Action Input Contexts

Headers: [`src/physics_world.hpp`](../src/physics_world.hpp), [`src/input_actions.hpp`](../src/input_actions.hpp)

- **PhysicsWorld**: rigid-body dynamics and raycasts (`apply_central_force`, `apply_force_at_point`, `apply_torque`, `apply_impulse_at_point`, `point_velocity`, `set_body_enabled`, adaptive CFL substeps, and `ignored_node` raycasts) reusable by arbitrary custom game controllers inside game modules.
- **InputActionSystem**: context-isolated action/axis mapping (`InputContext::gameplay`, `InputContext::ui`, `InputContext::cutscene`, `InputContext::secondary`) with analog trigger/button values (`action_value(InputAction)`), normalized `move_axis()`, secondary-context 2D `secondary_axis()`, `look_delta()`, rumble motors, conflict detection, and JSON persistence.

---

## 6. Materials & 2D/Projected UI Overlay

- **Glass Materials** ([`src/cooked_model_asset.hpp`](../src/cooked_model_asset.hpp)):
  - `material_coverage_glass_bit` marks architectural/prop dielectric glass for physical specular reflection/transmission rendering and non-opaque ray-tracing BLAS handling.
- **UI Overlay Canvas** ([`src/game_overlay.hpp`](../src/game_overlay.hpp)):
  - `GameOverlayCanvas`: zero steady-state allocation 2D HUD renderer with `512x512` R8 SDF font atlas (`GameUiFontAtlas`, ASCII + Cyrillic + UI glyphs, `.vcrfont.json` + PNG I/O), `draw_rect`, `draw_rect_outline`, `draw_rounded_panel`, `draw_line`, `draw_progress_bar`, `draw_arc`, `draw_crosshair`, `draw_text_px`, `draw_text_shadowed`, `draw_text_wrapped`, `measure_text_px`, `measure_text_wrapped`, and `project_world_indicator`.
  - Submitted automatically at the end of Stage 5 via `Renderer::set_game_overlay_vertices` with `VK_FILTER_LINEAR` and `GameUiPushConstants` SDF antialiasing (`shaders/game_ui.vert`, `shaders/game_ui.frag`).

---

## 7. Content Manifests & Cooker CLI

Headers/Tools: [`src/content_manifest.hpp`](../src/content_manifest.hpp), [`tools/vcr_content_cooker.cpp`](../tools/vcr_content_cooker.cpp)

```powershell
build/windows-release/bin/vcr_content_cooker.exe `
  --export-dir <project>/assets/<pack>/export `
  --cooked-dir <project>/assets/<pack>/cooked `
  [--dry-run]
```

Validates `version == 1` schemas (`asset_manifest.json`, `scene_layout.json`, `animation_manifest.json`), imports deduplicated `.glb`/`.gltf` assets into `--cooked-dir`, and writes `cooked_catalog.json` atomically without mutating `--export-dir`.

---

## 8. C++ Game Module ABI v2 & SDK

Headers/CMake: [`src/game_module_api.hpp`](../src/game_module_api.hpp), [`src/game_module_loader.hpp`](../src/game_module_loader.hpp), [`cmake/VcrEngineSdk.cmake`](../cmake/VcrEngineSdk.cmake)

- **ABI v2 Contract**: Game DLLs export `vcr_get_game_module_api_v2` with `api_version == VCR_GAME_MODULE_API_VERSION` (`2U`) and `abi_stamp == vcr_current_module_abi_stamp()`, verifying pointer size, iterator debug level, and MSVC Debug/Release CRT compatibility across the DLL boundary.
- **Cross-DLL Error Reporting**: `configure_session(id, host, error_buf, error_buf_size)` writes UTF-8 diagnostics into caller-owned storage instead of returning heap-allocated `std::string` across CRT heaps.
- **Configuration-Matched SDK Linking**: `vcr_engine` uses `DEBUG_POSTFIX "d"` (`vcr_engined.dll` / `vcr_engined.lib` in Debug, `vcr_engine.dll` / `vcr_engine.lib` in Release/ASan), and `VcrEngineSdk.cmake` selects the configuration-matched binaries strictly while setting `DEBUG_POSTFIX ""` on game module targets.
- **Shadow-Copy Hardening**: `LoadedGameModule::load` rejects non-regular files and symlinks on both the source DLL and temporary shadow-copy directories, and binds `bound_session_` cleanup to unload/failure paths before `FreeLibrary`.

---

## 9. Runtime Game UI, Localization & World Markers (Stage 5)

Headers: [`src/game_overlay.hpp`](../src/game_overlay.hpp), [`src/localization.hpp`](../src/localization.hpp), [`src/game_ui_runtime.hpp`](../src/game_ui_runtime.hpp), [`src/scene_components.hpp`](../src/scene_components.hpp)

- **Resolution Scaling & Anchors (`UiScaler`, `UiAnchorPreset`, `UiScaleMode`)**: Scales UI elements from a `1920x1080` reference across `720p–4K` and ultrawide viewports using `fit`, `match_height`, `match_width`, or `pixel_constant` modes with configurable `safe_area_fraction` and 10 anchor presets (`top_left`..`stretch_fill`).
- **SDF Font Atlases (`.vcrfont.json` + PNG)**: `GameUiFontAtlas::build_default_sdf()` generates a `512x512` signed-distance atlas with ASCII, Cyrillic (`U+0400..U+045F`), and UI glyphs. `save_game_ui_font_atlas_file` / `load_game_ui_font_atlas_file` serialize metrics and companion `.png` textures; `Renderer::set_game_ui_font_atlas` uploads custom atlases.
- **Localization (`LocalizationSystem`, `.vcrloc.json`)**: Provides built-in `en` and `ru` UI string tables, sorted binary-searched zero-allocation runtime lookups (`get`, `try_get`, `resolve`, `format_into` with `{arg}` placeholders), fallback locale chain, and directory loading from `assets/localization/*.vcrloc.json`.
- **Runtime Menus, HUD & Dialogues (`GameUiRuntime`, `.vcrdlg.json`)**: Dear ImGui-free in-game UI integrated into `GameplaySession`, standalone `Application`, and `vulkan_editor` Play mode:
  - Screens: `in_game` (HUD, crosshair, interaction prompt, toasts, subtitles, world markers), `main_menu`, `pause_menu`, `settings` (`graphics`, `audio`, `controls` with live keyboard & gamepad rebinding, `interface_loc`), and multi-choice `dialogue` graphs (`*.vcrdlg.json`) that can set `GameplayWorld` variables.
  - CLI options: `--main-menu` starts the session on the main menu; `--locale <code>` selects the initial UI locale.
  - Custom game module compatibility: when a module attaches `on_input_ai` or `on_camera_audio_overlay` without calling `GameplaySession::configure_game_ui`, default auto-pause and built-in HUD drawing stay disabled so custom HUD/pause logic keeps control.
- **3D World Markers & Editor / MCP Integration**:
  - `UiMarkerComponent` (`objective`, `waypoint`, `interact`, `speaker`, `hazard`, `info`) projects 3D entity positions via `project_world_indicator` with off-screen edge clamping, distance fading, optional health bars, subtitles, and `dialogue_id` triggers (`"dialogue"` default gameplay behavior).
  - Editor tools: `Inspector -> UI Marker` section with undo support, docked `UI & Localization` panel, Play viewport hover/click cursor tracking, shipping validation (`ship_project` for `.vcrloc.json`, `.vcrdlg.json`, `.vcrfont.json` + `.png`), and MCP tools `editor_set_ui_marker`, `editor_remove_ui_marker`, and `editor_configure_ui`.
