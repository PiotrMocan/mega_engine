"""Assemble 'Neon: Night City' both in Blender 5.2 and as a mega_engine scene JSON."""
import hashlib
import json
import math
import uuid
import bpy
from mathutils import Vector

ns = bpy.app.driver_namespace['neon_city_builder']
scene = ns['scene']
root = ns['ROOT']
out = ns['OUT']
bpy.context.window.scene = scene

previous = bpy.data.collections.get('NC_Level')
if previous is not None:
    for obj in list(previous.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.collections.remove(previous)

layout = bpy.data.collections.new('NC_Level')
scene.collection.children.link(layout)

nodes = []


def make_uuid(index: int, name: str) -> str:
    digest = bytearray(hashlib.sha256(f'neon_night_city:{index}:{name}'.encode('utf-8')).digest()[:16])
    digest[6] = (digest[6] & 0x0F) | 0x40
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(digest)))


def add_node(
    name: str,
    kind: str,
    parent=None,
    pos=(0.0, 0.0, 0.0),
    rot=(0.0, 0.0, 0.0),
    scale=(1.0, 1.0, 1.0),
    asset: str | None = None,
    light: dict | None = None,
    expanded: bool = False,
) -> int:
    idx = len(nodes)
    entry = {
        'uuid': make_uuid(idx, name),
        'name': name,
        'kind': kind,
        'parent': parent,
        'visible': True,
        'expanded': expanded,
        'color': [1.0, 1.0, 1.0, 1.0],
        'pbr': {
            'metallic': 0.0,
            'roughness': 0.8,
            'ior': 1.5,
            'model': 'standard',
            'subsurface_profile': 'none',
            'subsurface_strength': 0.0,
            'ambient_occlusion': 1.0,
        },
        'transform': {
            'translation': [round(float(pos[0]), 4), round(float(pos[1]), 4), round(float(pos[2]), 4)],
            'rotation_degrees': [round(float(rot[0]), 4), round(float(rot[1]), 4), round(float(rot[2]), 4)],
            'scale': [round(float(scale[0]), 4), round(float(scale[1]), 4), round(float(scale[2]), 4)],
        },
    }
    if asset is not None:
        entry['asset_path'] = f'neon_city/kit/{asset}.vcrasset.json'
    if light is not None:
        entry['light'] = light
    nodes.append(entry)
    return idx


world = add_node('Neon: Night City', 'folder', expanded=True)
env_folder = add_node('Environment', 'folder', world, expanded=True)
add_node('Night Sky & Stars', 'environment', env_folder)
add_node('Moonlight Key', 'directional_light', env_folder)

groups = {
    name: add_node(name, 'folder', world)
    for name in (
        'Wet Boulevard',
        'Sidewalks & Bollards',
        'Cyberpunk Street Blocks',
        'Skyline Megatowers',
        'Skybridge & Signage',
        'Street Props & Vehicles',
        'Neon Lighting Rig',
    )
}


def place_model(
    asset: str,
    label: str,
    pos,
    yaw_deg: float = 0.0,
    scale=(1.0, 1.0, 1.0),
    group: str = 'Street Props & Vehicles',
    pitch_deg: float = 0.0,
):
    add_node(label, 'model', groups[group], pos, (pitch_deg, yaw_deg, 0.0), scale, asset=asset)
    src = ns['assets'][asset]
    obj = bpy.data.objects.new(f'NC_Lv_{len(nodes)}_{asset}', src.data)
    layout.objects.link(obj)
    obj.location = (pos[0], -pos[2], pos[1])
    obj.rotation_euler = (math.radians(pitch_deg), 0.0, math.radians(yaw_deg))
    obj.scale = (scale[0], scale[2], scale[1])
    return obj


def add_point_light(label: str, pos, color, intensity: float, radius: float):
    add_node(
        label,
        'point_light',
        groups['Neon Lighting Rig'],
        pos,
        light={
            'color': [round(float(c), 4) for c in color],
            'intensity_candela': round(float(intensity), 3),
            'range': round(float(radius), 3),
        },
    )


