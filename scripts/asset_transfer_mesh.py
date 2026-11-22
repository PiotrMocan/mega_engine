"""Read GLB mesh accessors and build the tangent basis for native importers."""
import json
import struct

import numpy as np


def normalized(v, fallback):
    lengths = np.linalg.norm(v, axis=1, keepdims=True)
    result = v/np.maximum(lengths, 1e-20)
    result[lengths[:, 0] < 1e-12] = fallback
    return result


def tangent_basis(pos, normal, uv, indices):
    """Angle-weighted basis on native authoring UVs, matching Assimp imports."""
    tri = indices.reshape(-1, 3)
    p, t = pos[tri], uv[tri]
    e1, e2 = p[:, 1]-p[:, 0], p[:, 2]-p[:, 0]
    d1, d2 = t[:, 1]-t[:, 0], t[:, 2]-t[:, 0]
    det = d1[:, 0]*d2[:, 1]-d1[:, 1]*d2[:, 0]
    inverse = np.divide(1., det, out=np.zeros_like(det), where=np.abs(det) > 1e-15)
    tan = (e1*d2[:, 1, None]-e2*d1[:, 1, None])*inverse[:, None]
    bit = (e2*d1[:, 0, None]-e1*d2[:, 0, None])*inverse[:, None]
    accumulated_t = np.zeros_like(pos)
    accumulated_b = np.zeros_like(pos)
    for corner in range(3):
        a = normalized(p[:, (corner+1) % 3]-p[:, corner], [1, 0, 0])
        b = normalized(p[:, (corner+2) % 3]-p[:, corner], [0, 1, 0])
        weight = np.arccos(np.clip(np.sum(a*b, axis=1), -1., 1.))[:, None]
        np.add.at(accumulated_t, tri[:, corner], tan*weight)
        np.add.at(accumulated_b, tri[:, corner], bit*weight)
    accumulated_t -= normal*np.sum(normal*accumulated_t, axis=1)[:, None]
    # Orthogonal fallback for degenerate UV triangles, without deleting geometry.
    axis = np.zeros_like(pos)
    axis[:, 2] = 1
    axis[np.abs(normal[:, 2]) > .99] = [0, 1, 0]
    fallback = normalized(np.cross(axis, normal), [1, 0, 0])
    missing = np.linalg.norm(accumulated_t, axis=1) < 1e-10
    accumulated_t = normalized(accumulated_t, [1, 0, 0])
    accumulated_t[missing] = fallback[missing]
    sign = np.where(np.sum(np.cross(normal, accumulated_t)*accumulated_b, axis=1) < 0., -1., 1.)
    return np.column_stack((accumulated_t, sign))


class Glb:
    def __init__(self, path):
        self.data = path.read_bytes()
        magic, version, length = struct.unpack_from('<III', self.data)
        assert magic == 0x46546c67 and version == 2 and length == len(self.data), path
        size, kind = struct.unpack_from('<II', self.data, 12)
        assert kind == 0x4e4f534a
        self.json = json.loads(self.data[20:20+size])
        size_bin, kind_bin = struct.unpack_from('<II', self.data, 20+size)
        assert kind_bin == 0x004e4942
        self.binary = memoryview(self.data)[28+size:28+size+size_bin]

    def accessor(self, index):
        a = self.json['accessors'][index]
        assert 'sparse' not in a and not a.get('normalized', False)
        view = self.json['bufferViews'][a['bufferView']]
        dtype = {5121:'u1', 5123:'<u2', 5125:'<u4', 5126:'<f4'}[a['componentType']]
        width = {'SCALAR':1, 'VEC2':2, 'VEC3':3, 'VEC4':4}[a['type']]
        offset = view.get('byteOffset', 0)+a.get('byteOffset', 0)
        stride = view.get('byteStride', np.dtype(dtype).itemsize*width)
        assert offset + (a['count']-1)*stride+width*np.dtype(dtype).itemsize <= len(self.binary)
        return np.ndarray((a['count'], width), dtype=dtype, buffer=self.binary,
                          offset=offset, strides=(stride, np.dtype(dtype).itemsize)).copy()
