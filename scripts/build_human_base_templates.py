#!/usr/bin/env python3
"""Compile MakeHuman / MPFB2 CC0 human assets into VCR binary base templates.

Outputs:
  - assets/character_templates/human_female.vcrcharbase
  - assets/character_templates/human_male.vcrcharbase

Uses Python 3.11+ standard library only. Caches upstream downloads in %TEMP%.
"""

from __future__ import annotations

import gzip
import json
import math
import os
import struct
import tempfile
import urllib.request
import zlib
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

MPFB_DATA_URL = "https://raw.githubusercontent.com/makehumancommunity/mpfb2/master/src/mpfb/data"
CACHE_ROOT = Path(tempfile.gettempdir()) / "vcr_mpfb2_cc0_cache"
MASK_SIZE = 512
BONE_COUNT = 50
LANDMARK_CHAIN_COUNT = 6
MIN_MORPH_DELTA_M = 0.00015

# Binary format: "VCRCBAS2", version 2
MAGIC = b"VCRCBAS2"
VERSION = 2

# Region IDs shared with src/character_package.cpp
REGION_NONE = 0
REGION_HEAD = 1
REGION_NECK = 2
REGION_TORSO_UPPER = 3
REGION_TORSO_LOWER = 4
REGION_PELVIS = 5
REGION_UPPER_ARM = 6
REGION_FOREARM = 7
REGION_HAND = 8
REGION_THIGH_UPPER = 9
REGION_THIGH_LOWER = 10
REGION_SHIN = 11
REGION_ANKLE_BOOT = 12
REGION_FOOT = 13
REGION_INNER_MOUTH = 14

# Ordered strictly to match C++ MorphChannel enum (0..17)
MORPH_ORDER = [
    "musculature_pos",
    "musculature_neg",
    "body_fat_pos",
    "body_fat_neg",
    "shoulder_width",
    "chest_depth",
    "waist_width",
    "hip_width",
    "jaw_width",
    "chin_length",
    "cheekbone_prominence",
    "brow_ridge",
    "nose_length",
    "nose_width",
    "eye_size",
    "eye_spacing",
    "mouth_width",
    "lip_fullness",
]

PART_ORDER = ["body", "eyes", "tights", "skirt", "scalp"]


