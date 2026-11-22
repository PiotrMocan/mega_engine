"""Replant an authored Forest Steppe scene into editable, deterministic terrain layers.

Usage: python scripts/replant_forest_steppe.py INPUT_SCENE OUTPUT_SCENE
Reads the meadow tiles and cooks the groundcover mesh and atlas in the asset root.
Use --output-assets to stage the patch in another Forest Steppe project. The output scene must be new.
"""
import argparse
from collections import Counter, defaultdict
from io import BytesIO
import json
import math
import struct
import sys
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.extend(str(ROOT / '.tools' / name / 'python') for name in ('forest-steppe', 'pine-benchmark'))
import numpy as np
from PIL import Image, ImageDraw, ImageFilter


CELLS = 64
SPACING = 2.5
TILE_SIZE = CELLS * SPACING
MASK_SIDE = 128
GROUNDCOVER = 'meadow_environment/steppe_meadow_sward.vcrasset.json'
FAMILIES = (
    ('Wildflowers', ('sm_flower_', 'sm_meadow_daisy')),
    ('Thickets', ('sm_bush_', 'sm_maple_bush', 'sm_grey_willow', 'sm_poplar_plant')),
    ('Grasses', ('grass_', 'sm_grass', 'sm_dead_grass', 'sm_plants_')),
)