def add_spot_light(
    label: str,
    pos,
    rot,
    color,
    intensity: float,
    radius: float,
    inner_deg: float = 22.0,
    outer_deg: float = 54.0,
):
    add_node(
        label,
        'spot_light',
        groups['Neon Lighting Rig'],
        pos,
        rot=rot,
        light={
            'color': [round(float(c), 4) for c in color],
            'intensity_candela': round(float(intensity), 3),
            'range': round(float(radius), 3),
            'inner_cone_degrees': round(float(inner_deg), 2),
            'outer_cone_degrees': round(float(outer_deg), 2),
        },
    )


# 1. Rain-slicked 88m boulevard + 2 crosswalks + raised sidewalks with cyan LED curbs
road_zs = [28, 20, 12, 4, -4, -12, -20, -28, -36, -44, -52]
for idx_r, z in enumerate(road_zs):
    is_cross = z in (12, -12)
    r_asset = 'road_crosswalk' if is_cross else 'road_straight'
    place_model(r_asset, f'Boulevard {idx_r:02d}', (0.0, 0.0, z), 0.0, group='Wet Boulevard')
    place_model('sidewalk_module', f'Sidewalk West {idx_r:02d}', (-6.25, 0.0, z), 0.0, group='Sidewalks & Bollards')
    place_model('sidewalk_module', f'Sidewalk East {idx_r:02d}', (6.25, 0.0, z), 180.0, group='Sidewalks & Bollards')

# Bollards guarding both crosswalks
for idx_b, z in enumerate((15.2, 8.8, -8.8, -15.2)):
    place_model('neon_bollard', f'Bollard W {idx_b}', (-4.32, 0.18, z), 0.0, group='Sidewalks & Bollards')
    place_model('neon_bollard', f'Bollard E {idx_b}', (4.32, 0.18, z), 0.0, group='Sidewalks & Bollards')

# 2. Urban canyon street-wall blocks (West X=-13.2 facing +X; East X=+13.2 facing -X)
block_zs = [20, 12, 4, -4, -12, -20, -28, -36, -44]
for i, z in enumerate(block_zs):
    west_asset = 'arcade_building' if (i % 2 == 0) else 'corp_plaza_building'
    east_asset = 'corp_plaza_building' if (i % 2 == 0) else 'arcade_building'
    place_model(west_asset, f'West Block {i:02d}', (-13.2, 0.0, z), 90.0, group='Cyberpunk Street Blocks')
    place_model(east_asset, f'East Block {i:02d}', (13.2, 0.0, z), -90.0, group='Cyberpunk Street Blocks')

    # Storefront awning bounce lights onto wet sidewalks & puddles
    if west_asset == 'arcade_building':
        add_point_light(f'Arcade Glow W {i:02d}', (-7.4, 3.2, z - 1.6), (1.0, 0.48, 0.08), 32.0, 12.5)
        add_point_light(f'Arcade Pink W {i:02d}', (-7.4, 3.2, z + 1.6), (1.0, 0.10, 0.62), 34.0, 12.5)
    else:
        add_point_light(f'Corp Cyan W {i:02d}', (-7.5, 3.8, z), (0.08, 0.92, 1.0), 38.0, 14.0)

    if east_asset == 'arcade_building':
        add_point_light(f'Arcade Glow E {i:02d}', (7.4, 3.2, z + 1.6), (1.0, 0.48, 0.08), 32.0, 12.5)
        add_point_light(f'Arcade Pink E {i:02d}', (7.4, 3.2, z - 1.6), (1.0, 0.10, 0.62), 34.0, 12.5)
    else:
        add_point_light(f'Corp Cyan E {i:02d}', (7.5, 3.8, z), (0.08, 0.92, 1.0), 38.0, 14.0)

