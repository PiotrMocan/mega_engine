# Vegetation rendering and measurements

## Shared shader families

Materials select data (textures, PBR parameters, alpha coverage and a shading
model). They do not compile a new shader per material. Textured models use the
`editor_model` family; untextured scene primitives use the `scene` family.
Temporal, SSS-output and ray-query variants are fixed render-pass variants,
independent of the number of materials.

The four fragment entry points now include `editor_model_fragment.glsl` or
`scene_fragment.glsl`. Both reuse `surface_lighting_common.glsl` for lighting,
shadow lookup and environment sampling. Explicit versus implicit shadow LOD,
derivatives before alpha/LOD discard, and temporal outputs are preserved by
compile-time definitions. The settled pine-scene capture before and after this
refactor matched pixel for pixel (1920 x 1080, AA off).

## Geometry preparation

Imported model preparation deduplicates complete vertex tuples, optimizes the
post-transform cache independently inside each material/LOD draw range, and
orders vertex fetches starting from the coarsest LOD. UV/normal seams, positions,
triangle winding, material assignment, triangle counts and LOD errors are kept.
This happens on asset preparation, not every frame. Source assets are unchanged.

`--model-vertices auto|float|packed` selects the GPU upload format. `auto` uses
the 28-byte packed format when `A2B10G10R10_SNORM_PACK32` supports vertex-buffer
fetch, otherwise the original 48-byte format. An explicit unsupported `packed`
request fails with a diagnostic. Positions and UVs stay binary32. Normal and
tangent directions use signed 10-bit components; tangent handedness remains
exact. Unit tests measure less than 0.11 degree direction error. This is a lossy
direction encoding: lighting and normal-based tessellation displacement can
change slightly. Ray-tracing BLAS uses the selected stride and the original
binary32 positions. The cooked file format stays unchanged.

Triangle order can change the result at exactly overlapping surfaces. On the
pine scene, 493 of 2,073,600 pixels changed after geometry reordering; mean
absolute RGB channel differences were 0.0130, 0.0111 and 0.0068 on a 0..255
scale. Repeating the baseline capture gave zero changed pixels. This is not a
claim of pixel-exact equivalence for geometry reordering.

## Initial controlled comparison, 2026-11-12

Native Windows Release, Ryzen 7 9700X, Radeon RX 6800, driver 32.0.21045.16010,
1920 x 1080, IMMEDIATE, validation disabled, same saved pine scene (11,533
nodes), 5 seconds warmup and 15 seconds measurement, three runs each. Windows
Balanced power plan; clocks controlled by the driver, not locked. Environment
and process snapshots are stored with the raw results. No concurrent renderer
or editor was running during measurement.

| Scenario | Original median FPS | Cache/fetch median FPS | Change |
|---|---:|---:|---:|
| Stationary camera | 113.79 | 140.37 | +23.4% |
| Animated camera | 111.08 | 132.85 | +19.6% |

Stationary GPU frame fell from about 8.54 to 6.91 ms. Geometry accounted for
about 8.42 ms of the original frame. Ray tracing was disabled and stationary
shadows were cached. The reported 43.76 million triangles and selected LODs
were unchanged. Animated-camera shadow time fell from about 0.49 to 0.28 ms.
These are local measurements, not predictions for other GPUs.

A full depth prepass probe reached 72.85 FPS, so it was not enabled as a
vegetation optimization. It repeats too much geometry work for this scene.

With shared shaders and cache/fetch optimization, three runs on the same binary
gave 140.51 FPS for `--model-vertices float` (range 140.46..140.54) and 148.14 FPS
for `--model-vertices packed` (148.07..148.16), a further 5.4%. Packed GPU frame
was 6.532 ms, geometry 6.421 ms and 1% low 140.33 FPS. Relative to the original
113.79 FPS, the combined stationary improvement is 30.2%. No LOD thresholds,
resolution or triangle counts were reduced to obtain these measurements.

After integrating the separate GPU grass family, the final ordinary-forest
check measured 147.92..148.25 FPS (median 148.03), GPU frame 6.544 ms and 1% low
139.67 FPS. This is +30.1% against the original scene, with no regression from
the grass specialization in this test.

The integrated moving-camera check measured 142.51..142.65 FPS, median 142.55
(+28.3% against the original 111.08). Median GPU frame was 6.805 ms and shadow
time 0.241 ms. The camera path matches the original moving-camera comparison.

Packed versus float changed 34,669 pixels of the stationary capture, with a
maximum channel difference of 5/255 and mean absolute channel differences
0.00707, 0.00631, 0.00371. The silhouettes matched on visual inspection. This
single scene is not an exhaustive quality test for all materials or displacement.
Packing also normalizes direction vectors. Use `--model-vertices float` for
authored non-unit normals whose magnitude is intentionally used by displacement.

Evidence stays under the allowed build tree, not in versioned assets:

- `build/windows-release/vegetation-before-20261112/run-{1,2,3}.json`
- `build/windows-release/vegetation-fetch-after-20261112/run-{1,2,3}.json`
- `build/windows-release/vegetation-moving-before-20261112/run-{1,2,3}.json`
- `build/windows-release/vegetation-moving-fetch-20261112/run-{1,2,3}.json`
- `build/windows-release/vegetation-float-final-20261112/run-{1,2,3}.json`
- `build/windows-release/vegetation-packed-final-20261112/run-{1,2,3}.json`
- `build/windows-release/vegetation-integrated-20261112/run-{1,2,3}.json`
- `build/windows-release/vegetation-moving-integrated-20261112/run-{1,2,3}.json`
- `build/windows-release/vegetation-images/`
- `build/windows-release/vegetation-profile-20261112/{baseline,optimized}.rgp`

RGP captures include counters, but detailed counter interpretation is still
pending: the desktop was locked during attempted profiler UI inspection.
GPU pass timestamps and controlled A/B results support the geometry bottleneck;
they do not establish a particular hardware-cache stall percentage.

Reproduce a three-run scene comparison from native PowerShell:

```powershell
scripts/benchmark_vegetation_windows.ps1 -OutputName vegetation-local
scripts/benchmark_vegetation_windows.ps1 -OutputName vegetation-local-moving -ExtraArguments '--animate-camera'
```

Keep resolution, saved scene, AA, visibility, lighting and LOD settings equal
between runs. Benchmark JSON records the actual backend and relevant options.

## Tighter transform bounds for LOD, second pass (2026-11-12)

Camera and shadow LOD selection previously scaled local geometric error by the
Frobenius norm of the instance transform. A uniformly scaled object therefore
paid an unnecessary `sqrt(3)` multiplier. CPU visibility and both GPU visibility
shaders now use the smaller of two conservative bounds on maximum stretch:
the square root of the absolute row-sum norm of `transpose(A) * A`, and the
Frobenius norm. For orthogonal transform columns this approaches the largest
axis scale; shear remains bounded. A small float-rounding margin is included.
The GPU camera/shadow paths share `lod_scale.glsl`; CPU code uses the same math.

This selects existing simpler LODs sooner without raising the configured pixel
error or altering authored errors, mesh files, textures or scene placement.
Unlike the first optimization pass, **the submitted triangle count changes**.
The geometric error budget does not guarantee identical alpha coverage or
lighting when an authored LOD changes.

Fresh native Windows Release comparison on the same RX 6800 / Ryzen 7 9700X,
driver 32.0.21045.16010, 1920 x 1080, IMMEDIATE, AA off, validation disabled,
saved 11,533-node scene, 5 s warmup + 15 s measurement, three runs per variant:

| Stationary metric | Before | After |
|---|---:|---:|
| Median FPS | 148.42 | 168.30 (+13.4%) |
| FPS range | 148.18..148.43 | 167.13..169.25 |
| Median GPU frame | 6.527 ms | 5.729 ms |
| Median GPU geometry | 6.415 ms | 5.610 ms |
| Median CPU frame | 6.738 ms | 5.942 ms |
| Median 1% low | 140.65 FPS | 142.58 FPS |
| Submitted triangles | 43,760,825 | 37,027,377 (-15.4%) |
| LOD0 / LOD1 / LOD2 / LOD3 instances | 27 / 85 / 454 / 6806 | 10 / 25 / 257 / 7080 |

The post-change 1% low range was 136.81..149.06 FPS, so its improvement is much
less stable than average FPS. Shadows were cached in this stationary case.
Clocks remain driver-managed under the Windows Balanced plan; no other renderer,
editor, build or analyzer was running during the final measurement series.

With `--animate-camera`, the same three-run comparison also exercises shadow
refreshes along the camera path:

| Moving-camera metric | Before | After |
|---|---:|---:|
| Median FPS | 131.78 | 170.93 (+29.7%) |
| FPS range | 129.90..132.46 | 170.57..171.35 |
| Median GPU frame | 7.404 ms | 5.677 ms |
| Median GPU geometry | 6.453 ms | 5.042 ms |
| Median GPU shadow time per frame | 0.798 ms | 0.487 ms |
| Median CPU frame | 7.588 ms | 5.850 ms |
| Median 1% low | 78.45 FPS | 96.40 FPS |

These are fresh comparisons against the pre-change executable and the current
saved scene, not a continuation of the earlier moving-camera result above.
Shadow timing is averaged over all frames, including frames with cached tiles.
The stationary median command-record time rose from 0.0735 to 0.0791 ms;
the moving case fell from 0.1182 to 0.1094 ms. The 0.0056 ms stationary cost is
accepted against the 0.798 ms reduction in total GPU time and improved total
frame time. The change adds scalar math to visibility selection, with no new
draw-recording or synchronization operations; the timing delta's hardware cause
has not been isolated. Remaining CPU stage timings are recorded alongside the
raw results in `build/windows-release/vegetation-lod-stage-comparison.json`.

The settled image comparison changes 43,362 of 2,073,600 pixels (2.09%). Mean
absolute RGB differences are 0.2429, 0.1825, 0.1743 on a 0..255 scale, with maxima
148, 124, 126. Inspection shows small foliage/shadow detail changes; this is
not pixel-exact rendering or an exhaustive quality test of authored LODs.

