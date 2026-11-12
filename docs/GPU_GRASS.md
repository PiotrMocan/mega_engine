# Procedural GPU Grass Prototype

## Scope and boundaries

This document describes the bounded procedural GPU grass prototype built on the
renderer's existing C++23 / Vulkan 1.3 model, visibility, and indirect-draw
architecture (`src/gpu_grass.hpp`, `shaders/grass_vertex.glsl`,
`scripts/generate_gpu_grass_fixture.py`).

Explicit scope boundaries:

- **Not mesh shaders or Nanite:** Grass patches use standard indexed triangle
  draws, vertex-shader evaluation, and four authored patch LODs. This path does
  not use `VK_EXT_mesh_shader`, hardware tessellation, or micropolygon/cluster
  virtualization.
- **Separate from the canonical pine scene:** The fixture generator writes an
  isolated asset root inside an existing approved `build/windows-*` tree and
  verifies that `assets/scenes/editor_scene.json` is untouched. It does not
  replace or modify user scenes or imported tree assets.
- **Honest CPU/GPU cost:** There is no per-blade CPU work in the frame loop,
  but rendering grass is neither "zero CPU" nor "free GPU". CPU frame packet
  setup, visibility dispatch (or CPU visibility fallback), command recording,
  and `vkQueueSubmit2` remain bounded per-frame work, while dense patches
  consume real GPU vertex, shadow, rasterization, and fragment throughput.

## Encoded patch mesh and material contract

Grass patches are stored as ordinary cooked assets (`VCRMESH\0` v1 binary +
`.vcrasset.json` + `.vcrmaterial.json`) so they flow through
`load_cooked_model_asset`, `prepare_authored_model_lods`,
`optimize_model_geometry`, and packed/float vertex upload without a bespoke file
format.

### Material contract

A grass patch material uses:

- `pbr.model`: `"gpu_grass"` (`MaterialModel::gpu_grass = 3`)
- `pbr.subsurface_profile`: `"foliage"` (`SubsurfaceProfile::foliage = 4`)
- `pbr.subsurface_strength`: `0.65` (`gpu_grass_default_subsurface_strength`)
- `alpha_mode`: `"opaque"` (opaque geometry, no alpha-mask discard)
- `double_sided`: `true` (`VK_CULL_MODE_NONE` for thin two-sided blades)
- `textures`: `{}` (non-masked, non-tessellated; feeds the shared foliage PBR
  and subsurface lighting path)

This model token is accepted only in cooked mesh materials. Scene/prefab node
PBR settings and primitive edits cannot select it. The material editor keeps an
existing grass geometry family fixed; changing an ordinary mesh to grass would
require reloading and validating its encoding. Normal PBR color/roughness
editing remains available.

### Encoded `texcoord` contract

Each cooked vertex encodes its procedural blade address in `binary32` UVs:

- `texcoord[0] = float((blade_index << 2) | corner_code)`
  - `blade_index` in `[0, 4096)` (`max_gpu_grass_blades_per_patch = 4096`)
  - `corner_code` in `{0 = left_edge (side 0.0), 1 = center_tip (side 0.5, valid only at height_t == 1.0), 2 = right_edge (side 1.0)}`
- `texcoord[1] = -2.0 + height_t`, where `height_t` in `[0, 1]` maps
  `texcoord[1]` into `[-2.0, -1.0]` (`gpu_grass_uv_v_bias = -2.0`)

Design properties:

1. Small non-negative integers in `U` (`<= 16382`) and dyadic fractions in `V`
   (`[-2, -1]`) are exact in IEEE-754 `binary32` and survive both lossless LOD
   vertex deduplication and `--model-vertices packed` (which keeps `binary32`
   positions and UVs while quantizing only normal/tangent directions).
2. Placing `V` inside `[-2, -1]` prevents ordinary `[0, 1]` imported meshes from
   accidentally decoding as procedural grass.

### Load-time validation (`validate_gpu_grass_submesh`)

On CPU asset preparation, submeshes assigned to `MaterialModel::gpu_grass` are
validated once at load time:

- Non-empty whole-triangle index ranges within vertex/index buffer bounds across
  LOD0 and all populated secondary LODs;
- Valid encoded grass UVs (`blade_index < 4096`, `corner_code <= 2`,
  `center_tip` restricted to `height_t >= 0.999`);
- Cooked vertex positions finite and within `2.5e-3` local units
  (`gpu_grass_reference_position_tolerance`) of the `wind_seconds == 0.0`
  CPU reference evaluator `evaluate_gpu_grass_blade`;
- Every triangle belongs to a single `blade_index`, uses more than one corner
  code, and spans at least `0.04` in `height_t`.

