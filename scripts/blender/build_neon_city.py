"""Author the modular 'Neon: Night City' kit inside Blender 5.2 via Blender MCP.

Units: metres, Z up in Blender -> exported as Y-up glTF 2.0 with tangents.
Every asset has ground-level origin (Z=0 in Blender -> Y=0 in engine) and
three explicitly authored LOD levels (LOD0, LOD1, LOD2) for high FPS.
"""
from pathlib import Path
import json
import math
import bpy
import numpy as np

ROOT = Path(globals().get('VCR_ROOT', r'C:\GDF\mega_engine'))
OUT = ROOT / 'art/neon_city'
EXPORT = OUT / 'exports'
TEX_DIR = OUT / 'textures'
for directory in (OUT, EXPORT, TEX_DIR):
    directory.mkdir(parents=True, exist_ok=True)

PREFIX = 'NC_'
if bpy.context.object and bpy.context.object.mode != 'OBJECT':
    bpy.ops.object.mode_set(mode='OBJECT')

existing_scene = bpy.data.scenes.get('Neon Night City - Authored Kit')
if existing_scene is not None:
    scene = existing_scene
    for col in list(scene.collection.children):
        for obj in list(col.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        bpy.data.collections.remove(col)
    for obj in list(scene.collection.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
else:
    scene = bpy.data.scenes.new('Neon Night City - Authored Kit')

bpy.context.window.scene = scene
scene.unit_settings.system = 'METRIC'
scene.unit_settings.scale_length = 1.0

materials = {}
assets = {}
manifest = {
    'units': 'metres',
    'export_axes': 'Y up, -Z forward',
    'unlit_materials': [
        'NC_neon_cyan',
        'NC_neon_magenta',
        'NC_neon_amber',
        'NC_neon_lime',
        'NC_window_atlas',
    ],
    'assets': {},
}


def tile_noise(size: int, cells: int, rng: np.random.Generator) -> np.ndarray:
    grid = rng.random((cells, cells), dtype=np.float32)
    coords = np.arange(size, dtype=np.float32) * (cells / float(size))
    i0 = np.floor(coords).astype(np.int32)
    f = coords - i0
    u = f * f * (3.0 - 2.0 * f)
    i1 = (i0 + 1) % cells
    i0 = i0 % cells
    c00 = grid[i0[:, None], i0[None, :]]
    c10 = grid[i0[:, None], i1[None, :]]
    c01 = grid[i1[:, None], i0[None, :]]
    c11 = grid[i1[:, None], i1[None, :]]
    nx0 = c00 * (1.0 - u[None, :]) + c10 * u[None, :]
    nx1 = c01 * (1.0 - u[None, :]) + c11 * u[None, :]
    return nx0 * (1.0 - u[:, None]) + nx1 * u[:, None]


def save_image(name: str, rgb: np.ndarray, non_color: bool = False):
    h, w = rgb.shape[:2]
    pixels = np.ones((h, w, 4), dtype=np.float32)
    pixels[:, :, :3] = np.clip(rgb, 0.0, 1.0)
    img_name = PREFIX + name
    img = bpy.data.images.get(img_name) or bpy.data.images.new(img_name, w, h, alpha=False)
    img.colorspace_settings.name = 'Non-Color' if non_color else 'sRGB'
    if tuple(img.size) != (w, h):
        img.scale(w, h)
    img.pixels.foreach_set(pixels.ravel())
    img.filepath_raw = str(TEX_DIR / f'{name}.png')
    img.file_format = 'PNG'
    img.save()
    return img


def height_to_normal(height: np.ndarray, strength: float) -> np.ndarray:
    gy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    gx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    normal = np.dstack((-gx * strength, -gy * strength, np.ones_like(gx)))
    norm = np.linalg.norm(normal, axis=2, keepdims=True)
    normal = normal / np.maximum(norm, 1.0e-6)
    return normal * 0.5 + 0.5


def register_pbr_material(
    key: str,
    rgb: np.ndarray,
    height: np.ndarray,
    rough: np.ndarray,
    metal: np.ndarray,
    normal_strength: float = 6.0,
    emission_strength: float = 0.0,
):
    color_img = save_image(f'{key}_basecolor', rgb, non_color=False)
    normal_img = save_image(
        f'{key}_normal', height_to_normal(height, normal_strength), non_color=True
    )
    mr_rgb = np.dstack((np.ones_like(rough), np.clip(rough, 0.045, 1.0), np.clip(metal, 0.0, 1.0)))
    mr_img = save_image(f'{key}_metallic_roughness', mr_rgb, non_color=True)

    mat_name = PREFIX + key
    mat = bpy.data.materials.get(mat_name) or bpy.data.materials.new(mat_name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()

    bsdf = nodes.new('ShaderNodeBsdfPrincipled')
    out = nodes.new('ShaderNodeOutputMaterial')
    links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])

    tex_c = nodes.new('ShaderNodeTexImage')
    tex_c.image = color_img
    links.new(tex_c.outputs['Color'], bsdf.inputs['Base Color'])

    if emission_strength > 0.0:
        links.new(tex_c.outputs['Color'], bsdf.inputs['Emission Color'])
        bsdf.inputs['Emission Strength'].default_value = emission_strength

    tex_n = nodes.new('ShaderNodeTexImage')
    tex_n.image = normal_img
    nmap = nodes.new('ShaderNodeNormalMap')
    links.new(tex_n.outputs['Color'], nmap.inputs['Color'])
    links.new(nmap.outputs['Normal'], bsdf.inputs['Normal'])

    tex_mr = nodes.new('ShaderNodeTexImage')
    tex_mr.image = mr_img
    sep = nodes.new('ShaderNodeSeparateColor')
    links.new(tex_mr.outputs['Color'], sep.inputs['Color'])
    links.new(sep.outputs['Green'], bsdf.inputs['Roughness'])
    links.new(sep.outputs['Blue'], bsdf.inputs['Metallic'])

    materials[key] = mat
    return mat


def build_materials():
    # 1. Wet rain-puddled asphalt (1024x1024)
    sz = 1024
    rng = np.random.default_rng(4011)
    yy_a, xx_a = np.mgrid[0:sz, 0:sz].astype(np.float32) / float(sz)
    n_broad = tile_noise(sz, 4, rng)
    n_med = tile_noise(sz, 12, rng)
    n_grain = tile_noise(sz, 96, rng)
    n_micro = rng.random((sz, sz), dtype=np.float32)
    puddle_field = 0.54 * n_broad + 0.34 * n_med + 0.12 * tile_noise(sz, 24, rng)
    t_pud = np.clip((puddle_field - 0.47) * 3.8, 0.0, 1.0)
    puddle = t_pud * t_pud * (3.0 - 2.0 * t_pud)
    t_wet = np.clip((puddle_field - 0.36) * 3.2, 0.0, 1.0)
    wet_rim = t_wet * t_wet * (3.0 - 2.0 * t_wet)

    dry_rgb = (0.052 + 0.024 * n_grain + 0.012 * n_micro)[:, :, None] * np.array(
        [0.90, 0.95, 1.10], dtype=np.float32
    )
    wet_rgb = dry_rgb * (1.0 - 0.34 * wet_rim[:, :, None])
    water_rgb = np.broadcast_to(np.array([0.028, 0.036, 0.054], dtype=np.float32), wet_rgb.shape)
    asphalt_rgb = wet_rgb * (1.0 - puddle[:, :, None]) + water_rgb * puddle[:, :, None]

    # Subtle wind-rippled water film + underlying aggregate so specular streaks stretch smoothly
    ripples = (
        0.5
        + 0.30 * np.sin(yy_a * math.tau * 34.0 + 2.1 * np.sin(xx_a * math.tau * 7.0))
        + 0.20 * np.cos(xx_a * math.tau * 29.0 + 1.7 * np.cos(yy_a * math.tau * 9.0))
    ).astype(np.float32)
    stone_h = (0.52 * n_grain + 0.36 * n_micro) * (1.0 - 0.66 * puddle) + 0.16 * ripples * puddle
    asphalt_rough = (0.34 + 0.12 * n_grain - 0.13 * wet_rim) * (1.0 - puddle) + (
        0.108 + 0.032 * n_med
    ) * puddle
    asphalt_metal = (0.07 + 0.13 * puddle).astype(np.float32)
    register_pbr_material('wet_asphalt', asphalt_rgb, stone_h, asphalt_rough, asphalt_metal, 4.2)

    # 2. Reflective road markings (512x512)
    sz = 512
    rng = np.random.default_rng(4012)
    wear = tile_noise(sz, 18, rng)
    grit = rng.random((sz, sz), dtype=np.float32)
    paint_mask = np.clip((wear - 0.22) * 5.0, 0.25, 1.0)
    mark_rgb = (
        paint_mask[:, :, None] * np.array([0.74, 0.80, 0.88], dtype=np.float32)
        + (1.0 - paint_mask[:, :, None]) * np.array([0.05, 0.055, 0.065], dtype=np.float32)
    )
    mark_h = 0.25 * paint_mask + 0.08 * grit * (1.0 - 0.5 * paint_mask)
    mark_rough = 0.20 + 0.24 * (1.0 - paint_mask) + 0.06 * grit
    register_pbr_material(
        'road_marking', mark_rgb, mark_h, mark_rough, np.full((sz, sz), 0.05, np.float32), 4.0
    )

    # 3. Damp basalt sidewalk pavers (1024x1024)
    sz = 1024
    rng = np.random.default_rng(4013)
    yy, xx = np.mgrid[0:sz, 0:sz].astype(np.float32) / float(sz)
    tiles = 8.0
    fx = np.abs(np.mod(xx * tiles, 1.0) - 0.5) * 2.0
    fy = np.abs(np.mod(yy * tiles, 1.0) - 0.5) * 2.0
    grout = np.clip((np.maximum(fx, fy) - 0.93) / 0.07, 0.0, 1.0)
    tile_id = (
        np.floor(xx * tiles).astype(np.int32) * 17 + np.floor(yy * tiles).astype(np.int32) * 31
    ) % 29
    tile_var = 0.88 + 0.22 * (tile_id.astype(np.float32) / 29.0)
    damp = tile_noise(sz, 6, rng)
    grain = tile_noise(sz, 72, rng)
    paver_rgb = (
        (0.068 * tile_var + 0.016 * grain)[:, :, None]
        * np.array([0.90, 0.95, 1.08], dtype=np.float32)
        * (1.0 - 0.48 * grout[:, :, None])
        * (1.0 - 0.28 * np.clip((damp - 0.48) * 4.0, 0.0, 1.0)[:, :, None])
    )
    paver_h = 0.65 * (1.0 - grout) + 0.12 * grain
    paver_rough = np.clip(0.36 - 0.22 * np.clip((damp - 0.45) * 3.5, 0.0, 1.0) + 0.28 * grout, 0.07, 0.85)
    register_pbr_material(
        'sidewalk_paver', paver_rgb, paver_h, paver_rough, np.full((sz, sz), 0.08, np.float32), 7.5
    )

    # 4. Cyberpunk architectural fluted facade panels (1024x1024)
    sz = 1024
    rng = np.random.default_rng(4014)
    yy, xx = np.mgrid[0:sz, 0:sz].astype(np.float32) / float(sz)
    seam_x = np.clip((np.abs(np.mod(xx * 4.0, 1.0) - 0.5) * 2.0 - 0.965) / 0.035, 0.0, 1.0)
    seam_y = np.clip((np.abs(np.mod(yy * 2.0, 1.0) - 0.5) * 2.0 - 0.965) / 0.035, 0.0, 1.0)
    seams = np.maximum(seam_x, seam_y)
    flutes = 0.5 + 0.5 * np.cos(xx * math.tau * 32.0)
    streaks = tile_noise(sz, 24, rng) * (0.65 + 0.35 * np.sin(xx * math.tau * 11.0))
    base_f = (0.056 + 0.014 * flutes - 0.018 * seams - 0.010 * streaks)[:, :, None] * np.array(
        [0.88, 0.94, 1.12], dtype=np.float32
    )
    fac_h = 0.35 * flutes - 0.85 * seams + 0.05 * tile_noise(sz, 90, rng)
    fac_rough = np.clip(0.34 + 0.14 * streaks + 0.20 * seams, 0.16, 0.78)
    fac_metal = np.clip(0.38 - 0.20 * seams, 0.05, 0.65).astype(np.float32)
    register_pbr_material('facade_panel', base_f, fac_h, fac_rough, fac_metal, 6.5)

    # 5. Brushed gunmetal steel (512x512)
    sz = 512
    rng = np.random.default_rng(4015)
    yy, xx = np.mgrid[0:sz, 0:sz].astype(np.float32) / float(sz)
    brush = tile_noise(sz, 96, rng) * 0.35 + np.roll(rng.random((sz, sz), np.float32), 1, axis=1) * 0.15
    joint = np.clip((np.abs(np.mod(yy * 4.0, 1.0) - 0.5) * 2.0 - 0.96) / 0.04, 0.0, 1.0)
    gm_rgb = (0.145 + 0.035 * brush - 0.06 * joint)[:, :, None] * np.array(
        [0.92, 0.96, 1.08], dtype=np.float32
    )
    gm_h = 0.18 * brush - 0.7 * joint
    gm_rough = np.clip(0.22 + 0.10 * brush + 0.25 * joint, 0.12, 0.65)
    gm_metal = np.full((sz, sz), 0.88, dtype=np.float32)
    register_pbr_material('gunmetal', gm_rgb, gm_h, gm_rough, gm_metal, 5.0)

    # 6. Reflective dark architectural & canopy curtain glass (512x512)
    sz = 512
    yy, xx = np.mgrid[0:sz, 0:sz].astype(np.float32) / float(sz)
    mullion = np.maximum(
        np.clip((np.abs(np.mod(xx * 2.0, 1.0) - 0.5) * 2.0 - 0.96) / 0.04, 0.0, 1.0),
        np.clip((np.abs(np.mod(yy * 2.0, 1.0) - 0.5) * 2.0 - 0.96) / 0.04, 0.0, 1.0),
    )
    glass_rgb = (1.0 - mullion[:, :, None]) * np.array([0.024, 0.042, 0.072], dtype=np.float32) + (
        mullion[:, :, None] * np.array([0.07, 0.08, 0.10], dtype=np.float32)
    )
    glass_h = -0.45 * mullion
    glass_rough = 0.052 * (1.0 - mullion) + 0.34 * mullion
    glass_metal = 0.22 * (1.0 - mullion) + 0.78 * mullion
    register_pbr_material(
        'glass_curtain',
        glass_rgb.astype(np.float32),
        glass_h.astype(np.float32),
        glass_rough.astype(np.float32),
        glass_metal.astype(np.float32),
        3.5,
    )

    # 7. Metallic silver-cobalt cyber-coupe clearcoat (512x512)
    sz = 512
    rng = np.random.default_rng(4017)
    flake = rng.random((sz, sz), dtype=np.float32) * 0.02
    paint_rgb = np.array([0.185, 0.255, 0.415], dtype=np.float32)[None, None, :] + flake[:, :, None]
    paint_h = flake * 0.25
    paint_rough = np.full((sz, sz), 0.175, dtype=np.float32)
    paint_metal = np.full((sz, sz), 0.80, dtype=np.float32)
    register_pbr_material('cyber_paint', paint_rgb, paint_h, paint_rough, paint_metal, 1.5)

    # 8..11. Dual-Layout Katakana Cyberpunk Neon Sign Atlas (1024x1024, unlit in engine)
    #   - Top band    U[0.02..0.98], V[0.54..0.96]: Wide horizontal storefront/billboard banner
    #   - Bottom-left U[0.04..0.78], V[0.03..0.49]: Tall vertical blade sign / kiosk display
    #   - Bottom-right U[0.85..0.99], V[0.03..0.49]: Solid pure primary neon tube swatch
    GLYPHS = {
        'NE': [((0.48, 0.90), (0.54, 0.77)), ((0.18, 0.74), (0.80, 0.74)), ((0.78, 0.74), (0.16, 0.24)), ((0.50, 0.48), (0.50, 0.10)), ((0.58, 0.44), (0.84, 0.24))],
        'O':  [((0.15, 0.68), (0.85, 0.68)), ((0.58, 0.90), (0.58, 0.14)), ((0.58, 0.14), (0.40, 0.22)), ((0.56, 0.66), (0.16, 0.22))],
        'N':  [((0.18, 0.78), (0.40, 0.62)), ((0.20, 0.18), (0.84, 0.76))],
        'SA': [((0.12, 0.66), (0.88, 0.66)), ((0.34, 0.88), (0.34, 0.42)), ((0.68, 0.90), (0.68, 0.36)), ((0.68, 0.36), (0.42, 0.12))],
        'I':  [((0.78, 0.88), (0.16, 0.42)), ((0.52, 0.58), (0.52, 0.10))],
        'BA': [((0.34, 0.78), (0.14, 0.20)), ((0.64, 0.78), (0.86, 0.20)), ((0.76, 0.88), (0.82, 0.76)), ((0.86, 0.86), (0.92, 0.74))],
        'NA': [((0.14, 0.68), (0.86, 0.68)), ((0.52, 0.90), (0.52, 0.44)), ((0.52, 0.44), (0.28, 0.12))],
        'TO': [((0.36, 0.90), (0.36, 0.10)), ((0.38, 0.56), (0.78, 0.36))],
        'SHI':[((0.18, 0.82), (0.38, 0.72)), ((0.14, 0.58), (0.34, 0.48)), ((0.22, 0.16), (0.84, 0.74))],
        'TE': [((0.24, 0.86), (0.76, 0.86)), ((0.12, 0.60), (0.88, 0.60)), ((0.52, 0.60), (0.52, 0.34)), ((0.52, 0.34), (0.30, 0.12))],
        'CH': [((0.22, 0.78), (0.54, 0.50)), ((0.54, 0.50), (0.22, 0.22)), ((0.50, 0.78), (0.82, 0.50)), ((0.82, 0.50), (0.50, 0.22))],
        'BAR':[((0.16, 0.50), (0.84, 0.50))],
    }

    def add_seg_glow(field_p, field_c, xx, yy, x0, y0, x1, y1, r_core, r_halo):
        dx = x1 - x0
        dy = y1 - y0
        denom = max(dx * dx + dy * dy, 1.0e-8)
        t = np.clip(((xx - x0) * dx + (yy - y0) * dy) / denom, 0.0, 1.0)
        dist2 = (xx - (x0 + t * dx)) ** 2 + (yy - (y0 + t * dy)) ** 2
        field_c += np.exp(-dist2 / (r_core * r_core))
        field_p += np.exp(-dist2 / (r_halo * r_halo))

    def draw_rect_tube(field_p, field_c, xx, yy, x0, y0, x1, y1, r_core, r_halo):
        add_seg_glow(field_p, field_c, xx, yy, x0, y0, x1, y0, r_core, r_halo)
        add_seg_glow(field_p, field_c, xx, yy, x1, y0, x1, y1, r_core, r_halo)
        add_seg_glow(field_p, field_c, xx, yy, x1, y1, x0, y1, r_core, r_halo)
        add_seg_glow(field_p, field_c, xx, yy, x0, y1, x0, y0, r_core, r_halo)

    def build_neon_sign(key: str, primary: tuple, accent: tuple, h_word: list, v_word: list):
        s = 1024
        yy_s, xx_s = np.mgrid[0:s, 0:s].astype(np.float32) / float(s)
        p_col = np.array(primary, dtype=np.float32)
        a_col = np.array(accent, dtype=np.float32)
        white = np.array([1.0, 0.97, 0.99], dtype=np.float32)

        scan = 0.88 + 0.12 * np.sin(yy_s * math.tau * 160.0)
        canvas = (0.032 * p_col + np.array([0.009, 0.011, 0.020], dtype=np.float32))[None, None, :] * scan[:, :, None]

        p_halo = np.zeros((s, s), dtype=np.float32)
        p_core = np.zeros((s, s), dtype=np.float32)
        a_halo = np.zeros((s, s), dtype=np.float32)
        a_core = np.zeros((s, s), dtype=np.float32)

        # 1. Wide Horizontal Banner Region: U[0.02..0.98], V[0.54..0.96]
        draw_rect_tube(p_halo, p_core, xx_s, yy_s, 0.035, 0.565, 0.965, 0.935, 0.0055, 0.020)
        draw_rect_tube(a_halo, a_core, xx_s, yy_s, 0.055, 0.590, 0.945, 0.910, 0.0038, 0.013)
        n_h = len(h_word)
        for idx_g, g_name in enumerate(h_word):
            gx0 = 0.085 + idx_g * (0.83 / n_h)
            gw = (0.83 / n_h) * 0.82
            gy0, gh = 0.625, 0.250
            is_acc = (idx_g == 0 or idx_g == n_h - 1)
            th, tc = (a_halo, a_core) if is_acc else (p_halo, p_core)
            for (sx0, sy0), (sx1, sy1) in GLYPHS[g_name]:
                add_seg_glow(
                    th, tc, xx_s, yy_s,
                    gx0 + sx0 * gw, gy0 + sy0 * gh,
                    gx0 + sx1 * gw, gy0 + sy1 * gh,
                    0.0052, 0.017,
                )

        # 2. Vertical Blade Sign Region: U[0.04..0.78], V[0.03..0.49]
        draw_rect_tube(p_halo, p_core, xx_s, yy_s, 0.065, 0.045, 0.755, 0.475, 0.0060, 0.020)
        draw_rect_tube(a_halo, a_core, xx_s, yy_s, 0.100, 0.062, 0.720, 0.458, 0.0040, 0.013)
        n_v = len(v_word)
        for idx_g, g_name in enumerate(v_word):
            # Stack top-to-bottom within V[0.08..0.44]
            gy0 = 0.435 - (idx_g + 1) * (0.35 / n_v)
            gh = (0.35 / n_v) * 0.82
            gx0, gw = 0.18, 0.46
            is_acc = (idx_g == n_v - 1)
            th, tc = (a_halo, a_core) if is_acc else (p_halo, p_core)
            for (sx0, sy0), (sx1, sy1) in GLYPHS[g_name]:
                add_seg_glow(
                    th, tc, xx_s, yy_s,
                    gx0 + sx0 * gw, gy0 + sy0 * gh,
                    gx0 + sx1 * gw, gy0 + sy1 * gh,
                    0.0055, 0.018,
                )

        canvas = (
            canvas
            + p_col[None, None, :] * np.clip(p_halo * 0.62 + p_core * 0.85, 0.0, 1.35)[:, :, None]
            + a_col[None, None, :] * np.clip(a_halo * 0.55 + a_core * 0.82, 0.0, 1.30)[:, :, None]
            + white[None, None, :] * (0.42 * np.clip(p_core + a_core, 0.0, 1.0))[:, :, None]
        )

        # 3. Dedicated Pure Solid Neon Tube Swatch: U[0.84..1.00], V[0.00..0.50]
        swatch = (xx_s >= 0.84) & (yy_s <= 0.50)
        solid_col = np.clip(p_col * 0.96 + white * 0.14, 0.0, 1.0)
        canvas[swatch] = solid_col

        canvas = np.clip(canvas, 0.0, 1.0)
        register_pbr_material(
            key,
            canvas,
            np.zeros((s, s), dtype=np.float32),
            np.full((s, s), 0.16, dtype=np.float32),
            np.zeros((s, s), dtype=np.float32),
            1.0,
            emission_strength=3.5,
        )

    build_neon_sign('neon_cyan', (0.04, 0.94, 1.00), (1.00, 0.14, 0.66), ['CH', 'SA', 'I', 'BA', 'BAR', 'NE', 'O', 'N'], ['SA', 'I', 'BA', 'CH'])
    build_neon_sign('neon_magenta', (1.00, 0.08, 0.60), (0.06, 0.94, 1.00), ['CH', 'NA', 'I', 'TO', 'BAR', 'SHI', 'TE', 'I'], ['NE', 'O', 'N', 'CH'])
    build_neon_sign('neon_amber', (1.00, 0.46, 0.05), (1.00, 0.12, 0.10), ['SA', 'I', 'BA', 'BAR', 'NA', 'I', 'TO', 'CH'], ['NA', 'I', 'TO', 'SHI'])
    build_neon_sign('neon_lime', (0.10, 1.00, 0.48), (0.06, 0.92, 1.00), ['TE', 'O', 'N', 'BAR', 'SA', 'I', 'BA', 'CH'], ['TE', 'SHI', 'O', 'N'])

    # 12. High-rise inhabited night window atlas (1024x1024, unlit in engine)
    sz = 1024
    rng = np.random.default_rng(5200)
    cols, rows = 16, 12
    yy, xx = np.mgrid[0:sz, 0:sz].astype(np.float32) / float(sz)
    col_idx = np.minimum((xx * cols).astype(np.int32), cols - 1)
    row_idx = np.minimum((yy * rows).astype(np.int32), rows - 1)
    lx = np.mod(xx * cols, 1.0)
    ly = np.mod(yy * rows, 1.0)

    # Outer structural mullion + inner fine casement sub-mullions (center vertical + upper transom)
    sub_mullion = (np.abs(lx - 0.50) < 0.022) | (np.abs(ly - 0.66) < 0.024)
    pane = (lx > 0.09) & (lx < 0.91) & (ly > 0.12) & (ly < 0.89) & (~sub_mullion)
    win_rgb = np.broadcast_to(
        np.array([0.009, 0.012, 0.022], dtype=np.float32), (sz, sz, 3)
    ).copy()

    palette = [
        (0.014, 0.020, 0.036),  # dark night glass
        (0.96, 0.64, 0.28),     # warm tungsten office/apartment
        (0.16, 0.86, 1.00),     # cool cyan monitor room
        (0.94, 0.18, 0.64),     # neon magenta studio
        (0.66, 0.84, 1.00),     # crisp ice-blue fluorescent floor
    ]
    probs = [0.46, 0.26, 0.16, 0.06, 0.06]
    cell_choice = rng.choice(len(palette), size=(rows, cols), p=probs)
    cell_bright = rng.uniform(0.52, 0.96, size=(rows, cols)).astype(np.float32)
    cell_blinds = rng.uniform(0.0, 0.68, size=(rows, cols)).astype(np.float32)

    chosen_rgb = np.zeros((sz, sz, 3), dtype=np.float32)
    for idx_p, col_val in enumerate(palette):
        mask_p = cell_choice[row_idx, col_idx] == idx_p
        chosen_rgb[mask_p] = np.array(col_val, dtype=np.float32)

    bright = cell_bright[row_idx, col_idx]
    blind_drop = cell_blinds[row_idx, col_idx]
    vert_grad = (0.45 + 0.55 * ly) * (0.82 + 0.18 * np.sin(lx * math.pi))
    slats = 0.72 + 0.28 * np.sin(ly * math.tau * 16.0)
    blind_mask = np.where(ly > (1.0 - blind_drop), slats * 0.50, 1.0)
    desk_shadow = np.where((ly < 0.26) & (lx > 0.22) & (lx < 0.78), 0.36, 1.0)

    lit = (cell_choice[row_idx, col_idx] > 0).astype(np.float32)
    interior = chosen_rgb * (
        (1.0 - lit)[:, :, None]
        + (lit * bright * vert_grad * blind_mask * desk_shadow)[:, :, None]
    )
    win_rgb[pane] = interior[pane]

    register_pbr_material(
        'window_atlas',
        win_rgb,
        np.where(pane, 0.0, 0.4).astype(np.float32),
        np.where(pane, 0.08, 0.45).astype(np.float32),
        np.full((sz, sz), 0.15, dtype=np.float32),
        3.0,
        emission_strength=2.2,
    )


build_materials()

current = None
parts = []
level = 0


def link_part(obj, name: str, mat_key: str):
    obj.name = f'{PREFIX}{name}'
    for col in list(obj.users_collection):
        col.objects.unlink(obj)
    current.objects.link(obj)
    obj.data.materials.clear()
    obj.data.materials.append(materials[mat_key])
    parts.append(obj)
    return obj


def box(name: str, loc, scale, mat_key: str, bevel: float = 0.015, rot=None):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=loc)
    obj = link_part(bpy.context.object, name, mat_key)
    obj.scale = scale
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if rot is not None:
        obj.rotation_euler = rot
    if bevel > 0.0 and level == 0:
        mod = obj.modifiers.new('Bevel', 'BEVEL')
        mod.width = bevel
        mod.segments = 1
        mod.affect = 'EDGES'
        bpy.ops.object.modifier_apply(modifier=mod.name)
    return obj