# 3. Projecting vertical neon blade signs & corresponding saturated wall/puddle lights
sign_specs = [
    (-8.42, 6.2, 16.5, 0.0, 'neon_sign_magenta', (1.0, 0.08, 0.60)),
    (8.42, 6.8, 15.0, 180.0, 'neon_sign_cyan', (0.06, 0.94, 1.00)),
    (-8.42, 7.4, 6.2, 0.0, 'neon_sign_cyan', (0.08, 0.96, 0.88)),
    (8.42, 5.8, 5.0, 180.0, 'neon_sign_magenta', (1.0, 0.10, 0.58)),
    (-8.42, 6.5, -10.5, 0.0, 'neon_sign_magenta', (1.0, 0.08, 0.60)),
    (8.42, 7.6, -11.5, 180.0, 'neon_sign_cyan', (0.06, 0.94, 1.00)),
    (-8.42, 8.2, -22.0, 0.0, 'neon_sign_cyan', (0.12, 1.00, 0.52)),
    (8.42, 6.4, -23.5, 180.0, 'neon_sign_magenta', (1.0, 0.10, 0.58)),
    (-8.42, 7.0, -34.0, 0.0, 'neon_sign_magenta', (1.0, 0.12, 0.62)),
    (8.42, 7.8, -35.0, 180.0, 'neon_sign_cyan', (0.06, 0.94, 1.00)),
]
for idx_s, (sx, sy, sz, syaw, sasset, scol) in enumerate(sign_specs):
    place_model(sasset, f'Vertical Neon Sign {idx_s:02d}', (sx, sy, sz), syaw, group='Skybridge & Signage')
    lx = sx + (1.45 if sx < 0 else -1.45)
    add_point_light(f'Neon Sign Halo {idx_s:02d}', (lx, sy + 2.1, sz), scol, 52.0, 16.0)

# 4. Elevated Skybridge Overpass framing the midground + billboard glow
place_model('skybridge_overpass', 'Midground Skybridge', (0.0, 0.0, -5.5), 0.0, group='Skybridge & Signage')
add_point_light('Skybridge Billboard Magenta', (-3.1, 7.8, -2.8), (1.0, 0.08, 0.60), 85.0, 22.0)
add_point_light('Skybridge Billboard Cyan', (3.1, 7.8, -2.8), (0.06, 0.94, 1.00), 85.0, 22.0)
for ux in (-3.2, 0.0, 3.2):
    add_spot_light(
        f'Skybridge Road Spot {ux:+.1f}',
        (ux, 6.1, -5.5),
        (-90.0, 0.0, 0.0),
        (1.0, 0.56, 0.14),
        68.0,
        14.0,
        25.0,
        58.0,
    )

# 5. Towering Skyline Megatowers (flanking & avenue focal terminus)
megatowers = [
    ('megatower_a', 'Terminus Megatower West', (-8.8, 0.0, -63.0), 12.0),
    ('megatower_b', 'Terminus Megatower East', (9.2, 0.0, -60.0), -18.0),
    ('megatower_a', 'Skyline Tower NW', (-27.0, 0.0, -18.0), 90.0),
    ('megatower_b', 'Skyline Tower NE', (27.5, 0.0, -22.0), -90.0),
    ('megatower_b', 'Skyline Tower Far W', (-26.0, 0.0, -44.0), 0.0),
    ('megatower_a', 'Skyline Tower Far E', (28.0, 0.0, -46.0), 180.0),
]
for m_asset, m_label, m_pos, m_yaw in megatowers:
    place_model(m_asset, m_label, m_pos, m_yaw, group='Skyline Megatowers')

add_point_light('Terminus Crown Glow Cyan', (-8.8, 36.0, -53.0), (0.06, 0.94, 1.00), 220.0, 42.0)
add_point_light('Terminus Crown Glow Lime', (9.2, 30.0, -51.0), (0.14, 1.00, 0.52), 180.0, 38.0)

