"""Generate an overlapping authored-model LOD benchmark; no renderer/build is run.

Default counts 1 and 1024; --capacity-stress selects 100000 model nodes, without
an extra camera node. Output belongs to an existing approved Windows build tree.
The overlapping patch cloud stresses LOD dispatch/index capacity, not shading.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import sys

# Reuse the cooked mesh/header checks without creating scripts/__pycache__.
sys.dont_write_bytecode = True
from generate_lod_fixture_windows import checked_write, digest, mesh, node, write_json

ROOT = Path(__file__).resolve().parents[1]
TREES = {"Debug": "windows-debug", "Release": "windows-release", "ASan": "windows-asan"}
GENERATOR = "vcr.lod-benchmark.v1"
ERRORS = (0.0, 1 / 16, 3 / 32)
HALF_EXTENTS = (1.0, 31 / 32, 15 / 16)
SUBDIVISIONS = (8, 4, 1)
SCALE = 0.04
DEPTH = 7.25
PITCH = math.radians(12)
FOCAL = 1080 / (2 * math.tan(math.radians(32.5)))


def projection_error(camera_x: float, x: float, up: float, depth: float, error: float) -> float:
    nearest = depth - 2 * math.sin(PITCH) * math.cos(PITCH) * SCALE
    maximum = max(abs(x - camera_x) + SCALE, abs(up) + SCALE)
    return error * math.sqrt(3) * SCALE * FOCAL / nearest * (1 + maximum / nearest)


def mathematical_contract() -> dict:
    corners = [(x, up, depth) for x in (3.98, 4.02)
               for up in (-0.02, 0.02) for depth in (7.248, 7.252)]
    for x, up, depth in corners:
        # All sources can coarsen to LOD2 at the right camera endpoint and
        # refine to LOD0 at the left, with margin around the hysteresis gates.
        assert projection_error(4, x, up, depth, ERRORS[2]) < 0.8
        assert projection_error(-4, x, up, depth, ERRORS[1]) > 1.0
        nearest = depth - 2 * math.sin(PITCH) * math.cos(PITCH) * SCALE
        assert max(abs(x - 4), abs(x + 4)) + SCALE < nearest * math.tan(math.radians(32.5)) * 16 / 9
        assert abs(up) + SCALE < nearest * math.tan(math.radians(32.5))
    return {
        "resolution": [1920, 1080], "vertical_fov_degrees": 65, "camera_z": 12,
        "camera_position": "(4*sin(0.6*t), 4, 12)", "camera_pitch_degrees": -12,
        "period_seconds": 2 * math.pi / 0.6, "world_scale": SCALE,
        "center_view_depth": DEPTH, "nearest_aabb_depth": DEPTH - 2 * math.sin(PITCH) * math.cos(PITCH) * SCALE,
        "focal_pixels": FOCAL, "pixel_budget": 1, "coarsen_threshold": 0.8, "refine_threshold": 1,
        "center_projected_error_ranges": [[projection_error(4, 4, 0, DEPTH, error),
                                            projection_error(-4, 4, 0, DEPTH, error)] for error in ERRORS[1:]],
        "center_transition_times_seconds": {"1_to_2": 1.93643848, "2_to_1": 4.47234107,
                                            "1_to_0": 6.63870594, "0_to_1": 10.41713556},
        "cloud_all_corners_cross_lod0_and_lod2": True, "cloud_all_corners_inside_frustum": True,
        "proof_scope": "Double arithmetic against current conservative projection; runtime CPU/GPU probe is still mandatory.",
    }


def generate(output: Path, count: int) -> dict:
    relative_root = f"count-{count}"
    asset_root = output / relative_root / "assets"
    paths = []
    material = {"schema": "vcr.asset", "version": 1, "kind": "material", "name": "LOD overlap stress",
                "pbr": {"base_color": [0.25, 0.5, 0.75, 1], "metallic": 0, "roughness": 0.65,
                        "ambient_occlusion": 1, "ior": 1.5, "model": "unlit"}, "textures": {}}
    paths.append(write_json(asset_root, "lod_benchmark/materials/surface.vcrmaterial.json", material))
    levels = []
    for level, (subdivisions, half) in enumerate(zip(SUBDIVISIONS, HALF_EXTENTS)):
        data, vertex_count, index_count = mesh(subdivisions, half)
        paths.append(checked_write(asset_root, f"lod_benchmark/mesh/lod{level}.vcrmesh", data))
        deviation = math.sqrt(2) * (1 - half)
        assert deviation <= ERRORS[level]
        metadata = {
            "schema": "vcr.asset", "version": 1, "kind": "model", "name": f"LOD overlap stress {level}",
            "mesh": f"mesh/lod{level}.vcrmesh",
            "geometry": {"vertex_count": vertex_count, "index_count": index_count, "index_type": "uint32",
                         "primitive": "triangles", "vertex_stride": 48},
            "materials": ["materials/surface.vcrmaterial.json"],
            "submeshes": [{"name": "Surface", "first_index": 0, "index_count": index_count,
                           "material_index": 0, "material": "materials/surface.vcrmaterial.json"}],
        }
        if level == 0:
            metadata["lods"] = [{"model": f"lod_benchmark/lod{i}.vcrasset.json", "geometric_error": ERRORS[i]}
                                for i in (1, 2)]
        paths.append(write_json(asset_root, f"lod_benchmark/lod{level}.vcrasset.json", metadata))
        levels.append({"level": level, "triangles": index_count // 3, "half_extent": half,
                       "actual_maximum_surface_deviation": deviation, "declared_error": ERRORS[level]})
    nodes = []
    for i in range(count):
        x = 4 if count == 1 else 4 + ((i % 101) / 100 - 0.5) * 0.04
        up = 0 if count == 1 else (((i // 101) % 97) / 96 - 0.5) * 0.04
        depth = DEPTH if count == 1 else DEPTH + (i / (count - 1) - 0.5) * 0.004
        translation = [x, 4 - depth * math.sin(PITCH) + up * math.cos(PITCH),
                       12 - depth * math.cos(PITCH) - up * math.sin(PITCH)]
        instance = node(f"LOD overlap {i}", "model", translation)
        instance["asset_path"] = "lod_benchmark/lod0.vcrasset.json"
        instance["transform"]["rotation_degrees"] = [-12, 0, 0]
        instance["transform"]["scale"] = [SCALE] * 3
        nodes.append(instance)
    assert len(nodes) == count <= 100000
    # CLI overrides the camera. Omitting camera/folder/light nodes leaves all
    # 100000 slots available to models in the explicit capacity workload.
    scene = {"format": "vulkan_cube_scene", "version": 1,
             "lighting": {"dynamic": True, "voxel_gi": False, "cycle": False, "hour": 14},
             "nodes": nodes}
    paths.append(write_json(asset_root, "scenes/editor_scene.json", scene))
    return {"source_instances": count, "node_count": count, "camera_nodes": 0, "asset_root": str(asset_root),
            "classification": "single model transition" if count == 1 else "overlap synthetic capacity/CPU dispatch stress, not a shading benchmark",
            "levels": levels, "expected_maximum_dual_indices": 2 * count,
            "files": [{"path": str(path.relative_to(output)), "bytes": path.stat().st_size,
                       "sha256": digest(path)} for path in paths]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configuration", choices=TREES, default="Release")
    parser.add_argument("--counts", nargs="+", type=int, choices=(1, 1024), default=None)
    parser.add_argument("--capacity-stress", action="store_true")
    options = parser.parse_args()
    if os.name != "nt":
        parser.error("Use native Windows Python.")
    if options.capacity_stress and options.counts is not None:
        parser.error("--capacity-stress selects 100000 alone; do not also pass --counts")
    counts = [100000] if options.capacity_stress else sorted(set(options.counts or [1, 1024]))
    tree = (ROOT / "build" / TREES[options.configuration]).resolve()
    if not tree.is_dir():
        parser.error("The approved build tree must already exist")
    output = tree / ("lod-benchmark-capacity" if options.capacity_stress else "lod-benchmark")
    output.resolve().relative_to(tree)
    ownership = output / "fixture-manifest.json"
    if output.exists() and any(output.iterdir()):
        if not ownership.is_file() or json.loads(ownership.read_text(encoding="utf-8")).get("generator") != GENERATOR:
            parser.error("Refusing to overwrite a nonempty directory without this generator's ownership manifest")
    canonical = ROOT / "assets/scenes/editor_scene.json"
    before = digest(canonical)
    contract = mathematical_contract()
    variants = [generate(output, count) for count in counts]
    if digest(canonical) != before:
        raise RuntimeError("Canonical scene changed concurrently")
    manifest = {"generator": GENERATOR, "configuration": options.configuration, "capacity_stress": options.capacity_stress,
                "canonical_scene_sha256": before, "canonical_scene_unchanged": True, "native_windows": True,
                "gpu_executed": False, "cooked_loader_executed": False, "mathematical_contract": contract, "variants": variants}
    write_json(output, "fixture-manifest.json", manifest)
    print(json.dumps({"manifest": str(ownership), "counts": counts, "gpu_executed": False}))


if __name__ == "__main__":
    main()