def cyl(name: str, loc, radius: float, depth: float, mat_key: str, verts: int = 12, rot=None):
    v_cnt = max(6, verts if level == 0 else (max(6, verts // 2) if level == 1 else 6))
    bpy.ops.mesh.primitive_cylinder_add(vertices=v_cnt, radius=radius, depth=depth, location=loc)
    obj = link_part(bpy.context.object, name, mat_key)
    if rot is not None:
        obj.rotation_euler = rot
    return obj


def prism_mesh(name: str, verts, faces, mat_key: str):
    mesh_data = bpy.data.meshes.new(f'{PREFIX}{name}_m')
    mesh_data.from_pydata(verts, [], faces)
    mesh_data.update()
    obj = bpy.data.objects.new(f'{PREFIX}{name}', mesh_data)
    return link_part(obj, name, mat_key)


# ---------------------------------------------------------------------------
# 14 Modular Cyberpunk Assets (Blender Z-up; front faces -Y -> +Z in engine)
# ---------------------------------------------------------------------------

def build_road_straight():
    # 8m wide (X: -4..+4) x 8m long (Y: -4..+4), subtle crown at centre
    box('road_base', (0.0, 0.0, -0.04), (8.0, 8.0, 0.08), 'wet_asphalt', bevel=0.0)
    # Dashed centre line + solid edge lines
    for y in (-3.0, -1.0, 1.0, 3.0):
        box('center_dash', (0.0, y, 0.004), (0.14, 1.15, 0.008), 'road_marking', bevel=0.0)
    for x in (-3.65, 3.65):
        box('edge_line', (x, 0.0, 0.004), (0.12, 7.96, 0.008), 'road_marking', bevel=0.0)
    if level < 2:
        for x in (-3.82, 3.82):
            for y in (-2.0, 2.0):
                box('storm_grate', (x, y, 0.005), (0.30, 0.62, 0.012), 'gunmetal', bevel=0.005)


def build_road_crosswalk():
    box('road_base', (0.0, 0.0, -0.04), (8.0, 8.0, 0.08), 'wet_asphalt', bevel=0.0)
    # Stop bars + 7 zebra crosswalk stripes
    for y in (-2.65, 2.65):
        box('stop_bar', (0.0, y, 0.004), (7.2, 0.28, 0.008), 'road_marking', bevel=0.0)
    for i in range(-3, 4):
        box(f'zebra_{i}', (i * 0.98, 0.0, 0.005), (0.52, 3.8, 0.009), 'road_marking', bevel=0.0)


def build_sidewalk_module():
    # 4.5m wide (X: -2.25..+2.25) x 8m long (Y: -4..+4), street curb on +X side (X=+2.25)
    box('paver_deck', (-0.12, 0.0, 0.09), (4.26, 8.0, 0.18), 'sidewalk_paver', bevel=0.01)
    box('curb_stone', (2.13, 0.0, 0.095), (0.24, 8.0, 0.19), 'gunmetal', bevel=0.015)
    # Recessed glowing cyan safety LED strip along the curb face
    box('curb_led', (2.255, 0.0, 0.13), (0.025, 7.86, 0.035), 'neon_cyan', bevel=0.0)


def build_arcade_building():
    # 8m wide (X: -4..+4), 10m deep (Y: -5..+5, shopfront on -Y), 15.6m tall
    # Ground floor recessed shopfront + side piers
    box('upper_mass', (0.0, 0.2, 9.8), (7.92, 9.6, 11.6), 'facade_panel', bevel=0.03)
    box('ground_core', (0.0, 0.55, 2.0), (7.80, 8.9, 4.0), 'facade_panel', bevel=0.02)
    for x in (-3.65, 0.0, 3.65):
        box('pier', (x, -4.45, 2.0), (0.62, 0.95, 4.0), 'gunmetal', bevel=0.015)

    # Shopfront display windows & glowing arcade/ramen signage on -Y face
    for x in (-1.82, 1.82):
        box('shop_glass', (x, -4.02, 1.75), (2.85, 0.08, 2.55), 'glass_curtain', bevel=0.0)
        box('shop_kickplate', (x, -4.06, 0.26), (2.95, 0.16, 0.52), 'gunmetal', bevel=0.01)

    # Slanted street canopy + glowing amber & magenta fascia signs
    box('canopy_deck', (0.0, -4.65, 3.65), (7.85, 1.35, 0.16), 'gunmetal', bevel=0.015, rot=(math.radians(6.0), 0.0, 0.0))
    box('fascia_sign_amber', (-1.9, -5.28, 3.85), (3.55, 0.12, 0.78), 'neon_amber', bevel=0.01)
    box('fascia_sign_magenta', (1.95, -5.28, 3.85), (3.50, 0.12, 0.78), 'neon_magenta', bevel=0.01)
    box('under_canopy_tube', (0.0, -4.85, 3.50), (7.40, 0.08, 0.06), 'neon_cyan', bevel=0.0)

    # Upper floors inhabited night window bays (-Y front and +/-X sides)
    box('front_windows', (0.0, -4.63, 9.6), (6.95, 0.08, 9.6), 'window_atlas', bevel=0.0)
    for sx in (-3.98, 3.98):
        box('side_windows', (sx, 0.2, 9.6), (0.08, 7.6, 9.4), 'window_atlas', bevel=0.0)

    # Architectural horizontal ledges, balconies, AC units & rooftop crown
    for z in (6.2, 9.6, 13.0):
        box('floor_ledge', (0.0, -4.72, z), (7.96, 0.34, 0.20), 'gunmetal', bevel=0.01)
    if level < 2:
        for z in (6.85, 10.25):
            box('balcony_rail', (-1.9, -4.95, z), (3.3, 0.06, 0.85), 'gunmetal', bevel=0.008)
        for z in (5.4, 8.8, 12.2):
            box('ac_box', (2.85, -4.82, z), (0.92, 0.42, 0.62), 'gunmetal', bevel=0.01)
        box('side_pipe_a', (-3.82, -4.55, 8.2), (0.12, 0.12, 14.2), 'gunmetal', bevel=0.0)

    box('roof_parapet', (0.0, 0.2, 15.75), (8.02, 9.75, 0.36), 'gunmetal', bevel=0.02)
    box('roof_neon_trim', (0.0, -4.72, 15.78), (7.88, 0.08, 0.14), 'neon_magenta', bevel=0.0)
    if level < 2:
        box('roof_hvac', (1.6, 1.2, 16.45), (2.4, 2.8, 1.1), 'gunmetal', bevel=0.02)


def build_corp_plaza_building():
    # 8m wide x 10m deep x 19.8m tall sleek glass-and-titanium corporate cyber block
    box('core_tower', (0.0, 0.3, 10.0), (7.86, 9.4, 19.6), 'facade_panel', bevel=0.03)
    # Reflective glass curtain wall + glowing night office bands behind/flush
    box('curtain_glass_front', (0.0, -4.43, 11.2), (7.10, 0.08, 14.4), 'glass_curtain', bevel=0.0)
    for z in (6.2, 10.0, 13.8, 17.4):
        box('office_band', (0.0, -4.46, z), (6.80, 0.06, 2.25), 'window_atlas', bevel=0.0)
    for sx in (-3.95, 3.95):
        box('side_office_grid', (sx, 0.3, 11.2), (0.08, 7.8, 14.0), 'window_atlas', bevel=0.0)

    # Vertical structural titanium fins along front facade
    fin_xs = (-3.72, -1.86, 0.0, 1.86, 3.72) if level < 2 else (-3.72, 0.0, 3.72)
    for x in fin_xs:
        box('vert_fin', (x, -4.62, 10.0), (0.22, 0.44, 19.6), 'gunmetal', bevel=0.01)

    # Ground-floor colonnade & glowing cyan corporate horizon sign
    box('lobby_glass', (0.0, -4.15, 1.9), (6.90, 0.10, 3.6), 'glass_curtain', bevel=0.0)
    box('corp_horizon_sign', (0.0, -4.90, 4.05), (7.75, 0.18, 0.68), 'neon_cyan', bevel=0.01)
    box('crown_fascia', (0.0, -4.70, 19.45), (7.95, 0.36, 0.55), 'gunmetal', bevel=0.02)
    box('crown_neon_bar', (0.0, -4.92, 19.52), (7.75, 0.08, 0.22), 'neon_cyan', bevel=0.0)
    if level < 2:
        cyl('roof_spire', (-2.2, -2.5, 22.2), 0.09, 4.8, 'gunmetal', 8)
        box('spire_beacon', (-2.2, -2.5, 24.7), (0.22, 0.22, 0.22), 'neon_amber', bevel=0.0)


def build_megatower_a():
    # 16m x 16m x 58m monumental 3-tier cyberpunk skyscraper
    tiers = [
        (0.0, 22.0, 15.8, 11.0),   # z_bottom=0..22, center=11
        (22.0, 42.0, 13.2, 32.0),  # z=22..42
        (42.0, 56.0, 10.4, 49.0),  # z=42..56
    ]
    for idx, (z0, z1, w, zc) in enumerate(tiers):
        h = z1 - z0
        box(f'tier_body_{idx}', (0.0, 0.0, zc), (w, w, h), 'facade_panel', bevel=0.06)
        # Window grids on all 4 faces
        half = w * 0.5 + 0.03
        ww = w * 0.80
        wh = h * 0.84
        for sign in (-1.0, 1.0):
            box(f'win_fb_{idx}', (0.0, sign * half, zc), (ww, 0.06, wh), 'window_atlas', bevel=0.0)
            box(f'win_lr_{idx}', (sign * half, 0.0, zc), (0.06, ww, wh), 'window_atlas', bevel=0.0)
        # Glowing vertical corner neon ribs!
        rib_mat = 'neon_cyan' if idx != 1 else 'neon_magenta'
        c_off = w * 0.5 + 0.06
        for sx in (-1.0, 1.0):
            for sy in (-1.0, 1.0):
                box(f'corner_rib_{idx}', (sx * c_off, sy * c_off, zc), (0.18, 0.18, h * 0.94), rib_mat, bevel=0.0)
        # Setback collar ledge
        box(f'tier_collar_{idx}', (0.0, 0.0, z1), (w + 0.45, w + 0.45, 0.45), 'gunmetal', bevel=0.02)

    # Crown neon halo & twin communication spires
    for sy in (-1.0, 1.0):
        box('crown_billboard', (0.0, sy * 5.35, 54.2), (8.6, 0.14, 1.65), 'neon_magenta', bevel=0.0)
    if level < 2:
        for sx in (-2.6, 2.6):
            cyl('mega_spire', (sx, -2.0, 60.5), 0.16, 9.0, 'gunmetal', 8)
            box('mega_beacon', (sx, -2.0, 65.1), (0.38, 0.38, 0.38), 'neon_amber', bevel=0.0)


def build_megatower_b():
    # 14m x 14m x 46m brutalist-cyber high-rise with emerald/cyan neon crown
    box('lower_shaft', (0.0, 0.0, 14.0), (13.6, 13.6, 28.0), 'facade_panel', bevel=0.05)
    box('upper_cantilever', (0.8, -0.8, 36.5), (12.2, 12.6, 17.0), 'facade_panel', bevel=0.05)
    for sign in (-1.0, 1.0):
        box('low_win_y', (0.0, sign * 6.84, 14.5), (10.8, 0.06, 23.5), 'window_atlas', bevel=0.0)
        box('low_win_x', (sign * 6.84, 0.0, 14.5), (0.06, 10.8, 23.5), 'window_atlas', bevel=0.0)
        box('hi_win_y', (0.8, -0.8 + sign * 6.34, 36.5), (9.8, 0.06, 13.8), 'window_atlas', bevel=0.0)
        box('hi_win_x', (0.8 + sign * 6.14, -0.8, 36.5), (0.06, 9.8, 13.8), 'window_atlas', bevel=0.0)

    # Glowing emerald-lime & cyan architectural bands
    for z in (28.1, 44.8):
        box('lime_band_y', (0.8, -7.18, z), (11.8, 0.14, 0.42), 'neon_lime', bevel=0.0)
        box('lime_band_y2', (0.8, 5.58, z), (11.8, 0.14, 0.42), 'neon_lime', bevel=0.0)
    box('vert_cyan_spine', (-5.2, -7.18, 22.0), (0.32, 0.16, 41.0), 'neon_cyan', bevel=0.0)


def build_skybridge_overpass():
    # Spans across X: -8.5..+8.5 (17m span), deck at Z=6.6..9.6, piers at X=+/-6.8
    for sx in (-6.8, 6.8):
        box('pier_col', (sx, 0.0, 3.3), (1.15, 3.2, 6.6), 'facade_panel', bevel=0.02)
        box('pier_rib', (sx - math.copysign(0.60, sx), 0.0, 3.2), (0.08, 0.22, 5.6), 'neon_cyan', bevel=0.0)

    # Main bridge deck, roof, and glazed pedestrian gallery
    box('deck_girder', (0.0, 0.0, 6.55), (17.6, 4.2, 0.55), 'gunmetal', bevel=0.02)
    box('roof_girder', (0.0, 0.0, 9.55), (17.6, 4.2, 0.45), 'gunmetal', bevel=0.02)
    for sy in (-1.92, 1.92):
        box('gallery_glass', (0.0, sy, 8.05), (15.4, 0.08, 2.45), 'glass_curtain', bevel=0.0)
    box('walkway_glow', (0.0, 0.0, 9.25), (14.8, 0.35, 0.06), 'neon_cyan', bevel=0.0)

    # Steel truss verticals + panoramic front (-Y) and rear (+Y) neon billboards!
    truss_xs = (-5.0, -2.5, 0.0, 2.5, 5.0) if level < 2 else (-4.0, 0.0, 4.0)
    for x in truss_xs:
        for sy in (-2.05, 2.05):
            box('truss_post', (x, sy, 8.05), (0.18, 0.18, 2.55), 'gunmetal', bevel=0.008)

    # Twin panoramic neon billboards facing the camera (-Y) and down-street (+Y)
    for sy in (-2.24, 2.24):
        box('billboard_backing_l', (-3.1, sy, 8.05), (5.4, 0.14, 1.95), 'facade_panel', bevel=0.01)
        box('billboard_face_mag', (-3.1, sy + math.copysign(0.09, sy), 8.05), (5.15, 0.05, 1.72), 'neon_magenta', bevel=0.0)
        box('billboard_backing_r', (3.1, sy, 8.05), (5.4, 0.14, 1.95), 'facade_panel', bevel=0.01)
        box('billboard_face_cya', (3.1, sy + math.copysign(0.09, sy), 8.05), (5.15, 0.05, 1.72), 'neon_cyan', bevel=0.0)

    # Underside sodium/amber road light pods
    for x in (-3.2, 0.0, 3.2):
        box('under_pod', (x, 0.0, 6.23), (0.85, 1.6, 0.08), 'neon_amber', bevel=0.0)


def build_neon_sign_vertical(main_mat: str, accent_mat: str, height_m: float):
    # Origin at mounting base (X=0 wall side, extends out toward +X into street by 1.45m; double-sided on +/-Y)
    z_mid = height_m * 0.5
    box('sign_cabinet', (0.92, 0.0, z_mid), (1.18, 0.18, height_m), 'facade_panel', bevel=0.015)
    for sy in (-0.105, 0.105):
        box('sign_face', (0.92, sy, z_mid), (1.06, 0.03, height_m - 0.16), main_mat, bevel=0.0)
    # Outer accent neon bar along outer edge
    box('outer_neon_tube', (1.55, 0.0, z_mid), (0.05, 0.08, height_m - 0.24), accent_mat, bevel=0.0)
    # Wall mounting brackets toward X=0
    for z in (height_m * 0.2, height_m * 0.8):
        box('wall_strut', (0.35, 0.0, z), (0.72, 0.10, 0.10), 'gunmetal', bevel=0.008)


def build_street_lamp_cyber():
    # Sleek 6.4m cantilevered streetlight: base at (0,0,0), road arm reaches +X (2.1m over wet asphalt)
    cyl('base_plinth', (0.0, 0.0, 0.45), 0.18, 0.90, 'gunmetal', 10)
    cyl('mast', (0.0, 0.0, 3.35), 0.095, 5.50, 'gunmetal', 10)
    # Cantilevered upper arm toward +X
    box('road_arm', (1.05, 0.0, 6.18), (2.25, 0.14, 0.14), 'gunmetal', bevel=0.01, rot=(0.0, math.radians(-8.0), 0.0))
    box('road_luminaire', (1.95, 0.0, 6.26), (0.92, 0.28, 0.10), 'gunmetal', bevel=0.01)
    box('road_led_emitter', (1.95, 0.0, 6.19), (0.82, 0.20, 0.04), 'neon_cyan', bevel=0.0)
    # Lower pedestrian sidewalk lamp toward -X
    box('walk_arm', (-0.42, 0.0, 3.85), (0.85, 0.10, 0.10), 'gunmetal', bevel=0.008)
    box('walk_emitter', (-0.68, 0.0, 3.78), (0.38, 0.16, 0.04), 'neon_amber', bevel=0.0)
    box('mast_accent_strip', (0.10, 0.0, 2.20), (0.03, 0.05, 1.80), 'neon_cyan', bevel=0.0)


def build_vending_kiosk():
    # 1.25m wide x 0.85m deep x 2.15m tall cyber vending machine (front faces -Y)
    box('cabinet', (0.0, 0.0, 1.075), (1.24, 0.84, 2.15), 'facade_panel', bevel=0.02)
    box('trim_frame', (0.0, -0.41, 1.08), (1.18, 0.06, 2.08), 'gunmetal', bevel=0.01)
    box('header_marquee', (0.0, -0.445, 1.92), (1.06, 0.03, 0.26), 'neon_magenta', bevel=0.0)
    box('drink_display', (-0.14, -0.44, 1.24), (0.74, 0.03, 0.92), 'neon_lime', bevel=0.0)
    box('touch_panel', (0.38, -0.445, 1.24), (0.20, 0.03, 0.58), 'neon_cyan', bevel=0.0)
    box('pickup_bay', (0.0, -0.43, 0.42), (0.88, 0.05, 0.28), 'gunmetal', bevel=0.008)


def build_cyber_coupe():
    # Sleek wedge cyberpunk sports coupe: 1.92m wide (X), 4.55m long (Y: front at -2.28, rear at +2.28), 1.26m tall
    # Sculpted wedge lower body
    w = 0.94
    body_verts = [
        (-w * 0.90, -2.28, 0.22), (w * 0.90, -2.28, 0.22),   # 0,1 front splitter bottom
        (-w * 0.92, -2.20, 0.56), (w * 0.92, -2.20, 0.56),   # 2,3 front nose crease
        (-w, -0.95, 0.78), (w, -0.95, 0.78),                 # 4,5 cowl / windshield base
        (-w, 1.35, 0.84), (w, 1.35, 0.84),                   # 6,7 rear haunch
        (-w * 0.92, 2.26, 0.78), (w * 0.92, 2.26, 0.78),     # 8,9 rear deck tail
        (-w * 0.88, 2.24, 0.28), (w * 0.88, 2.24, 0.28),     # 10,11 rear diffuser bottom
        (-w, -1.00, 0.20), (w, -1.00, 0.20),                 # 12,13 side sill front
        (-w, 1.30, 0.20), (w, 1.30, 0.20),                   # 14,15 side sill rear
    ]
    body_faces = [
        (0, 1, 3, 2),       # front nose
        (2, 3, 5, 4),       # hood
        (4, 5, 7, 6),       # beltline deck
        (6, 7, 9, 8),       # rear deck
        (8, 9, 11, 10),     # tail panel
        (0, 2, 4, 12), (12, 4, 6, 14), (14, 6, 8, 10),   # left flank
        (1, 13, 5, 3), (13, 15, 7, 5), (15, 11, 9, 7),   # right flank
        (0, 12, 13, 1), (12, 14, 15, 13), (14, 10, 11, 15),  # underbody
    ]
    prism_mesh('coupe_body', body_verts, body_faces, 'cyber_paint')

    # Wrap-around raked glass cockpit canopy
    cw = 0.78
    canopy_verts = [
        (-cw, -0.92, 0.78), (cw, -0.92, 0.78),
        (-cw * 0.82, -0.22, 1.24), (cw * 0.82, -0.22, 1.24),
        (-cw * 0.80, 0.72, 1.22), (cw * 0.80, 0.72, 1.22),
        (-cw * 0.92, 1.52, 0.83), (cw * 0.92, 1.52, 0.83),
    ]
    canopy_faces = [
        (0, 1, 3, 2),  # windshield
        (2, 3, 5, 4),  # roof glass
        (4, 5, 7, 6),  # fastback rear window
        (0, 2, 4, 6),  # left side glass
        (1, 7, 5, 3),  # right side glass
    ]
    prism_mesh('coupe_canopy', canopy_verts, canopy_faces, 'glass_curtain')

    # Full-width glowing front cyan laser lightbar, side-sill LEDs & rear amber-crimson tailbar!
    box('front_lightbar', (0.0, -2.23, 0.54), (1.66, 0.06, 0.065), 'neon_cyan', bevel=0.0)
    box('rear_lightbar', (0.0, 2.27, 0.72), (1.68, 0.06, 0.085), 'neon_amber', bevel=0.0)
    box('front_splitter', (0.0, -2.27, 0.19), (1.74, 0.14, 0.05), 'gunmetal', bevel=0.005)
    box('rear_diffuser', (0.0, 2.22, 0.25), (1.64, 0.16, 0.12), 'gunmetal', bevel=0.008)
    for sx in (-0.955, 0.955):
        box('sill_led', (sx, 0.12, 0.23), (0.025, 1.96, 0.03), 'neon_cyan', bevel=0.0)
    for sx in (-0.64, 0.64):
        box('roof_rail', (sx, 0.25, 1.235), (0.045, 0.96, 0.025), 'gunmetal', bevel=0.004)

    # 4 wheels + alloy rims
    for wy in (-1.38, 1.38):
        for wx in (-0.88, 0.88):
            cyl('tire', (wx, wy, 0.34), 0.34, 0.24, 'facade_panel', 16, rot=(0.0, math.radians(90.0), 0.0))
            cyl(
                'rim',
                (wx + math.copysign(0.11, wx), wy, 0.34),
                0.23,
                0.05,
                'gunmetal',
                12,
                rot=(0.0, math.radians(90.0), 0.0),
            )
            if level == 0:
                box(
                    'rim_glow',
                    (wx + math.copysign(0.138, wx), wy, 0.34),
                    (0.015, 0.30, 0.03),
                    'neon_cyan',
                    bevel=0.0,
                )


def build_neon_bollard():
    cyl('bollard_post', (0.0, 0.0, 0.48), 0.11, 0.96, 'gunmetal', 12)
    cyl('bollard_ring', (0.0, 0.0, 0.82), 0.116, 0.09, 'neon_cyan', 12)
    cyl('bollard_cap', (0.0, 0.0, 0.98), 0.115, 0.06, 'facade_panel', 12)


BUILDERS = {
    'road_straight': build_road_straight,
    'road_crosswalk': build_road_crosswalk,
    'sidewalk_module': build_sidewalk_module,
    'arcade_building': build_arcade_building,
    'corp_plaza_building': build_corp_plaza_building,
    'megatower_a': build_megatower_a,
    'megatower_b': build_megatower_b,
    'skybridge_overpass': build_skybridge_overpass,
    'neon_sign_magenta': lambda: build_neon_sign_vertical('neon_magenta', 'neon_cyan', 4.8),
    'neon_sign_cyan': lambda: build_neon_sign_vertical('neon_cyan', 'neon_lime', 4.2),
    'street_lamp_cyber': build_street_lamp_cyber,
    'vending_kiosk': build_vending_kiosk,
    'cyber_coupe': build_cyber_coupe,
    'neon_bollard': build_neon_bollard,
}


def finalize_mesh(key: str):
    for sel_o in list(bpy.context.selected_objects):
        sel_o.select_set(False)
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    if len(parts) > 1:
        bpy.ops.object.join()
    obj = bpy.context.object
    obj.name = PREFIX + key
    obj.data.name = PREFIX + key + '_mesh'

    scene.cursor.location = (0.0, 0.0, 0.0)
    bpy.ops.object.origin_set(type='ORIGIN_CURSOR')
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    mesh_d = obj.data
    uv_layer = mesh_d.uv_layers.new(name='UVMap') if not mesh_d.uv_layers else mesh_d.uv_layers[0]

    # Assign UVs on clean untriangulated quads first:
    # - Wide horizontal signs -> top banner region U[0.02..0.98], V[0.54..0.96]
    # - Vertical blade signs  -> lower-left region U[0.04..0.78], V[0.03..0.49]
    # - Thin neon LED strips  -> pure solid swatch (0.92, 0.25)
    sign_mats = {
        PREFIX + 'neon_cyan',
        PREFIX + 'neon_magenta',
        PREFIX + 'neon_amber',
        PREFIX + 'neon_lime',
    }

    for poly in mesh_d.polygons:
        n = poly.normal
        dom = max(range(3), key=lambda i: abs(n[i]))
        axes = ((1, 2), (0, 2), (0, 1))[dom]
        mname = mesh_d.materials[poly.material_index].name

        if mname in sign_mats:
            coords_2d = [
                (
                    mesh_d.vertices[mesh_d.loops[li].vertex_index].co[axes[0]],
                    mesh_d.vertices[mesh_d.loops[li].vertex_index].co[axes[1]],
                )
                for li in poly.loop_indices
            ]
            u_min = min(c[0] for c in coords_2d)
            u_max = max(c[0] for c in coords_2d)
            v_min = min(c[1] for c in coords_2d)
            v_max = max(c[1] for c in coords_2d)
            du = u_max - u_min
            dv = v_max - v_min
            if du > 0.22 and dv > 0.22:
                if du >= dv * 1.30:
                    u0, u1, v0, v1 = 0.02, 0.98, 0.54, 0.96
                else:
                    u0, u1, v0, v1 = 0.04, 0.78, 0.03, 0.49
                for li, (cu, cv) in zip(poly.loop_indices, coords_2d):
                    nu = (cu - u_min) / du
                    nv = (cv - v_min) / dv
                    uv_layer.data[li].uv = (u0 + nu * (u1 - u0), v0 + nv * (v1 - v0))
                continue
            for li in poly.loop_indices:
                uv_layer.data[li].uv = (0.92, 0.25)
            continue

        if mname == PREFIX + 'window_atlas':
            scale_u, scale_v = 13.5, 15.0
        elif 'wet_asphalt' in mname or 'sidewalk_paver' in mname:
            scale_u = scale_v = 8.0
        elif 'facade_panel' in mname:
            scale_u = scale_v = 4.0
        else:
            scale_u = scale_v = 2.0

        for li in poly.loop_indices:
            co = mesh_d.vertices[mesh_d.loops[li].vertex_index].co
            uv_layer.data[li].uv = (co[axes[0]] / scale_u + 0.5, co[axes[1]] / scale_v + 0.5)

    tri = obj.modifiers.new('Triangulate', 'TRIANGULATE')
    bpy.ops.object.modifier_apply(modifier=tri.name)

    mesh_d = obj.data
    mesh_d.calc_loop_triangles()
    return obj, len(mesh_d.loop_triangles)


def build_asset(name: str):
    global current, parts, level
    entries = []
    for level in range(3):
        suffix = '' if level == 0 else f'_lod{level}'
        key = name + suffix
        current = bpy.data.collections.new(PREFIX + key)
        scene.collection.children.link(current)
        parts = []
        BUILDERS[name]()
        obj, triangles = finalize_mesh(key)
        bpy.ops.export_scene.gltf(
            filepath=str(EXPORT / f'{key}.glb'),
            export_format='GLB',
            use_selection=True,
            export_yup=True,
            export_animations=False,
            export_tangents=True,
            export_materials='EXPORT',
        )
        assets[key] = obj
        entries.append(
            {
                'level': level,
                'file': f'{key}.glb',
                'triangles': triangles,
                'materials': len({m.name for m in obj.data.materials}),
            }
        )
        obj.hide_render = True
        obj.hide_set(True)
    manifest['assets'][name] = entries
    (OUT / 'asset_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return entries


result = {'scene': scene.name, 'materials': len(materials), 'ready_assets': list(BUILDERS)}