def fetch_bytes(rel_path: str) -> bytes:
    cache_path = CACHE_ROOT / rel_path
    if cache_path.is_file():
        return cache_path.read_bytes()
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    url = f"{MPFB_DATA_URL}/{rel_path}"
    req = urllib.request.Request(url, headers={"User-Agent": "vcr-character-template-builder/1.0"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = resp.read()
    tmp = cache_path.with_suffix(cache_path.suffix + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, cache_path)
    return data


def load_json_asset(rel_path: str):
    raw = fetch_bytes(rel_path)
    if rel_path.endswith(".gz"):
        raw = gzip.decompress(raw)
    return json.loads(raw.decode("utf-8"))


def fetch_bytes_optional(rel_path: str) -> Optional[bytes]:
    try:
        return fetch_bytes(rel_path)
    except Exception as exc:
        print(f"  [warn] optional asset unavailable ({rel_path}): {exc}")
        return None


def load_json_asset_optional(rel_path: str):
    raw = fetch_bytes_optional(rel_path)
    if raw is None:
        return None
    if rel_path.endswith(".gz"):
        raw = gzip.decompress(raw)
    return json.loads(raw.decode("utf-8"))


def decode_png_rgba(raw: bytes) -> Tuple[int, int, bytearray]:
    """Decode an 8-bit non-interlaced PNG (RGB, Gray, Gray+A, or RGBA) in pure stdlib."""
    if len(raw) < 8 or raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Invalid PNG header")
    pos = 8
    w = h = bit_depth = color_type = 0
    idat = bytearray()
    while pos + 8 <= len(raw):
        length, ctype = struct.unpack(">I4s", raw[pos:pos + 8])
        chunk = raw[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            w, h, bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break
        pos += 12 + length
    if w <= 0 or h <= 0 or bit_depth != 8:
        raise ValueError(f"Unsupported PNG format w={w} h={h} bd={bit_depth}")
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise ValueError(f"Unsupported PNG color_type={color_type}")
    decomp = zlib.decompress(bytes(idat))
    stride = w * channels
    rows = bytearray(h * stride)
    for y in range(h):
        flt = decomp[y * (stride + 1)]
        src = decomp[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
        d_off = y * stride
        p_off = (y - 1) * stride
        if flt == 0:
            rows[d_off:d_off + stride] = src
        elif flt == 1:
            for i in range(stride):
                a = rows[d_off + i - channels] if i >= channels else 0
                rows[d_off + i] = (src[i] + a) & 0xFF
        elif flt == 2:
            for i in range(stride):
                b = rows[p_off + i] if y > 0 else 0
                rows[d_off + i] = (src[i] + b) & 0xFF
        elif flt == 3:
            for i in range(stride):
                a = rows[d_off + i - channels] if i >= channels else 0
                b = rows[p_off + i] if y > 0 else 0
                rows[d_off + i] = (src[i] + ((a + b) >> 1)) & 0xFF
        elif flt == 4:
            for i in range(stride):
                a = rows[d_off + i - channels] if i >= channels else 0
                b = rows[p_off + i] if y > 0 else 0
                c = rows[p_off + i - channels] if (y > 0 and i >= channels) else 0
                pa = abs(b - c)
                pb = abs(a - c)
                pc = abs(a + b - 2 * c)
                p = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                rows[d_off + i] = (src[i] + p) & 0xFF
        else:
            raise ValueError(f"Unknown PNG scanline filter {flt}")
    if channels == 4:
        return w, h, rows
    rgba = bytearray(w * h * 4)
    if channels == 3:
        for i in range(w * h):
            rgba[i * 4:i * 4 + 3] = rows[i * 3:i * 3 + 3]
            rgba[i * 4 + 3] = 255
    elif channels == 2:
        for i in range(w * h):
            g = rows[i * 2]
            rgba[i * 4] = g; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = g
            rgba[i * 4 + 3] = rows[i * 2 + 1]
    else:
        for i in range(w * h):
            g = rows[i]
            rgba[i * 4] = g; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = g; rgba[i * 4 + 3] = 255
    return w, h, rgba


def load_target(rel_path: str) -> Dict[int, Tuple[float, float, float]]:
    raw = fetch_bytes(rel_path)
    if rel_path.endswith(".gz"):
        raw = gzip.decompress(raw)
    deltas: Dict[int, Tuple[float, float, float]] = {}
    for line in raw.decode("utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        parts = s.split()
        if len(parts) < 4:
            continue
        vid = int(parts[0])
        dx, dy, dz = float(parts[1]), float(parts[2]), float(parts[3])
        if dx != 0.0 or dy != 0.0 or dz != 0.0:
            deltas[vid] = (dx, dy, dz)
    return deltas


def combine_weighted_targets(weighted_paths: List[Tuple[float, str]]) -> Dict[int, Tuple[float, float, float]]:
    acc: Dict[int, List[float]] = {}
    for weight, path in weighted_paths:
        if abs(weight) < 1e-8:
            continue
        t = load_target(path)
        for vid, (dx, dy, dz) in t.items():
            cur = acc.get(vid)
            if cur is None:
                acc[vid] = [dx * weight, dy * weight, dz * weight]
            else:
                cur[0] += dx * weight
                cur[1] += dy * weight
                cur[2] += dz * weight
    return {vid: (v[0], v[1], v[2]) for vid, v in acc.items()}


def parse_base_obj():
    text = fetch_bytes("3dobjs/base.obj").decode("utf-8")
    vertices: List[Tuple[float, float, float]] = []
    vts: List[Tuple[float, float]] = []
    # Each face: (obj_face_idx, group_name, list_of_(v_idx0, vt_idx0_or_minus1))
    faces: List[Tuple[int, str, List[Tuple[int, int]]]] = []
    joint_verts: Dict[str, List[int]] = {}
    cur_group = "none"
    face_idx = 0

    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("v "):
            p = s.split()
            vertices.append((float(p[1]), float(p[2]), float(p[3])))
        elif s.startswith("vt "):
            p = s.split()
            vts.append((float(p[1]), 1.0 - float(p[2])))
        elif s.startswith("g "):
            cur_group = s[2:].strip()
        elif s.startswith("f "):
            corners: List[Tuple[int, int]] = []
            for tok in s[2:].split():
                parts = tok.split("/")
                vi = int(parts[0]) - 1
                ti = (int(parts[1]) - 1) if (len(parts) > 1 and parts[1]) else -1
                corners.append((vi, ti))
            if cur_group.startswith("joint-"):
                jv = joint_verts.setdefault(cur_group, [])
                for vi, _ in corners:
                    if vi not in jv:
                        jv.append(vi)
            faces.append((face_idx, cur_group, corners))
            face_idx += 1
    return vertices, vts, faces, joint_verts


def build_skin_weights(weights_ge: dict, weights_def: dict, face_groups: Dict[str, set], base_positions_dm: List[Tuple[float, float, float]]):
    """Map MakeHuman game_engine + default facial weights onto VCR's 50-bone skeleton."""
    vcount = len(base_positions_dm)
    raw_weights: List[Dict[int, float]] = [{} for _ in range(vcount)]

    def add_w(bone_idx: int, mh_name: str, scale: float = 1.0, src: dict = weights_ge):
        entries = src.get("weights", {}).get(mh_name, [])
        for pair in entries:
            vi = int(pair[0])
            w = float(pair[1]) * scale
            if 0 <= vi < vcount and w > 1e-6:
                raw_weights[vi][bone_idx] = raw_weights[vi].get(bone_idx, 0.0) + w

    # Primary body 0..34
    ge_direct = [
        (1, "pelvis", 1.0),
        (2, "spine_01", 1.0),
        (3, "spine_02", 1.0),
        (4, "spine_03", 1.0),
        (5, "neck_01", 1.0),
        (6, "head", 1.0),
        (7, "clavicle_l", 1.0),
        (10, "hand_l", 1.0),
        (11, "thumb_01_l", 1.0), (11, "thumb_02_l", 1.0), (11, "thumb_03_l", 1.0),
        (12, "index_01_l", 1.0), (12, "index_02_l", 1.0), (12, "index_03_l", 1.0),
        (13, "middle_01_l", 1.0), (13, "middle_02_l", 1.0), (13, "middle_03_l", 1.0),
        (13, "ring_01_l", 0.5), (13, "ring_02_l", 0.5), (13, "ring_03_l", 0.5),
        (14, "ring_01_l", 0.5), (14, "ring_02_l", 0.5), (14, "ring_03_l", 0.5),
        (14, "pinky_01_l", 1.0), (14, "pinky_02_l", 1.0), (14, "pinky_03_l", 1.0),
        (15, "clavicle_r", 1.0),
        (18, "hand_r", 1.0),
        (19, "thumb_01_r", 1.0), (19, "thumb_02_r", 1.0), (19, "thumb_03_r", 1.0),
        (20, "index_01_r", 1.0), (20, "index_02_r", 1.0), (20, "index_03_r", 1.0),
        (21, "middle_01_r", 1.0), (21, "middle_02_r", 1.0), (21, "middle_03_r", 1.0),
        (21, "ring_01_r", 0.5), (21, "ring_02_r", 0.5), (21, "ring_03_r", 0.5),
        (22, "ring_01_r", 0.5), (22, "ring_02_r", 0.5), (22, "ring_03_r", 0.5),
        (22, "pinky_01_r", 1.0), (22, "pinky_02_r", 1.0), (22, "pinky_03_r", 1.0),
        (23, "thigh_l", 1.0),
        (24, "calf_l", 1.0),
        (25, "foot_l", 1.0),
        (26, "ball_l", 1.0),
        (27, "thigh_r", 1.0),
        (28, "calf_r", 1.0),
        (29, "foot_r", 1.0),
        (30, "ball_r", 1.0),
    ]
    for bidx, mname, sc in ge_direct:
        add_w(bidx, mname, sc, weights_ge)

    # Split upperarm / lowerarm across primary & twist joints cleanly using weights.default.json
    add_w(8, "upperarm01.L", 1.0, weights_def)
    add_w(31, "upperarm02.L", 1.0, weights_def)
    add_w(9, "lowerarm01.L", 1.0, weights_def)
    add_w(32, "lowerarm02.L", 1.0, weights_def)
    add_w(16, "upperarm01.R", 1.0, weights_def)
    add_w(33, "upperarm02.R", 1.0, weights_def)
    add_w(17, "lowerarm01.R", 1.0, weights_def)
    add_w(34, "lowerarm02.R", 1.0, weights_def)

    # Pectoral subtle blend (scaled 0.45 so spine_03 stays dominant)
    add_w(35, "breast.L", 0.45, weights_def)
    add_w(36, "breast.R", 0.45, weights_def)

    # Ensure every vertex that received no arm twist from default gets fallback game_engine upperarm/lowerarm
    for (bidx, mname) in [(8, "upperarm_l"), (9, "lowerarm_l"), (16, "upperarm_r"), (17, "lowerarm_r")]:
        twist_b = {8: 31, 9: 32, 16: 33, 17: 34}[bidx]
        for vi, w in weights_ge.get("weights", {}).get(mname, []):
            vi_i = int(vi)
            if 0 <= vi_i < vcount and (raw_weights[vi_i].get(bidx, 0.0) + raw_weights[vi_i].get(twist_b, 0.0)) < 1e-4:
                raw_weights[vi_i][bidx] = raw_weights[vi_i].get(bidx, 0.0) + float(w)

    # Extract 12-bone facial mini-rig weights on body vertices (0..13379)
    facial_map: List[Tuple[int, str, float]] = [
        (38, "jaw", 1.0),
        (38, "temporalis01.L", 0.35), (38, "temporalis01.R", 0.35),
        (38, "temporalis02.L", 0.35), (38, "temporalis02.R", 0.35),
        (39, "oris01", 1.0), (39, "oris02", 1.0), (39, "oris06", 0.8),
        (39, "levator02.L", 0.7), (39, "levator02.R", 0.7),
        (40, "oris05", 1.0), (40, "oris04.L", 0.9), (40, "oris04.R", 0.9),
        (40, "oris03.L", 0.5), (40, "oris03.R", 0.5),
        (41, "oris06.L", 1.0), (41, "oris07.L", 1.0),
        (41, "risorius02.L", 0.9), (41, "risorius03.L", 0.9), (41, "levator03.L", 0.6),
        (42, "oris06.R", 1.0), (42, "oris07.R", 1.0),
        (42, "risorius02.R", 0.9), (42, "risorius03.R", 0.9), (42, "levator03.R", 0.6),
        (45, "orbicularis03.L", 1.0), (45, "orbicularis04.L", 0.45),
        (46, "orbicularis03.R", 1.0), (46, "orbicularis04.R", 0.45),
        (47, "oculi01.L", 1.0), (47, "oculi02.L", 0.8),
        (48, "oculi01.R", 1.0), (48, "oculi02.R", 0.8),
    ]
    facial_per_v: Dict[int, Dict[int, float]] = {}
    for bidx, mname, sc in facial_map:
        for vi, w in weights_def.get("weights", {}).get(mname, []):
            vi_i = int(vi)
            if vi_i not in face_groups["body"]:
                continue
            target_b = bidx
            # Move inner brow weights near midline onto Brow_Center (49)
            if bidx in (47, 48) and abs(base_positions_dm[vi_i][0]) < 0.14:
                target_b = 49
            fw = float(w) * sc
            if fw > 1e-4:
                d = facial_per_v.setdefault(vi_i, {})
                d[target_b] = d.get(target_b, 0.0) + fw

    for vi_i, fd in facial_per_v.items():
        head_w = raw_weights[vi_i].get(6, 0.0)
        if head_w < 0.05:
            continue
        fsum = sum(fd.values())
        if fsum <= 1e-6:
            continue
        # Preserve overall weight sum while transferring facial influence from Head
        take = min(head_w * 0.88, fsum)
        scale_f = take / fsum
        raw_weights[vi_i][6] = max(0.05, head_w - take)
        for fb, fw in fd.items():
            raw_weights[vi_i][fb] = raw_weights[vi_i].get(fb, 0.0) + fw * scale_f

    # Hard overrides for eyes, inner mouth, lashes, hair
    for vi_i in face_groups["helper-l-eye"]:
        raw_weights[vi_i] = {43: 1.0}
    for vi_i in face_groups["helper-r-eye"]:
        raw_weights[vi_i] = {44: 1.0}
    for vi_i in face_groups["helper-upper-teeth"]:
        raw_weights[vi_i] = {6: 1.0}
    for vi_i in (face_groups["helper-lower-teeth"] | face_groups["helper-tongue"]):
        raw_weights[vi_i] = {38: 0.92, 6: 0.08}
    for vi_i in face_groups["helper-l-eyelashes"]:
        raw_weights[vi_i] = {45: 0.85, 6: 0.15}
    for vi_i in face_groups["helper-r-eyelashes"]:
        raw_weights[vi_i] = {46: 0.85, 6: 0.15}
    for vi_i in face_groups["helper-hair"]:
        raw_weights[vi_i] = {6: 1.0}

    # Subtle tailcoat weight on lowest skirt vertices
    for vi_i in face_groups["helper-skirt"]:
        y_dm = base_positions_dm[vi_i][1]
        if y_dm < -1.0:
            t_coat = min(0.55, (-1.0 - y_dm) / 6.0)
            wmap = dict(raw_weights[vi_i])
            if wmap:
                for k in list(wmap.keys()):
                    wmap[k] *= (1.0 - t_coat)
                wmap[37] = wmap.get(37, 0.0) + t_coat
                raw_weights[vi_i] = wmap

    # Final top-4 normalization
    skin_rows: List[Tuple[Tuple[int, int, int, int], Tuple[float, float, float, float]]] = []
    for vi in range(vcount):
        wmap = raw_weights[vi]
        if not wmap:
            # Fallback for unassigned helper verts
            pos_y = base_positions_dm[vi][1]
            fb = 6 if pos_y > 5.5 else (4 if pos_y > 2.0 else 1)
            skin_rows.append(((fb, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)))
            continue
        top = sorted(wmap.items(), key=lambda kv: kv[1], reverse=True)[:4]
        s = sum(kv[1] for kv in top)
        if s <= 1e-8:
            skin_rows.append(((1, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)))
            continue
        js = [kv[0] for kv in top] + [0] * (4 - len(top))
        ws = [kv[1] / s for kv in top] + [0.0] * (4 - len(top))
        skin_rows.append((tuple(js), tuple(ws)))  # type: ignore[arg-type]

    return skin_rows, facial_per_v


def classify_vertex_region(vi: int, pos_m: Tuple[float, float, float], skin_row, ref_y: dict) -> int:
    """Classify skin/tights vertex anatomically using bone weights & Y height in meters."""
    j0 = skin_row[0][0]
    wmap = {skin_row[0][k]: skin_row[1][k] for k in range(4) if skin_row[1][k] > 0.0}
    y = pos_m[1]

    if j0 in (6, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49) or y >= ref_y["head_base"]:
        return REGION_HEAD
    if j0 == 5 or y >= ref_y["neck_base"]:
        return REGION_NECK
    if wmap.get(10, 0.0) + wmap.get(11, 0.0) + wmap.get(12, 0.0) + wmap.get(13, 0.0) + wmap.get(14, 0.0) \
       + wmap.get(18, 0.0) + wmap.get(19, 0.0) + wmap.get(20, 0.0) + wmap.get(21, 0.0) + wmap.get(22, 0.0) > 0.40:
        return REGION_HAND
    if wmap.get(9, 0.0) + wmap.get(32, 0.0) + wmap.get(17, 0.0) + wmap.get(34, 0.0) > 0.42:
        return REGION_FOREARM
    if wmap.get(8, 0.0) + wmap.get(31, 0.0) + wmap.get(16, 0.0) + wmap.get(33, 0.0) > 0.40:
        return REGION_UPPER_ARM
    if wmap.get(25, 0.0) + wmap.get(26, 0.0) + wmap.get(29, 0.0) + wmap.get(30, 0.0) > 0.45 or y < ref_y["ankle"] - 0.02:
        return REGION_FOOT
    if j0 in (24, 28):
        if y < ref_y["ankle"] + 0.08:
            return REGION_ANKLE_BOOT
        return REGION_SHIN
    if j0 in (23, 27):
        thigh_mid = 0.5 * (ref_y["hip"] + ref_y["knee"])
        if y < ref_y["knee"] + 0.02:
            return REGION_SHIN
        return REGION_THIGH_UPPER if y >= thigh_mid + 0.03 else REGION_THIGH_LOWER
    # Torso / pelvis
    if y >= ref_y["chest_mid"]:
        return REGION_TORSO_UPPER
    if y >= ref_y["waist"]:
        return REGION_TORSO_LOWER
    return REGION_PELVIS


def triangulate_face(corners: List[Tuple[int, int]]) -> List[Tuple[Tuple[int, int], Tuple[int, int], Tuple[int, int]]]:
    tris = []
    for k in range(1, len(corners) - 1):
        tris.append((corners[0], corners[k], corners[k + 1]))
    return tris


def compute_body_ao_and_curvature(
    base_positions_m: List[Tuple[float, float, float]],
    faces,
) -> Tuple[List[float], List[float]]:
    """Compute per-vertex cosine hemisphere AO [0..1] and discrete mean curvature [-1..1] for 'body'."""
    vcount = len(base_positions_m)
    body_tris_raw: List[Tuple[int, int, int]] = []
    body_verts_set: set = set()
    neighbors: List[set] = [set() for _ in range(vcount)]

    for _, gname, corners in faces:
        if gname != "body":
            continue
        for tri in triangulate_face(corners):
            v0, v1, v2 = tri[0][0], tri[1][0], tri[2][0]
            if v0 == v1 or v1 == v2 or v2 == v0:
                continue
            body_tris_raw.append((v0, v1, v2))
            body_verts_set.add(v0)
            body_verts_set.add(v1)
            body_verts_set.add(v2)
            neighbors[v0].add(v1); neighbors[v0].add(v2)
            neighbors[v1].add(v0); neighbors[v1].add(v2)
            neighbors[v2].add(v0); neighbors[v2].add(v1)

    # Area-weighted smooth vertex normals
    normals = [[0.0, 0.0, 0.0] for _ in range(vcount)]
    tri_data = []
    CELL = 0.055
    INV_CELL = 1.0 / CELL
    grid: Dict[Tuple[int, int, int], List[int]] = {}

    for ti, (v0, v1, v2) in enumerate(body_tris_raw):
        p0 = base_positions_m[v0]
        p1 = base_positions_m[v1]
        p2 = base_positions_m[v2]
        e1x, e1y, e1z = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
        e2x, e2y, e2z = p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
        nx = e1y * e2z - e1z * e2y
        ny = e1z * e2x - e1x * e2z
        nz = e1x * e2y - e1y * e2x
        for vi in (v0, v1, v2):
            normals[vi][0] += nx
            normals[vi][1] += ny
            normals[vi][2] += nz
        tri_data.append((p0[0], p0[1], p0[2], e1x, e1y, e1z, e2x, e2y, e2z))
        ix0 = int(math.floor(min(p0[0], p1[0], p2[0]) * INV_CELL))
        ix1 = int(math.floor(max(p0[0], p1[0], p2[0]) * INV_CELL))
        iy0 = int(math.floor(min(p0[1], p1[1], p2[1]) * INV_CELL))
        iy1 = int(math.floor(max(p0[1], p1[1], p2[1]) * INV_CELL))
        iz0 = int(math.floor(min(p0[2], p1[2], p2[2]) * INV_CELL))
        iz1 = int(math.floor(max(p0[2], p1[2], p2[2]) * INV_CELL))
        for ix in range(ix0, ix1 + 1):
            for iy in range(iy0, iy1 + 1):
                for iz in range(iz0, iz1 + 1):
                    grid.setdefault((ix, iy, iz), []).append(ti)

    unit_normals: List[Tuple[float, float, float]] = [(0.0, 1.0, 0.0)] * vcount
    curvature = [0.0] * vcount
    for vi in body_verts_set:
        nx, ny, nz = normals[vi]
        ln = math.sqrt(nx * nx + ny * ny + nz * nz)
        if ln > 1e-8:
            nx /= ln; ny /= ln; nz /= ln
        else:
            nx, ny, nz = 0.0, 1.0, 0.0
        unit_normals[vi] = (nx, ny, nz)

        nbrs = neighbors[vi]
        if nbrs:
            px, py, pz = base_positions_m[vi]
            lx = ly = lz = el_sum = 0.0
            for nj in nbrs:
                qx, qy, qz = base_positions_m[nj]
                dx, dy, dz = qx - px, qy - py, qz - pz
                el_sum += math.sqrt(dx * dx + dy * dy + dz * dz)
                lx += dx; ly += dy; lz += dz
            avg_el = el_sum / len(nbrs)
            if avg_el > 1e-5:
                inv_n = 1.0 / len(nbrs)
                # Positive = convex ridge, negative = concave crevice
                k = -((lx * inv_n) * nx + (ly * inv_n) * ny + (lz * inv_n) * nz) / avg_el
                curvature[vi] = max(-1.0, min(1.0, k * 2.4))

    # Precomputed 16 cosine-weighted Fibonacci hemisphere directions in local +Z tangent frame
    RAYS = 16
    local_dirs: List[Tuple[float, float, float]] = []
    golden_angle = 2.399963229728653
    for i in range(RAYS):
        z = math.sqrt((i + 0.5) / RAYS)
        r = math.sqrt(max(0.0, 1.0 - z * z))
        phi = i * golden_angle
        local_dirs.append((r * math.cos(phi), r * math.sin(phi), z))

    MAX_T = 0.14
    INV_MAX_T = 1.0 / MAX_T
    EPS_N = 0.0014
    EPS_DET = 1e-7
    ao = [1.0] * vcount
    visited = [0] * len(tri_data)
    stamp = 0
    step_ts = (0.0, 0.042, 0.088, 0.132)

    for vi in body_verts_set:
        nx, ny, nz = unit_normals[vi]
        px, py, pz = base_positions_m[vi]
        ox = px + nx * EPS_N
        oy = py + ny * EPS_N
        oz = pz + nz * EPS_N
        # Build tangent frame rotated pseudo-randomly per vertex to avoid banding
        ang = ((vi * 1103515245 + 12345) & 0xFFFF) * (6.2831853 / 65536.0)
        up = (1.0, 0.0, 0.0) if abs(ny) > 0.85 else (0.0, 1.0, 0.0)
        tx = up[1] * nz - up[2] * ny
        ty = up[2] * nx - up[0] * nz
        tz = up[0] * ny - up[1] * nx
        tl = math.sqrt(tx * tx + ty * ty + tz * tz)
        if tl > 1e-6:
            tx /= tl; ty /= tl; tz /= tl
        else:
            tx, ty, tz = 1.0, 0.0, 0.0
        bx = ny * tz - nz * ty
        by = nz * tx - nx * tz
        bz = nx * ty - ny * tx
        ca, sa = math.cos(ang), math.sin(ang)
        rx = tx * ca + bx * sa; ry = ty * ca + by * sa; rz = tz * ca + bz * sa
        ux = bx * ca - tx * sa; uy = by * ca - ty * sa; uz = bz * ca - tz * sa

        occ_sum = 0.0
        for lx, ly, lz in local_dirs:
            dx = rx * lx + ux * ly + nx * lz
            dy = ry * lx + uy * ly + ny * lz
            dz = rz * lx + uz * ly + nz * lz
            stamp += 1
            best_t = MAX_T
            prev_cell = None
            for st in step_ts:
                if st >= best_t:
                    break
                cell = (
                    int(math.floor((ox + dx * st) * INV_CELL)),
                    int(math.floor((oy + dy * st) * INV_CELL)),
                    int(math.floor((oz + dz * st) * INV_CELL)),
                )
                if cell == prev_cell:
                    continue
                prev_cell = cell
                cand = grid.get(cell)
                if not cand:
                    continue
                for ti in cand:
                    if visited[ti] == stamp:
                        continue
                    visited[ti] = stamp
                    t0x, t0y, t0z, e1x, e1y, e1z, e2x, e2y, e2z = tri_data[ti]
                    pxv = dy * e2z - dz * e2y
                    pyv = dz * e2x - dx * e2z
                    pzv = dx * e2y - dy * e2x
                    det = e1x * pxv + e1y * pyv + e1z * pzv
                    if -EPS_DET < det < EPS_DET:
                        continue
                    inv_det = 1.0 / det
                    txv = ox - t0x; tyv = oy - t0y; tzv = oz - t0z
                    u = (txv * pxv + tyv * pyv + tzv * pzv) * inv_det
                    if u < 0.0 or u > 1.0:
                        continue
                    qxv = tyv * e1z - tzv * e1y
                    qyv = tzv * e1x - txv * e1z
                    qzv = txv * e1y - tyv * e1x
                    v = (dx * qxv + dy * qyv + dz * qzv) * inv_det
                    if v < 0.0 or u + v > 1.0:
                        continue
                    t = (e2x * qxv + e2y * qyv + e2z * qzv) * inv_det
                    if 0.002 < t < best_t:
                        best_t = t
            if best_t < MAX_T:
                occ_sum += 1.0 - best_t * INV_MAX_T

        raw_ao = max(0.08, min(1.0, 1.0 - (occ_sum / RAYS) * 0.92))
        crease_ao = 1.0 - max(0.0, -curvature[vi]) * 0.22
        ao[vi] = max(0.06, min(1.0, raw_ao * crease_ao))

    return ao, curvature


def compute_nail_segments(joint_centers_m: Dict[str, Tuple[float, float, float]]):
    """Build 20 anatomical distal nail descriptors from MakeHuman finger/toe tip joints."""
    specs = []
    # Hands: fingers 1..5 on left/right
    for side in ("l", "r"):
        finger_widths = [0.0068, 0.0056, 0.0058, 0.0054, 0.0046]
        for idx in range(1, 6):
            j_dip = f"joint-{side}-finger-{idx}-3"
            j_tip = f"joint-{side}-finger-{idx}-4"
            if j_dip in joint_centers_m and j_tip in joint_centers_m:
                up = (0.0, 0.86, 0.51) if idx == 1 else (0.0, 1.0, 0.0)
                specs.append((joint_centers_m[j_dip], joint_centers_m[j_tip], up, finger_widths[idx - 1], False))
        # Toes: hallux uses 1-2..1-3, lesser toes use k-3..k-4
        toe_specs = [
            (f"joint-{side}-toe-1-2", f"joint-{side}-toe-1-3", 0.0085),
            (f"joint-{side}-toe-2-3", f"joint-{side}-toe-2-4", 0.0045),
            (f"joint-{side}-toe-3-3", f"joint-{side}-toe-3-4", 0.0042),
            (f"joint-{side}-toe-4-3", f"joint-{side}-toe-4-4", 0.0038),
            (f"joint-{side}-toe-5-3", f"joint-{side}-toe-5-4", 0.0035),
        ]
        for j_dip, j_tip, hw in toe_specs:
            if j_dip in joint_centers_m and j_tip in joint_centers_m:
                specs.append((joint_centers_m[j_dip], joint_centers_m[j_tip], (0.0, 1.0, 0.0), hw, True))
    return specs


def extract_landmark_chains(
    base_positions_m: List[Tuple[float, float, float]],
    faces,
    joint_centers_m: Dict[str, Tuple[float, float, float]],
) -> List[List[int]]:
    """Extract 6 ordered welded 'body' vertex chains: l_upper_lid, r_upper_lid, l_lower_lid, r_lower_lid, l_brow, r_brow."""
    body_head_verts = []
    lash_groups: Dict[str, set] = {
        "helper-l-eyelashes-2": set(),
        "helper-r-eyelashes-2": set(),
        "helper-l-eyelashes-1": set(),
        "helper-r-eyelashes-1": set(),
    }
    l_eye = joint_centers_m["joint-l-eye"]
    r_eye = joint_centers_m["joint-r-eye"]
    eye_y_min = min(l_eye[1], r_eye[1]) - 0.045
    eye_z_min = min(l_eye[2], r_eye[2]) - 0.035

    body_verts_all = set()
    for _, gname, corners in faces:
        if gname == "body":
            for vi, _ in corners:
                body_verts_all.add(vi)
        elif gname in lash_groups:
            for vi, _ in corners:
                lash_groups[gname].add(vi)

    for vi in body_verts_all:
        p = base_positions_m[vi]
        if p[1] >= eye_y_min and p[2] >= eye_z_min:
            body_head_verts.append(vi)

    def lid_chain(group_name: str, eye_c: Tuple[float, float, float], is_upper: bool) -> List[int]:
        lverts = lash_groups.get(group_name, set())
        best_map: Dict[int, float] = {}
        # Match anterior root lash vertices to nearest body eyelid rim vertex
        max_z = max((base_positions_m[v][2] for v in lverts), default=eye_c[2])
        for lv in lverts:
            lp = base_positions_m[lv]
            if lp[2] > max_z - 0.0025:
                continue  # ignore distal lash tips, keep lid root
            best_v = -1
            best_d2 = 0.015 * 0.015
            for bv in body_head_verts:
                bp = base_positions_m[bv]
                if (bp[0] >= 0.0) != (lp[0] >= 0.0):
                    continue
                if is_upper and bp[1] < eye_c[1] - 0.006:
                    continue
                if not is_upper and bp[1] > eye_c[1] + 0.005:
                    continue
                dx = bp[0] - lp[0]; dy = bp[1] - lp[1]; dz = bp[2] - lp[2]
                d2 = dx * dx + dy * dy + dz * dz
                if d2 < best_d2:
                    best_d2 = d2
                    best_v = bv
            if best_v >= 0:
                score = base_positions_m[best_v][1] if is_upper else -base_positions_m[best_v][1]
                if best_v not in best_map or score > best_map[best_v]:
                    best_map[best_v] = score
        pts = sorted(best_map.keys(), key=lambda vi: abs(base_positions_m[vi][0]))
        if not pts:
            return []
        # Resample along |x| bins to 18 smooth, deduplicated vertices
        xs = [abs(base_positions_m[v][0]) for v in pts]
        x0, x1 = min(xs), max(xs)
        out: List[int] = []
        bins = 18
        for bi in range(bins):
            t = bi / float(bins - 1)
            tx = x0 + (x1 - x0) * t
            cand = min(pts, key=lambda v: abs(abs(base_positions_m[v][0]) - tx))
            if not out or out[-1] != cand:
                out.append(cand)
        return out

    def brow_chain(eye_c: Tuple[float, float, float], is_left: bool) -> List[int]:
        out: List[int] = []
        bins = 18
        for bi in range(bins):
            brow_u = bi / float(bins - 1)
            target_ax = 0.0155 + (0.0565 - 0.0155) * brow_u
            arch_y = eye_c[1] + 0.0205 + 0.0068 * math.sin(min(1.0, brow_u * 1.15) * math.pi * 0.82) - 0.002 * (brow_u ** 2)
            best_v = -1
            best_score = 1e9
            for bv in body_head_verts:
                bp = base_positions_m[bv]
                if (bp[0] >= 0.0) != is_left:
                    continue
                if bp[2] < eye_c[2] - 0.008:
                    continue
                d2 = ((abs(bp[0]) - target_ax) ** 2) + ((bp[1] - arch_y) * 1.35) ** 2
                if d2 < best_score:
                    best_score = d2
                    best_v = bv
            if best_v >= 0 and (not out or out[-1] != best_v):
                out.append(best_v)
        return out

    return [
        lid_chain("helper-l-eyelashes-2", l_eye, True),
        lid_chain("helper-r-eyelashes-2", r_eye, True),
        lid_chain("helper-l-eyelashes-1", l_eye, False),
        lid_chain("helper-r-eyelashes-1", r_eye, False),
        brow_chain(l_eye, True),
        brow_chain(r_eye, False),
    ]


def build_uv_maps_512(
    faces,
    vts: List[Tuple[float, float]],
    base_positions_m: List[Tuple[float, float, float]],
    lip_face_ids: set,
    eyelid_face_ids: set,
    scalp_face_ids: set,
    joint_centers_m: Dict[str, Tuple[float, float, float]],
    ao_per_v: List[float],
    curv_per_v: List[float],
    sss_tex: Optional[Tuple[int, int, bytearray]],
) -> Tuple[bytes, bytes]:
    """Rasterize 512x512 skin_mask (lips/brows/creases/blush) and 512x512 skin_detail (AO/curv/SSS/nail+scalp)."""
    SS = MASK_SIZE
    mask_buf = bytearray(SS * SS * 4)
    det_buf = bytearray(SS * SS * 4)
    coverage = bytearray(SS * SS)

    l_eye = joint_centers_m["joint-l-eye"]
    r_eye = joint_centers_m["joint-r-eye"]
    head_c = joint_centers_m["joint-head"]
    nail_segs = compute_nail_segments(joint_centers_m)

    lip_core_verts = set()
    lip_halo_verts = set()
    body_tris = []

    for fid, gname, corners in faces:
        if gname == "body":
            in_lip = fid in lip_face_ids
            in_lid = fid in eyelid_face_ids
            in_scalp = fid in scalp_face_ids
            if in_lip:
                for vi, _ in corners:
                    lip_core_verts.add(vi)
            for tri in triangulate_face(corners):
                body_tris.append((tri, in_lip, in_lid, in_scalp))

    for tri, in_lip, _, _ in body_tris:
        vis = [tri[0][0], tri[1][0], tri[2][0]]
        if any(v in lip_core_verts for v in vis):
            for v in vis:
                if v not in lip_core_verts:
                    lip_halo_verts.add(v)

    def sample_sss_norm(u: float, v: float) -> float:
        if sss_tex is None:
            return 0.45
        sw, sh, sbytes = sss_tex
        px = max(0, min(sw - 1, int(u * sw)))
        py = max(0, min(sh - 1, int(v * sh)))
        raw_val = sbytes[(py * sw + px) * 4]
        return max(0.0, min(1.0, (raw_val - 70.0) / 56.0))

    def eval_nail_weight(p: Tuple[float, float, float]) -> float:
        px, py, pz = p
        best = 0.0
        for dip, tip, up, half_w, is_toe in nail_segs:
            vx = tip[0] - dip[0]; vy = tip[1] - dip[1]; vz = tip[2] - dip[2]
            seg_len2 = vx * vx + vy * vy + vz * vz
            if seg_len2 < 1e-7:
                continue
            seg_len = math.sqrt(seg_len2)
            # Only consider vertices reasonably close to this distal digit
            if abs(px - tip[0]) > 0.032 or abs(py - tip[1]) > 0.032 or abs(pz - tip[2]) > 0.032:
                continue
            t = ((px - dip[0]) * vx + (py - dip[1]) * vy + (pz - dip[2]) * vz) / seg_len2
            if t < 0.22 or t > 0.98:
                continue
            cx = dip[0] + vx * t; cy = dip[1] + vy * t; cz = dip[2] + vz * t
            rx = px - cx; ry = py - cy; rz = pz - cz
            dor = rx * up[0] + ry * up[1] + rz * up[2]
            if dor <= 0.0012:
                continue
            # Lateral distance perpendicular to axis and dorsal up
            ax, ay, az = vx / seg_len, vy / seg_len, vz / seg_len
            lx = up[1] * az - up[2] * ay
            ly = up[2] * ax - up[0] * az
            lz = up[0] * ay - up[1] * ax
            lat = abs(rx * lx + ry * ly + rz * lz)
            if lat > half_w * 1.15:
                continue
            u_n = (t - 0.60) / 0.38
            v_n = lat / half_w
            ell = math.sqrt(u_n * u_n + v_n * v_n)
            if ell < 1.0:
                score = 1.0 - ell * ell * 0.45
                if is_toe:
                    score *= 0.90
                best = max(best, score)
        return best

    def eval_point_mask(p: Tuple[float, float, float], lip_w: float, lid_w: float) -> Tuple[int, int, int, int]:
        px, py, pz = p
        lip_depth_fade = max(0.0, min(1.0, (pz - (l_eye[2] + 0.002)) / 0.014))
        lip_val = max(0.0, min(1.0, lip_w * lip_depth_fade))

        ey_c = l_eye[1] if px >= 0.0 else r_eye[1]
        dx_abs = abs(px)
        brow_u = (dx_abs - 0.0155) / (0.056 - 0.0155)
        brow_val = 0.0
        if 0.0 <= brow_u <= 1.0 and pz > l_eye[2] - 0.004:
            arch_y = ey_c + 0.0205 + 0.0068 * math.sin(min(1.0, brow_u * 1.15) * math.pi * 0.82) - 0.002 * (brow_u ** 2)
            half_h = 0.0048 * (1.0 - 0.55 * (brow_u ** 1.6)) * (0.45 + 0.55 * math.sin(min(1.0, brow_u) * math.pi) ** 0.35)
            dy_b = abs(py - arch_y) / max(0.0018, half_h)
            end_fade = min(1.0, min(brow_u / 0.14, (1.0 - brow_u) / 0.18))
            if dy_b < 1.25:
                brow_val = max(0.0, 1.0 - (dy_b / 1.25) ** 2) * end_fade

        crease = 0.0
        if lid_w > 0.05 and pz > l_eye[2] - 0.008:
            upper_bias = 1.0 if py >= ey_c - 0.004 else 0.55
            crease = max(crease, lid_w * 0.72 * upper_bias)
        nostril_y = l_eye[1] - 0.0375
        if abs(py - nostril_y) < 0.0075 and 0.0045 < dx_abs < 0.0155 and pz > l_eye[2] + 0.006:
            d_n = math.hypot((dx_abs - 0.0095) / 0.0048, (py - nostril_y) / 0.0052)
            if d_n < 1.0:
                crease = max(crease, (1.0 - d_n ** 2) * 0.65)

        blush = 0.0
        d_ch = math.hypot((dx_abs - 0.041) / 0.026, (py - (ey_c - 0.026)) / 0.020)
        if d_ch < 1.0 and pz > l_eye[2] - 0.025:
            blush = max(blush, (1.0 - d_ch * d_ch) * 0.75)
        d_nt = math.hypot(dx_abs / 0.014, (py - (ey_c - 0.028)) / 0.018)
        if d_nt < 1.0 and pz > l_eye[2] + 0.015:
            blush = max(blush, (1.0 - d_nt * d_nt) * 0.50)
        if dx_abs > 0.068 and abs(py - ey_c) < 0.036 and pz < head_c[2] + 0.025:
            blush = max(blush, 0.55)

        return (
            int(max(0, min(255, round(lip_val * 255)))),
            int(max(0, min(255, round(brow_val * 255)))),
            int(max(0, min(255, round(crease * 255)))),
            int(max(0, min(255, round(blush * 255)))),
        )

    def edge(ax, ay, bx, by, cx, cy):
        return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax)

    for (c0, c1, c2), in_lip, in_lid, in_scalp in body_tris:
        v0, t0 = c0
        v1, t1 = c1
        v2, t2 = c2
        if t0 < 0 or t1 < 0 or t2 < 0:
            continue
        uv0, uv1, uv2 = vts[t0], vts[t1], vts[t2]
        x0, y0 = uv0[0] * SS - 0.5, uv0[1] * SS - 0.5
        x1, y1 = uv1[0] * SS - 0.5, uv1[1] * SS - 0.5
        x2, y2 = uv2[0] * SS - 0.5, uv2[1] * SS - 0.5
        area = edge(x0, y0, x1, y1, x2, y2)
        if abs(area) < 1e-6:
            continue
        minx = max(0, int(math.floor(min(x0, x1, x2))))
        maxx = min(SS - 1, int(math.ceil(max(x0, x1, x2))))
        miny = max(0, int(math.floor(min(y0, y1, y2))))
        maxy = min(SS - 1, int(math.ceil(max(y0, y1, y2))))

        def vert_lip_w(vi: int) -> float:
            if in_lip:
                return 1.0
            if vi in lip_core_verts:
                return 0.72
            if vi in lip_halo_verts:
                return 0.12
            return 0.0

        lw0, lw1, lw2 = vert_lip_w(v0), vert_lip_w(v1), vert_lip_w(v2)
        ao0, ao1, ao2 = ao_per_v[v0], ao_per_v[v1], ao_per_v[v2]
        cu0, cu1, cu2 = curv_per_v[v0], curv_per_v[v1], curv_per_v[v2]
        ld = 1.0 if in_lid else 0.0
        p0, p1, p2 = base_positions_m[v0], base_positions_m[v1], base_positions_m[v2]
        inv_area = 1.0 / area

        for py_i in range(miny, maxy + 1):
            for px_i in range(minx, maxx + 1):
                w0 = edge(x1, y1, x2, y2, px_i, py_i) * inv_area
                w1 = edge(x2, y2, x0, y0, px_i, py_i) * inv_area
                w2 = edge(x0, y0, x1, y1, px_i, py_i) * inv_area
                if w0 < -0.025 or w1 < -0.025 or w2 < -0.025:
                    continue
                ws = w0 + w1 + w2
                if abs(ws) < 1e-6:
                    continue
                w0 /= ws; w1 /= ws; w2 /= ws
                p_interp = (
                    p0[0] * w0 + p1[0] * w1 + p2[0] * w2,
                    p0[1] * w0 + p1[1] * w1 + p2[1] * w2,
                    p0[2] * w0 + p1[2] * w1 + p2[2] * w2,
                )
                lw_i = lw0 * w0 + lw1 * w1 + lw2 * w2
                idx4 = (py_i * SS + px_i) * 4
                mask_buf[idx4:idx4 + 4] = bytes(eval_point_mask(p_interp, lw_i, ld))

                ao_i = max(0.0, min(1.0, ao0 * w0 + ao1 * w1 + ao2 * w2))
                curv_i = max(-1.0, min(1.0, cu0 * w0 + cu1 * w1 + cu2 * w2))
                u_i = max(0.0, min(1.0, uv0[0] * w0 + uv1[0] * w1 + uv2[0] * w2))
                v_i = max(0.0, min(1.0, uv0[1] * w0 + uv1[1] * w1 + uv2[1] * w2))
                sss_i = sample_sss_norm(u_i, v_i)
                # Boost SSS scatter on geometric ears, nostril alae, and distal digits
                px, py, pz = p_interp
                dx_abs = abs(px)
                if dx_abs > 0.068 and abs(py - l_eye[1]) < 0.036 and pz < head_c[2] + 0.022:
                    sss_i = max(sss_i, 0.88)
                elif abs(py - (l_eye[1] - 0.037)) < 0.009 and dx_abs < 0.017 and pz > l_eye[2] + 0.008:
                    sss_i = max(sss_i, 0.72)

                # Alpha detail tag: 1..127 fingernail/toenail, 128..255 scalp follicle zone
                tag_a = 0
                if in_scalp and py >= l_eye[1] + 0.015:
                    scalp_fade = max(0.0, min(1.0, (py - (l_eye[1] + 0.015)) / 0.025))
                    tag_a = 128 + int(round(scalp_fade * 127.0))
                else:
                    nw = eval_nail_weight(p_interp)
                    if nw > 0.02:
                        tag_a = max(1, min(127, int(round(nw * 127.0))))

                det_buf[idx4] = int(max(0, min(255, round(ao_i * 255.0))))
                det_buf[idx4 + 1] = int(max(0, min(255, round(128.0 + curv_i * 127.0))))
                det_buf[idx4 + 2] = int(max(0, min(255, round(sss_i * 255.0))))
                det_buf[idx4 + 3] = tag_a
                coverage[py_i * SS + px_i] = 1

    # 5-pass UV island edge dilation across both 512x512 buffers
    for _ in range(5):
        new_cov = bytearray(coverage)
        for y in range(SS):
            row = y * SS
            for x in range(SS):
                if coverage[row + x]:
                    continue
                mrs = mgs = mbs = mas = 0
                drs = dgs = dbs = das = cnt = 0
                for dy in (-1, 0, 1):
                    yy = y + dy
                    if 0 <= yy < SS:
                        for dx in (-1, 0, 1):
                            xx = x + dx
                            if 0 <= xx < SS and coverage[yy * SS + xx]:
                                idx4 = (yy * SS + xx) * 4
                                mrs += mask_buf[idx4]; mgs += mask_buf[idx4 + 1]
                                mbs += mask_buf[idx4 + 2]; mas += mask_buf[idx4 + 3]
                                drs += det_buf[idx4]; dgs += det_buf[idx4 + 1]
                                dbs += det_buf[idx4 + 2]; das += det_buf[idx4 + 3]
                                cnt += 1
                if cnt > 0:
                    idx4 = (row + x) * 4
                    mask_buf[idx4] = mrs // cnt
                    mask_buf[idx4 + 1] = mgs // cnt
                    mask_buf[idx4 + 2] = mbs // cnt
                    mask_buf[idx4 + 3] = mas // cnt
                    det_buf[idx4] = drs // cnt
                    det_buf[idx4 + 1] = dgs // cnt
                    det_buf[idx4 + 2] = dbs // cnt
                    det_buf[idx4 + 3] = das // cnt
                    new_cov[row + x] = 1
        coverage = new_cov

    return bytes(mask_buf), bytes(det_buf)


def unwrap_seam_uvs(corners: List[Tuple[int, Tuple[float, float], int, int]]) -> List[Tuple[int, float, float, int, int]]:
    """For 3 corners of a triangle, unwrap wrapping cylindrical UVs cleanly across u=0/1 seam."""
    u0, u1, u2 = corners[0][1][0], corners[1][1][0], corners[2][1][0]
    if max(u0, u1, u2) - min(u0, u1, u2) > 0.5:
        out = []
        for vi, (u, v), reg, flg in corners:
            out.append((vi, u + 1.0 if u < 0.5 else u, v, reg, flg))
        return out
    return [(vi, uv[0], uv[1], reg, flg) for vi, uv, reg, flg in corners]


def build_template_for_gender(gender: str, out_path: Path):
    print(f"[build_human_base_templates] Compiling '{gender}' -> {out_path}")
    vertices_dm, vts, faces, joint_verts = parse_base_obj()
    vcount = len(vertices_dm)

    weights_ge = load_json_asset("rigs/standard/weights.game_engine.json")
    weights_def = load_json_asset("rigs/standard/weights.default.json")
    lips_json = load_json_asset("uv_layers/lips_solid.json.gz")
    eyelids_json = load_json_asset("uv_layers/eyelids_solid.json.gz")
    scalp_json = load_json_asset_optional("uv_layers/scalp_solid.json.gz")
    lip_face_ids = {int(k) for k in lips_json.keys()}
    eyelid_face_ids = {int(k) for k in eyelids_json.keys()}
    scalp_face_ids = {int(k) for k in scalp_json.keys()} if scalp_json else set()
    sss_raw = fetch_bytes_optional("textures/sss.png")
    sss_tex = decode_png_rgba(sss_raw) if sss_raw is not None else None

    # 1. Canonical gender base shape (average of African/Asian/Caucasian young + 0.5 idealproportions)
    base_targets = combine_weighted_targets([
        (1.0 / 3.0, f"targets/macrodetails/african-{gender}-young.target.gz"),
        (1.0 / 3.0, f"targets/macrodetails/asian-{gender}-young.target.gz"),
        (1.0 / 3.0, f"targets/macrodetails/caucasian-{gender}-young.target.gz"),
        (0.50, f"targets/macrodetails/proportions/{gender}-young-averagemuscle-averageweight-idealproportions.target.gz"),
    ])

    morphed_dm: List[Tuple[float, float, float]] = list(vertices_dm)
    for vid, (dx, dy, dz) in base_targets.items():
        if 0 <= vid < vcount:
            x, y, z = morphed_dm[vid]
            morphed_dm[vid] = (x + dx, y + dy, z + dz)

    # Compute floor Y in decimeters using morphed joint-ground + body foot soles
    ground_y = sum(morphed_dm[vi][1] for vi in joint_verts["joint-ground"]) / len(joint_verts["joint-ground"])
    body_verts_set = set()
    group_verts: Dict[str, set] = {}
    for _, gname, corners in faces:
        s = group_verts.setdefault(gname, set())
        for vi, _ in corners:
            s.add(vi)
            if gname == "body":
                body_verts_set.add(vi)
    min_body_y_dm = min(morphed_dm[vi][1] for vi in body_verts_set)
    floor_dm = min(ground_y, min_body_y_dm)

    base_positions_m: List[Tuple[float, float, float]] = [
        (x * 0.10, (y - floor_dm) * 0.10, z * 0.10)
        for (x, y, z) in morphed_dm
    ]
    crown_m = max(base_positions_m[vi][1] for vi in body_verts_set)
    sole_m = min(base_positions_m[vi][1] for vi in body_verts_set)
    print(f"  height in base pose: {crown_m - sole_m:.4f} m (sole_y={sole_m:.4f} m)")

    def mean_pos_m(jname: str) -> Tuple[float, float, float]:
        vis = joint_verts[jname]
        n = float(len(vis))
        return (
            sum(base_positions_m[v][0] for v in vis) / n,
            sum(base_positions_m[v][1] for v in vis) / n,
            sum(base_positions_m[v][2] for v in vis) / n,
        )

    joint_centers_m = {jn: mean_pos_m(jn) for jn in joint_verts.keys()}

    # 2. Build 50-bone skinning table & bone vertex locators
    face_groups = {
        "body": group_verts.get("body", set()),
        "helper-l-eye": group_verts.get("helper-l-eye", set()),
        "helper-r-eye": group_verts.get("helper-r-eye", set()),
        "helper-upper-teeth": group_verts.get("helper-upper-teeth", set()),
        "helper-lower-teeth": group_verts.get("helper-lower-teeth", set()),
        "helper-tongue": group_verts.get("helper-tongue", set()),
        "helper-l-eyelashes": group_verts.get("helper-l-eyelashes-1", set()) | group_verts.get("helper-l-eyelashes-2", set()),
        "helper-r-eyelashes": group_verts.get("helper-r-eyelashes-1", set()) | group_verts.get("helper-r-eyelashes-2", set()),
        "helper-hair": group_verts.get("helper-hair", set()),
        "helper-skirt": group_verts.get("helper-skirt", set()),
    }
    skin_rows, facial_per_v = build_skin_weights(weights_ge, weights_def, face_groups, morphed_dm)

    def top_verts_for_bone(bidx: int, limit: int = 8) -> List[int]:
        cands = []
        for vi, fd in facial_per_v.items():
            if bidx in fd:
                cands.append((fd[bidx], vi))
        cands.sort(reverse=True)
        return [vi for _, vi in cands[:limit]]

    def top_pectoral_verts(mh_name: str, limit: int = 12) -> List[int]:
        cands = [(float(w), int(vi)) for vi, w in weights_def.get("weights", {}).get(mh_name, [])]
        cands.sort(reverse=True)
        return [vi for _, vi in cands[:limit]]

    # 50 bone position definitions: (vertex_indices_list, offset_xyz)
    bone_locators: List[Tuple[List[int], Tuple[float, float, float]]] = [
        ([], (0.0, 0.0, 0.0)),  # 0 Root
        (joint_verts["joint-pelvis"], (0.0, 0.0, 0.0)),  # 1 Pelvis
        (joint_verts["joint-spine-4"], (0.0, 0.0, 0.0)),  # 2 Spine_01 (lumbar!)
        (joint_verts["joint-spine-3"], (0.0, 0.0, 0.0)),  # 3 Spine_02
        (joint_verts["joint-spine-2"] + joint_verts["joint-spine-1"], (0.0, 0.0, 0.0)),  # 4 Spine_03
        (joint_verts["joint-neck"], (0.0, 0.0, 0.0)),  # 5 Neck
        (joint_verts["joint-head"], (0.0, 0.0, 0.0)),  # 6 Head
        (joint_verts["joint-l-clavicle"], (0.0, 0.0, 0.0)),  # 7 Clavicle_L
        (joint_verts["joint-l-shoulder"], (0.0, 0.0, 0.0)),  # 8 UpperArm_L
        (joint_verts["joint-l-elbow"], (0.0, 0.0, 0.0)),  # 9 Forearm_L
        (joint_verts["joint-l-hand"], (0.0, 0.0, 0.0)),  # 10 Hand_L
        (joint_verts["joint-l-finger-1-1"], (0.0, 0.0, 0.0)),  # 11 Thumb_L
        (joint_verts["joint-l-finger-2-1"], (0.0, 0.0, 0.0)),  # 12 Index_L
        (joint_verts["joint-l-finger-3-1"], (0.0, 0.0, 0.0)),  # 13 Middle_L
        (joint_verts["joint-l-finger-5-1"], (0.0, 0.0, 0.0)),  # 14 Pinky_L
        (joint_verts["joint-r-clavicle"], (0.0, 0.0, 0.0)),  # 15 Clavicle_R
        (joint_verts["joint-r-shoulder"], (0.0, 0.0, 0.0)),  # 16 UpperArm_R
        (joint_verts["joint-r-elbow"], (0.0, 0.0, 0.0)),  # 17 Forearm_R
        (joint_verts["joint-r-hand"], (0.0, 0.0, 0.0)),  # 18 Hand_R
        (joint_verts["joint-r-finger-1-1"], (0.0, 0.0, 0.0)),  # 19 Thumb_R
        (joint_verts["joint-r-finger-2-1"], (0.0, 0.0, 0.0)),  # 20 Index_R
        (joint_verts["joint-r-finger-3-1"], (0.0, 0.0, 0.0)),  # 21 Middle_R
        (joint_verts["joint-r-finger-5-1"], (0.0, 0.0, 0.0)),  # 22 Pinky_R
        (joint_verts["joint-l-upper-leg"], (0.0, 0.0, 0.0)),  # 23 Thigh_L
        (joint_verts["joint-l-knee"], (0.0, 0.0, 0.0)),  # 24 Shin_L
        (joint_verts["joint-l-ankle"], (0.0, 0.0, 0.0)),  # 25 Foot_L
        (joint_verts["joint-l-foot-1"], (0.0, 0.0, 0.0)),  # 26 Toe_L
        (joint_verts["joint-r-upper-leg"], (0.0, 0.0, 0.0)),  # 27 Thigh_R
        (joint_verts["joint-r-knee"], (0.0, 0.0, 0.0)),  # 28 Shin_R
        (joint_verts["joint-r-ankle"], (0.0, 0.0, 0.0)),  # 29 Foot_R
        (joint_verts["joint-r-foot-1"], (0.0, 0.0, 0.0)),  # 30 Toe_R
        (joint_verts["joint-l-shoulder"] + joint_verts["joint-l-elbow"], (0.0, 0.0, 0.0)),  # 31 ShoulderTwist_L
        (joint_verts["joint-l-elbow"] + joint_verts["joint-l-hand"], (0.0, 0.0, 0.0)),  # 32 ForearmTwist_L
        (joint_verts["joint-r-shoulder"] + joint_verts["joint-r-elbow"], (0.0, 0.0, 0.0)),  # 33 ShoulderTwist_R
        (joint_verts["joint-r-elbow"] + joint_verts["joint-r-hand"], (0.0, 0.0, 0.0)),  # 34 ForearmTwist_R
        (top_pectoral_verts("breast.L"), (0.0, 0.0, 0.0)),  # 35 Pectoral_L
        (top_pectoral_verts("breast.R"), (0.0, 0.0, 0.0)),  # 36 Pectoral_R
        (joint_verts["joint-pelvis"], (0.0, -0.05, -0.08)),  # 37 TailCoatBase
        (joint_verts["joint-jaw"], (0.0, 0.0, 0.0)),  # 38 Jaw
        (top_verts_for_bone(39), (0.0, 0.0, 0.0)),  # 39 Lip_Upper
        (top_verts_for_bone(40), (0.0, 0.0, 0.0)),  # 40 Lip_Lower
        (top_verts_for_bone(41), (0.0, 0.0, 0.0)),  # 41 Mouth_Corner_L
        (top_verts_for_bone(42), (0.0, 0.0, 0.0)),  # 42 Mouth_Corner_R
        (joint_verts["joint-l-eye"], (0.0, 0.0, 0.0)),  # 43 Eye_L
        (joint_verts["joint-r-eye"], (0.0, 0.0, 0.0)),  # 44 Eye_R
        (joint_verts["joint-l-upperlid"], (0.0, 0.0, 0.0)),  # 45 Eyelid_Upper_L
        (joint_verts["joint-r-upperlid"], (0.0, 0.0, 0.0)),  # 46 Eyelid_Upper_R
        (top_verts_for_bone(47), (0.0, 0.0, 0.0)),  # 47 Brow_L
        (top_verts_for_bone(48), (0.0, 0.0, 0.0)),  # 48 Brow_R
        (top_verts_for_bone(49), (0.0, 0.0, 0.0)),  # 49 Brow_Center
    ]
    assert len(bone_locators) == BONE_COUNT

    # 3. Reference heights for clean anatomical region classification
    pelvis_y_m = joint_centers_m["joint-pelvis"][1]
    spine2_y_m = joint_centers_m["joint-spine-2"][1]
    spine3_y_m = joint_centers_m["joint-spine-3"][1]
    neck_y_m = joint_centers_m["joint-neck"][1]
    head_y_m = joint_centers_m["joint-head"][1]
    hip_y_m = joint_centers_m["joint-l-upper-leg"][1]
    knee_y_m = joint_centers_m["joint-l-knee"][1]
    ankle_y_m = joint_centers_m["joint-l-ankle"][1]

    ref_y = {
        "head_base": 0.45 * neck_y_m + 0.55 * head_y_m,
        "neck_base": neck_y_m - 0.035,
        "chest_mid": spine2_y_m - 0.02,
        "waist": 0.55 * pelvis_y_m + 0.45 * spine3_y_m,
        "hip": hip_y_m,
        "knee": knee_y_m,
        "ankle": ankle_y_m,
    }

    # 4. Compute 18 sparse morph channels (converted to meters)
    def diff_targets(pos_list: List[Tuple[float, str]], neg_list: List[Tuple[float, str]]) -> Dict[int, Tuple[float, float, float]]:
        weighted = list(pos_list) + [(-w, p) for (w, p) in neg_list]
        return combine_weighted_targets(weighted)

    chest_extra_pos = [(0.55, "targets/torso/measure-bust-circ-incr.target.gz"),
                       (0.45, "targets/torso/torso-scale-depth-incr.target.gz")]
    if gender == "female":
        chest_extra_pos.append((0.35, "targets/breast/breast-volume-vert-up.target.gz"))

    morph_dicts: Dict[str, Dict[int, Tuple[float, float, float]]] = {
        "musculature_pos": combine_weighted_targets([(1.0, f"targets/macrodetails/universal-{gender}-young-maxmuscle-averageweight.target.gz")]),
        "musculature_neg": combine_weighted_targets([(1.0, f"targets/macrodetails/universal-{gender}-young-minmuscle-averageweight.target.gz")]),
        "body_fat_pos": combine_weighted_targets([(1.0, f"targets/macrodetails/universal-{gender}-young-averagemuscle-maxweight.target.gz")]),
        "body_fat_neg": combine_weighted_targets([(1.0, f"targets/macrodetails/universal-{gender}-young-averagemuscle-minweight.target.gz")]),
        "shoulder_width": diff_targets(
            [(0.55, "targets/torso/measure-shoulder-dist-incr.target.gz"), (0.35, "targets/torso/torso-vshape-incr.target.gz")],
            [(0.55, "targets/torso/measure-shoulder-dist-decr.target.gz"), (0.35, "targets/torso/torso-vshape-decr.target.gz")],
        ),
        "chest_depth": diff_targets(
            chest_extra_pos,
            [(0.55, "targets/torso/measure-bust-circ-decr.target.gz"), (0.45, "targets/torso/torso-scale-depth-decr.target.gz")],
        ),
        "waist_width": diff_targets(
            [(0.65, "targets/torso/measure-waist-circ-incr.target.gz")],
            [(0.65, "targets/torso/measure-waist-circ-decr.target.gz")],
        ),
        "hip_width": diff_targets(
            [(0.55, "targets/torso/measure-hips-circ-incr.target.gz"), (0.45, "targets/hip/hip-scale-horiz-incr.target.gz")],
            [(0.55, "targets/torso/measure-hips-circ-decr.target.gz"), (0.45, "targets/hip/hip-scale-horiz-decr.target.gz")],
        ),
        "jaw_width": diff_targets(
            [(0.55, "targets/chin/chin-width-incr.target.gz"), (0.45, "targets/chin/chin-bones-incr.target.gz")],
            [(0.55, "targets/chin/chin-width-decr.target.gz"), (0.45, "targets/chin/chin-bones-decr.target.gz")],
        ),
        "chin_length": diff_targets(
            [(0.60, "targets/chin/chin-height-incr.target.gz"), (0.45, "targets/chin/chin-prominent-incr.target.gz")],
            [(0.60, "targets/chin/chin-height-decr.target.gz"), (0.45, "targets/chin/chin-prominent-decr.target.gz")],
        ),
        "cheekbone_prominence": diff_targets(
            [(0.60, "targets/cheek/l-cheek-bones-incr.target.gz"), (0.60, "targets/cheek/r-cheek-bones-incr.target.gz")],
            [(0.60, "targets/cheek/l-cheek-bones-decr.target.gz"), (0.60, "targets/cheek/r-cheek-bones-decr.target.gz")],
        ),
        "brow_ridge": diff_targets(
            [(0.60, "targets/forehead/forehead-trans-forward.target.gz"), (0.50, "targets/eyebrows/eyebrows-trans-forward.target.gz")],
            [(0.60, "targets/forehead/forehead-trans-backward.target.gz"), (0.50, "targets/eyebrows/eyebrows-trans-backward.target.gz")],
        ),
        "nose_length": diff_targets(
            [(0.55, "targets/nose/nose-scale-vert-incr.target.gz"), (0.50, "targets/nose/nose-scale-depth-incr.target.gz")],
            [(0.55, "targets/nose/nose-scale-vert-decr.target.gz"), (0.50, "targets/nose/nose-scale-depth-decr.target.gz")],
        ),
        "nose_width": diff_targets(
            [(0.55, "targets/nose/nose-scale-horiz-incr.target.gz"), (0.45, "targets/nose/nose-flaring-incr.target.gz")],
            [(0.55, "targets/nose/nose-scale-horiz-decr.target.gz"), (0.45, "targets/nose/nose-flaring-decr.target.gz")],
        ),
        "eye_size": diff_targets(
            [(0.55, "targets/eyes/l-eye-scale-incr.target.gz"), (0.55, "targets/eyes/r-eye-scale-incr.target.gz")],
            [(0.55, "targets/eyes/l-eye-scale-decr.target.gz"), (0.55, "targets/eyes/r-eye-scale-decr.target.gz")],
        ),
        "eye_spacing": diff_targets(
            [(0.55, "targets/eyes/l-eye-trans-out.target.gz"), (0.55, "targets/eyes/r-eye-trans-out.target.gz")],
            [(0.55, "targets/eyes/l-eye-trans-in.target.gz"), (0.55, "targets/eyes/r-eye-trans-in.target.gz")],
        ),
        "mouth_width": diff_targets(
            [(0.65, "targets/mouth/mouth-scale-horiz-incr.target.gz")],
            [(0.65, "targets/mouth/mouth-scale-horiz-decr.target.gz")],
        ),
        "lip_fullness": diff_targets(
            [(0.55, "targets/mouth/mouth-upperlip-volume-incr.target.gz"), (0.55, "targets/mouth/mouth-lowerlip-volume-incr.target.gz")],
            [(0.55, "targets/mouth/mouth-upperlip-volume-decr.target.gz"), (0.55, "targets/mouth/mouth-lowerlip-volume-decr.target.gz")],
        ),
    }

    morph_channels: List[List[Tuple[int, float, float, float]]] = []
    for mname in MORPH_ORDER:
        deltas_m: List[Tuple[int, float, float, float]] = []
        for vid, (dx, dy, dz) in sorted(morph_dicts[mname].items()):
            if 0 <= vid < vcount:
                dxm, dym, dzm = dx * 0.10, dy * 0.10, dz * 0.10
                if abs(dxm) + abs(dym) + abs(dzm) >= MIN_MORPH_DELTA_M:
                    deltas_m.append((vid, dxm, dym, dzm))
        morph_channels.append(deltas_m)

    # 5. Build the 5 Mesh Parts with clean UVs & seam unwrapping
    torso_top_m = neck_y_m + 0.02
    torso_bot_m = sole_m
    torso_span_m = max(0.2, torso_top_m - torso_bot_m)
    head_top_m = crown_m
    head_base_m = joint_centers_m["joint-neck"][1]

    parts_data: Dict[str, Tuple[List[Tuple[int, int, int, float, float]], List[int]]] = {}
    for pname in PART_ORDER:
        corner_map: Dict[Tuple[int, int, int, int, int], int] = {}
        corners_list: List[Tuple[int, int, int, float, float]] = []
        indices_list: List[int] = []

        def emit_corner(vi: int, reg: int, flg: int, u: float, v: float) -> int:
            uq = int(round(u * 65536.0))
            vq = int(round(v * 65536.0))
            key = (vi, reg, flg, uq, vq)
            ex = corner_map.get(key)
            if ex is not None:
                return ex
            idx = len(corners_list)
            corners_list.append((vi, reg, flg, u, v))
            corner_map[key] = idx
            return idx

        if pname == "body":
            for _, gname, face_corners in faces:
                if gname not in ("body", "helper-upper-teeth", "helper-lower-teeth", "helper-tongue"):
                    continue
                is_inner = (gname != "body")
                for tri in triangulate_face(face_corners):
                    for vi, ti in tri:
                        uv = vts[ti] if ti >= 0 else (0.5, 0.5)
                        reg = REGION_INNER_MOUTH if is_inner else classify_vertex_region(vi, base_positions_m[vi], skin_rows[vi], ref_y)
                        flg = 1 if (not is_inner and reg == REGION_HEAD and abs(base_positions_m[vi][0]) < 0.055 and base_positions_m[vi][2] > joint_centers_m["joint-head"][2]) else 0
                        indices_list.append(emit_corner(vi, reg, flg, uv[0], uv[1]))

        elif pname == "eyes":
            eye_specs = [
                ("helper-l-eye", joint_centers_m["joint-l-eye"], 0),
                ("helper-r-eye", joint_centers_m["joint-r-eye"], 1),
            ]
            for gname, ec, flg in eye_specs:
                # Measure exact eyeball max XY radius around eye center
                eye_r = 0.0145
                for _, fg, face_corners in faces:
                    if fg == gname:
                        for vi, _ in face_corners:
                            p = base_positions_m[vi]
                            eye_r = max(eye_r, math.hypot(p[0] - ec[0], p[1] - ec[1]))
                eye_r *= 1.02
                for _, fg, face_corners in faces:
                    if fg != gname:
                        continue
                    for tri in triangulate_face(face_corners):
                        for vi, _ in tri:
                            px, py, pz = base_positions_m[vi]
                            dx, dy, dz = px - ec[0], py - ec[1], pz - ec[2]
                            # Front hemisphere maps pupil/iris inside center around (0.5, 0.5); rear maps onto sclera border
                            if dz >= 0.0:
                                u = max(0.04, min(0.96, 0.5 + dx / (2.0 * eye_r)))
                                v = max(0.04, min(0.96, 0.5 - dy / (2.0 * eye_r)))
                            else:
                                dxy = max(1e-5, math.hypot(dx, dy))
                                u = max(0.02, min(0.98, 0.5 + (dx / dxy) * 0.47))
                                v = max(0.02, min(0.98, 0.5 - (dy / dxy) * 0.47))
                            indices_list.append(emit_corner(vi, REGION_HEAD, flg, u, v))

        elif pname == "tights":
            shld_x_m = abs(joint_centers_m["joint-l-shoulder"][0])
            for _, gname, face_corners in faces:
                if gname != "helper-tights":
                    continue
                for tri in triangulate_face(face_corners):
                    raw_c = []
                    for vi, _ in tri:
                        px, py, pz = base_positions_m[vi]
                        reg = classify_vertex_region(vi, (px, py, pz), skin_rows[vi], ref_y)
                        if reg in (REGION_UPPER_ARM, REGION_FOREARM, REGION_HAND) or abs(px) > shld_x_m + 0.04:
                            # Sleeve UV: u wraps around arm, v moves from shoulder toward wrist
                            u = 0.5 + math.atan2(py - joint_centers_m["joint-l-shoulder"][1], pz - joint_centers_m["joint-l-shoulder"][2]) / (2.0 * math.pi)
                            v = max(0.05, min(0.95, abs(px) / 0.72))
                            flg = 1
                        else:
                            # Torso/legs cylindrical UV with u=0.5 down front centerline +Z
                            ang = math.atan2(px, pz)  # 0 at +Z front, +/-pi at -Z back
                            u = 0.5 + ang / (2.0 * math.pi)
                            v = max(0.0, min(1.0, (torso_top_m - py) / torso_span_m))
                            flg = 0
                        raw_c.append((vi, (u, v), reg, flg))
                    # Only unwrap seam around torso/legs when vertices share cylinder wrap
                    if raw_c[0][3] == 0 and raw_c[1][3] == 0 and raw_c[2][3] == 0:
                        unwrapped = unwrap_seam_uvs(raw_c)
                    else:
                        unwrapped = [(vi, uv[0], uv[1], reg, flg) for vi, uv, reg, flg in raw_c]
                    for vi, u, v, reg, flg in unwrapped:
                        indices_list.append(emit_corner(vi, reg, flg, u, v))

        elif pname == "skirt":
            sk_ys = [base_positions_m[vi][1] for _, g, fc in faces if g == "helper-skirt" for vi, _ in fc]
            sk_top = max(sk_ys) if sk_ys else torso_top_m
            sk_bot = min(sk_ys) if sk_ys else sole_m
            sk_span = max(0.1, sk_top - sk_bot)
            for _, gname, face_corners in faces:
                if gname != "helper-skirt":
                    continue
                for tri in triangulate_face(face_corners):
                    raw_c = []
                    for vi, _ in tri:
                        px, py, pz = base_positions_m[vi]
                        u = 0.5 + math.atan2(px, pz) / (2.0 * math.pi)
                        v = max(0.0, min(1.0, (sk_top - py) / sk_span))
                        reg = REGION_PELVIS if py > pelvis_y_m - 0.10 else REGION_THIGH_LOWER
                        raw_c.append((vi, (u, v), reg, 0))
                    for vi, u, v, reg, flg in unwrap_seam_uvs(raw_c):
                        indices_list.append(emit_corner(vi, reg, flg, u, v))

        elif pname == "scalp":
            for _, gname, face_corners in faces:
                if gname != "helper-hair":
                    continue
                for tri in triangulate_face(face_corners):
                    raw_c = []
                    # Check triangle min Y to separate cranial cap vs lower back drape
                    tri_min_y = min(base_positions_m[vi][1] for vi, _ in tri)
                    tri_max_z = max(base_positions_m[vi][2] for vi, _ in tri)
                    # Skip any stray front hair proxy faces dropping across eye/face plane
                    if tri_min_y < joint_centers_m["joint-l-eye"][1] + 0.012 and tri_max_z > joint_centers_m["joint-head"][2] + 0.025:
                        continue
                    is_cranial = (tri_min_y >= joint_centers_m["joint-neck"][1] - 0.015)
                    for vi, _ in tri:
                        px, py, pz = base_positions_m[vi]
                        reg = REGION_HEAD if is_cranial else REGION_NECK
                        u = 0.5 + math.atan2(px, pz - joint_centers_m["joint-head"][2]) / (2.0 * math.pi)
                        v = max(0.0, min(1.0, (head_top_m - py) / max(0.15, head_top_m - torso_bot_m * 0.4 - head_base_m * 0.5)))
                        raw_c.append((vi, (u, v), reg, 1 if is_cranial else 0))
                    for vi, u, v, reg, flg in unwrap_seam_uvs(raw_c):
                        indices_list.append(emit_corner(vi, reg, flg, u, v))

        parts_data[pname] = (corners_list, indices_list)
        print(f"  part '{pname}': {len(corners_list)} corners, {len(indices_list)//3} tris")

    # 6. Compute body hemisphere AO + mean curvature, landmark chains, and 512x512 RGBA8 UV maps
    ao_per_v, curv_per_v = compute_body_ao_and_curvature(base_positions_m, faces)
    landmark_chains = extract_landmark_chains(base_positions_m, faces, joint_centers_m)
    print(f"  landmarks: {[len(c) for c in landmark_chains]}")
    mask_rgba, detail_rgba = build_uv_maps_512(
        faces, vts, base_positions_m, lip_face_ids, eyelid_face_ids, scalp_face_ids,
        joint_centers_m, ao_per_v, curv_per_v, sss_tex,
    )

    # 7. Serialize binary .vcrcharbase v2 file
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_out = out_path.with_suffix(".vcrcharbase.tmp")
    gender_id = 0 if gender == "female" else 1
    with tmp_out.open("wb") as f:
        # 48-byte header
        f.write(struct.pack(
            "<8sIIIIIIIIII",
            MAGIC, VERSION, gender_id, vcount, len(morph_channels),
            len(PART_ORDER), MASK_SIZE, BONE_COUNT, len(landmark_chains), 1, 0,
        ))
        # Base welded positions (float3)
        for x, y, z in base_positions_m:
            f.write(struct.pack("<fff", x, y, z))
        # Per-welded-vertex 50-bone skinning weights (4x uint16 + 4x float)
        for js, ws in skin_rows:
            f.write(struct.pack("<HHHHffff", js[0], js[1], js[2], js[3], ws[0], ws[1], ws[2], ws[3]))
        # 50 bone vertex locators
        for v_list, (ox, oy, oz) in bone_locators:
            f.write(struct.pack("<fffI", ox, oy, oz, len(v_list)))
            if v_list:
                f.write(struct.pack(f"<{len(v_list)}H", *v_list))
        # 18 sparse morph channels
        for deltas in morph_channels:
            f.write(struct.pack("<I", len(deltas)))
            for vid, dx, dy, dz in deltas:
                f.write(struct.pack("<HHfff", vid, 0, dx, dy, dz))
        # 5 mesh parts
        for pname in PART_ORDER:
            corners, indices = parts_data[pname]
            f.write(struct.pack("<II", len(corners), len(indices)))
            for vi, reg, flg, u, v in corners:
                f.write(struct.pack("<HBBff", vi, reg, flg, u, v))
            if indices:
                f.write(struct.pack(f"<{len(indices)}I", *indices))
        # 512x512 RGBA8 skin feature mask & 512x512 RGBA8 skin detail map (AO/Curv/SSS/Nail+Scalp)
        assert len(mask_rgba) == MASK_SIZE * MASK_SIZE * 4
        assert len(detail_rgba) == MASK_SIZE * MASK_SIZE * 4
        f.write(mask_rgba)
        f.write(detail_rgba)
        # 6 landmark welded vertex chains
        for chain in landmark_chains:
            f.write(struct.pack("<H", len(chain)))
            if chain:
                f.write(struct.pack(f"<{len(chain)}H", *chain))

    os.replace(tmp_out, out_path)
    print(f"  wrote {out_path} ({out_path.stat().st_size:,} bytes)")


def main():
    repo_root = Path(__file__).resolve().parent.parent
    out_dir = repo_root / "assets" / "character_templates"
    build_template_for_gender("female", out_dir / "human_female.vcrcharbase")
    build_template_for_gender("male", out_dir / "human_male.vcrcharbase")
    print("[build_human_base_templates] Done.")


if __name__ == "__main__":
    main()
