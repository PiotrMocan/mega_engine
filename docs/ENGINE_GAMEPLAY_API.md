# Engine Gameplay & Content API Reference

This document describes the reusable C++23 core engine APIs available for game projects (such as third-person urban sandbox games) without modifying core renderer internals or embedding project-specific mission logic into the engine.

See also [CONTENT_PIPELINE_CONTRACT.md](CONTENT_PIPELINE_CONTRACT.md) for the Blender/glTF export JSON schema (`asset_manifest.json`, `scene_layout.json`, `animation_manifest.json`).

---

## 1. Coordinate System & Units

- **Units**: metric meters (`1.0` = 1 meter), radians internally in C++ physics/controllers.
- **World / Runtime Axes**: right-handed, `+Y` up, `+Z` forward for authored characters and vehicles (`yaw = 0` points along `+Z`).
- **JSON Quaternions**: serialized as `[x, y, z, w]` arrays in content manifests (converted cleanly to/from `glm::quat(w, x, y, z)`).

---

## 2. Authoritative Runtime Simulation Pipeline (`GameplaySession`)

Headers: [`src/gameplay_session.hpp`](../src/gameplay_session.hpp), [`src/gameplay_framework.hpp`](../src/gameplay_framework.hpp)

`GameplaySession` provides deterministic ordering across 5 per-frame stages (`dt = clamp(delta_seconds, 0.0f, 0.1f)`) and supports opt-in project customization with zero per-frame heap allocations:

1. **Stage 1 — Input & AI** (`on_input_ai`)
2. **Stage 2 — Movement & Physics** (`AnimatedCharacterController` or `CharacterController`, `on_movement_physics`, `PhysicsWorld::step`, trigger & entity updates)
3. **Stage 3 — Transforms & Child Attachments** (`RuntimeTransformRegistry::sync_outputs` updating live `CollisionWorld` colliders + `Renderer` transforms, `on_post_transforms`)
4. **Stage 4 — Animation & IK** (`AnimatorRuntimeSystem::update` reading updated live world matrices, `on_post_animation`)
5. **Stage 5 — Camera, Spatial Audio & UI Overlay** (`GameplayCameraRig` / `ThirdPersonCamera`, `GameOverlayCanvas`, `Renderer::set_game_overlay_vertices`, `AudioEngine` spatial sync from `RuntimeTransformRegistry`)

### Project Customization Hooks

- `GameplaySession::set_player_setup(const GameplayPlayerSetup&)`:
  - `custom_player_node`: bind a scene node instead of spawning the default `"Runtime Player"` cube.
  - `disable_default_player_cube`: opt out of automatic cube creation.
  - `use_animated_character` & `animated_character_config`: switch on `AnimatedCharacterController`.
  - `spawn_position`, `spawn_yaw_radians`, `initial_input_context`.
- `GameplaySession::set_extension_callbacks(const GameplayExtensionCallbacks&)`:
  - Stateless C function pointers + `void* user_data` for `on_start`, `on_stop`, `on_input_ai`, `on_movement_physics`, `on_post_transforms`, `on_post_animation`, `on_camera_audio_overlay`, `on_save_project_state`, `on_load_project_state`.
- `GameStateSnapshot::custom_project_json`: persisted under `"project_state"` in save files via `save_game_state` / `load_game_state`.

---

## 3. Authoritative Transform Hierarchy & Sockets (`RuntimeTransformRegistry`)

Header: [`src/runtime_transform_registry.hpp`](../src/runtime_transform_registry.hpp)

- Register runtime nodes (`register_node`, `register_node_pose`) and local sockets (`register_socket(owner, name, local_pose)`).
- Attach rigid children (`attach_child`, `attach_child_to_socket`) for doors, wheels, headlights, or seated drivers with cycle detection.
- Query world transforms (`world_matrix`, `world_pose`, `socket_world_matrix`, `socket_world_pose`) or call `sync_outputs(&collision_world, &renderer_sink)` once after transform updates.

Standard vehicle/character socket conventions:
- `seat_driver`, `entry_driver`, `exit_driver`, `hand_l`, `hand_r`, `foot_l`, `foot_r`.

---

## 4. Animated Character & Root Motion

Headers: [`src/animated_character.hpp`](../src/animated_character.hpp), [`src/skeletal_animation.hpp`](../src/skeletal_animation.hpp)

- `AnimatedCharacterController`:
  - Data-driven `CharacterClipBinding` slots for `idle`, `walk`, `run`, `stop`, `turn_left`, `turn_right`, `jump`, `fall`, `land`, plus explicit `root_motion_bone` and `set_override_clip`.
  - Root-motion deltas are extracted via `extract_root_motion_delta(skeleton, bone_idx, ...)` and resolved against `CollisionWorld::resolve_capsule`.
