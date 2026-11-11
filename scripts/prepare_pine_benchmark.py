"""Prepare the local pine library through the existing editor importer (Windows).

Run export_pine_ue.py in UE first. Source UE content and the canonical engine
scene remain unchanged. Generated licensed content lives under assets/pine_benchmark.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / '.tools/pine-benchmark'
EXPORT = WORK / 'export'
ASSETS = ROOT / 'assets'
sys.path.insert(0, str(WORK / 'python'))
from PIL import Image


def normalize_glb(source, destination):
    data = source.read_bytes()
    size = struct.unpack_from('<I', data, 12)[0]
    document = json.loads(data[20:20+size])
    offset = 20 + size
    binary_size = struct.unpack_from('<I', data, offset)[0]
    binary = bytearray(data[offset+8:offset+8+binary_size])
    def add_base_color(part, mode):
        path = WORK / (part+'_base_color.png')
        if not path.exists():
            with Image.open(EXPORT / ('T_Pine_Tree_01_'+part+'_BC.tga')) as image:
                image.convert(mode).resize((2048,2048), Image.Resampling.LANCZOS).save(path)
        png = path.read_bytes()
        while len(binary) % 4:
            binary.append(0)
        image_offset = len(binary)
        binary.extend(png)
        document['bufferViews'].append({'buffer':0,'byteOffset':image_offset,'byteLength':len(png)})
        document['images'].append({'name':'Pine_'+part+'_BaseColor','mimeType':'image/png',
                                   'bufferView':len(document['bufferViews'])-1})
        document['textures'].append({'source':len(document['images'])-1})
        return len(document['textures'])-1
    bark_index = add_base_color('Bark','RGB')
    needles_index = add_base_color('Cluster','RGBA')
    for material in document['materials']:
        # Common baseline: standard PBR, static trees, no UE-only SSS or WPO.
        material.pop('extensions', None)
        material.pop('occlusionTexture', None)
        pbr = material['pbrMetallicRoughness']
        pbr['baseColorFactor'] = [1,1,1,1]
        pbr['baseColorTexture'] = {'index':bark_index if 'Bark' in material.get('name','') else needles_index}
        pbr['metallicFactor'] = 0
        pbr['roughnessFactor'] = 0.85
        pbr.pop('metallicRoughnessTexture', None)
        if material.get('alphaMode') == 'MASK':
            material['alphaCutoff'] = round(material.get('alphaCutoff', 0.5)*65535)/65535
    document.pop('extensionsRequired', None)
    document.pop('extensionsUsed', None)
    document['buffers'][0]['byteLength'] = len(binary)
    raw = json.dumps(document, separators=(',', ':'), allow_nan=False).encode()
    raw += b' ' * (-len(raw) % 4)
    binary += b'\0' * (-len(binary) % 4)
    result = struct.pack('<III', 0x46546C67, 2, 28+len(raw)+len(binary))
    result += struct.pack('<II', len(raw), 0x4E4F534A) + raw
    result += struct.pack('<II', len(binary), 0x004E4942) + binary
    destination.write_bytes(result)


def main():
    if os.name != 'nt':
        raise RuntimeError('Use native Windows Python')
    original = ASSETS / 'scenes/editor_scene.json'
    before = hashlib.sha256(original.read_bytes()).hexdigest()
    sources = WORK / 'normalized'
    sources.mkdir(exist_ok=True)
    (ASSETS / 'pine_benchmark/runtime/models').mkdir(parents=True, exist_ok=True)
    models = []
    for tree in json.loads((EXPORT / 'library.json').read_text()):
        source = sources / (tree['name'] + '.glb')
        normalize_glb(EXPORT / tree['lods'][0], source)
        models.append((source, ASSETS / 'pine_benchmark/runtime/models' / tree['name'] / (tree['name']+'.vcrasset.json')))
    log = (WORK / 'import-editor.log').open('w', encoding='utf-8')
    process = subprocess.Popen([str(ROOT / 'build/windows-release/bin/vulkan_editor.exe'),
        '--mcp-stdio', '--no-validation', '--width', '640', '--height', '480'], cwd=ROOT,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8')
    request_id = 0
    def rpc(method, params):
        nonlocal request_id
        request_id += 1
        process.stdin.write(json.dumps({'jsonrpc':'2.0', 'id':request_id, 'method':method, 'params':params})+'\n')
        process.stdin.flush()
        response = json.loads(process.stdout.readline())
        if 'error' in response or response.get('id') != request_id:
            raise RuntimeError(str(response))
        return response['result']
    def call(name, arguments=None):
        result = rpc('tools/call', {'name':name, 'arguments':arguments or {}})
        if result.get('isError'):
            raise RuntimeError(str(result))
        return result
    try:
        rpc('initialize', {'protocolVersion':'2025-11-25', 'capabilities':{},
            'clientInfo':{'name':'pine-benchmark-import', 'version':'1'}})
        process.stdin.write(json.dumps({'jsonrpc':'2.0', 'method':'notifications/initialized'})+'\n')
        process.stdin.flush()
        for source, model in models:
            if model.exists():
                continue
            call('editor.assets.import', {'source_path':str(source), 'destination_directory':'pine_benchmark/runtime/models'})
            deadline = time.monotonic()+180
            while time.monotonic() < deadline:
                state = call('editor.get_state').get('structuredContent', {})
                if not state.get('asset_import_pending', True):
                    if not model.exists():
                        raise RuntimeError(str(state))
                    break
                time.sleep(0.25)
            else:
                raise TimeoutError(str(source))
            print(model.relative_to(ASSETS), flush=True)
        call('engine.quit')
        process.wait(timeout=30)
        if process.returncode:
            raise RuntimeError('Editor exit '+str(process.returncode))
    finally:
        if process.poll() is None:
            process.stdin.close()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        log.close()
    if hashlib.sha256(original.read_bytes()).hexdigest() != before:
        raise RuntimeError('Canonical scene changed during import')
    (WORK / 'import-result.json').write_text(json.dumps({'models':[str(p.relative_to(ASSETS)) for _,p in models],
        'canonical_scene_sha256':before, 'canonical_scene_unchanged':True}, indent=2))

if __name__ == '__main__':
    main()
