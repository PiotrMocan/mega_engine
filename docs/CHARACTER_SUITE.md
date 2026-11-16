# Character Studio (`vulkan_character_studio`)

Unified Vulkan 1.3 character creation, wardrobe, surface painting, animation/voice/dialogue authoring, and engine asset cooking application for **Mega Engine**.

## Overview

All character workflows live in a single native executable (`vulkan_character_studio.exe` on Windows) sharing one Vulkan 1.3 PBR/AAA skin + eye + strand-groom viewport, 50-bone skeletal rig preview, and non-destructive `.vcrchar.json` package state:

| Tab | Workspace (`--tab` / `--workspace`) | Capabilities |
| :--- | :--- | :--- |
| **1. Anatomy & Groom** | `gen` / `anatomy` / `1` | Full-character AI generator, body AI, gender (`female`/`male`), archetype, stature morphs, micro-sculpt brush (Add/Subtract/Smooth/Flatten) via triangle-exact raycast, skin & eye PBR controls, strand-groom hair & stubble parameters |
| **2. Wardrobe & Paint** | `outfit` / `wardrobe` / `paint` / `2` | Wardrobe stylist AI, layered garments (top, bottom, footwear, studio underwear, cape, pauldrons, gloves, belt), fabric presets & 4-slot palette, 3D surface Vertex Paint brush (`Albedo`/`Roughness`/`Metallic`), interior-body occlusion culling toggle |
| **3. Acting, Voice & Dialogue** | `act` / `acting` / `dialogue` / `3` | Acting AI, locomotion & temperament controls, procedural keyframe clip inspector (`idle`, `walk`, `run`, `greet`, `alert`), FACS facial expression preview (`neutral`, `friendly`, `suspicious`, `happy`, `angry`, `focused`), bilingual EN/RU formant voice synthesizer + real-time SDL audio playback & WAV export, branching dialogue node & choice editor |
| **4. Cook & Verify** | `cook` / `export` / `verify` / `4` | Live package dependency & topology summary, texture resolution selector (`256`/`512`/`1024`), selective output toggles (`--no-voice`, `--no-prefabs`, `--no-loc`), atomic publish into `<project>/assets/` (`.vcrmodel`, `.vcrmat.json`, textures, `.vcrcanim.json`, `.vcraudio`, `.vcrdlg.json`, `.vcrprefab.json`, `loc_en`/`loc_ru`), and loader round-trip verification (`VERIFIED`) |

## Quick Launch

```cmd
scripts\run_character_studio_windows.cmd --project . --character characters\lyra_vance.vcrchar.json
```

Shortcuts in the GUI:
- `1`–`4`: Switch workspace tab (`Anatomy & Groom`, `Wardrobe & Paint`, `Acting, Voice & Dialogue`, `Cook & Verify`)
- `Ctrl+S`: Save `.vcrchar.json`
- `Ctrl+N`: Create New Character modal
- `F`: Frame camera on full body (`--focus-face` frames the face)
- `G`: Toggle ground grid
- `Space`: Play/Pause shared clip playback

Studio top-bar controls also expose **Project Character Selector**, **Refresh**, **New**, **Save**, **Cook & Verify**, unsaved-change protection on character switching, and shared bottom transport (`Play/Pause`, `Bind Pose`, clip selector, camera reset).

## Headless CLI (`--cli` / `--dry-run`)

Single-command end-to-end creation, modification, cooking, and verification without opening a window:

```cmd
vulkan_character_studio.exe --cli --project C:\MyGame --character hero_01 --name "Lyra" --full-prompt "athletic crimson scout" --cook --verify --json
```

Key options:
- `--project <dir>`: Target project root
- `--character <path|slug>` (alias `--input <path>`): Package file or slug under `<project>/characters/`
- `--tab <gen|outfit|act|cook>` (alias `--workspace`) & `--stage <gen|outfit|act|all>`
- `--prompt <text>`, `--full-prompt <text>`, `--body-prompt <text>`, `--outfit-prompt <text>`, `--act-prompt <text>`, `--offline` (deterministic offline fallback instead of local Ollama)
- Morph / identity / wardrobe / acting flags: `--name`, `--role`, `--seed`, `--gender`, `--archetype`, `--height`, `--musculature`, `--skin`, `--top`, `--bottom`, `--footwear`, `--hair`, `--material`, `--secondary-material`, `--primary-color`, `--secondary-color`, `--accent-color`, `--footwear-color`, `--hair-color`, `--[no-]cape`, `--[no-]belt`, `--[no-]gloves`, `--[no-]pauldrons`, `--[no-]beard`, `--[no-]studio-underwear`, `--[no-]occlude-body`, `--temperament`, `--pitch`, `--rate`, `--walk-speed`, `--run-speed`, `--add-node <id:sp:en[:ru]>`
- Output & verification: `--export-wav <path>`, `--cook`, `--tex-res <256|512|1024>`, `--no-voice`, `--no-prefabs`, `--no-loc`, `--verify`, `--dry-run`, `--json`

### Groom controls

The **Anatomy & Groom** inspector saves these scalp settings in the package's `wardrobe` object.
They are also available as CLI and GUI preview flags. Existing packages use the defaults below.

| Inspector | Flag / JSON key | Range | Default |
| :--- | :--- | :--- | ---: |
| Hair Density | `--hair-density` / `hair_density` | 0.15–1.5 of the style strand budget | 1.0 |
| Hair Length | `--hair-length` / `hair_length` | 0.6–1.4 of style length (ponytail: hanging tail) | 1.0 |
| Hair Width | `--hair-width` / `hair_width` | 0.5–1.5 of ribbon width | 1.0 |
| Hair Clumping | `--hair-clumping` / `hair_clumping` | 0–1 attraction toward compatible guides | 0.35 |
| Hair Flyaways | `--hair-flyaways` / `hair_flyaways` | 0–1 loose-fiber amount for bob/ponytail scalp | 0.15 |
| Hair Part | `--hair-part` / `hair_part` | −1–1 lateral bob part position | 0.25 |

Slider changes rebuild the preview on release. Lower density reduces emitted scalp geometry;
brows, lashes and beard retain their own budgets and deterministic random streams.
Settings are finite-checked and clamped when loading/cooking; they do not add per-frame work.

Scalp roots and the skin's follicle shadow use the same continuous anatomical mask, including
forehead/ear exclusions. Boundary triangles are sampled per follicle. Area-weighted root normals
and four integration substeps per guide segment reduce polygon-shaped growth patches and large
collision jumps. The emitted segment count is unchanged; the bob allocates more of its samples
near the curved roots. A bounded 72×37 radial contact field is built from the morphed cranial mesh
at generation time, with analytic fallbacks for open surfaces and the jaw/neck. It is an approximate
cranial collision proxy, not full body strand simulation. Collision corrections no longer accumulate
down a strand, and interpolation never crosses bob part groups or mohawk crest/side groups.

This remains a procedural ribbon groom. Artist-authored guides and painted growth masks, dynamic
hair LOD, motion/collision simulation and temporal/subpixel coverage need separate production work;
the controls above do not constitute an AAA quality certification or a measured FPS improvement.

## MCP Automation Server (`--mcp-stdio`)

Pass `--mcp-stdio` to expose JSON-RPC 2.0 MCP tools on stdin/stdout (`serverInfo.name = "vulkan_character_studio"`), including:
- Workspace & project management: `list_project_characters`, `load_character`, `new_character`, `set_workspace_tab`, `cook_character`, `get_character_summary`, `save_character`, `request_quit`
- AI generators: `ai_generate_full_character`, `ai_generate_body`, `ai_generate_outfit`, `ai_generate_acting`