# 6. Dual-arm Cyber Streetlamps + crisp downward road spotlights & wet-boulevard sheen fill
lamp_zs = [25.0, 15.0, 4.5, -6.0, -16.5, -27.0, -38.5, -50.0]
for idx_l, lz in enumerate(lamp_zs):
    place_model('street_lamp_cyber', f'Streetlamp W {idx_l:02d}', (-4.42, 0.18, lz), 0.0, group='Street Props & Vehicles')
    place_model('street_lamp_cyber', f'Streetlamp E {idx_l:02d}', (4.42, 0.18, lz), 180.0, group='Street Props & Vehicles')
    add_spot_light(
        f'Road Lamp Spot W {idx_l:02d}',
        (-2.45, 6.22, lz),
        (-90.0, 0.0, 0.0),
        (0.32, 0.90, 1.00),
        78.0,
        17.5,
        26.0,
        64.0,
    )
    add_spot_light(
        f'Road Lamp Spot E {idx_l:02d}',
        (2.45, 6.22, lz),
        (-90.0, 0.0, 0.0),
        (0.32, 0.90, 1.00),
        78.0,
        17.5,
        26.0,
        64.0,
    )

# Soft overhead neon canyon bounce lights so center-lane puddles catch continuous cyan/magenta ribbons
for idx_b2, (bx, by, bz, bcol, bint) in enumerate([
    (0.6, 4.8, 20.2, (0.10, 0.88, 1.00), 46.0),
    (-0.5, 4.8, 9.2, (1.00, 0.14, 0.64), 44.0),
    (0.5, 5.0, -1.2, (0.08, 0.92, 1.00), 46.0),
    (-0.4, 5.2, -19.5, (1.00, 0.16, 0.62), 48.0),
]):
    add_point_light(f'Boulevard Neon Sheen {idx_b2:02d}', (bx, by, bz), bcol, bint, 16.5)

# 7. Sidewalk Vending Kiosk clusters
kiosks = [
    (-7.85, 0.18, 17.6, 90.0),
    (-7.85, 0.18, 16.1, 90.0),
    (7.85, 0.18, 13.8, -90.0),
    (-7.85, 0.18, 1.5, 90.0),
    (7.85, 0.18, -1.8, -90.0),
    (7.85, 0.18, -3.3, -90.0),
    (-7.85, 0.18, -17.5, 90.0),
    (7.85, 0.18, -21.0, -90.0),
]
for idx_k, (kx, ky, kz, kyaw) in enumerate(kiosks):
    place_model('vending_kiosk', f'Vending Kiosk {idx_k:02d}', (kx, ky, kz), kyaw, group='Street Props & Vehicles')
    gx = kx + (0.95 if kx < 0 else -0.95)
    add_point_light(f'Kiosk Glow {idx_k:02d}', (gx, 1.35, kz), (0.14, 1.0, 0.55), 14.0, 6.5)

# 8. Sleek Cyber Coupes cruising along the rain-slicked avenue
# Car 1: Foreground left lane heading down-avenue (-Z), showing glowing crimson-amber tailbar over crosswalk!
place_model('cyber_coupe', 'Hero Cyber Coupe Outbound', (-1.85, 0.01, 14.6), 175.0, group='Street Props & Vehicles')
add_point_light('Hero Coupe Taillight Pool', (-1.95, 0.72, 17.0), (1.0, 0.22, 0.05), 36.0, 9.5)
add_spot_light(
    'Hero Coupe Headlight Beam',
    (-1.65, 0.58, 12.1),
    (-8.0, 0.0, 0.0),
    (0.55, 0.94, 1.0),
    95.0,
    22.0,
    16.0,
    38.0,
)

# Car 2: Mid-foreground right lane oncoming (+Z), cyan laser lightbar reflecting on wet puddles!
place_model('cyber_coupe', 'Oncoming Cyber Coupe', (1.92, 0.01, 2.8), -4.0, group='Street Props & Vehicles')
add_point_light('Oncoming Coupe Cyan Front Glow', (1.85, 0.58, 5.2), (0.08, 0.95, 1.0), 42.0, 11.5)
add_point_light('Oncoming Coupe Tail Glow', (2.0, 0.72, 0.3), (1.0, 0.25, 0.05), 22.0, 7.5)

# Car 3 & 4: Midground avenue traffic beneath & beyond the skybridge
place_model('cyber_coupe', 'Midground Coupe Outbound', (-1.95, 0.01, -15.5), 180.0, group='Street Props & Vehicles')
add_point_light('Midground Coupe Tail Glow', (-1.95, 0.72, -13.0), (1.0, 0.22, 0.05), 28.0, 8.5)
place_model('cyber_coupe', 'Distant Coupe Oncoming', (1.95, 0.01, -28.5), 0.0, group='Street Props & Vehicles')
add_point_light('Distant Coupe Headlight Glow', (1.95, 0.58, -26.0), (0.12, 0.95, 1.0), 34.0, 10.5)

