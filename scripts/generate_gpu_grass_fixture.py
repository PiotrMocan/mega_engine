"""Generate procedural GPU grass patch LOD assets and a dense meadow fixture scene.

Uses native Windows Python and the standard library only. No renderer, shader
compiler, CMake configure/build, or GPU process is launched. Output is restricted
to an existing approved Windows build tree (`windows-debug`, `windows-release`,
or `windows-asan`) and protected by an ownership manifest so unrelated folders
are never overwritten. Canonical repository assets and scenes remain untouched.
See docs/GPU_GRASS.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct


GENERATOR = "vcr.gpu-grass-fixture.v1"
ROOT = Path(__file__).resolve().parents[1]
TREES = {
    "Debug": "windows-debug",
    "Release": "windows-release",
    "ASan": "windows-asan",
}
RESERVED_BUILD_SUBDIRS = frozenset({
    "bin",
    "lib",
    "cmakefiles",
    "_deps",
    "generated",
    "testing",
})

# Mirror constants from src/gpu_grass.hpp.
MAX_BLADES_PER_PATCH = 4096
UV_V_BIAS = -2.0
PATCH_ROOT_HALF_EXTENT = 0.44
MIN_BLADE_HEIGHT = 0.34
MAX_BLADE_HEIGHT = 0.68
MIN_BLADE_HALF_WIDTH = 0.011
MAX_BLADE_HALF_WIDTH = 0.020
MIN_NATURAL_BEND = 0.06
MAX_NATURAL_BEND = 0.22
MAX_WIND_DISPLACEMENT = 0.34
DEFAULT_SUBSURFACE_STRENGTH = 0.65
REFERENCE_POSITION_TOLERANCE = 2.5e-3

CORNER_LEFT = 0
CORNER_TIP = 1
CORNER_RIGHT = 2

# Nested blade prefixes [0..N_lod) and dyadic segment counts so every vertex of
# LOD1/LOD2/LOD3 is bit-for-bit identical to its LOD0 counterpart.
LOD_SPECS: tuple[tuple[int, int], ...] = (
    (128, 4),
    (64, 2),
    (32, 2),
    (16, 1),
)

MATERIAL_NAME = "GpuGrassPatch"
MATERIAL_REL = "materials/grass_blade.vcrmaterial.json"
MODEL_ASSET_REL = "grass_patch/lod0.vcrasset.json"


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def clamp_f32(value: float, low: float, high: float) -> float:
    return f32(max(f32(low), min(f32(high), f32(value))))


def mix_f32(a: float, b: float, t: float) -> float:
    fa, fb, ft = f32(a), f32(b), f32(t)
    return f32(fa + f32(f32(fb - fa) * ft))


def add3_f32(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
    return (f32(a[0] + b[0]), f32(a[1] + b[1]), f32(a[2] + b[2]))


def scale3_f32(v: tuple[float, float, float], s: float) -> tuple[float, float, float]:
    fs = f32(s)
    return (f32(v[0] * fs), f32(v[1] * fs), f32(v[2] * fs))


def cross3_f32(
    a: tuple[float, float, float],
    b: tuple[float, float, float],
) -> tuple[float, float, float]:
    return (
        f32(f32(a[1] * b[2]) - f32(a[2] * b[1])),
        f32(f32(a[2] * b[0]) - f32(a[0] * b[2])),
        f32(f32(a[0] * b[1]) - f32(a[1] * b[0])),
    )


def normalize3_f32(v: tuple[float, float, float]) -> tuple[float, float, float]:
    len_sq = f32(f32(f32(v[0] * v[0]) + f32(v[1] * v[1])) + f32(v[2] * v[2]))
    inv_len = f32(1.0 / f32(math.sqrt(len_sq)))
    return (f32(v[0] * inv_len), f32(v[1] * inv_len), f32(v[2] * inv_len))


def gpu_grass_hash_u32(value: int) -> int:
    v = value & 0xFFFFFFFF
    v ^= v >> 16
    v = (v * 0x7FEB352D) & 0xFFFFFFFF
    v ^= v >> 15
    v = (v * 0x846CA68B) & 0xFFFFFFFF
    v ^= v >> 16
    return v


def gpu_grass_hash_unit(seed: int) -> float:
    return f32((gpu_grass_hash_u32(seed) & 0x00FFFFFF) * (1.0 / 16777216.0))


def encode_gpu_grass_uv(blade_index: int, corner: int, height_t: float) -> tuple[float, float]:
    if not (0 <= blade_index < MAX_BLADES_PER_PATCH):
        raise ValueError(f"blade_index {blade_index} outside [0, {MAX_BLADES_PER_PATCH})")
    if corner not in (CORNER_LEFT, CORNER_TIP, CORNER_RIGHT):
        raise ValueError(f"invalid grass corner code {corner}")
    clamped_t = clamp_f32(height_t, 0.0, 1.0)
    if corner == CORNER_TIP and clamped_t < 0.999:
        raise ValueError("center_tip corner requires height_t == 1.0")
    packed_u = (blade_index << 2) | corner
    return (f32(float(packed_u)), f32(f32(UV_V_BIAS) + clamped_t))


def blade_parameters(blade_index: int) -> dict[str, float | tuple[float, float]]:
    two_pi = f32(6.28318530718)
    r2_x = f32(((blade_index * 12664745 + 4538167) & 0x00FFFFFF) * (1.0 / 16777216.0))
    r2_z = f32(((blade_index * 9560333 + 9142341) & 0x00FFFFFF) * (1.0 / 16777216.0))
    seed_base = (blade_index * 8) & 0xFFFFFFFF
    jitter_x = f32(f32(gpu_grass_hash_unit(seed_base + 0) - f32(0.5)) * f32(0.06))
    jitter_z = f32(f32(gpu_grass_hash_unit(seed_base + 7) - f32(0.5)) * f32(0.06))
    inner_extent = f32(f32(PATCH_ROOT_HALF_EXTENT) - f32(0.03))
    root_x = clamp_f32(
        f32(f32(f32(r2_x * f32(2.0) - f32(1.0)) * inner_extent) + jitter_x),
        -PATCH_ROOT_HALF_EXTENT,
        PATCH_ROOT_HALF_EXTENT,
    )
    root_z = clamp_f32(
        f32(f32(f32(r2_z * f32(2.0) - f32(1.0)) * inner_extent) + jitter_z),
        -PATCH_ROOT_HALF_EXTENT,
        PATCH_ROOT_HALF_EXTENT,
    )
    yaw = f32(gpu_grass_hash_unit(seed_base + 1) * two_pi)
    blade_dir = (f32(math.cos(yaw)), f32(math.sin(yaw)))
    blade_side = (f32(-blade_dir[1]), blade_dir[0])
    height = mix_f32(MIN_BLADE_HEIGHT, MAX_BLADE_HEIGHT, gpu_grass_hash_unit(seed_base + 2))
    base_half_width = mix_f32(
        MIN_BLADE_HALF_WIDTH,
        MAX_BLADE_HALF_WIDTH,
        gpu_grass_hash_unit(seed_base + 3),
    )
    natural_bend = mix_f32(
        MIN_NATURAL_BEND,
        MAX_NATURAL_BEND,
        gpu_grass_hash_unit(seed_base + 4),
    )
    stiffness = mix_f32(0.65, 1.15, gpu_grass_hash_unit(seed_base + 5))
    return {
        "root_xz": (root_x, root_z),
        "blade_dir": blade_dir,
        "blade_side": blade_side,
        "height": height,
        "base_half_width": base_half_width,
        "natural_bend": natural_bend,
        "stiffness": stiffness,
    }


def evaluate_reference_blade_vertex(
    blade_index: int,
    corner: int,
    height_t: float,
) -> tuple[float, ...]:
    """Mirror vcr::evaluate_gpu_grass_blade(..., wind_seconds=0.0F) in binary32."""
    params = blade_parameters(blade_index)
    root_x, root_z = params["root_xz"]  # type: ignore[misc]
    dir_x, dir_z = params["blade_dir"]  # type: ignore[misc]
    side_x, side_z = params["blade_side"]  # type: ignore[misc]
    height = float(params["height"])  # type: ignore[arg-type]
    base_half_width = float(params["base_half_width"])  # type: ignore[arg-type]
    natural_bend = float(params["natural_bend"])  # type: ignore[arg-type]

    t = clamp_f32(height_t, 0.0, 1.0)
    clamped_side = clamp_f32(corner * 0.5, 0.0, 1.0)
    signed_side = f32(f32(clamped_side * f32(2.0)) - f32(1.0))
    width_profile = f32(
        f32(1.0) - f32(f32(t * t) * f32(f32(0.65) + f32(f32(0.35) * t)))
    )
    across_scale = f32(f32(signed_side * base_half_width) * width_profile)
    across_x = f32(side_x * across_scale)
    across_z = f32(side_z * across_scale)

    bend_x = f32(dir_x * natural_bend)
    bend_z = f32(dir_z * natural_bend)
    bend_mag_sq = f32(f32(bend_x * bend_x) + f32(bend_z * bend_z))
    vertical_scale = f32(1.0 / f32(math.sqrt(f32(1.0 + f32(f32(0.85) * bend_mag_sq)))))
    bend_factor = f32(t * t)

    spine_pos = (
        f32(root_x + f32(bend_x * bend_factor)),
        f32(f32(height * t) * vertical_scale),
        f32(root_z + f32(bend_z * bend_factor)),
    )
    d_spine_dt = (
        f32(bend_x * f32(2.0 * t)),
        f32(height * vertical_scale),
        f32(bend_z * f32(2.0 * t)),
    )

    local_tangent = normalize3_f32((side_x, f32(0.0), side_z))
    spine_dir = normalize3_f32(d_spine_dt)
    face_normal = normalize3_f32(cross3_f32(spine_dir, local_tangent))
    curved_normal = normalize3_f32(
        add3_f32(face_normal, scale3_f32(local_tangent, f32(signed_side * f32(0.28))))
    )
    uv_u, uv_v = encode_gpu_grass_uv(blade_index, corner, t)

    return (
        f32(spine_pos[0] + across_x),
        spine_pos[1],
        f32(spine_pos[2] + across_z),
        curved_normal[0],
        curved_normal[1],
        curved_normal[2],
        uv_u,
        uv_v,
        local_tangent[0],
        local_tangent[1],
        local_tangent[2],
        f32(1.0),
    )


def build_patch_lod_mesh(
    blade_count: int,
    segments: int,
) -> tuple[bytes, list[tuple[float, ...]], list[int]]:
    if not (1 <= blade_count <= MAX_BLADES_PER_PATCH):
        raise ValueError("blade_count out of valid gpu_grass patch bounds")
    if not (1 <= segments <= 16) or (1.0 / segments) < 0.04:
        raise ValueError("segments must keep per-triangle height delta >= 0.04")

    vertices: list[tuple[float, ...]] = []
    indices: list[int] = []

    for blade_index in range(blade_count):
        base_vertex = len(vertices)
        for row in range(segments):
            height_t = row / segments
            vertices.append(evaluate_reference_blade_vertex(blade_index, CORNER_LEFT, height_t))
            vertices.append(evaluate_reference_blade_vertex(blade_index, CORNER_RIGHT, height_t))
        tip_index = base_vertex + 2 * segments
        vertices.append(evaluate_reference_blade_vertex(blade_index, CORNER_TIP, 1.0))

        for seg in range(segments - 1):
            l0 = base_vertex + 2 * seg
            r0 = l0 + 1
            l1 = l0 + 2
            r1 = l0 + 3
            indices.extend((l0, l1, r0, r0, l1, r1))

        last_left = base_vertex + 2 * (segments - 1)
        last_right = last_left + 1
        indices.extend((last_left, tip_index, last_right))

    header = struct.pack(
        "<8sIIQQII",
        b"VCRMESH\0",
        1,
        48,
        len(vertices),
        len(indices),
        1,
        0,
    )
    submesh = struct.pack("<QQII", 0, len(indices), 0, 0)
    payload = b"".join(struct.pack("<12f", *v) for v in vertices)
    index_bytes = struct.pack(f"<{len(indices)}I", *indices)
    data = header + submesh + payload + index_bytes

    # Independent binary and contract checks mirroring CookedModelAsset + gpu_grass.hpp.
    assert len(header) == 40 and len(submesh) == 24
    assert len(vertices) == blade_count * (2 * segments + 1)
    assert len(indices) == blade_count * (2 * segments - 1) * 3
    assert len(data) == 64 + 48 * len(vertices) + 4 * len(indices)
    assert all(0 <= idx < len(vertices) for idx in indices)
    assert all(math.isfinite(val) for vertex in vertices for val in vertex)

    for offset in range(0, len(indices), 3):
        tri = [vertices[indices[offset + c]] for c in range(3)]
        blades_in_tri = []
        corners_in_tri = []
        heights_in_tri = []
        for v in tri:
            packed_u = round(v[6])
            assert abs(v[6] - packed_u) <= 1.0e-5 and packed_u >= 0
            b_idx = packed_u >> 2
            c_bits = packed_u & 3
            h_t = v[7] - UV_V_BIAS
            assert 0 <= b_idx < blade_count and c_bits in (0, 1, 2)
            assert -1.0e-5 <= h_t <= 1.0 + 1.0e-5
            if c_bits == CORNER_TIP:
                assert h_t >= 0.999
            ref = evaluate_reference_blade_vertex(b_idx, c_bits, h_t)
            pos_err = math.dist(v[0:3], ref[0:3])
            assert pos_err <= REFERENCE_POSITION_TOLERANCE
            blades_in_tri.append(b_idx)
            corners_in_tri.append(c_bits)
            heights_in_tri.append(h_t)

        assert blades_in_tri[0] == blades_in_tri[1] == blades_in_tri[2]
        assert not (corners_in_tri[0] == corners_in_tri[1] == corners_in_tri[2])
        assert max(heights_in_tri) - min(heights_in_tri) >= 0.04

        e1 = (tri[1][0] - tri[0][0], tri[1][1] - tri[0][1], tri[1][2] - tri[0][2])
        e2 = (tri[2][0] - tri[0][0], tri[2][1] - tri[0][1], tri[2][2] - tri[0][2])
        geom_n = (
            e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0],
        )
        avg_n = (
            tri[0][3] + tri[1][3] + tri[2][3],
            tri[0][4] + tri[1][4] + tri[2][4],
            tri[0][5] + tri[1][5] + tri[2][5],
        )
        assert (geom_n[0] * avg_n[0] + geom_n[1] * avg_n[1] + geom_n[2] * avg_n[2]) > 0.0

    return data, vertices, indices


def compute_conservative_lod_errors() -> list[dict[str, float]]:
    """Derive conservative model-local geometric errors from LOD0, including blade removals.

    Returns per-level diagnostics for LOD0..LOD3 with:
      - actual_t0_surface_deviation: sampled Hausdorff deviation of LOD0 reference
        geometry from LODk piecewise-linear reference geometry at wind_seconds = 0;
      - analytic_wind_upper_bound: conservative upper bound across all height parameters,
        blade widths, segment chordal approximations, and arbitrary wind phases/rotations;
      - declared_error: strictly increasing dyadic (1/64) upper bound written to metadata.
    """
    lod0_blades, lod0_segments = LOD_SPECS[0]
    info = []
    for b in range(lod0_blades):
        p = blade_parameters(b)
        rx, rz = p["root_xz"]  # type: ignore[misc]
        dx, dz = p["blade_dir"]  # type: ignore[misc]
        h = float(p["height"])  # type: ignore[arg-type]
        w = float(p["base_half_width"])  # type: ignore[arg-type]
        nb = float(p["natural_bend"])  # type: ignore[arg-type]
        st = float(p["stiffness"])  # type: ignore[arg-type]
        info.append((float(rx), float(rz), float(dx * nb), float(dz * nb), h, w, nb, st))

    def sample_mesh_points(blade_count: int, segments: int, subdivisions: int) -> list[tuple[float, float, float]]:
        pts: list[tuple[float, float, float]] = []
        for b in range(blade_count):
            for seg in range(segments):
                t0 = seg / segments
                t1 = (seg + 1) / segments
                c_left_1 = CORNER_TIP if seg + 1 == segments else CORNER_LEFT
                c_right_1 = CORNER_TIP if seg + 1 == segments else CORNER_RIGHT
                l0 = evaluate_reference_blade_vertex(b, CORNER_LEFT, t0)[0:3]
                r0 = evaluate_reference_blade_vertex(b, CORNER_RIGHT, t0)[0:3]
                l1 = evaluate_reference_blade_vertex(b, c_left_1, t1)[0:3]
                r1 = evaluate_reference_blade_vertex(b, c_right_1, t1)[0:3]
                for iu in range(subdivisions + 1):
                    u = iu / subdivisions
                    left_u = (
                        l0[0] + (l1[0] - l0[0]) * u,
                        l0[1] + (l1[1] - l0[1]) * u,
                        l0[2] + (l1[2] - l0[2]) * u,
                    )
                    right_u = (
                        r0[0] + (r1[0] - r0[0]) * u,
                        r0[1] + (r1[1] - r0[1]) * u,
                        r0[2] + (r1[2] - r0[2]) * u,
                    )
                    for side_w in (0.0, 0.5, 1.0):
                        pts.append((
                            left_u[0] + (right_u[0] - left_u[0]) * side_w,
                            left_u[1] + (right_u[1] - left_u[1]) * side_w,
                            left_u[2] + (right_u[2] - left_u[2]) * side_w,
                        ))
        return pts

    def pair_wind_bound(bi: int, bj: int, segs_k: int) -> float:
        rxi, rzi, bxi, bzi, hi, wi, nbi, sti = info[bi]
        rxj, rzj, bxj, bzj, hj, wj, nbj, stj = info[bj]
        dr = math.hypot(rxi - rxj, rzi - rzj)
        db0 = math.hypot(bxi - bxj, bzi - bzj)
        # Instance scale/shear and world-space gust phase are unrestricted.
        # Bound independent phases instead of assuming world distance == local distance.
        dsin1 = 2.0
        dalong = (
            0.1144 * abs(sti - stj)
            + 0.066 * (abs(sti - stj) + min(sti, stj) * dsin1)
            + 0.0796 * (sti + stj)
        )
        dcross = 0.045 * (sti + stj)
        dwind = math.hypot(dalong, dcross)

        max_bj_bend = nbj + MAX_WIND_DISPLACEMENT
        chord_j = (0.25 * max_bj_bend + 0.425 * wj) / (segs_k * segs_k)
        chord_i = (0.25 * (nbi + MAX_WIND_DISPLACEMENT) + 0.425 * wi) / (lod0_segments * lod0_segments)

        vs_min_i = 1.0 / math.sqrt(1.0 + 0.85 * (nbi + MAX_WIND_DISPLACEMENT) ** 2)
        vs_max_i = 1.0 / math.sqrt(1.0 + 0.85 * max(0.0, nbi - MAX_WIND_DISPLACEMENT) ** 2)
        vs_min_j = 1.0 / math.sqrt(1.0 + 0.85 * (nbj + MAX_WIND_DISPLACEMENT) ** 2)
        vs_max_j = 1.0 / math.sqrt(1.0 + 0.85 * max(0.0, nbj - MAX_WIND_DISPLACEMENT) ** 2)
        dy_max = max(abs(hi * vs_max_i - hj * vs_min_j), abs(hj * vs_max_j - hi * vs_min_i))

        # Independent maxima of t, t*t and width profile bound the entire
        # continuous blade; chord bounds cover both piecewise-linear meshes.
        # This does not infer a continuous maximum from a finite sample grid.
        dxz = dr + db0 + dwind + wi + wj + chord_i + chord_j
        return math.hypot(dxz, dy_max) + 1.0e-4

    lod0_samples = sample_mesh_points(lod0_blades, lod0_segments, subdivisions=2)
    diagnostics: list[dict[str, float]] = [{
        "actual_t0_surface_deviation": 0.0,
        "analytic_wind_upper_bound": 0.0,
        "declared_error": 0.0,
    }]

    previous_declared = 0.0
    for blade_count, segments in LOD_SPECS[1:]:
        kept_samples = sample_mesh_points(blade_count, segments, subdivisions=4)
        t0_deviation = max(
            min(math.dist(p, q) for q in kept_samples)
            for p in lod0_samples
        )
        kept_chord_bound = max(
            (0.25 * (info[j][6] + MAX_WIND_DISPLACEMENT) + 0.425 * info[j][5]) / (segments * segments)
            for j in range(blade_count)
        )
        if blade_count < lod0_blades:
            removed_bound = max(
                min(pair_wind_bound(i, j, segments) for j in range(blade_count))
                for i in range(blade_count, lod0_blades)
            )
        else:
            removed_bound = 0.0

        analytic_bound = max(t0_deviation, kept_chord_bound, removed_bound)
        declared = math.ceil(analytic_bound * 64.0) / 64.0
        if declared <= previous_declared:
            declared = previous_declared + (1.0 / 64.0)

        assert 0.0 < t0_deviation <= analytic_bound <= declared
        assert declared > previous_declared
        previous_declared = declared
        diagnostics.append({
            "actual_t0_surface_deviation": round(t0_deviation, 6),
            "analytic_wind_upper_bound": round(analytic_bound, 6),
            "declared_error": declared,
        })

    return diagnostics


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ensure_no_symlinks_under(base: Path, target: Path) -> None:
    resolved_base = base.resolve()
    relative_parts = target.relative_to(base).parts if target.is_relative_to(base) else target.resolve().relative_to(resolved_base).parts
    cursor = resolved_base
    for part in relative_parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise RuntimeError(f"Refusing to traverse symbolic link: {cursor}")


def resolve_approved_output_dir(
    parser: argparse.ArgumentParser,
    configuration: str,
    explicit_output: str | None,
) -> tuple[Path, str]:
    approved_trees: dict[str, Path] = {}
    for cfg_name, folder_name in TREES.items():
        candidate = ROOT / "build" / folder_name
        if candidate.is_symlink():
            parser.error(f"Approved build tree cannot be a symbolic link: {candidate}")
        approved_trees[cfg_name] = candidate.resolve()

    if explicit_output is None:
        build_tree = approved_trees[configuration]
        if not build_tree.is_dir():
            parser.error(f"Approved build tree does not exist: {build_tree}")
        target = build_tree / "gpu-grass-fixture"
        ensure_no_symlinks_under(build_tree, target)
        return target.resolve(), configuration

    raw_target = Path(explicit_output)
    if not raw_target.is_absolute():
        raw_target = Path.cwd() / raw_target
    if raw_target.is_symlink():
        parser.error("Target path cannot be a symbolic link")
    resolved_target = raw_target.resolve()

    matched_cfg: str | None = None
    matched_tree: Path | None = None
    for cfg_name, tree_path in approved_trees.items():
        if resolved_target == tree_path:
            parser.error("Refusing to write directly into the root of a build tree; use a dedicated subdirectory")
        if resolved_target.is_relative_to(tree_path):
            matched_cfg = cfg_name
            matched_tree = tree_path
            break

    if matched_tree is None or matched_cfg is None:
        parser.error(
            "Target directory must reside strictly inside an approved build tree "
            "(build/windows-debug, build/windows-release, or build/windows-asan)"
        )
    if not matched_tree.is_dir():
        parser.error(f"Approved build tree does not exist: {matched_tree}")

    rel_parts = resolved_target.relative_to(matched_tree).parts
    if not rel_parts or rel_parts[0].lower() in RESERVED_BUILD_SUBDIRS:
        parser.error(f"Refusing to write inside reserved build directory: {rel_parts[0] if rel_parts else matched_tree}")

    ensure_no_symlinks_under(matched_tree, resolved_target)
    return resolved_target, matched_cfg


def checked_write(root: Path, relative: str, data: bytes) -> Path:
    rel_path = Path(relative)
    if rel_path.is_absolute() or ".." in rel_path.parts:
        raise ValueError(f"Invalid relative fixture path: {relative}")
    destination = root / rel_path
    resolved_root = root.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    ensure_no_symlinks_under(resolved_root, destination)
    destination.resolve().relative_to(resolved_root)
    destination.write_bytes(data)
    return destination


def write_json(root: Path, relative: str, payload: object) -> Path:
    encoded = (json.dumps(payload, indent=2, allow_nan=False) + "\n").encode("utf-8")
    return checked_write(root, relative, encoded)


def make_scene_node(
    name: str,
    kind: str,
    parent: int | None,
    translation: list[float],
    rotation_degrees: list[float] | None = None,
    scale: list[float] | None = None,
    color: list[float] | None = None,
    pbr: dict[str, object] | None = None,
    expanded: bool = False,
    asset_path: str | None = None,
) -> dict[str, object]:
    node_data: dict[str, object] = {
        "name": name,
        "kind": kind,
        "parent": parent,
        "visible": True,
        "expanded": expanded,
        "transform": {
            "translation": translation,
            "rotation_degrees": rotation_degrees if rotation_degrees is not None else [0.0, 0.0, 0.0],
            "scale": scale if scale is not None else [1.0, 1.0, 1.0],
        },
        "color": color if color is not None else [1.0, 1.0, 1.0, 1.0],
        "pbr": pbr if pbr is not None else {
            "metallic": 0.0,
            "roughness": 0.65,
            "ambient_occlusion": 1.0,
            "ior": 1.5,
            "model": "standard",
            "subsurface_profile": "none",
            "subsurface_strength": 0.0,
        },
    }
    if asset_path is not None:
        node_data["asset_path"] = asset_path
    return node_data


def build_meadow_scene(grid_x: int, grid_z: int, patch_spacing: float) -> dict[str, object]:
    grass_color = [0.14, 0.34, 0.07, 1.0]
    grass_pbr: dict[str, object] = {
        "metallic": 0.0,
        "roughness": 0.58,
        "ambient_occlusion": 1.0,
        "ior": 1.45,
        "model": "gpu_grass",
        "subsurface_profile": "foliage",
        "subsurface_strength": DEFAULT_SUBSURFACE_STRENGTH,
    }
    ground_color = [0.06, 0.10, 0.04, 1.0]
    ground_pbr: dict[str, object] = {
        "metallic": 0.0,
        "roughness": 0.92,
        "ambient_occlusion": 1.0,
        "ior": 1.5,
        "model": "standard",
        "subsurface_profile": "none",
        "subsurface_strength": 0.0,
    }

    meadow_span_x = (grid_x - 1) * patch_spacing
    meadow_span_z = (grid_z - 1) * patch_spacing
    near_z = 3.4
    center_z = near_z - 0.5 * meadow_span_z
    ground_half_extent = max(80.0, 0.5 * max(meadow_span_x, meadow_span_z) + 24.0)

    nodes: list[dict[str, object]] = [
        make_scene_node("World", "folder", None, [0.0, 0.0, 0.0], expanded=True),
        make_scene_node("Sky", "environment", 0, [0.0, 0.0, 0.0]),
        make_scene_node("Sun", "directional_light", 0, [0.0, 12.0, 0.0]),
        make_scene_node(
            "Meadow Ground",
            "ground",
            0,
            [0.0, 0.0, round(center_z, 4)],
            scale=[round(ground_half_extent, 4), 1.0, round(ground_half_extent, 4)],
            color=ground_color,
            pbr=ground_pbr,
        ),
        make_scene_node(
            "Close Meadow Camera",
            "camera",
            0,
            [0.0, 1.05, 1.8],
            rotation_degrees=[-15.0, 0.0, 0.0],
        ),
        make_scene_node("Grass Meadow", "folder", 0, [0.0, 0.0, 0.0], expanded=False),
    ]

    jitter_limit = min(0.14, 0.25 * patch_spacing)
    patch_index = 0
    for iz in range(grid_z):
        for ix in range(grid_x):
            seed = (patch_index * 4 + 17) & 0xFFFFFFFF
            jx = (gpu_grass_hash_unit(seed + 0) * 2.0 - 1.0) * jitter_limit
            jz = (gpu_grass_hash_unit(seed + 1) * 2.0 - 1.0) * jitter_limit
            yaw_deg = gpu_grass_hash_unit(seed + 2) * 360.0
            uniform_scale = 0.92 + 0.16 * gpu_grass_hash_unit(seed + 3)

            world_x = (ix - (grid_x - 1) * 0.5) * patch_spacing + jx
            world_z = near_z - iz * patch_spacing + jz
            s_val = round(uniform_scale, 4)

            nodes.append(
                make_scene_node(
                    f"Grass Patch {patch_index:05d}",
                    "model",
                    5,
                    [round(world_x, 4), 0.0, round(world_z, 4)],
                    rotation_degrees=[0.0, round(yaw_deg, 3), 0.0],
                    scale=[s_val, s_val, s_val],
                    color=grass_color,
                    # Model nodes use their cooked asset's material; the node
                    # PBR payload belongs to primitive geometry only.
                    pbr={**grass_pbr, "model": "standard"},
                    asset_path=MODEL_ASSET_REL,
                )
            )
            patch_index += 1

    return {
        "format": "vulkan_cube_scene",
        "version": 1,
        "startup_camera": 4,
        "subsurface": "full",
        "tone_mapper": "gt7",
        "lighting": {
            "dynamic": True,
            "voxel_gi": False,
            "cycle": False,
            "hour": 15.5,
            "day_seconds": 600.0,
            "gi_extent": 64.0,
            "gi_strength": 1.0,
            "triangle_budget": 65536,
            "gi_update_hz": 30.0,
            "shadow_resolution": 2048,
            "shadow_depth": "auto",
            "cascade_cache": False,
            "ray_tracing": "off",
        },
        "nodes": nodes,
    }


def generate_fixture(
    output_dir: Path,
    configuration: str,
    grid_x: int,
    grid_z: int,
    patch_spacing: float,
) -> dict[str, object]:
    asset_root = output_dir / "assets"
    written_paths: list[Path] = []

    material_json = {
        "schema": "vcr.asset",
        "version": 1,
        "kind": "material",
        "name": MATERIAL_NAME,
        "alpha_mode": "opaque",
        "double_sided": True,
        "pbr": {
            "base_color": [0.14, 0.34, 0.07, 1.0],
            "metallic": 0.0,
            "roughness": 0.58,
            "ambient_occlusion": 1.0,
            "ior": 1.45,
            "model": "gpu_grass",
            "subsurface_profile": "foliage",
            "subsurface_strength": DEFAULT_SUBSURFACE_STRENGTH,
        },
        "textures": {},
    }
    written_paths.append(
        write_json(output_dir, f"assets/grass_patch/{MATERIAL_REL}", material_json)
    )

    lod_diagnostics = compute_conservative_lod_errors()
    lod_references = [
        {
            "model": f"grass_patch/lod{level}.vcrasset.json",
            "geometric_error": lod_diagnostics[level]["declared_error"],
        }
        for level in range(1, len(LOD_SPECS))
    ]

    unique_vertices: set[bytes] = set()
    lod0_vertex_count = 0
    level_summaries: list[dict[str, object]] = []

    for level, (blade_count, segments) in enumerate(LOD_SPECS):
        mesh_bytes, vertices, indices = build_patch_lod_mesh(blade_count, segments)
        for v in vertices:
            unique_vertices.add(struct.pack("<12f", *v))
        if level == 0:
            lod0_vertex_count = len(vertices)

        mesh_rel = f"mesh/lod{level}.vcrmesh"
        written_paths.append(
            checked_write(output_dir, f"assets/grass_patch/{mesh_rel}", mesh_bytes)
        )

        model_json: dict[str, object] = {
            "schema": "vcr.asset",
            "version": 1,
            "kind": "model",
            "name": f"GPU Grass Patch LOD{level}",
            "mesh": mesh_rel,
            "generate_lods": False,
            "geometry": {
                "vertex_count": len(vertices),
                "index_count": len(indices),
                "index_type": "uint32",
                "primitive": "triangles",
                "vertex_stride": 48,
            },
            "materials": [MATERIAL_REL],
            "submeshes": [
                {
                    "name": "GrassBlades",
                    "first_index": 0,
                    "index_count": len(indices),
                    "material_index": 0,
                    "material": MATERIAL_REL,
                }
            ],
        }
        if level == 0:
            model_json["lods"] = lod_references

        written_paths.append(
            write_json(output_dir, f"assets/grass_patch/lod{level}.vcrasset.json", model_json)
        )

        diag = lod_diagnostics[level]
        level_summaries.append({
            "level": level,
            "blades": blade_count,
            "segments_per_blade": segments,
            "vertices": len(vertices),
            "indices": len(indices),
            "triangles": len(indices) // 3,
            "actual_t0_surface_deviation": diag["actual_t0_surface_deviation"],
            "analytic_wind_upper_bound": diag["analytic_wind_upper_bound"],
            "declared_geometric_error": diag["declared_error"],
        })

    # Every LOD1/LOD2/LOD3 vertex must deduplicate losslessly into LOD0's vertex set.
    assert len(unique_vertices) == lod0_vertex_count

    scene_json = build_meadow_scene(grid_x, grid_z, patch_spacing)
    written_paths.append(write_json(output_dir, "assets/scenes/editor_scene.json", scene_json))

    patch_instances = grid_x * grid_z
    lod0_tris = int(level_summaries[0]["triangles"])  # type: ignore[arg-type]
    return {
        "generator": GENERATOR,
        "configuration": configuration,
        "asset_root": str(asset_root),
        "scene_path": str(asset_root / "scenes" / "editor_scene.json"),
        "shared_model_asset": MODEL_ASSET_REL,
        "shared_material_asset": f"grass_patch/{MATERIAL_REL}",
        "patch_instances": patch_instances,
        "grid_dimensions": [grid_x, grid_z],
        "patch_spacing_meters": patch_spacing,
        "lod0_total_blades": patch_instances * LOD_SPECS[0][0],
        "lod0_total_triangles": patch_instances * lod0_tris,
        "runtime_deduplicated_shared_vertices": len(unique_vertices),
        "runtime_combined_indices": sum(int(item["indices"]) for item in level_summaries),  # type: ignore[arg-type]
        "levels": level_summaries,
        "native_windows": True,
        "cooked_loader_executed": False,
        "gpu_executed": False,
        "files": [
            {
                "path": str(path.relative_to(output_dir)),
                "bytes": path.stat().st_size,
                "sha256": digest(path),
            }
            for path in written_paths
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--configuration",
        choices=tuple(TREES.keys()),
        default="Release",
        help="Approved existing Windows build configuration (default: Release)",
    )
    parser.add_argument(
        "--output",
        default=None,
        help="Optional explicit target subdirectory inside an existing approved build tree",
    )
    parser.add_argument(
        "--grid-x",
        type=int,
        default=64,
        help="Meadow patch count along X (1..250, default: 64)",
    )
    parser.add_argument(
        "--grid-z",
        type=int,
        default=64,
        help="Meadow patch count along Z (1..250, default: 64)",
    )
    parser.add_argument(
        "--patch-spacing",
        type=float,
        default=0.68,
        help="Center-to-center patch spacing in meters (0.25..4.0, default: 0.68)",
    )
    options = parser.parse_args()

    if os.name != "nt":
        parser.error("Run with native Windows Python; Linux/WSL Python is outside the project policy")
    if not (1 <= options.grid_x <= 250) or not (1 <= options.grid_z <= 250):
        parser.error("--grid-x and --grid-z must each be inside [1, 250]")
    if options.grid_x * options.grid_z > 50_000:
        parser.error("Total patch count (grid_x * grid_z) must stay <= 50000")
    if not math.isfinite(options.patch_spacing) or not (0.25 <= options.patch_spacing <= 4.0):
        parser.error("--patch-spacing must be finite and inside [0.25, 4.0]")

    output_dir, resolved_cfg = resolve_approved_output_dir(
        parser,
        options.configuration,
        options.output,
    )
    manifest_path = output_dir / "fixture-manifest.json"
    if output_dir.exists() and any(output_dir.iterdir()):
        if not manifest_path.is_file():
            parser.error("Refusing to overwrite a nonempty folder without this fixture's ownership manifest")
        try:
            existing_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            parser.error(f"Unable to verify existing fixture ownership manifest: {exc}")
        if existing_manifest.get("generator") != GENERATOR:
            parser.error("Refusing to overwrite a folder owned by a different generator")

    canonical_scene = ROOT / "assets" / "scenes" / "editor_scene.json"
    before_sha = digest(canonical_scene) if canonical_scene.is_file() else None

    result = generate_fixture(
        output_dir=output_dir,
        configuration=resolved_cfg,
        grid_x=options.grid_x,
        grid_z=options.grid_z,
        patch_spacing=options.patch_spacing,
    )

    after_sha = digest(canonical_scene) if canonical_scene.is_file() else None
    if before_sha != after_sha:
        raise RuntimeError("Canonical scene changed during fixture generation; inspect concurrent edits")
    result["canonical_scene_sha256"] = before_sha
    result["canonical_scene_unchanged"] = True

    write_json(output_dir, "fixture-manifest.json", result)
    print(
        json.dumps(
            {
                "manifest": str(manifest_path),
                "asset_root": result["asset_root"],
                "patch_instances": result["patch_instances"],
                "blades_per_lod": [spec[0] for spec in LOD_SPECS],
                "triangles_per_lod": [lvl["triangles"] for lvl in result["levels"]],  # type: ignore[index]
                "declared_lod_errors": [lvl["declared_geometric_error"] for lvl in result["levels"][1:]],  # type: ignore[index]
                "canonical_scene_unchanged": True,
            }
        )
    )


if __name__ == "__main__":
    main()
