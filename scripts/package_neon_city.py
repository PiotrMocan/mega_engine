"""Package Neon: Night City imported assets with shared PBR/unlit materials and authored LODs."""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import time

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets/neon_city'
KIT = ASSETS / 'kit'
STAGE = ROOT / 'build/windows-release/neon-import-cooked'

manifest = json.loads((ROOT / 'art/neon_city/asset_manifest.json').read_text(encoding='utf-8'))
lod_errors = json.loads((ROOT / 'art/neon_city/lod_surface_error_samples.json').read_text(encoding='utf-8'))
unlit_names = set(manifest.get('unlit_materials', []))


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')


if KIT.exists():
    KIT.resolve().relative_to(ASSETS.resolve())
    previous = STAGE / 'history' / f'kit-{time.time_ns()}'
    previous.resolve().relative_to(STAGE.resolve())
    previous.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(KIT), str(previous))

for directory in (KIT / 'mesh', KIT / 'materials', KIT / 'textures', STAGE):
    directory.mkdir(parents=True, exist_ok=True)

seen_materials: dict[str, dict] = {}
texture_hashes: dict[str, str] = {}
report: dict[str, object] = {}

for asset, levels in manifest['assets'].items():
    for entry in levels:
        name = Path(entry['file']).stem
        source = ASSETS / name
        staged = STAGE / name
        if source.exists():
            source.resolve().relative_to(ASSETS.resolve())
            staged.resolve().relative_to(STAGE.resolve())
            if staged.exists():
                history = STAGE / 'history' / f'{name}-{staged.stat().st_mtime_ns}'
                history.resolve().relative_to(STAGE.resolve())
                history.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(staged), str(history))
            shutil.move(str(source), str(staged))

        metadata = json.loads((staged / f'{name}.vcrasset.json').read_text(encoding='utf-8'))
        used = sorted({s['material_index'] for s in metadata['submeshes']})
        remap = {old: new for new, old in enumerate(used)}
        references = []

        for old in used:
            material = json.loads((staged / metadata['materials'][old]).read_text(encoding='utf-8'))
            material_name = material['name']
            is_unlit = material_name in unlit_names

            if is_unlit:
                material['textures'] = {
                    k: v for k, v in material['textures'].items() if k == 'base_color'
                }
                material['pbr']['model'] = 'unlit'
                material['pbr']['base_color'] = [1.0, 1.0, 1.0, 1.0]
                material['pbr']['metallic'] = 0.0
                material['pbr']['roughness'] = 0.25
                material['pbr']['ior'] = 1.5
            else:
                material['textures'] = {
                    k: v for k, v in material['textures'].items() if k in ('base_color', 'normal')
                }
                material['pbr']['model'] = 'standard'
                material['pbr']['base_color'] = [1.0, 1.0, 1.0, 1.0]
                material['pbr']['ior'] = 1.52 if 'glass' in material_name else 1.5

            for semantic, reference in list(material['textures'].items()):
                texture = json.loads((staged / reference).read_text(encoding='utf-8'))
                image = staged / texture['source']
                raw = image.read_bytes()
                key = hashlib.sha256(raw).hexdigest()
                dest_name = f'{material_name}_{semantic}'
                if key in texture_hashes:
                    dest_name = texture_hashes[key]
                else:
                    texture_hashes[key] = dest_name
                    (KIT / 'textures' / f'{dest_name}.png').write_bytes(raw)
                    texture['source'] = f'textures/{dest_name}.png'
                    texture['name'] = dest_name
                    texture['source_reference'] = 'procedural Blender MCP Neon City material'
                    write_json(KIT / 'textures' / f'{dest_name}.vcrtexture.json', texture)
                material['textures'][semantic] = f'textures/{dest_name}.vcrtexture.json'

            if not is_unlit:
                short_key = material_name.removeprefix('NC_')
                mr_source = ROOT / 'art/neon_city/textures' / f'{short_key}_metallic_roughness.png'
                mr_name = f'{material_name}_metallic_roughness'
                if mr_source.exists():
                    shutil.copyfile(mr_source, KIT / 'textures' / f'{mr_name}.png')
                    write_json(
                        KIT / 'textures' / f'{mr_name}.vcrtexture.json',
                        {
                            'schema': 'vcr.asset',
                            'version': 1,
                            'kind': 'texture',
                            'name': mr_name,
                            'source': f'textures/{mr_name}.png',
                            'usage': 'metallic_roughness',
                            'color_space': 'linear',
                            'decode': 'deferred',
                        },
                    )
                    material['textures']['metallic_roughness'] = f'textures/{mr_name}.vcrtexture.json'
                    material['pbr']['metallic'] = 1.0
                    material['pbr']['roughness'] = 1.0

            ref_path = f'materials/{material_name}.vcrmaterial.json'
            if material_name not in seen_materials:
                write_json(KIT / ref_path, material)
                seen_materials[material_name] = material
            references.append(ref_path)

        for submesh in metadata['submeshes']:
            submesh['material_index'] = remap[submesh['material_index']]
            submesh['material'] = references[submesh['material_index']]
        metadata['materials'] = references

        binary = bytearray((staged / metadata['mesh']).read_bytes())
        assert struct.unpack_from('<I', binary, 32)[0] == len(metadata['submeshes'])
        for idx_s, submesh in enumerate(metadata['submeshes']):
            struct.pack_into('<I', binary, 40 + 24 * idx_s + 16, submesh['material_index'])
        (KIT / 'mesh' / f'{name}.vcrmesh').write_bytes(binary)
        metadata['mesh'] = f'mesh/{name}.vcrmesh'
        metadata['source'] = f'Blender MCP: art/neon_city/exports/{name}.glb'

        if entry['level'] == 0:
            errors = lod_errors[asset]['geometric_error']
            metadata['lods'] = [
                {
                    'model': f'neon_city/kit/{asset}_lod{i}.vcrasset.json',
                    'geometric_error': errors[i - 1],
                }
                for i in (1, 2)
            ]
        else:
            metadata['generate_lods'] = False

        write_json(KIT / f'{name}.vcrasset.json', metadata)

    report[asset] = {
        'triangles': [e['triangles'] for e in levels],
        'material_count': levels[0]['materials'],
    }

report['package'] = {
    'models': sum(len(levels) for levels in manifest['assets'].values()),
    'materials': len(seen_materials),
    'unlit_materials': sorted(m for m in seen_materials if m in unlit_names),
    'texture_images': sum(
        p.suffix.lower() in ('.png', '.jpg', '.exr') for p in (KIT / 'textures').iterdir()
    ),
    'bytes': sum(p.stat().st_size for p in KIT.rglob('*') if p.is_file()),
}
write_json(ROOT / 'art/neon_city/packaging_report.json', report)
print(json.dumps(report, indent=2))