## Load-time reference geometry vs GPU vertex evaluation

- **Once at load on CPU:** Cooked patch vertices store the static `wind_seconds == 0.0`
  reference pose. Local visibility/culling bounds are expanded once via
  `enlarge_gpu_grass_local_bounds` to cover the `±0.44 m` root footprint,
  natural bend (`<= 0.22 m`), maximum wind displacement (`<= 0.34 m`), blade
  half-width (`<= 0.020 m`), and vertical margin (`0.04 m`).
- **Per frame on GPU:** Thousands of translated, rotated, and uniformly scaled
  meadow patches are submitted as standard model instances sharing one vertex
  buffer, one index buffer, and one material. No per-blade CPU updates run
  during steady-state frames.
- **Vertex-stage blade and wind reconstruction:** When enabled, the vertex
  shader reconstructs the R2 quasi-random blade root, blade yaw/height/width,
  quadratic spine bend with vertical foreshortening, splayed curved normal for
  rounded ribbon shading, root-to-tip AO scale (`mix(0.42, 1.0, sqrt(t))`), and
  world-space travelling gusts plus per-blade flutter.
- **Anchored roots and seamless phase wrap:** Blade roots (`height_t == 0.0`)
  receive zero wind offset and stay anchored to the patch ground under any
  instance transform. Active wind frequencies are prime multiples (`59`, `97`,
  `211`) of `2 * pi / 256 s`; `wrap_gpu_grass_wind_seconds` maps `[256, +inf)`
  into `[256, 512)` where the `0.5 s` startup smoothstep envelope is saturated
  at `1.0`, preserving continuous positions and temporal motion vectors across
  wrap boundaries while reserving `0.0` for the exact static reference pose.

## Pipeline specialization (`constant_id = 7`)

Adding dynamic runtime grass branches directly inside the shared ordinary model
vertex shaders regressed the ordinary 11,533-node pine forest scene from roughly
`148 FPS` to `133 FPS` in measurements. The precise hardware stall mechanism
was not established from counters.

To eliminate that overhead on ordinary models without multiplying per-material
shaders, `shaders/grass_vertex.glsl` gates procedural grass evaluation behind
Vulkan specialization constant `constant_id = 7` (`VCR_ENABLE_GPU_GRASS`,
defaulting to `false` on standard model pipelines) and pairs with dedicated
prewarmed grass pipelines (`constant_id = 7` specialized to `true`). Three
final forest runs measured 147.92..148.25 FPS, median 148.03 FPS.

## Patch culling, nested LODs, and conservative error bounds

### Visibility and fallback

Grass patch instances use the existing visibility architecture:

- `--visibility-backend auto` / `gpu`: GPU compute frustum/occlusion culling,
  per-instance LOD selection against the expanded conservative patch bounds, and
  compacted indirect draw batches.
- `--visibility-backend cpu`: Optional CPU visibility and LOD selection fallback
  using the same conservative `Bounds` and `select_lod` threshold math when GPU
  compute culling is disabled or unavailable.

### Nested four-LOD patch hierarchy

Blade roots use a fixed-point 2D R2 low-discrepancy sequence so every prefix
`[0 .. N_lod)` remains evenly distributed across all four quadrants of the
`0.88 m x 0.88 m` patch footprint. Segment counts use nested dyadic subdivisions
(`4 -> 2 -> 2 -> 1`), making every vertex in LOD1, LOD2, and LOD3 bit-identical
to a vertex in LOD0:

| Level | Blades (`[0..N)`) | Segments / blade | Cooked vertices | Cooked indices | Triangles / patch | Sampled `t = 0` deviation (m) | All-wind upper bound (m) | Declared `geometric_error` |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| LOD0 | 128 | 4 | 1,152 | 2,688 | 896 | 0.000000 | 0.000000 | 0.0 |
| LOD1 | 64 | 2 | 320 | 576 | 192 | 0.271693 | 0.745369 | 0.75 |
| LOD2 | 32 | 2 | 160 | 288 | 96 | 0.311710 | 0.770525 | 0.78125 |
| LOD3 | 16 | 1 | 48 | 48 | 16 | 0.329202 | 0.929624 | 0.9375 |

During `prepare_authored_model_lods` and `optimize_model_geometry`, the `1,680`
cooked vertices across the four files deduplicate losslessly to **`1,152`
shared GPU vertices** (`3,600` combined indices), ordered from LOD3 forward so
distant patches fetch a compact contiguous prefix of the vertex buffer.

### Conservative LOD error vs pixel-error quality tradeoff

