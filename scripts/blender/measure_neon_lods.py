"""Measure bidirectional BVH surface error between LOD0 and LOD1/LOD2 for Neon Night City."""
import json
import bpy
from mathutils.bvhtree import BVHTree

ns = bpy.app.driver_namespace['neon_city_builder']
report = {}
for name in ns['BUILDERS']:
    base = ns['assets'][name].data
    bv0 = BVHTree.FromPolygons(
        [v.co for v in base.vertices],
        [list(p.vertices) for p in base.polygons],
        all_triangles=True,
    )
    samples = []
    for level in (1, 2):
        lo = ns['assets'][f'{name}_lod{level}'].data
        bvl = BVHTree.FromPolygons(
            [v.co for v in lo.vertices],
            [list(p.vertices) for p in lo.polygons],
            all_triangles=True,
        )
        worst = 0.0
        for source, target in ((base, bvl), (lo, bv0)):
            for point in [v.co for v in source.vertices] + [p.center for p in source.polygons]:
                hit = target.find_nearest(point)
                if hit[0] is not None:
                    worst = max(worst, float(hit[3]))
        samples.append(round(worst, 6))
    selected = [
        round(max(samples[0] * 1.15 + 0.002, 0.004), 6),
        round(max(samples[1] * 1.15 + 0.005, samples[0] * 1.15 + 0.008, 0.015), 6),
    ]
    report[name] = {'max_sampled_distance': samples, 'geometric_error': selected}

(ns['OUT'] / 'lod_surface_error_samples.json').write_text(
    json.dumps(report, indent=2) + '\n', encoding='utf-8'
)
result = report
