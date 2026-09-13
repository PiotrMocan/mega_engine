"""Install the user-supplied broken-brick PBR set on the demo wall."""
from pathlib import Path
import json
import shutil


ROOT = Path(__file__).resolve().parents[1]
USER_SOURCE = Path(
    r'C:\Users\olymp\Downloads\broken_brick_wall_2k.blend\textures')
SOURCE = ROOT / 'art/freight_yard/textures/broken_brick_wall'
KIT = ROOT / 'assets/freight_yard/kit'
MAPS = {
    'base_color': ('broken_brick_wall_diff_2k.jpg', 'srgb'),
    'normal': ('broken_brick_wall_nor_gl_2k.exr', 'linear'),
    'metallic_roughness': ('broken_brick_wall_rough_2k.exr', 'linear'),
    'displacement': ('broken_brick_wall_disp_2k.png', 'linear'),
}


def stage_source(user_source=USER_SOURCE, source=SOURCE):
    """Copy the supplied maps into the authored game content."""
    source.mkdir(parents=True, exist_ok=True)
    for filename, _ in MAPS.values():
        source_file = user_source / filename
        if not source_file.is_file():
            raise FileNotFoundError(source_file)
        shutil.copyfile(source_file, source / filename)


def validate_source(source=SOURCE):
    for filename, _ in MAPS.values():
        if not (source / filename).is_file():
            raise FileNotFoundError(source / filename)


def write_json(path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')


def apply_brick_wall(kit=KIT, source=SOURCE, tessellation_settings=None):
    validate_source(source)
    source_reference = 'materials/FY_brick_tess_source.vcrmaterial.json'
    source_path = kit / source_reference
    material = json.loads(source_path.read_text(encoding='utf-8'))
    material['name'] = 'FY_broken_brick_wall'
    material['textures'] = {}
    for semantic, (filename, color_space) in MAPS.items():
        destination = kit / 'textures' / filename
        shutil.copyfile(source / filename, destination)
        texture_name = f'FY_broken_brick_wall_{semantic}'
        texture_reference = f'textures/{texture_name}.vcrtexture.json'
        write_json(kit / texture_reference, {
            'schema': 'vcr.asset',
            'version': 1,
            'kind': 'texture',
            'name': texture_name,
            'source': 'textures/' + filename,
            'source_reference': (
                'user-supplied broken_brick_wall_2k.blend/textures/' +
                filename),
            'usage': semantic,
            'color_space': color_space,
            'decode': 'deferred',
        })
        material['textures'][semantic] = texture_reference
    material['pbr'].update(
        base_color=[1.0, 1.0, 1.0, 1.0],
        metallic=0.0,
        roughness=1.0)
    material['tessellation'] = tessellation_settings or {
        'enabled': True,
        'displacement_scale': 0.055,
        'displacement_center': 0.5,
        'target_edge_pixels': 12.0,
        'maximum_factor': 32.0,
        'up_facing_only': False,
        'normal_threshold': -1.0,
        # The texture tiles across the wall, so UV-boundary fading is disabled.
        'uv_edge_fade': 0.0,
    }
    material_reference = 'materials/FY_broken_brick_wall.vcrmaterial.json'
    write_json(kit / material_reference, material)

    for name in (
            'broken_brick_wall',
            'broken_brick_wall_lod1',
            'broken_brick_wall_lod2'):
        asset_path = kit / f'{name}.vcrasset.json'
        asset = json.loads(asset_path.read_text(encoding='utf-8'))
        replacements = {
            index: material_reference
            for index, reference in enumerate(asset['materials'])
            if reference in (source_reference, material_reference)
        }
        if not replacements:
            raise ValueError(f'{asset_path} has no brick face material slot')
        asset['materials'] = [
            replacements.get(index, reference)
            for index, reference in enumerate(asset['materials'])
        ]
        for submesh in asset['submeshes']:
            if submesh['material_index'] in replacements:
                submesh['material'] = material_reference
        write_json(asset_path, asset)

    if source_path.is_file():
        source_path.unlink()


if __name__ == '__main__':
    stage_source()
    print('Staged the supplied broken-brick PBR maps for Freight Yard.')
