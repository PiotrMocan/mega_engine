"""Generate tiny authored-LOD assets using native Windows Python and stdlib only.

No renderer, compiler or GPU process is launched. Generated files stay under an
existing approved Windows build tree; the canonical project scene is read only
to verify that its SHA-256 remains unchanged. See docs/LOD_TRANSITION_FIXTURES.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct


GENERATOR = "vcr.lod-transition-fixture.v1"
ROOT = Path(__file__).resolve().parents[1]
TREES = {"Debug": "windows-debug", "Release": "windows-release", "ASan": "windows-asan"}
SUBDIVISIONS = (8, 4, 1)
ERRORS = (0.0625, 0.25)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked_write(root: Path, relative: str, data: bytes) -> Path:
    path = root / relative
    path.resolve().relative_to(root.resolve())
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def write_json(root: Path, relative: str, data: object) -> Path:
    return checked_write(root, relative, (json.dumps(data, indent=2, allow_nan=False) + "\n").encode("utf-8"))


def mesh(subdivisions: int, half_extent: float) -> tuple[bytes, int, int]:
    # All positions and UVs are dyadic, so the nested planar tessellations do
    # not introduce decimal coordinate conversion error in the coverage case.
    vertices = []
    for y in range(subdivisions + 1):
        for x in range(subdivisions + 1):
            vertices.append((
                (2 * x / subdivisions - 1) * half_extent,
                (2 * y / subdivisions - 1) * half_extent,
                0.0, 0.0, 0.0, 1.0,
                x / subdivisions, y / subdivisions,
                1.0, 0.0, 0.0, 1.0,
            ))
    indices = []
    for y in range(subdivisions):
        for x in range(subdivisions):
            a = y * (subdivisions + 1) + x
            b, c, d = a + 1, a + subdivisions + 1, a + subdivisions + 2
            indices.extend((a, b, d, a, d, c))  # CCW, normal +Z.

    header = struct.pack("<8sIIQQII", b"VCRMESH\0", 1, 48, len(vertices), len(indices), 1, 0)
    submesh = struct.pack("<QQII", 0, len(indices), 0, 0)
    data = header + submesh
    data += b"".join(struct.pack("<12f", *vertex) for vertex in vertices)
    data += struct.pack(f"<{len(indices)}I", *indices)
    # Independent binary sanity checks, not a replacement for the C++ loader.
    assert len(header) == 40 and len(submesh) == 24
    assert len(data) == 64 + 48 * len(vertices) + 4 * len(indices)
    assert len(indices) == 6 * subdivisions * subdivisions
    assert all(0 <= index < len(vertices) for index in indices)
    assert all(math.isfinite(value) for vertex in vertices for value in vertex)
    for offset in range(0, len(indices), 3):
        a, b, c = (vertices[indices[offset + i]] for i in range(3))
        assert (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) > 0
    unpacked = struct.unpack_from("<8sIIQQII", data)
    assert unpacked == (b"VCRMESH\0", 1, 48, len(vertices), len(indices), 1, 0)
    return data, len(vertices), len(indices)


def node(name: str, kind: str, translation: list[float]) -> dict:
    return {
        "name": name, "kind": kind, "parent": None,
        "visible": True, "expanded": False,
        "transform": {"translation": translation, "rotation_degrees": [0, 0, 0], "scale": [1, 1, 1]},
        "color": [1, 1, 1, 1],
        "pbr": {"metallic": 0, "roughness": 0.65, "ambient_occlusion": 1, "ior": 1.5, "model": "standard"},
    }


def generate_variant(output: Path, variant: str, camera_z: float) -> dict:
    root = output / variant
    material = {
        "schema": "vcr.asset", "version": 1, "kind": "material", "name": "LodFixture",
        "pbr": {"base_color": [0.25, 0.5, 0.75, 1], "metallic": 0, "roughness": 0.65,
                "ambient_occlusion": 1, "ior": 1.5, "model": "unlit"},
        "textures": {},
    }
    paths = [write_json(root, "assets/lod_fixture/materials/surface.vcrmaterial.json", material)]
    levels = []
    scales = (1.0, 1.0, 1.0) if variant == "coverage" else (1.0, 31 / 32, 7 / 8)
    for level, (subdivisions, half_extent) in enumerate(zip(SUBDIVISIONS, scales)):
        data, vertex_count, index_count = mesh(subdivisions, half_extent)
        paths.append(checked_write(root, f"assets/lod_fixture/mesh/lod{level}.vcrmesh", data))
        metadata = {
            "schema": "vcr.asset", "version": 1, "kind": "model", "name": f"LOD fixture {variant} {level}",
            "mesh": f"mesh/lod{level}.vcrmesh",
            "geometry": {"vertex_count": vertex_count, "index_count": index_count,
                         "index_type": "uint32", "primitive": "triangles", "vertex_stride": 48},
            "materials": ["materials/surface.vcrmaterial.json"],
            "submeshes": [{"name": "Surface", "first_index": 0, "index_count": index_count,
                           "material_index": 0, "material": "materials/surface.vcrmaterial.json"}],
        }
        if level == 0:
            metadata["lods"] = [
                {"model": f"lod_fixture/lod{lod}.vcrasset.json", "geometric_error": error}
                for lod, error in enumerate(ERRORS, start=1)
            ]
        paths.append(write_json(root, f"assets/lod_fixture/lod{level}.vcrasset.json", metadata))
        deviation = math.sqrt(2) * (1 - half_extent)
        if level:
            assert deviation <= ERRORS[level - 1]
        levels.append({"level": level, "vertices": vertex_count, "indices": index_count,
                       "triangles": index_count // 3, "half_extent": half_extent,
                       "actual_maximum_surface_deviation": deviation,
                       "declared_error": 0 if level == 0 else ERRORS[level - 1]})

    model = node("LOD fixture surface", "model", [0, 0, 0])
    model["asset_path"] = "lod_fixture/lod0.vcrasset.json"
    camera = node("Fixture camera", "camera", [0, 0, camera_z])
    scene = {"format": "vulkan_cube_scene", "version": 1, "startup_camera": 1,
             "lighting": {"dynamic": False, "voxel_gi": False, "cycle": False},
             "nodes": [model, camera]}
    paths.append(write_json(root, "assets/scenes/editor_scene.json", scene))
    return {
        "variant": variant, "asset_root": str(root / "assets"), "camera_z": camera_z,
        "levels": levels, "material": "opaque unlit, identical across LODs",
        "runtime_combined_vertices": sum(level["vertices"] for level in levels),
        "runtime_combined_indices": sum(level["indices"] for level in levels),
        "files": [{"path": str(path.relative_to(output)), "bytes": path.stat().st_size,
                   "sha256": digest(path)} for path in paths],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configuration", choices=TREES, default="Debug")
    parser.add_argument("--camera-z", type=float, default=12.0,
                        help="Generated startup-camera distance, >0.1 and <=400; does not change runtime CLI")
    options = parser.parse_args()
    if os.name != "nt":
        parser.error("Run with native Windows Python; Linux/WSL Python is outside the project policy")
    if not math.isfinite(options.camera_z) or not 0.1 < options.camera_z <= 400:
        parser.error("--camera-z must be finite, >0.1 and <=400")
    build_tree = (ROOT / "build" / TREES[options.configuration]).resolve()
    if not build_tree.is_dir():
        parser.error("The selected approved build tree must already exist")
    output = build_tree / "lod-fixture"
    output.resolve().relative_to(build_tree)
    manifest = output / "fixture-manifest.json"
    if output.exists() and any(output.iterdir()):
        if not manifest.is_file() or json.loads(manifest.read_text(encoding="utf-8")).get("generator") != GENERATOR:
            parser.error("Refusing to overwrite a nonempty folder without this fixture's ownership manifest")
    canonical = ROOT / "assets/scenes/editor_scene.json"
    before = digest(canonical)
    variants = [generate_variant(output, variant, options.camera_z) for variant in ("coverage", "geometry")]
    if digest(canonical) != before:
        raise RuntimeError("Canonical scene changed during generation; inspect concurrent changes")
    result = {"generator": GENERATOR, "configuration": options.configuration,
              "canonical_scene_sha256": before, "canonical_scene_unchanged": True,
              "native_windows": True, "cooked_loader_executed": False, "gpu_executed": False,
              "variants": variants}
    write_json(output, "fixture-manifest.json", result)
    print(json.dumps({"manifest": str(manifest), "asset_roots": [v["asset_root"] for v in variants],
                      "triangles_per_lod": [128, 32, 2], "canonical_scene_unchanged": True}))


if __name__ == "__main__":
    main()
