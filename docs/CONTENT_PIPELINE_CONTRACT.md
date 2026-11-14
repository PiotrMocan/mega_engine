# VCR Engine Content Pipeline Contract

This document defines the formal engineering contract for authoring, exporting, validating, cooking, and runtime ingestion of content assets into the VCR Engine (`vulkan_cube_renderer`).

---

## 1. Coordinate System & Units

The VCR Engine operates on a strict Cartesian coordinate system:

- **Handedness**: Right-handed
- **Up Axis**: `+Y`
- **Forward Axis**: `+Z`
- **Right Axis**: `+X` (satisfying $\vec{X} \times \vec{Y} = \vec{Z}$)
- **World Units**: Metric meters (`1.0` unit = `1.0` meter)
- **Scale**: Positive, finite scale components; uniform scale `[1.0, 1.0, 1.0]` is normal. Reflection / negative scale is prohibited in source content manifests.

### Quaternions

All orientation vectors and rotation components in manifests and data interchange files are formatted as:

$$\mathbf{q} = [x, y, z, w]$$

- **Array order**: `[x, y, z, w]` where $w$ is the real scalar component and $(x, y, z)$ is the imaginary vector component.
- **Identity rotation**: `[0.0, 0.0, 0.0, 1.0]`
- **Validation**: Quaternions must contain finite components and satisfy $\|\mathbf{q}\|^2 > 10^{-8}$ (norm $> 10^{-4}$). Quaternions are normalized upon loading into engine runtime structures.
- **GLM Integration**: Internally, GLM constructs quaternions as `glm::quat(w, x, y, z)`. The loader correctly maps input index 3 to scalar $w$, and indices 0, 1, 2 to $(x, y, z)$.

---

## 2. Blender Authoring & glTF 2.0 Export Contract

Blender utilizes a default coordinate convention of **`+Z` Up, `-Y` Forward, `+X` Right**.

To ensure 1:1 orientation and scale alignment between Blender scenes and the VCR runtime:

### Pre-Export Checklist in Blender
1. **Apply Transforms**: All meshes and armatures must have transforms applied prior to export:
   `Object -> Apply -> All Transforms` (`Ctrl + A`).
2. **Facing Direction**:
   - Characters and directional props must face towards Blender's `-Y` axis (which maps to VCR's `+Z` forward axis).
   - Character feet and ground contact points must rest on Blender's ground plane ($Z = 0$).
3. **Unit Scale**:
   - Scene unit system: Metric.
   - Unit Scale: `1.0`.

