"""Install the Freight Yard concrete materials and tessellation demo maps.

The original photographic concrete stays on walls and barriers. A coherent,
Blender-generated PBR/height set is assigned only to the courtyard slabs so
height displacement cannot pull vertical kit pieces into one another.
"""
from pathlib import Path
import json
import shutil


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'art/freight_yard/textures/dirty_concrete'
TESSELLATION_SOURCE = ROOT / 'art/freight_yard/textures/tessellated_concrete'
KIT = ROOT / 'assets/freight_yard/kit'
BASE_MAPS = {
    'base_color': ('dirty_concrete_diff_2k.jpg', 'srgb'),
    'normal': ('dirty_concrete_nor_gl_2k.exr', 'linear'),
    'metallic_roughness': ('dirty_concrete_rough_2k.jpg', 'linear'),
}
TESSELLATION_MAPS = {
    'base_color': ('tess_concrete_base_color.png', 'srgb'),
    'normal': ('tess_concrete_normal.png', 'linear'),
    'metallic_roughness': ('tess_concrete_metallic_roughness.png', 'linear'),
    'displacement': ('tess_concrete_height.png', 'linear'),
}


def validate_source(source=SOURCE, tessellation_source=TESSELLATION_SOURCE):
    for filename, _ in BASE_MAPS.values():
        if not (source / filename).is_file():
            raise FileNotFoundError(source / filename)
    for filename, _ in TESSELLATION_MAPS.values():
        if not (tessellation_source / filename).is_file():
            raise FileNotFoundError(tessellation_source / filename)


def write_json(path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')


def install_maps(kit, material, prefix, maps, source, source_reference):
    material['textures'] = {}
    for semantic, (filename, color_space) in maps.items():
        shutil.copyfile(source / filename, kit / 'textures' / filename)
        name = f'{prefix}_{semantic}'
        metadata = {
            'schema': 'vcr.asset',
            'version': 1,
            'kind': 'texture',
            'name': name,
            'source': 'textures/' + filename,
            'source_reference': source_reference + '/' + filename,
            'usage': semantic,
            'color_space': color_space,
            'decode': 'deferred',
        }
        relative = 'textures/' + name + '.vcrtexture.json'
        write_json(kit / relative, metadata)
        material['textures'][semantic] = relative


def apply_concrete(
        kit=KIT, source=SOURCE,
        tessellation_source=TESSELLATION_SOURCE,
        tessellation_settings=None):
    validate_source(source, tessellation_source)
    material_path = kit / 'materials/FY_concrete.vcrmaterial.json'
    concrete = json.loads(material_path.read_text(encoding='utf-8'))
    install_maps(
        kit, concrete, 'FY_concrete', BASE_MAPS, source,
        'user-supplied dirty_concrete_2k.blend/textures')
    concrete['pbr'].update(
        base_color=[1.0, 1.0, 1.0, 1.0],
        metallic=0.0,
        roughness=1.0)
    concrete.pop('tessellation', None)
    write_json(material_path, concrete)

    tessellated = json.loads(json.dumps(concrete))
    tessellated['name'] = 'FY_concrete_tessellated'
    install_maps(
        kit, tessellated, 'FY_tess_concrete', TESSELLATION_MAPS,
        tessellation_source,
        'ImageGen height source + Blender coherent PBR derivation')
    tessellated['tessellation'] = tessellation_settings or {
        'enabled': True,
        'displacement_scale': 0.12,
        'displacement_center': 0.5,
        'target_edge_pixels': 10.0,
        'maximum_factor': 16.0,
        'up_facing_only': True,
        'normal_threshold': 0.9,
        'uv_edge_fade': 0.04,
    }
    tessellated_path = (
        kit / 'materials/FY_concrete_tessellated.vcrmaterial.json')
    write_json(tessellated_path, tessellated)

    material_reference = 'materials/FY_concrete_tessellated.vcrmaterial.json'
    tessellation_source_reference = (
        'materials/FY_concrete_tess_source.vcrmaterial.json')
    for name in ('concrete_slab', 'concrete_slab_lod1', 'concrete_slab_lod2'):
        asset_path = kit / f'{name}.vcrasset.json'
        asset = json.loads(asset_path.read_text(encoding='utf-8'))
        replacements = {
            index: material_reference
            for index, reference in enumerate(asset['materials'])
            if reference in (
                tessellation_source_reference,
                material_reference,
            )
        }
        if not replacements:
            raise ValueError(
                f'{asset_path} has no dedicated slab-top material slot')
        asset['materials'] = [
            replacements.get(index, reference)
            for index, reference in enumerate(asset['materials'])
        ]
        for submesh in asset['submeshes']:
            if submesh['material_index'] in replacements:
                submesh['material'] = material_reference
        write_json(asset_path, asset)

    unused_source_material = kit / tessellation_source_reference
    if unused_source_material.is_file():
        unused_source_material.unlink()


if __name__ == '__main__':
    apply_concrete()
    print(
        'Installed coherent concrete height/normal maps; tessellation is '
        'enabled only on courtyard slabs.')
