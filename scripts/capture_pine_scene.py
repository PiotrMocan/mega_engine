"""Capture and validate the isolated pine scene through the engine's native MCP."""
import argparse
import base64
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / '.tools/pine-benchmark'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration', choices=('Release','Debug','ASan'), default='Release')
    parser.add_argument('--aa', default='off')
    parser.add_argument('--prepass', default='off')
    parser.add_argument('--lod', choices=('on','off'), default='off')
    args = parser.parse_args()
    tree = {'Release':'windows-release','Debug':'windows-debug','ASan':'windows-asan'}[args.configuration]
    name = f'capture-{args.configuration}-{args.aa}-{args.prepass}-lod-{args.lod}'
    environment = dict(os.environ, VCR_ASSET_ROOT=str(ROOT/'assets'))
    log = (WORK/(name+'.log')).open('w', encoding='utf-8')
    process = subprocess.Popen([str(ROOT/'build'/tree/'bin/vulkan_cube.exe'), '--mcp-stdio',
        '--benchmark', '--warmup','0', '--duration','60',
        '--validation', '--width','1280','--height','720', '--present','immediate', '--lod',args.lod,
        '--culling','frustum', '--dynamic-shadows','on', '--depth-prepass',args.prepass,
        '--aa',args.aa], cwd=WORK, env=environment, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8')
    request_id = 0
    def rpc(method, params):
        nonlocal request_id
        request_id += 1
        process.stdin.write(json.dumps({'jsonrpc':'2.0','id':request_id,'method':method,'params':params})+'\n')
        process.stdin.flush()
        response = json.loads(process.stdout.readline())
        if 'error' in response:
            raise RuntimeError(str(response))
        return response['result']
    def call(name):
        result = rpc('tools/call', {'name':name, 'arguments':{}})
        if result.get('isError'):
            raise RuntimeError(str(result))
        return result
    try:
        rpc('initialize', {'protocolVersion':'2025-11-25','capabilities':{},
            'clientInfo':{'name':'pine-validation','version':'1'}})
        process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        process.stdin.flush()
        time.sleep(3)
        state = call('debug.get_state')
        metrics = call('debug.get_metrics')
        capture = call('capture.screenshot')
        for content in capture.get('content', []):
            if content.get('type') == 'image':
                (WORK/(name+'.png')).write_bytes(base64.b64decode(content['data']))
        call('engine.quit')
        process.stdin.close()
        process.wait(timeout=30)
    finally:
        if process.poll() is None:
            process.stdin.close()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        log.close()
    text = (WORK/(name+'.log')).read_text(encoding='utf-8')
    for token in ('VUID-', '[Vulkan ERROR]', 'SYNC-HAZARD', 'AddressSanitizer'):
        if token in text:
            raise RuntimeError('Validation failure '+token+'; see '+name+'.log')
    if process.returncode or 'Live GPU allocations: 0 (0 bytes)' not in text:
        raise RuntimeError('Nonzero exit or missing zero-allocation shutdown: '+name)
    (WORK/(name+'.json')).write_text(json.dumps({'state':state,'metrics':metrics,'exit_code':0},indent=2))
    print(str(WORK/(name+'.png')))

if __name__ == '__main__':
    main()