### Standard glTF 2.0 Export Settings
When exporting `.glb` or `.gltf` from Blender (via `File -> Export -> glTF 2.0`):
- **Format**: `glTF Binary (.glb)` preferred for compact, single-file distribution.
- **Transform**:
  - `+Y Up`: **Enabled** (converts Blender's $Z$-up into standard glTF $+Y$-up).
- **Data -> Mesh**:
  - `Apply Modifiers`: **Enabled** (unless exporting skinned armatures with vertex groups).
  - `Normals`: **Enabled**.
  - `Tangents`: **Enabled** if normal mapping is present.
- **Animation**:
  - `Group by NLA Track`: **Disabled** (export discrete animation actions).
  - `Always Sample Animations`: **Enabled**.

---

## 3. Runtime Sockets Contract

Sockets represent named attachment frames local to an asset. Children attach directly to sockets on parents (e.g. holding tools, mounting modular attachments, or IK targets).

### Standard Socket Names
The runtime supports standard socket identifiers alongside custom names:

| Socket Name | Description | Typical Owner | Local Forward / Up |
| :--- | :--- | :--- | :--- |
| `mount_primary` | Primary attachment mount pose | Modular Asset / Prop | Forward `+Z`, Up `+Y` |
| `interact_entry` | Interaction trigger reference pose | Interactive Prop | Facing interaction side |
| `interact_exit` | Egress placement point | Interactive Prop | Ground level outside prop |
| `hand_l` | Left hand tool / mount point | Character / Armature | Aligned with palm |
| `hand_r` | Right hand tool / mount point | Character / Armature | Aligned with palm |
| `foot_l` | Left foot IK contact / surface FX | Character / Armature | Soles facing `-Y` |
| `foot_r` | Right foot IK contact / surface FX | Character / Armature | Soles facing `-Y` |

Custom sockets (e.g. `turret_mount`, `muzzle_fx`, `camera_attach`) are fully supported using arbitrary string identifiers.

---

## 4. Manifest Specifications

Content directories exported from tools or DCC pipelines provide three JSON manifest files at their root:
1. `asset_manifest.json`
2. `scene_layout.json`
3. `animation_manifest.json`

All files require `"version": 1`.

### 4.1 `asset_manifest.json`

Defines base asset entities, including geometry sources, LOD chains, composite subparts, sockets, and physics colliders.

#### Schema
```json
{
  "version": 1,
  "assets": [
    {
      "id": "modular_turret",
      "type": "prop",
      "file": "props/turret_base.glb",
      "lods": [
        { "level": 1, "file": "props/turret_base_lod1.glb" },
        { "level": 2, "file": "props/turret_base_lod2.glb" }
      ],
      "parts": [
        {
          "name": "rotor_left",
          "asset_id": "rotor_unit",
          "position": [-0.85, 0.35, 1.4],
          "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
          "scale": [1.0, 1.0, 1.0]
        }
      ],
      "sockets": [
        {
          "name": "mount_primary",
          "position": [-0.38, 0.55, -0.2],
          "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]
        },
        {
          "name": "interact_entry",
          "position": [-1.2, 0.0, -0.2],
          "rotation_xyzw": [0.0, 0.7071068, 0.0, 0.7071068]
        }
      ],
      "colliders": [
        {
          "shape": "box",
          "name": "body_bounds",
          "position": [0.0, 0.6, 0.0],
          "size": [0.9, 0.5, 2.1],
          "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
          "layer": 1,
          "is_trigger": false
        }
      ]
    }
  ]
}
```

*Note on Sockets:* `"sockets"` can be authored as an array of objects (`[{"name": "...", ...}]`) or as a key-value object map (`{"mount_primary": {"position": [...], "rotation_xyzw": [...]}}`). The parser transparently normalizes both representations.

### 4.2 `scene_layout.json`

Specifies world entity instances and non-rendered gameplay markers.

#### Schema
```json
{
  "version": 1,
  "instances": [
    {
      "id": "turret_01",
      "asset_id": "modular_turret",
      "position": [12.5, 0.0, -4.2],
      "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
      "scale": [1.0, 1.0, 1.0]
    }
  ],
  "markers": [
    {
      "id": "spawn_player_01",
      "kind": "player_start",
      "position": [10.0, 0.0, -4.2],
      "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
      "links": ["turret_01"],
      "group": "sector_west"
    }
  ]
}
```

### 4.3 `animation_manifest.json`

Maps skeletal and rigid animation actions to assets, configuring playback properties, root motion extraction, and synchronized gameplay events.

#### Schema
```json
{
  "version": 1,
  "clips": [
    {
      "asset_id": "character_humanoid",
      "name": "run_forward",
      "loop": true,
      "root_motion": true,
      "nominal_speed_mps": 4.5,
      "events": [
        { "time": 0.28, "name": "footstep_left", "payload": "surface_concrete" },
        { "time": 0.62, "name": "footstep_right", "payload": "surface_concrete" }
      ]
    }
  ]
}
```

---

## 5. Manifest Validation Rules

The content validator (`vcr::load_content_manifests`) enforces rigorous security, consistency, and sanity checks:

1. **Path Safety**:
   - All file references must be relative paths.
   - Absolute paths (rooted or drive paths) are strictly rejected.
   - Path traversal (`..`) is forbidden.
2. **Referential Integrity**:
   - `part.asset_id` must resolve to an existing `id` declared in `asset_manifest.json`.
   - `instance.asset_id` must resolve to an existing `id` declared in `asset_manifest.json`.
   - `clip.asset_id` must resolve to an existing `id` declared in `asset_manifest.json`.
3. **Identifier Uniqueness**:
   - Asset IDs must be globally unique within `asset_manifest.json`.
   - Instance IDs must be globally unique within `scene_layout.json`.
   - Marker IDs must be globally unique within `scene_layout.json`.
4. **Numeric Sanity**:
   - All floating-point numbers must be finite (no `NaN`, `+Inf`, `-Inf`).
   - Scale components must be strictly positive ($> 0$).
   - Quaternion lengths must be non-degenerate ($\|\mathbf{q}\|^2 > 10^{-8}$).
   - Speeds and animation event timestamps must be non-negative ($\ge 0$).

---

## 6. Content Cooker CLI (`vcr_content_cooker`)

The cooker is an offline build tool that processes source export directories into cooked runtime-ready packages.

### CLI Synopsis
```bash
vcr_content_cooker --export-dir <path> --cooked-dir <path> [--dry-run] [--help]
```

### Operational Invariants
- **Source Non-Mutation**: The cooker **NEVER** modifies or writes any file inside `--export-dir`. It rejects overlapping or mutually contained `--export-dir` / `--cooked-dir` paths before writing.
- **Path & Manifest Hardening**: Symlinks on input/output files and directories are rejected, and manifest/layout JSON inputs are capped at `16 MiB` before parsing.
- **Deduplication**: Source mesh files (`.glb`, `.gltf`) shared across assets or LODs are imported and processed exactly once.
- **Atomicity**: Outputs verify explicit `file.flush()` before closing, and `cooked_catalog.json` is staged to a temporary file (`.tmp`) and atomically committed via `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows.
- **Idempotency**: Existing cooked asset directories are cleanly purged and re-staged on repeat executions.

### Cooked Catalog Structure (`cooked_catalog.json`)
The resulting `cooked_catalog.json` in `--cooked-dir` provides the runtime with fast, direct asset resolution:

```json
{
  "version": 1,
  "assets": {
    "modular_turret": {
      "id": "modular_turret",
      "type": "prop",
      "source_file": "props/turret_base.glb",
      "primary_asset": "models/props/turret_base/turret_base.vcrmodel.json",
      "lods": [
        {
          "level": 1,
          "source_file": "props/turret_base_lod1.glb",
          "file": "models/props/turret_base_lod1/turret_base_lod1.vcrmodel.json"
        }
      ],
      "parts": [...],
      "sockets": [...],
      "colliders": [...]
    }
  },
  "scene_layout": {
    "instances": [...],
    "markers": [...]
  },
  "animation_manifest": {
    "clips": [...]
  }
}
```

---

## 7. Runtime Architecture & Zero Steady-State Allocation

The runtime components consuming cooked content adhere to the VCR engine performance mandate:

- **`RuntimeTransformRegistry`**:
  - Maintains hierarchical transforms (parent-child attachments and socket mounts).
  - Pre-allocates storage via `reserve(expected_nodes)`.
  - Recomputes topological depths only when attachment structure changes (`hierarchy_dirty_`).
  - Executes `propagate_hierarchy()` and `sync_outputs()` with **ZERO heap allocations** during frame loop execution.
  - Maintains an internal `SparseComponentPool<SceneNodeId, glm::mat4, max_scene_nodes>` (`live_world_nodes()`) allowing $O(1)$ lookup for renderer, collision, and animation IK systems.
