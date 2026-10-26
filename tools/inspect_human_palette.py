#!/usr/bin/env python3
"""Inspect the external Quaternius Human source palette and hand UVs.

Uses only the standard library; does not alter the source or Unreal assets.
The narrow readers support this pack's FBX 7400 and non-interlaced RGBA8 PNG.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import zlib


def read_fbx(data):
    if not data.startswith(b"Kaydara FBX Binary  \0\x1a\0"):
        raise ValueError("Expected binary FBX")
    version = struct.unpack_from("<I", data, 23)[0]
    if version != 7400:
        raise ValueError(f"Unsupported FBX version: {version}")
    offset = 27

    def read_property():
        nonlocal offset
        kind = chr(data[offset])
        offset += 1
        scalar = {"Y": "h", "C": "?", "I": "i", "F": "f", "D": "d", "L": "q"}
        if kind in scalar:
            fmt = "<" + scalar[kind]
            value = struct.unpack_from(fmt, data, offset)[0]
            offset += struct.calcsize(fmt)
            return value
        if kind in "SR":
            size = struct.unpack_from("<I", data, offset)[0]
            offset += 4
            value = data[offset:offset + size]
            offset += size
            return value
        if kind in "fdlibc":
            count, encoding, size = struct.unpack_from("<III", data, offset)
            offset += 12
            value = data[offset:offset + size]
            offset += size
            if encoding == 1:
                value = zlib.decompress(value)
            elif encoding != 0:
                raise ValueError(f"Unsupported FBX array encoding: {encoding}")
            fmt = {"f": "f", "d": "d", "l": "q", "i": "i", "b": "?", "c": "c"}[kind]
            return struct.unpack("<" + str(count) + fmt, value)
        raise ValueError(f"Unsupported FBX property: {kind}")

    def read_node():
        nonlocal offset
        end, count, _, name_size = struct.unpack_from("<IIIB", data, offset)
        offset += 13
        if end == 0:
            return None
        name = data[offset:offset + name_size].decode("utf-8")
        offset += name_size
        properties = [read_property() for _ in range(count)]
        children = []
        while offset < end:
            child = read_node()
            if child:
                children.append(child)
        if offset != end:
            raise ValueError("FBX node boundary mismatch")
        return name, properties, children

    nodes = []
    while offset < len(data):
        node = read_node()
        if node is None:
            break
        nodes.append(node)
    return nodes


def child(node, name):
    return next(item for item in node[2] if item[0] == name)


def read_png(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Expected PNG")
    offset, compressed = 8, []
    while offset < len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        chunk = data[offset + 8:offset + 8 + size]
        offset += size + 12
        if kind == b"IHDR":
            width, height, depth, colour, compression, filtering, interlace = struct.unpack(">IIBBBBB", chunk)
            if (depth, colour, compression, filtering, interlace) != (8, 6, 0, 0, 0):
                raise ValueError("Expected non-interlaced RGBA8 PNG")
        elif kind == b"IDAT":
            compressed.append(chunk)
    raw = zlib.decompress(b"".join(compressed))
    stride, rows, previous = width * 4, [], bytearray(width * 4)
    for y in range(height):
        start = y * (stride + 1)
        filtering = raw[start]
        row = bytearray(raw[start + 1:start + stride + 1])
        for x in range(stride):
            left, up, corner = row[x - 4] if x >= 4 else 0, previous[x], previous[x - 4] if x >= 4 else 0
            if filtering == 0:
                predictor = 0
            elif filtering == 1:
                predictor = left
            elif filtering == 2:
                predictor = up
            elif filtering == 3:
                predictor = (left + up) // 2
            elif filtering == 4:
                p = left + up - corner
                distances = [abs(p - value) for value in (left, up, corner)]
                predictor = (left, up, corner)[distances.index(min(distances))]
            else:
                raise ValueError(f"Unsupported PNG filter: {filtering}")
            row[x] = (row[x] + predictor) & 255
        rows.append([tuple(row[x:x + 4]) for x in range(0, stride, 4)])
        previous = row
    return width, height, rows


def inspect(root):
    fbx_bytes = (root / "FBX/Animated Human.fbx").read_bytes()
    png_bytes = (root / "Blend/Textures/ClothedLightSkin.png").read_bytes()
    nodes = read_fbx(fbx_bytes)
    objects = next(node for node in nodes if node[0] == "Objects")[2]
    connections = next(node for node in nodes if node[0] == "Connections")[2]
    by_id = {node[1][0]: node for node in objects}
    mesh = next(node for node in objects if node[0] == "Geometry" and node[1][2] == b"Mesh")
    layer = child(mesh, "LayerElementUV")
    if child(layer, "MappingInformationType")[1][0] != b"ByPolygonVertex" or child(layer, "ReferenceInformationType")[1][0] != b"IndexToDirect":
        raise ValueError("Expected polygon-vertex UVs indexed into the direct array")
    flat_uvs = child(layer, "UV")[1][0]
    uvs = list(zip(flat_uvs[::2], flat_uvs[1::2]))
    uv_indices = child(layer, "UVIndex")[1][0]
    vertices = child(mesh, "PolygonVertexIndex")[1][0]
    if len(uv_indices) != len(vertices) or not all(0 <= u <= 1 and 0 <= v <= 1 for u, v in uvs):
        raise ValueError("UV indices do not cover all polygon vertices inside the palette")
    width, height, pixels = read_png(png_bytes)
    skin = pixels[0][0]
    hands = []
    for bone in (node for node in objects if node[0] == "Model" and b"Hand" in node[1][1]):
        cluster_ids = [link[1][2] for link in connections if link[1][1] == bone[1][0]
                       and by_id.get(link[1][2], (None,))[0] == "Deformer"]
        for cluster_id in cluster_ids:
            cluster = by_id[cluster_id]
            if not any(node[0] == "Indexes" for node in cluster[2]):
                continue  # End bones have no skin influence.
            weighted = {v for v, w in zip(child(cluster, "Indexes")[1][0], child(cluster, "Weights")[1][0]) if w > 0.25}
            selected = {uvs[uv_indices[i]] for i, v in enumerate(vertices) if (v if v >= 0 else -v - 1) in weighted}
            colours = {pixels[min(height - 1, int((1 - v) * height))][min(width - 1, int(u * width))] for u, v in selected}
            hands.append({"bone": bone[1][1].split(b"\0")[0].decode(), "weighted_vertices_above_0_25": len(weighted),
                          "uvs": sorted(selected), "sampled_rgba": sorted(colours), "all_skin": colours == {skin}})
    if not {"LeftHand", "RightHand"}.issubset({hand["bone"] for hand in hands}):
        raise ValueError("Source does not expose both hand skin clusters")
    all_skin = all(hand["all_skin"] for hand in hands)
    return {"fbx_sha256": hashlib.sha256(fbx_bytes).hexdigest(),
            "png_sha256": hashlib.sha256(png_bytes).hexdigest(),
            "png_md5": hashlib.md5(png_bytes).hexdigest(),
            "palette_size": [width, height],
            "palette_rgba_counts": [{"rgba": rgba, "count": count} for rgba, count in sorted(Counter(pixel for row in pixels for pixel in row).items())],
            "hands": hands, "source_hand_uvs_all_sample_skin": all_skin,
            "scope": "Source UV/palette inspection only; does not verify imported mesh UVs, material rendering, or performance."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Extracted Animated Human by @Quaternius directory")
    args = parser.parse_args()
    report = inspect(args.source)
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report["source_hand_uvs_all_sample_skin"] else 1)