- `AnimatorRuntimeSystem::set_root_motion_bone(SceneNodeId, std::string_view)`: specifies the root-motion bone per animator node; root-motion translation is reconciled against `CollisionWorld` capsules during `update`.
- Renderer skeletal draws lock skinned assets to LOD0 with expanded bounds and skip static TLAS inclusion to prevent bone/LOD popping artifacts.

---

## 5. Vehicles, Occupancy & Cameras

- **Vehicle Physics** ([`src/vehicle_controller.hpp`](../src/vehicle_controller.hpp)):
  - `VehicleController` / `VehicleConfig::make_default_sedan`: 4-wheel raycast spring-damper suspension, lateral cornering grip + handbrake drift scaling, anti-roll bars, brake/reverse light flags, wheel spin/steer/compression, and engine RPM telemetry.
  - Backed by extended `PhysicsWorld` APIs (`apply_central_force`, `apply_force_at_point`, `apply_torque`, `apply_impulse_at_point`, `point_velocity`, `set_body_enabled`, adaptive CFL substeps, and `ignored_node` raycasts).
- **Vehicle Occupancy** ([`src/vehicle_interaction.hpp`](../src/vehicle_interaction.hpp)):
  - `VehicleInteractionController`: manages `on_foot`, `entering_vehicle`, `driving`, `exiting_vehicle`.
  - Evaluates `entry_driver` / `seat_driver` / `exit_driver` sockets, animates door opening angle, switches `desired_input_context()` (`gameplay` ↔ `vehicle`), toggles character collider via `CollisionWorld::set_collider_enabled`, and verifies `check_capsule_free` with fallback exit positions (staying in `driving` if all exits are blocked).
- **Camera & Input** ([`src/vehicle_camera.hpp`](../src/vehicle_camera.hpp), [`src/input_actions.hpp`](../src/input_actions.hpp)):
  - `VehicleCamera`: spring-damped chase camera with look-ahead, orbit auto-recenter, wall pull-in raycasts, and speed-based FOV boost.
  - `GameplayCameraRig`: smooth transition blending between on-foot and vehicle modes.
  - `InputActionSystem`: `InputContext::vehicle` with `vehicle_throttle`, `vehicle_brake`, `vehicle_steer_left`, `vehicle_steer_right`, `vehicle_handbrake`, `recover`, `quick_reset`, plus analog `vehicle_steer_axis()`, `vehicle_throttle_value()`, `vehicle_brake_value()`.

---

## 6. Pedestrian Crowd & City Traffic

- **Pedestrian Crowd** ([`src/navigation_crowd.hpp`](../src/navigation_crowd.hpp)):
  - `NavigationGraph` + `CrowdSystem`: preallocated budgeted agent simulation (`walk`, `wait_crosswalk`, `look_around`, `evade_threat`), neighbor separation, ground height snapping, and `SignalPassableFn` crosswalk queries.
- **Signals & Traffic** ([`src/traffic_system.hpp`](../src/traffic_system.hpp)):
  - `SignalIntersectionSystem` (`green -> yellow -> all_red` phases, `query_pedestrian_signal` helper).
  - `LaneGraph` + `TrafficSystem`: IDM longitudinal following, red/yellow stop-line braking, obstacle emergency braking, far-agent recycling, and `release_vehicle_to_player(vehicle_id)` hijacking handoff.

---

## 7. Materials & 2D/Projected UI Overlay

- **Glass Materials** ([`src/cooked_model_asset.hpp`](../src/cooked_model_asset.hpp)):
  - `material_coverage_glass_bit` enables two-sided Fresnel sky reflection + Bayer screen-door transparency in forward/deferred/depth shaders and non-opaque ray-tracing BLAS handling.
- **UI Overlay Canvas** ([`src/game_overlay.hpp`](../src/game_overlay.hpp)):
  - `GameOverlayCanvas`: zero steady-state allocation 2D HUD renderer with ASCII + Cyrillic bitmap atlas (`pack_rgba8`, `draw_rect`, `draw_rect_outline`, `draw_line`, `draw_progress_bar`, `draw_arc`, `draw_text`, `measure_text`, `project_world_to_screen`).
  - Submitted automatically at the end of Stage 5 via `Renderer::set_game_overlay_vertices`.

---

## 8. Content Manifests & Cooker CLI

Headers/Tools: [`src/content_manifest.hpp`](../src/content_manifest.hpp), [`tools/vcr_content_cooker.cpp`](../tools/vcr_content_cooker.cpp)

```powershell
build/windows-release/bin/vcr_content_cooker.exe `
  --export-dir <project>/assets/<pack>/export `
  --cooked-dir <project>/assets/<pack>/cooked `
  [--dry-run]
```

Validates `version == 1` schemas (`asset_manifest.json`, `scene_layout.json`, `animation_manifest.json`), imports deduplicated `.glb`/`.gltf` assets into `--cooked-dir`, and writes `cooked_catalog.json` atomically without mutating `--export-dir`.