`CookedModelLodReference::geometric_error` specifies a conservative model-local
deviation from LOD0 **including blade removals**. Unlike continuous surface
simplification, dropping whole `0.34–0.68 m` blades across a `0.88 m x 0.88 m`
patch produces sampled surface deviations of `0.27–0.33 m` at rest. Analytical
upper bounds of `0.75–0.93 m` also cover independent gust phases, instance scale,
and piecewise-linear approximation of both meshes. Inventing
artificially small errors (such as `0.01`) would violate the conservative LOD
contract.

Tradeoff under `select_lod` (`error * scale * pixels / distance <= threshold`):

- With the strict default one-pixel LOD budget, a `0.75`
  local error projects below `1` pixel only at large distances (hundreds of
  meters at 1080p), preserving `< 1 px` worst-case single-blade geometry.
- In the default 44 m meadow, visible patches therefore stay at LOD0. The
  prototype supplies and validates all four LODs but does not claim a distant
  density optimization in this benchmark. An explicit coverage-based grass
  density policy is future work; lowering geometric errors to force a better
  FPS result would misrepresent the current error contract.

## Ray tracing and Voxel GI coherence

Hardware ray-tracing BLAS geometry and voxel GI scene representations consume
the cooked static vertex positions (`wind_seconds == 0.0`) rather than re-running
vertex-shader wind every frame across thousands of patches. Whenever ray tracing
(`--rt shadows|gi|full`) or voxel GI (`--gi voxel`) is active, grass wind
freezes at `wind_seconds == 0.0` (`wrap_gpu_grass_wind_seconds(elapsed, true)`).
Rasterized grass, ray-traced shadows/GI, and voxelized lighting then match the
same static vertex positions. This avoids geometry disagreement caused by wind;
it does not eliminate unrelated shadow bias or ray-tracing artifacts.

## Fixture generator and schema

Generate the isolated grass fixture under an existing approved Windows build
tree (`Debug`, `Release`, or `ASan`) using native Windows Python:

```powershell
python -B scripts/generate_gpu_grass_fixture.py --configuration Release
```

Optional density/target arguments:

- `--configuration {Debug,Release,ASan}` (default `Release` ->
  `build/windows-release/gpu-grass-fixture`)
- `--output <subdir>`: explicit target path, accepted **only** if it resolves to
  a non-reserved subdirectory strictly inside an existing `build/windows-debug`,
  `build/windows-release`, or `build/windows-asan` tree
- `--grid-x 64 --grid-z 64 --patch-spacing 0.68` (default `4,096` patches =
  `524,288` blades and `3,670,016` LOD0 triangles reusing one model and one
  material)

Safety guarantees:

- Rejects non-Windows Python (`os.name != "nt"`), paths outside the three
  approved build trees, build-tree roots, reserved build folders (`bin`, `lib`,
  `CMakeFiles`, `_deps`, `generated`, `Testing`), and symbolic links.
- Refuses to overwrite any non-empty directory unless it carries a matching
  `fixture-manifest.json` (`"generator": "vcr.gpu-grass-fixture.v1"`).
- Verifies SHA-256 of `assets/scenes/editor_scene.json` before and after
  writing.

### Generated layout and actual schemas

```text
build/windows-release/gpu-grass-fixture/
  fixture-manifest.json
  assets/
    scenes/
      editor_scene.json
    grass_patch/
      lod0.vcrasset.json
      lod1.vcrasset.json
      lod2.vcrasset.json
      lod3.vcrasset.json
      mesh/
        lod0.vcrmesh
        lod1.vcrmesh
        lod2.vcrmesh
        lod3.vcrmesh
      materials/
        grass_blade.vcrmaterial.json
```

Material (`assets/grass_patch/materials/grass_blade.vcrmaterial.json`):

```json
{
  "schema": "vcr.asset",
  "version": 1,
  "kind": "material",
  "name": "GpuGrassPatch",
  "alpha_mode": "opaque",
  "double_sided": true,
  "pbr": {
    "base_color": [0.14, 0.34, 0.07, 1.0],
    "metallic": 0.0,
    "roughness": 0.58,
    "ambient_occlusion": 1.0,
    "ior": 1.45,
    "model": "gpu_grass",
    "subsurface_profile": "foliage",
    "subsurface_strength": 0.65
  },
  "textures": {}
}
```

Model LOD0 metadata (`assets/grass_patch/lod0.vcrasset.json`; `lod1..lod3` use
the same schema without `"lods"`):

