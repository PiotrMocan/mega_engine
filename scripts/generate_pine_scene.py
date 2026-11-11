"""Create a deterministic, flat pine workload and a shared UE placement manifest."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random

ROOT = Path(__file__).resolve().parents[1]


def node(name, kind, position, scale=1, yaw=0):
    return {'name':name, 'kind':kind, 'parent':None, 'visible':True, 'expanded':False,
        'transform':{'translation':position, 'rotation_degrees':[0,yaw,0], 'scale':[scale]*3},
        'color':[1,1,1,1], 'pbr':{'metallic':0, 'roughness':0.9, 'ambient_occlusion':1,
            'ior':1.5, 'model':'standard'}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--count', type=int, default=4096)
    parser.add_argument('--spacing', type=float, default=5.5)
    parser.add_argument('--seed', type=int, default=1327)
    parser.add_argument('--make-default', action='store_true', help='Preserve the current scene and select the forest at startup')
    args = parser.parse_args()
    if not 1 <= args.count <= 99000 or not math.isfinite(args.spacing) or args.spacing < 1:
        parser.error('count must be 1..99000 and spacing >= 1 meter')
    assets = ROOT / 'assets'
    model_prefix = 'pine_benchmark/runtime/models'
    rng = random.Random(args.seed)
    side = math.ceil(math.sqrt(args.count))
    extent = side*args.spacing
    nodes, placements = [], []
    for i in range(args.count):
        model = f'SM_Pine_Tree_{1+rng.randrange(3):02d}'
        position = [(i % side - (side-1)/2 + rng.uniform(-0.35,0.35))*args.spacing,
                    0, (i // side - (side-1)/2 + rng.uniform(-0.35,0.35))*args.spacing]
        scale, yaw = rng.uniform(0.8,1.2), rng.uniform(-180,180)
        tree = node(f'Pine {i:05d}', 'model', position, scale, yaw)
        tree['asset_path'] = f'{model_prefix}/{model}/{model}.vcrasset.json'
        if not (assets/tree['asset_path']).is_file():
            raise FileNotFoundError(tree['asset_path'])
        nodes.append(tree)
        placements.append({'model':model, 'position':position, 'scale':scale, 'yaw':yaw})
    camera = node('Benchmark Camera', 'camera', [0, 14, extent/2+25])
    camera['transform']['rotation_degrees'] = [-8,0,0]
    if args.count == 1:
        camera['transform']['translation'] = [0,10,32]
        camera['transform']['rotation_degrees'] = [0,0,0]
    startup = len(nodes)
    nodes.append(camera)
    floor = node('Flat forest floor', 'ground', [0,-0.02,0])
    floor['transform']['scale'] = [(extent+40)/2, 1, (extent+40)/2]
    floor['color'] = [0.12,0.16,0.075,1]
    nodes.append(floor)
    nodes.append(node('Sky', 'environment', [0,0,0]))
    nodes.append(node('Sun', 'directional_light', [0,0,0]))
    lighting = {'dynamic':True, 'voxel_gi':False, 'cycle':False, 'hour':14,
                'shadow_resolution':2048, 'cascade_cache':False}
    scene = {'format':'vulkan_cube_scene', 'version':1, 'startup_camera':startup,
             'lighting':lighting, 'tone_mapper':'reinhard', 'nodes':nodes}
    path = assets/'scenes/pine_forest.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(scene, separators=(',', ':'), allow_nan=False)+'\n'
    path.write_text(text, encoding='utf-8')
    if args.make_default:
        default = assets/'scenes/editor_scene.json'
        backup = assets/'scenes/freight_yard_scene.json'
        if default.exists() and not backup.exists():
            backup.write_bytes(default.read_bytes())
        default.write_text(text, encoding='utf-8')
    manifest = {'generator':'vcr.pine-benchmark.v1', 'seed':args.seed, 'tree_count':args.count,
        'spacing_m':args.spacing, 'extent_m':extent, 'trees_per_hectare':args.count/(extent*extent/10000),
        'scene_sha256':hashlib.sha256(text.encode()).hexdigest(), 'placements':placements,
        'camera':camera['transform'], 'vertical_fov_degrees':65, 'resolution':[1920,1080],
        'baseline':{'lod':False, 'occlusion':False, 'wind':False, 'gi':False, 'nanite':False,
                    'dynamic_resolution':False, 'screen_percentage':100, 'vsync':False}}
    output = ROOT/'.tools/pine-benchmark/shared-scene.json'
    output.write_text(json.dumps(manifest, separators=(',', ':')), encoding='utf-8')
    print(json.dumps({k:v for k,v in manifest.items() if k not in ('placements','camera','baseline')}))

if __name__ == '__main__':
    main()
