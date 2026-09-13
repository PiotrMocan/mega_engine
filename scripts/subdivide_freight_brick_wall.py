"""Prepare the cooked brick faces for GPU tessellation, without moving vertices.

Run with native Windows Python. Authoring GLBs remain the low-poly source;
package_freight_yard.py reapplies this deterministic step after each import.
Only the brick material is subdivided. Concrete and material settings survive.
"""
from pathlib import Path
import json
import math
import struct


ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / 'assets/freight_yard/kit'
MAXIMUM_EDGES = (0.125, 0.25, 0.5)
BRICK = 'materials/FY_broken_brick_wall.vcrmaterial.json'


def subdivide_model(metadata_path, maximum_edge):
    document = json.loads(metadata_path.read_text(encoding='utf-8'))
    mesh_path = (metadata_path.parent / document['mesh']).resolve()
    mesh_path.relative_to(metadata_path.parent.resolve())
    original = mesh_path.read_bytes()
    magic, version, stride, nv, ni, ns, reserved = struct.unpack_from(
        '<8sIIQQII', original)
    if (magic, version, stride, reserved) != (b'VCRMESH\0', 1, 48, 0):
        raise ValueError(f'Unsupported cooked mesh: {mesh_path}')
    offset = 40 + 24 * ns
    if len(original) != offset + nv * stride + ni * 4:
        raise ValueError('Cooked mesh size does not match its header')
    vertices = [struct.unpack_from('<12f', original, offset + i * stride)
                for i in range(nv)]
    indices = struct.unpack_from(f'<{ni}I', original, offset + nv * stride)
    if ns != len(document['submeshes']) or any(i >= nv for i in indices):
        raise ValueError('Invalid cooked mesh ranges')

    selected = []
    longest = 0.0
    for number, submesh in enumerate(document['submeshes']):
        first, count, material, padding = struct.unpack_from(
            '<QQII', original, 40 + 24 * number)
        if (first, count, material, padding) != (
                submesh['first_index'], submesh['index_count'],
                submesh['material_index'], 0):
            raise ValueError('Metadata and binary submesh ranges differ')
        if submesh['material'] != BRICK:
            continue
        selected.append(number)
        for start in range(first, first + count, 3):
            a, b, c = (vertices[i] for i in indices[start:start + 3])
            # This preparation is for the existing planar brick faces only.
            # Interpolating a curved surface would require a different policy.
            if not (a[3:6] == b[3:6] == c[3:6]
                    and a[8:12] == b[8:12] == c[8:12]):
                raise ValueError('Brick face normals/tangents are not constant')
            longest = max(longest, math.dist(a[:3], b[:3]),
                          math.dist(b[:3], c[:3]), math.dist(c[:3], a[:3]))
    if not selected:
        raise ValueError(f'No brick faces in {metadata_path}')

    factor = 1
    while longest / factor > maximum_edge * (1.0 + 1e-5):
        factor *= 2
    if factor == 1:
        return {'model': document['name'], 'changed': False}
    triangle_count = sum(s['index_count'] // 3 *
                         (factor * factor if i in selected else 1)
                         for i, s in enumerate(document['submeshes']))
    if triangle_count > 200_000:
        raise ValueError('Brick subdivision exceeds the offline triangle budget')

    new_vertices = list(vertices)
    new_indices = []
    cache = {}

    def vertex(triangle, i, j):
        # Canonical integer weights produce the SAME vertex on a shared edge,
        # regardless of either triangle's ordering. Power-of-two subdivision
        # keeps regular dyadic spacing within each original face.
        key = tuple(sorted((index, weight) for index, weight in
                           zip(triangle, (factor - i - j, i, j)) if weight))
        if len(key) == 1:
            return key[0][0]
        if key not in cache:
            value = tuple(sum(vertices[index][k] * weight for index, weight in key)
                          / factor for k in range(12))
            cache[key] = len(new_vertices)
            new_vertices.append(value)
        return cache[key]

    binary_submeshes = bytearray()
    for number, submesh in enumerate(document['submeshes']):
        first, count = submesh['first_index'], submesh['index_count']
        new_first = len(new_indices)
        if number not in selected:
            new_indices.extend(indices[first:first + count])
        else:
            for start in range(first, first + count, 3):
                triangle = indices[start:start + 3]
                for i in range(factor):
                    for j in range(factor - i):
                        a, b, c = (vertex(triangle, i, j),
                                   vertex(triangle, i + 1, j),
                                   vertex(triangle, i, j + 1))
                        new_indices.extend((a, b, c))
                        if i + j < factor - 1:
                            new_indices.extend((b, vertex(triangle, i + 1, j + 1), c))
        submesh['first_index'] = new_first
        submesh['index_count'] = len(new_indices) - new_first
        binary_submeshes.extend(struct.pack(
            '<QQII', new_first, submesh['index_count'], submesh['material_index'], 0))

    document['geometry']['vertex_count'] = len(new_vertices)
    document['geometry']['index_count'] = len(new_indices)
    binary = struct.pack('<8sIIQQII', magic, version, stride,
                         len(new_vertices), len(new_indices), ns, 0)
    binary += binary_submeshes
    binary += b''.join(struct.pack('<12f', *v) for v in new_vertices)
    binary += struct.pack(f'<{len(new_indices)}I', *new_indices)
    mesh_path.write_bytes(binary)
    metadata_path.write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
    return {'model': document['name'], 'changed': True, 'factor': factor,
            'triangles_before': ni // 3, 'triangles_after': len(new_indices) // 3,
            'maximum_brick_edge_metres': longest / factor}


def subdivide_brick_wall(kit=KIT):
    return [subdivide_model(
        kit / ('broken_brick_wall' + (f'_lod{level}' if level else '') +
               '.vcrasset.json'), maximum_edge)
        for level, maximum_edge in enumerate(MAXIMUM_EDGES)]


if __name__ == '__main__':
    print(json.dumps(subdivide_brick_wall(), indent=2))