```json
{
  "schema": "vcr.asset",
  "version": 1,
  "kind": "model",
  "name": "GPU Grass Patch LOD0",
  "mesh": "mesh/lod0.vcrmesh",
  "generate_lods": false,
  "geometry": {
    "vertex_count": 1152,
    "index_count": 2688,
    "index_type": "uint32",
    "primitive": "triangles",
    "vertex_stride": 48
  },
  "materials": ["materials/grass_blade.vcrmaterial.json"],
  "submeshes": [
    {
      "name": "GrassBlades",
      "first_index": 0,
      "index_count": 2688,
      "material_index": 0,
      "material": "materials/grass_blade.vcrmaterial.json"
    }
  ],
  "lods": [
    {"model": "grass_patch/lod1.vcrasset.json", "geometric_error": 0.75},
    {"model": "grass_patch/lod2.vcrasset.json", "geometric_error": 0.78125},
    {"model": "grass_patch/lod3.vcrasset.json", "geometric_error": 0.9375}
  ]
}
```

Scene (`assets/scenes/editor_scene.json`):

- `"format": "vulkan_cube_scene"`, `"version": 1`, `"startup_camera": 4`,
  `"subsurface": "full"`, `"tone_mapper": "gt7"`
- `"lighting"`: dynamic sun + sky enabled (`"dynamic": true`, `"hour": 15.5`),
  `"voxel_gi": false`, `"ray_tracing": "off"` so wind animates out of the box
- Nodes: `"World"` folder, `"Sky"` (`environment`), `"Sun"`
  (`directional_light`), `"Meadow Ground"` (`ground` at `y = 0`),
  `"Close Meadow Camera"` (`camera` at `[0.0, 1.05, 1.8]`, pitch `-15°` looking
  into close foreground blades and across the meadow), `"Grass Meadow"` folder,
  and `4,096` translated/rotated/uniformly-scaled `"model"` instances referencing
  `"grass_patch/lod0.vcrasset.json"`.

### Isolated launch via `VCR_ASSET_ROOT`

After a native Windows build, launch the
renderer against the generated fixture asset root without touching canonical
project scenes:

```powershell
$previousRoot = [Environment]::GetEnvironmentVariable('VCR_ASSET_ROOT', 'Process')
try {
    $env:VCR_ASSET_ROOT = (Resolve-Path 'build/windows-release/gpu-grass-fixture/assets').Path
    & '.\build\windows-release\bin\vulkan_cube.exe' --no-validation --present immediate --lighting dynamic --gi off --rt off --subsurface full --visibility-backend gpu
} finally {
    [Environment]::SetEnvironmentVariable('VCR_ASSET_ROOT', $previousRoot, 'Process')
}
```

## Local measurements and runtime validation, 2026-11-12

Ryzen 7 9700X / Radeon RX 6800, native Release, driver 32.0.21045.16010,
1920 x 1080, IMMEDIATE, AA off, full SSS, dynamic four-cascade 2048 shadows,
RT/GI off, stationary camera, three runs of 5 s warmup + 15 s measurement.
Both backends had 4,096 grass patches, 3,242 visible instances including the
ground, and 2,903,939 reported triangles. All visible patches stayed at LOD0.

| Visibility backend | Median FPS | Range | 1% low | GPU frame (ms) | Geometry (ms) | Shadow (ms) |
|---|---:|---:|---:|---:|---:|---:|
| GPU | 231.46 | 230.86..235.36 | 214.97 | 4.115 | 2.341 | 1.534 |
| CPU | 249.95 | 249.39..250.07 | 232.00 | 3.799 | 1.960 | 1.600 |

GPU visibility did not win this stationary fixture. Scene preparation was
about 0.0025 ms and command recording about 0.057..0.059 ms in both paths;
the visibility result can be cached for a stationary camera. This comparison
does not measure per-blade CPU animation: both paths evaluate wind on the GPU.
Raw results are under `build/windows-release/grass-{gpu,cpu}-20261112/`.

Debug captures exercised live wind with FSR3, full depth prepass and full SSS;
two captures recorded advancing wind time. ASan captures exercised MSAA4x and
full depth with live wind. Debug RT-shadow and RT+voxel-GI captures confirmed
zero wind time, visible grass, BLAS/TLAS use, and zero live allocations at
shutdown. Runtime validation reported no errors. The editor smoke loaded the
generated fixture and reported 256 active foliage materials/instances.

Repeat the measured visibility comparison after generating the fixture:

```powershell
scripts/benchmark_vegetation_windows.ps1 -OutputName grass-local-gpu -AssetRoot build/windows-release/gpu-grass-fixture/assets -ExtraArguments '--visibility-backend=gpu'
scripts/benchmark_vegetation_windows.ps1 -OutputName grass-local-cpu -AssetRoot build/windows-release/gpu-grass-fixture/assets -ExtraArguments '--visibility-backend=cpu'
```

The prototype does not add hierarchical cluster LOD, a coverage-based distance
density policy, terrain-aware procedural placement, interactive bending, or
dynamic grass BLAS updates. Those require separate visual and performance gates.