An independent overdraw-ordering probe fell to 140.82 FPS from the 148.42 FPS
baseline and was reverted. No overdraw reordering is part of this change.

Verification: Release, Debug and MSVC ASan each passed 13/13 CTest executables.
New tests cover rotated/nonuniform/reflected/singular transforms, overflow,
and 32,768 displacement directions across 256 arbitrary sheared matrices.
All shaders were compiled and passed `spirv-val` for Vulkan 1.3 / SPIR-V 1.5.
Debug captures covered the full forest through CPU and GPU visibility, plus
animated GPU grass with FSR and Bayer LOD transitions. ASan ran two animated
startup/shutdown cycles with resize, present/mode switches and AA off/SMAA/MSAA/
FSR. These runtime checks reported no Vulkan errors, ASan errors or live GPU
allocations. Existing unused vertex-output warnings remain.

MSVC `/analyze` + Core Check and clang-tidy ran on `scene_visibility.cpp` and
`renderer_scene.cpp`. No diagnostic points to the new norm calculation. Existing
generic bounds/style and vendor diagnostics remain; the reported hull index is
bounded by 8 lower-chain plus 7 upper-chain insertions into 16 slots. Alpha-word
indices in the unchanged opacity helper are bounded by its fixed map resolution.
No suppression was added. Manual review found no new allocation/resource,
thread-ownership or synchronization changes. Hardware-counter interpretation
was not completed: the Windows UI helper failed to launch its app server with
OS error 3, including after retry/reset. GPU timing results are measurements;
a particular hardware stall cause is not established.

Evidence:

- `build/windows-release/vegetation-second-before-20261112/`
- `build/windows-release/vegetation-lod-final-20261112/`
- `build/windows-release/vegetation-lod-moving-{before,after}-20261112/`
- `build/windows-release/vegetation-second-images/`
- `build/windows-release/vegetation-overdraw-probe-20261112/`
- `build/windows-debug/vegetation-lod-{cpu,gpu,grass}/`
- `build/windows-asan/render-validation/animated-gpu-lod-scale/`
- `build/windows-{release,debug,asan}/vegetation-lod-build.log`
- `build/windows-release/vegetation-lod-{shaders,msvc,tidy}.log`

## Research and next bottlenecks

Guerrilla's [GPU procedural placement](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn)
and [visibility work](https://www.guerrilla-games.com/read/decima-engine-visibility-in-horizon-zero-dawn)
describe separate placement and visibility stages. GPU work still consumes GPU
time and requires CPU submission/resource management; it is not free rendering.
Our existing compute visibility path already selects model LODs and compacts
visible instance indices for indirect draws.

AMD's [procedural grass example](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
uses patch generation, reduced distant blade density and GPU wind. A vertex
shader implementation can reuse those ideas on the existing Vulkan 1.3 path
without making mesh-shader support mandatory. It is not the same implementation
or a promise of the same performance.

The distant authored pine LODs still contain tens of thousands of triangles per
tree. A further large reduction will require coverage-preserving foliage LODs,
cluster impostors or a measured meshlet path. Alpha-card boundaries constrain
ordinary topology simplification. Whole-tree LOD selection already exists;
hierarchical cluster LOD and Nanite-style rendering are not implemented here.
Adding AO or ray tracing cannot by itself remove this geometric workload.

## Verification and remaining limits

Native Windows Release, Debug and ASan builds passed all 12 CTest executables.
All shader sources and fixed variants were compiled and checked with
`spirv-val` for Vulkan 1.3 / SPIR-V 1.5. Debug and ASan runtime checks covered
repeated startup/shutdown, resize, present/mode switches and AA off/SMAA/MSAA/FSR.
The editor MCP smoke also passed on the isolated grass scene. Successful
runtime gates reported zero Vulkan validation errors and zero live GPU
allocations; ASan reported no memory errors. Grass-specific evidence and
limitations are in [GPU_GRASS.md](GPU_GRASS.md).

Native MSVC `/analyze` + Core Check and clang-tidy were run on
`model_lod.cpp`, `asset_preparation.cpp`, `cooked_model_asset.cpp`,
`renderer.cpp` and `renderer_ray_tracing.cpp`. They are not warning-clean:
existing indexing/span/style suggestions and vendor-header diagnostics remain.
Reviewed optional-access diagnostics have explicit guards; the AA-array index
diagnostic is bounded by modulo array size; grass float-to-unsigned conversion
is finite and range-checked. No new high-confidence lifetime/bounds/leak
diagnostic was found. No suppressions were added.

Validation still emits unused shader-interface output warnings for the shared
color vertex stages reused by depth fragments. Those unused outputs are legal;
the warning is not a synchronization or object-lifetime failure. Detailed RGP
counter interpretation and any dedicated hardware allocation-counter capture
remain unperformed. No new steady-state allocation sites were introduced by
these changes on manual review.

The rendering-pipeline verifier's image assertions assume its procedural cube
fixture and failed when initially pointed at grass. The canonical lifecycle
verifier was rerun on its isolated fixture, with separate grass captures for
FSR/full-depth/SSS, MSAA, RT shadows and RT+voxel GI. This is not a claim that
the procedural-cube image assertion passed on a meadow.
