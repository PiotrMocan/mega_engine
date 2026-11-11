"""Run with UE 5.8 -ExecutePythonScript, never with standalone Python.

Exports a small reusable tree library without changing source meshes/materials.
Output is local licensed content, not intended for source control.
"""
import json
import traceback
from pathlib import Path
import unreal

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.tools/pine-benchmark/export'
OUT.mkdir(parents=True, exist_ok=True)

try:
    library = []
    for index in (1, 2, 3):
        name = f'SM_Pine_Tree_{index:02d}'
        mesh = unreal.load_asset('/Game/0_The_Pine_Vegetation/Meshes/' + name)
        options = unreal.GLTFExportOptions()
        options.set_editor_property('export_uniform_scale', 0.01)
        options.set_editor_property('use_mesh_quantization', False)
        options.set_editor_property('export_vertex_colors', False)
        slots = []
        for slot in mesh.get_editor_property('static_materials'):
            material = slot.get_editor_property('material_interface')
            textures = []
            for texture in unreal.MaterialEditingLibrary.get_material_used_textures(material):
                path = OUT / (texture.get_name() + '.tga')
                if not path.exists():
                    task = unreal.AssetExportTask()
                    task.object = texture
                    task.filename = str(path)
                    task.automated = True
                    task.prompt = False
                    task.replace_identical = False
                    task.exporter = unreal.TextureExporterTGA()
                    if not unreal.Exporter.run_asset_export_task(task):
                        raise RuntimeError('Texture export failed: ' + texture.get_path_name())
                textures.append({'asset': texture.get_path_name(), 'file': path.name})
            slots.append({'name': material.get_name(), 'textures': textures})
        levels = []
        for level in (0,):
            path = OUT / f'{name}_lod{level}.glb'
            options.set_editor_property('default_level_of_detail', level)
            if not path.exists():
                messages = unreal.GLTFExporter.export_to_gltf(mesh, str(path), options, set())
                (OUT / f'{name}_lod{level}.messages.txt').write_text(str(messages), encoding='utf-8')
            if not path.is_file():
                raise RuntimeError('Model export failed: ' + str(path))
            levels.append(path.name)
        library.append({'mesh': mesh.get_path_name(), 'name': name, 'materials': slots, 'lods': levels})
        (OUT / 'library.json').write_text(json.dumps(library, indent=2), encoding='utf-8')
    counts = []
    for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        for component in actor.get_components_by_class(unreal.InstancedStaticMeshComponent):
            mesh = component.get_editor_property('static_mesh')
            if mesh:
                counts.append({'mesh': mesh.get_path_name(), 'instances': component.get_instance_count()})
    (OUT / 'original-instance-counts.json').write_text(json.dumps(counts, indent=2), encoding='utf-8')
except Exception:
    (OUT / 'error.txt').write_text(traceback.format_exc(), encoding='utf-8')
    raise