def tile_at(x, z):
    return int((x + 640) // TILE_SIZE), int((z + 640) // TILE_SIZE)


def path_x(z):
    return 22 + 40 * np.sin((z + 80) / 190) + 12 * np.sin(z / 75)


def meadow_tiles(nodes, assets):
    tiles = {}
    for node in nodes:
        path = node.get('asset_path', '')
        if not path.startswith('landscapes/level_steppe_meadow/tile_') or not node['visible']:
            continue
        manifest_path = assets / path
        manifest = json.loads(manifest_path.read_bytes())
        data = (manifest_path.parent / manifest['mesh']).read_bytes()
        magic, version, stride, vertices, indices, parts, _ = struct.unpack_from('<8sIIQQII', data)
        if (magic, version, stride, vertices, parts) != (b'VCRMESH\0', 3, 48, 65 * 65, 1):
            raise ValueError(f'Unexpected landscape mesh layout in {path}')
        offset = len(data) - indices * 4 - vertices * stride
        xyz = np.frombuffer(data, '<f4', vertices * 12, offset).reshape(-1, 12)[:, :3]
        origin = xyz.min(axis=0)
        x, z = np.rint((xyz[:, [0, 2]] - origin[[0, 2]]) / SPACING).astype(int).T
        grid = np.full((CELLS + 1, CELLS + 1), np.nan)
        grid[z, x] = xyz[:, 1]
        if not np.isfinite(grid).all():
            raise ValueError(f'Incomplete heightfield in {path}')
        tiles[tile_at(origin[0], origin[2])] = grid
    if len(tiles) != 64:
        raise ValueError('Expected the 64 installed meadow tiles')
    return tiles


def hash32(values):
    values = np.array(values, dtype=np.uint32, copy=True)
    values ^= values >> 16
    values *= np.uint32(0x7feb352d)
    values ^= values >> 15
    values *= np.uint32(0x846ca68b)
    return values ^ (values >> 16)


def unit(keys):
    return (hash32(keys) & 0x00ffffff).astype(np.float32) / 16777216


def candidate_count(seed, mask, target):
    # Stop on the target-th accepted plant, even for isolated small colonies.
    keys = hash32(seed) + np.uint32(5) * np.arange(100000, dtype=np.uint32)
    x = (unit(keys) * MASK_SIDE).astype(int)
    z = (unit(keys + 1) * MASK_SIDE).astype(int)
    indices = np.flatnonzero(unit(keys + 2) < mask[z, x].astype(np.float32) / 255)
    if len(indices) < target:
        raise ValueError('Layer exceeds the candidate budget')
    return int(indices[target - 1]) + 1


def density_mask(plants, left, top, family):
    # Smooth the authored distribution into paintable patches at metre scale.
    points = np.array([node['transform']['translation'] for node in plants])
    density, _, _ = np.histogram2d(points[:, 2], points[:, 0], bins=CELLS,
                                   range=((top, top + TILE_SIZE), (left, left + TILE_SIZE)))
    sigma = 3 if family == 'Thickets' else 1.7
    axis = np.arange(-int(3*sigma), int(3*sigma) + 1)
    kernel = np.exp(-0.5 * (axis / sigma) ** 2)
    for dim in (0, 1):
        density = np.apply_along_axis(lambda row: np.convolve(row, kernel, 'same'), dim, density)
    ceiling = np.percentile(density[density > 0], 92)
    image = Image.fromarray(np.rint(255 * np.clip(density / ceiling, 0, 1)).astype('uint8'))
    mask = np.array(image.resize((MASK_SIDE, MASK_SIDE), Image.Resampling.BILINEAR))
    centres = (np.arange(MASK_SIDE) + 0.5) * TILE_SIZE / MASK_SIDE
    x, z = left + centres[None, :], top + centres[:, None]
    # Leave a margin for the entire texel, including its corners.
    path_clearance = 4.3 if family == 'Thickets' else 2.8
    mask[np.abs(x - path_x(z)) < path_clearance] = 0
    mask[((x - 91) / 15) ** 2 + ((z + 87) / 12) ** 2 < 1] = 0
    return mask


def species_counts(plants):
    counts = Counter(node['asset_path'] for node in plants)
    # Alternate dominant species between tiles; keep multiple variants in dense areas.
    chosen = counts.most_common(min(4, 1 + len(plants) // 160))
    total = sum(n for _, n in chosen)
    cumulative = previous = 0
    for asset, count in chosen:
        cumulative += count
        end = round(len(plants) * cumulative / total)
        yield asset, end - previous
        previous = end


def write_groundcover(assets):
    """Cook several short tufts together using the meadow set's grass atlas."""
    name = 'steppe_meadow_sward'
    material = f'materials/{name}.vcrmaterial.json'
    folder = assets / 'meadow_environment'
    meadow_material = json.loads((folder/'materials/MI_grass_meadow_02.vcrmaterial.json').read_bytes())
    source = json.loads((folder/meadow_material['textures']['base_color']).read_bytes())
    with Image.open(folder/source['source']) as image:
        rgba = np.array(image.convert('RGBA'))
    # Isolate the two photographed tufts from the loose leaves and seed heads on
    # the same atlas before simplifying their card outlines at a distance.
    silhouette = Image.fromarray((rgba[:, :, 3] > 12).astype('uint8')).filter(ImageFilter.MaxFilter(3))
    for u, v in [(0.244, 0.928), (0.724, 0.928)]:
        root = int(u * silhouette.width), int(v * silhouette.height)
        if silhouette.getpixel(root) != 1:
            raise ValueError('The source grass atlas has changed')
        ImageDraw.floodfill(silhouette, root, 2)
    rgba[np.asarray(silhouette) != 2, 3] = 0
    # Bring the close-cropped grass into the site's green grass palette. RGB under
    # the cutout matters too: uncovered black texels otherwise darken filtered blades.
    rgb = np.clip(rgba[:, :, :3] * [1.31, 1.34, 1.17] + [7, 8, 5], 0, 255)
    rgb[rgba[:, :, 3] == 0] = np.median(rgb[rgba[:, :, 3] > 128], axis=0)
    rgba[:, :, :3] = np.rint(rgb).astype('uint8')
    png = BytesIO()
    Image.fromarray(rgba).save(png, format='PNG')
    texture = f'textures/{name}.vcrtexture.json'
    texture_data = dict(schema='vcr.asset', version=1, kind='texture', name=name, decode='deferred',
                        source=f'textures/{name}.png', usage='base_color', color_space='srgb')
    material_data = dict(schema='vcr.asset', version=1, kind='material', name=name, double_sided=False,
                         alpha_mode='mask', alpha_cutoff=0.32, textures=dict(base_color=texture),
                         pbr=dict(base_color=[1,1,1,1], metallic=0, roughness=0.93, ambient_occlusion=1,
                                  model='subsurface', subsurface_profile='foliage', subsurface_strength=0.52))
    # Outline the two large tufts in the lower half of the atlas. The narrower
    # tip avoids sampling the neighbouring plants and limits transparent pixels.
    rows = [(0.988, 0.16, 0.34), (0.805, 0.012, 0.463),
            (0.595, 0.047, 0.44), (0.455, 0.12, 0.393), (0.235, 0.207, 0.315)]
    # x, z, width and height in metres, followed by heading in radians.
    tufts = [(0.0, 0.0, 0.96, 0.85, 0.35), (-0.57, 0.38, 0.83, 0.59, 1.67),
             (0.63, 0.21, 0.89, 0.76, 2.28), (0.23, -0.64, 0.86, 0.68, 0.03),
             (-0.58, -0.43, 0.74, 0.58, 1.11), (-1.05, 0.03, 0.76, 0.46, 0.76),
             (0.77, -0.63, 0.81, 0.51, 2.79)]
    vertices, cards = [], []
    for i, (cx, cz, width, height, heading) in enumerate(tufts):
        for cross in range(2 if i < 5 else 1):
            angle = heading + cross * 1.47
            dx, dz = math.cos(angle), math.sin(angle)
            nx, ny, nz = -0.28 * dz, math.sqrt(1 - 0.28**2), 0.28 * dx
            first = len(vertices)
            bounds = first + 2 * len(rows)
            for v, left, right in (*rows, (0.988, 0.005, 0.492), (0.229, 0.005, 0.492)):
                y = (0.988 - v) / (0.988 - 0.235) * height - 0.055
                for u in (left, right):
                    along = (u - 0.248) / 0.451 * width
                    vertices.append((cx + dx * along, y, cz + dz * along, nx, ny, nz,
                                     u + 0.5 * ((i + cross) % 2), 1 - v, dx, 0, dz, 1))
            cards.append((first, bounds))
    # At distance keep the outer tufts in place, simplifying the card outlines first.
    indices, starts, counts = [], [], []
    for level, active in enumerate((range(12), range(12), (0, 1, 2, 3, 4, 5, 6, 8, 10, 11),
                                    (0, 1, 2, 4, 5, 6, 8, 10, 11), (0, 1, 2, 4, 6, 8, 10, 11),
                                    (0, 2, 4, 6, 8, 10, 11))):
        starts.append(len(indices))
        for card in active:
            first, bounds = cards[card]
            strip = [first + 2*r for r in range(len(rows))] if level == 0 else [bounds, bounds+2]
            for a, b in zip(strip, strip[1:]):
                # Foliage on both sides shares the sky-facing tuft normal.
                indices.extend((a, b, a+1, a+1, b, b+1, a+1, b, a, b+1, b, a+1))
        counts.append(len(indices) - starts[-1])
    errors = (0., 0.006, 0.045, 0.08, 0.14, 0.2)
    mesh = (struct.pack('<8sIIQQII', b'VCRMESH\0', 3, 48, len(vertices), len(indices), 1, 0)
            + struct.pack('<QQII6I6I6f', 0, counts[0], 0, 0, *starts, *counts, *errors)
            + np.asarray(vertices, dtype='<f4').tobytes() + np.asarray(indices, dtype='<u4').tobytes())
    mesh_path = f'meshes/{name}.vcrmesh'
    manifest = dict(schema='vcr.asset', version=1, kind='model', name=name, mesh=mesh_path,
                    detail_aa='coverage', generate_lods=False, prebaked_lods=True, materials=[material],
                    geometry=dict(vertex_count=len(vertices), vertex_stride=48, index_count=len(indices),
                                  index_type='uint32', primitive='triangles'),
                    submeshes=[dict(name=name, first_index=0, index_count=counts[0],
                                    material_index=0, material=material)])
    files = [(folder/texture_data['source'], png.getvalue()),
             (folder/texture, json.dumps(texture_data, indent=2).encode()),
             (folder/material, json.dumps(material_data, indent=2).encode()),
             (folder/mesh_path, mesh), (assets/GROUNDCOVER, json.dumps(manifest, indent=2).encode())]
    for path, data in files:
        if path.exists() and path.read_bytes() == data:
            continue
        with path.open('xb') as output:
            output.write(data)


def make_node(name, kind, parent, identity, translation=(0, 0, 0)):
    return dict(name=name, kind=kind, parent=parent, expanded=False, visible=True,
                uuid=str(uuid.uuid5(uuid.NAMESPACE_URL, 'forest_steppe/planting/' + identity)),
                color=[1, 1, 1, 1], transform=dict(translation=translation,
                rotation_degrees=[0, 0, 0], scale=[1, 1, 1]))


def replant(scene, assets):
    nodes = scene['nodes']
    if any(n['kind'] == 'vegetation_layer' for n in nodes):
        raise ValueError('Use the original authored scene as input')
    tiles = meadow_tiles(nodes, assets)
    groups = defaultdict(lambda: defaultdict(list))
    removed = set()
    for i, node in enumerate(nodes):
        components = node.get('components', {})
        if set(components) != {'collider'} or components['collider'].get('enabled') is not False:
            continue
        name = Path(node['asset_path']).name.lower()
        family = next((f for f, prefixes in FAMILIES if name.startswith(prefixes)), None)
        if family and node['kind'] == 'model' and node['visible']:
            x, _, z = node['transform']['translation']
            groups[tile_at(x, z)][family].append(node)
            removed.add(i)
    if not removed:
        raise ValueError('No authored plants found')
    # Drop only folders emptied by this conversion; indices in the scene refer to records.
    children = defaultdict(set)
    for i, node in enumerate(nodes):
        children[node['parent']].add(i)
    for parent in sorted(children.keys() - {None}, reverse=True):
        if nodes[parent]['kind'] == 'folder' and children[parent] <= removed:
            removed.add(parent)
    keep = [i for i in range(len(nodes)) if i not in removed]
    remap = {old: new for new, old in enumerate(keep)}
    scene['nodes'] = saved = [nodes[i] for i in keep]
    for node in saved:
        if node['parent'] is not None:
            node['parent'] = remap[node['parent']]
        if node['parent'] == remap[0] and node['kind'] == 'folder':
            node['expanded'] = False
    if isinstance(scene['startup_camera'], int):
        scene['startup_camera'] = remap[scene['startup_camera']]
    root = len(saved)
    saved.append(make_node('Procedural vegetation', 'folder', remap[0], 'root'))
    candidates = plants_total = layers = 0
    for (tx, tz), families in sorted(groups.items()):
        tile_id = f'{tx}/{tz}'
        left, top = tx * TILE_SIZE - 640, tz * TILE_SIZE - 640
        parent = len(saved)
        name = 'Meadow 4, 5 - Trail entrance' if (tx, tz) == (4, 5) else f'Meadow {tx}, {tz}'
        saved.append(make_node(name, 'folder', root, tile_id))
        terrain = tiles[tx, tz]
        y = round(terrain.mean())
        offsets = np.rint((terrain - y) * 100).astype(int)
        if offsets.min() < -32768 or offsets.max() > 32767:
            raise ValueError('Terrain relief exceeds the local height range')
        field = dict(width=CELLS + 1, height=CELLS + 1, samples_cm=offsets.ravel().tolist())
        for family, plants in sorted(families.items()):
            mask = density_mask(plants, left, top, family)
            species = list(species_counts(plants))
            if family == 'Grasses':
                target = round(len(plants) * (0.32 + 0.55 * min(len(plants) / 3000, 1)))
                if target:
                    species.append((GROUNDCOVER, target))
            for asset, target in species:
                identity = f'{tile_id}/{asset}'
                seed = zlib.crc32(identity.encode())
                if asset == GROUNDCOVER:
                    # Spread the low grass into sparse fringes. Black texels still exclude all roots.
                    species_mask = np.rint(255 * (mask.astype(float) / 255) ** 0.68).astype('uint8')
                    low, high = 0.86, 1.36
                else:
                    species_mask = mask
                    sizes = [n['transform']['scale'][1] for n in plants if n['asset_path'] == asset]
                    low, high = np.percentile(sizes, [5, 95]).round(4)
                count = candidate_count(seed, species_mask, target)
                name = Path(asset).name.split('.')[0].removeprefix('SM_')
                node = make_node(f'{family} - {name}', 'vegetation_layer', parent, identity,
                                 [left + TILE_SIZE / 2, y, top + TILE_SIZE / 2])
                node.update(asset_path=asset, vegetation=dict(version=2, seed=seed, candidates=count,
                            width=TILE_SIZE, depth=TILE_SIZE, minimum_scale=low, maximum_scale=high,
                            mask=dict(width=MASK_SIDE, height=MASK_SIDE, pixels=species_mask.tobytes().hex()),
                            heightfield=field))
                saved.append(node)
                candidates += count
                plants_total += target
                layers += 1
    if candidates > 1_000_000:
        raise ValueError(f'{candidates} candidates exceed the scene budget')
    print(f'{plants_total} plants in {layers} layers, {len(groups)} tiles, '
          f'{candidates} candidates, {len(saved)} scene nodes')
    return scene


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--assets', type=Path, help='Asset root when processing a backup scene')
    parser.add_argument('--output-assets', type=Path, help='Prepare the grass patch in a separate project asset root')
    args = parser.parse_args()
    assets = args.assets or args.input.resolve().parents[1]
    if args.output.exists():
        parser.error(f'Output scene already exists: {args.output}')
    scene = replant(json.loads(args.input.read_bytes()), assets)
    data = json.dumps(scene, separators=(',', ':'), allow_nan=False).encode()
    write_groundcover(args.output_assets or assets)
    with args.output.open('xb') as output:
        output.write(data)
