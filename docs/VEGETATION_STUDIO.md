# Vulkan Vegetation Studio (`vulkan_vegetation_studio.exe`)

Standalone procedural vegetation authoring, 3D foliage painting, LOD optimization, and engine asset publishing suite running on the engine's Vulkan 1.3 renderer.

## Overview

`vulkan_vegetation_studio.exe` shares the same cooked formats (`*.vcrasset.json`, `*.vcrmesh`, `*.vcrmaterial.json`, `*.vcrtexture.json`, PNG textures) and shaders as `vulkan_cube` and `vulkan_editor`, allowing both human artists and autonomous AI agents to create, tune, stress-test, and publish vegetation straight into engine game projects.

## Archetypes & Built-in Presets (14 Presets Across 6 Archetypes)

| Archetype (`VegetationArchetype`) | Built-in Presets | Notes |
| --- | --- | --- |
| `grass_patch_gpu` | `meadow_fescue_gpu`, `steppe_dry_grass_gpu` | GPU-animated blade patches validated with `validate_gpu_grass_submesh` across LOD0–LOD3 |
| `grass_tuft` | `wild_fern_tuft`, `wetland_reed_clump` | Ground tufted herbaceous clusters and wetland reeds |
| `bush` | `forest_hazel_bush`, `arid_scrub_bush` | Multi-stem branching woody shrubs |
| `broadleaf_tree` | `silver_birch`, `ancient_oak`, `autumn_maple`, `winter_bare_oak` | Recursive branching crowns, full/sparse/bare foliage states |
| `conifer_tree` | `nordic_pine`, `alpine_spruce` | Whorl-tiered coniferous silhouettes |
| `palm_tree` | `tropical_coconut_palm`, `desert_date_palm` | Curved trunks, radial arched fronds, optional dry skirt |

## Key Studio Workflows

- **Seasonal / Foliage Variations**: Switch instantly between `full`, `sparse`, and `bare` crown states (`bare` strips foliage submeshes while keeping bark skeleton intact).
- **Interactive 3D Crown Painting & Pruning**: Arm the 3D brush in `Hero x1` arena to raycast onto plant clusters and paint spherical strokes (`tint`, `autumn`, `dry`, or `prune_bare`). Strokes persist inside `.vcrveg.json` sidecar recipes and round-trip losslessly.
- **Procedural Atlases & PBR Maps**: Generates `1024×1024` (`256/512/1024/2048` configurable) 4×4 botanical sprig & 1×8 horizontal palm frond PBR sets (`foliage_atlas` sRGB + alpha dilation, `foliage_normal`, `foliage_orm` with packed AO in `R`, roughness in `G`), `512×512` (`128..1024`) species-specific bark PBR sets (`bark_albedo`, `bark_normal`, `bark_orm`), and `256×256` GPU grass blade PBR sets (`grass_albedo`, `grass_normal`, `grass_orm`), previewed live inside Vulkan UI thumbnails (`UiTexturePreview`).
- **5 Scalability Arenas**:
  - `hero_single` (1 hero plant, interactive 3D crown painting)
  - `lod_lineup` (LOD0–LOD3 displayed simultaneously side by side regardless of camera distance)
  - `grove_100` (100 trees/patches, automatic 4-LOD switching + frustum/occlusion culling)
  - `forest_2500` (2,500 instances stress arena)
  - `meadow_10000` (10,000 instances high-density stress arena)
- **Optimization Budget Lab**: Tracks LOD0–LOD3 vertices/triangles/submeshes/geometric error, packed vs float vertex memory savings, RGBA texture budget, alpha overdraw estimate, budget grade (`A`–`D`), real-time FPS / 1% low / CPU & GPU ms timings, and resident GPU memory.

## Cooked Package Output Format

Exporting `<slug>` to `<project_dir>/assets/<category_subdir>/<slug>/` (or via `--out-asset-root <dir>`) produces:

- `<slug>.vcrveg.json` — lossless procedural recipe + paint strokes sidecar (`schema: vcr.vegetation_recipe`)
- `<slug>.vcrasset.json`, `<slug>_lod1.vcrasset.json`, `<slug>_lod2.vcrasset.json`, `<slug>_lod3.vcrasset.json`
- `<slug>.vcrmesh`, `<slug>_lod1.vcrmesh`, `<slug>_lod2.vcrmesh`, `<slug>_lod3.vcrmesh`
- `materials/bark.vcrmaterial.json`, `materials/foliage.vcrmaterial.json` (and/or `materials/grass_blade.vcrmaterial.json`) with `occlusion_in_metallic_roughness: true`
- `textures/*.vcrtexture.json` + `.png` PBR texture sets (`bark_albedo/normal/orm`, `foliage_atlas/normal/orm`, `grass_albedo/normal/orm`)

Both `vulkan_vegetation_studio` and `vulkan_editor` open `.vcrasset.json` / `.vcrveg.json` natively via `load_cooked_model_asset`, `load_cooked_texture_asset`, `open_vegetation_package`, and `list_project_vegetation_assets`.

## CLI & Agent Usage

```powershell
# Launch interactive studio with validation
scripts\run_vegetation_studio_windows.cmd

# Headless generation + cook straight into a project or explicit asset root
build\windows-release\bin\vulkan_vegetation_studio.exe --cli-generate --preset tropical_coconut_palm --project C:\path\to\project
build\windows-release\bin\vulkan_vegetation_studio.exe --cli-generate --preset forest_hazel_bush --out-asset-root C:\path\to\assets

# GUI + Vulkan validation smoke run
build\windows-debug\bin\vulkan_vegetation_studio.exe --smoke-frames 20 --validation

# Dedicated MCP stdio server (JSON-RPC 2.0, protocolVersion 2025-11-25)
build\windows-debug\bin\vulkan_vegetation_studio.exe --mcp-stdio --validation
```

## MCP Tools (`15` tools)

1. `veg.list_presets` — List all 14 built-in presets across the 6 archetypes.
2. `veg.get_state` — Return active `recipe`, `bounds`, `report`, `arena`, `camera`, `lighting`, `paint_brush`, and `metrics`.
3. `veg.apply_preset` — Switch active preset by `preset_id` with optional `seed`.
4. `veg.set_recipe` — Merge partial or full recipe JSON changes and regenerate LODs.
5. `veg.randomize_seed` — Assign a new deterministic seed and rebuild.
6. `veg.paint_foliage` — Apply a 3D spherical stroke (`tint`, `autumn`, `dry`, `prune_bare`).
7. `veg.clear_paint` — Remove all paint/pruning strokes.
8. `veg.set_arena` — Switch between `hero_single`, `lod_lineup`, `grove_100`, `forest_2500`, `meadow_10000`.
9. `veg.set_camera` — Position/target camera, set vertical FOV, or auto-frame arena bounds.
10. `veg.export_to_project` — Export cooked engine assets + `.vcrveg.json` sidecar to a project.
11. `veg.open_from_project` — Load an existing `.vcrveg.json` or `.vcrasset.json` package cleanly.
12. `veg.list_project_vegetation` — Scan a project's assets directory for `.vcrveg.json` packages.
13. `render.get_metrics` — Live FPS, 1% low, CPU/GPU timings, culling statistics, and resident VRAM.
14. `capture.screenshot` — Capture the Vulkan frame as inline PNG `ImageContent` and optional PNG file.
15. `engine.quit` — Cleanly shut down the studio.
