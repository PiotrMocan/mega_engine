"""Complete Blender MCP entry point for 'Neon: Night City'."""
from pathlib import Path
import bpy

root = Path(r'C:\GDF\mega_engine')
namespace = {'VCR_ROOT': str(root)}
directory = root / 'scripts/blender'

exec(compile((directory / 'build_neon_city.py').read_text(encoding='utf-8'), 'build_neon_city.py', 'exec'), namespace)
bpy.app.driver_namespace['neon_city_builder'] = namespace
for asset_name in namespace['BUILDERS']:
    namespace['build_asset'](asset_name)

for filename in ('measure_neon_lods.py', 'layout_neon_city.py'):
    exec(compile((directory / filename).read_text(encoding='utf-8'), filename, 'exec'), namespace)

result = namespace['result']
