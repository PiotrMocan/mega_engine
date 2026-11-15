# VCR Modular Character Creation Suite (Unix / GNU Pipeline)

`vulkan_cube_renderer` provides an AI-first, zero-Blender-friction **Character Creation Suite** composed of four independent single-purpose utilities exchanging a shared non-destructive `.vcrchar.json` package (`vcr.character_package` v1).

## 1. Architecture ("One Utility for One Task")

| Tool | Role | Modes |
| :--- | :--- | :--- |
| `vulkan_char_gen` | Anatomy proportions, archetype, face sliders, skin complexion & 3D mesh sculpting | Vulkan 3D GUI, `--cli`, `--mcp-stdio` |
| `vulkan_char_outfit` | Modular top/bottom/footwear/hair/accessories, body occlusion culling & 3D PBR texture painting | Vulkan 3D GUI, `--cli`, `--mcp-stdio` |
| `vulkan_char_act` | 12 procedural locomotion/gesture clips, 12-bone facial rig, EN/RU formant WAV synth + visemes & bilingual dialogue graph | Vulkan 3D GUI, `--cli`, `--mcp-stdio` |
| `vcr_char_cooker` | Headless compiler & validator publishing `.vcrmesh`, `.vcrskin`, `.vcranim`, `.vcrtexture.json`, `.vcrmaterial.json`, `.vcrasset.json`, `.vcraudio.json`, `.vcrdlg.json`, `.vcrloc.json`, and NPC/Player `.vcrprefab.json` directly into `<project>/assets/` | CLI (`--verify`, `--dry-run`, `--json`) |

All three studio GUI apps share `vcr_character_studio_shell` (Vulkan 1.3 offscreen viewport with Subsurface Scattering + live 50-bone GPU skinning + orbit/pan/zoom + raycast viewport picking + async Ollama AI worker + top handoff toolbar) and preserve unknown/foreign JSON keys (`extra_root_json`, `extra_body_json`, `extra_wardrobe_json`, `extra_acting_json`, `extra_dialogue_json`) across round-trips.

## 2. Shared Format & Rigging

- **Source Package**: `<project>/characters/<slug>.vcrchar.json`
- **Anatomical Base Models (CC0 1.0)**: MakeHuman / MPFB2 clean-topology female & male bases (`assets/character_templates/human_female.vcrcharbase`, `assets/character_templates/human_male.vcrcharbase`; notice in `LICENSES/MAKEHUMAN_MPFB_CC0.txt`)
- **Skeleton**: 50 bones — 38 body bones (`Hips`..toes & fingers) + 12 facial rig bones (`Jaw`, `Lip_Upper`, `Lip_Lower`, `Mouth_Corner_L/R`, `Eye_L/R`, `Eyelid_Upper_L/R`, `Brow_L/R`, `Brow_Center`)
- **Generated Clips (12)**: `idle`, `walk`, `run`, `stop`, `turn_left`, `turn_right`, `jump`, `fall`, `land`, `wave`, `talk`, `attack`

## 3. CLI & Ollama Automation Examples

```cmd
:: Step 1: Generate body & face via Ollama (or offline deterministic AI fallback)
build\windows-release\bin\vulkan_char_gen.exe --project C:\MyGame --character warden --cli --gender male --prompt "athletic veteran ranger, tall" --height 1.84 --ollama-host 127.0.0.1 --ollama-port 11434

:: Optional: Render a verification screenshot PNG directly from the Vulkan studio viewport
build\windows-release\bin\vulkan_char_gen.exe --project C:\MyGame --character warden --screenshot warden_full.png
build\windows-release\bin\vulkan_char_gen.exe --project C:\MyGame --character warden --focus-face --screenshot warden_face.png

:: Step 2: Outfit & materials (non-destructively preserves body & sculpt strokes)
build\windows-release\bin\vulkan_char_outfit.exe --project C:\MyGame --character warden --cli --top trench_coat --bottom cargo_pants --footwear combat_boots --cape

:: Step 3: Voice, temperament & bilingual EN/RU dialogue
build\windows-release\bin\vulkan_char_act.exe --project C:\MyGame --character warden --cli --prompt "calm tactical guard, low voice" --temperament stern --pitch 112

:: Step 4: Compile & verify engine-ready assets inside <project>/assets/
build\windows-release\bin\vcr_char_cooker.exe --input C:\MyGame\characters\warden.vcrchar.json --project C:\MyGame --verify --json
```

If local Ollama (`http://127.0.0.1:11434`) is unreachable or `--offline` is passed, every AI call falls back seamlessly to deterministic bilingual EN+RU rule-based generation.
