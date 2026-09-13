"""Apply the user's original 2K concrete maps without image resampling.

Run with native Windows Python. package_freight_yard.py also calls this after
cooking so rebuilding the demo preserves this material replacement.
"""
from pathlib import Path
import json
import shutil


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'art/freight_yard/textures/dirty_concrete'
KIT = ROOT / 'assets/freight_yard/kit'
MAPS = {
    'base_color': ('dirty_concrete_diff_2k.jpg', 'srgb'),
    'normal': ('dirty_concrete_nor_gl_2k.exr', 'linear'),
    'metallic_roughness': ('dirty_concrete_rough_2k.jpg', 'linear'),
}


def validate_source(source=SOURCE):
    for filename, _ in MAPS.values():
        if not (source / filename).is_file():
            raise FileNotFoundError(source / filename)


def apply_concrete(kit=KIT, source=SOURCE):
    material_path = kit / 'materials/FY_concrete.vcrmaterial.json'
    material = json.loads(material_path.read_text(encoding='utf-8'))
    # Validate every input before changing the current material references.
    validate_source(source)

    for semantic, (filename, color_space) in MAPS.items():
        shutil.copyfile(source / filename, kit / 'textures' / filename)
        name = 'FY_concrete_' + semantic
        metadata = {
            'schema': 'vcr.asset', 'version': 1, 'kind': 'texture',
            'name': name, 'source': 'textures/' + filename,
            'source_reference': 'user-supplied dirty_concrete_2k.blend/textures/' + filename,
            'usage': semantic, 'color_space': color_space, 'decode': 'deferred',
        }
        relative = 'textures/' + name + '.vcrtexture.json'
        (kit / relative).write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
        material['textures'][semantic] = relative

    # A grayscale roughness image already supplies G. Multiplying its B by
    # metallic=0 keeps concrete dielectric without a lossy channel repack.
    material['pbr'].update(base_color=[1.0, 1.0, 1.0, 1.0], metallic=0.0, roughness=1.0)
    material_path.write_text(json.dumps(material, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    apply_concrete()
    print('Applied original 2K concrete base color, OpenGL normal and roughness maps.')