# 9. Hero Startup Camera + Secondary Vista Cameras
hero_cam_pos = (1.45, 2.15, 23.6)
hero_cam_rot = (4.2, 3.8, 0.0)
cam_index = add_node('Hero Street Camera', 'camera', world, hero_cam_pos, hero_cam_rot)
add_node('Skybridge Vista Camera', 'camera', world, (-3.6, 5.2, 16.0), (-4.0, -10.0, 0.0))
add_node('Low Puddle Reflection Camera', 'camera', world, (0.65, 0.68, 19.5), (5.5, 2.0, 0.0))

document = {
    'format': 'vulkan_cube_scene',
    'version': 1,
    'startup_camera': cam_index,
    'tone_mapper': 'gt7',
    'lighting': {
        'dynamic': True,
        'voxel_gi': False,
        'cycle': False,
        'cascade_cache': True,
        'hour': 22.0,
        'day_seconds': 3600.0,
        'gi_extent': 96.0,
        'gi_strength': 1.0,
        'triangle_budget': 131072,
        'gi_update_hz': 30.0,
        'shadow_resolution': 2048,
        'shadow_depth': 'auto',
        'ray_tracing': 'off',
    },
    'fog': {
        'enabled': True,
        'color': [0.028, 0.036, 0.095],
        'density': 0.011,
        'height_falloff': 0.042,
        'base_height': 2.0,
        'start_distance': 14.0,
        'maximum_distance': 160.0,
        'maximum_opacity': 0.72,
    },
    'nodes': nodes,
}

scene_text = (
    '{\n'
    + ''.join(f'  {json.dumps(k)}: {json.dumps(v)},\n' for k, v in document.items() if k != 'nodes')
    + '  "nodes": [\n'
    + ',\n'.join('    ' + json.dumps(n) for n in nodes)
    + '\n  ]\n}\n'
)

for dest in (
    out / 'layout.json',
    root / 'assets/scenes/neon_night_city.json',
    root / 'assets/scenes/editor_scene.json',
):
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(scene_text, encoding='utf-8')

# Set up camera & night world in Blender viewport too so the user sees it live in Blender!
cam_data = bpy.data.cameras.new('NC_HeroCamera')
cam_data.lens = 28.0
cam_obj = bpy.data.objects.new('NC_HeroCamera', cam_data)
layout.objects.link(cam_obj)
cam_obj.location = (hero_cam_pos[0], -hero_cam_pos[2], hero_cam_pos[1])
look_target = Vector((0.0, 8.0, 4.2))
cam_obj.rotation_euler = (look_target - cam_obj.location).to_track_quat('-Z', 'Y').to_euler()
scene.camera = cam_obj

world_data = bpy.data.worlds.get('NC_MidnightWorld') or bpy.data.worlds.new('NC_MidnightWorld')
world_data.use_nodes = True
bg = world_data.node_tree.nodes.get('Background')
if bg is not None:
    bg.inputs[0].default_value = (0.010, 0.015, 0.038, 1.0)
    bg.inputs[1].default_value = 0.35
scene.world = world_data

for area in bpy.context.screen.areas:
    if area.type == 'VIEW_3D':
        space = area.spaces.active
        space.region_3d.view_perspective = 'CAMERA'
        space.shading.type = 'MATERIAL'

bpy.ops.wm.save_as_mainfile(filepath=str(out / 'neon_night_city.blend'))

result = {
    'total_nodes': len(nodes),
    'model_instances': sum(1 for n in nodes if n['kind'] == 'model'),
    'punctual_lights': sum(1 for n in nodes if n['kind'] in ('point_light', 'spot_light')),
    'blend_file': str(out / 'neon_night_city.blend'),
    'scene_file': str(root / 'assets/scenes/neon_night_city.json'),
}
