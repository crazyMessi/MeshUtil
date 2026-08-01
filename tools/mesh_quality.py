#!/usr/bin/env python3

import argparse
import json
import math
import struct
from pathlib import Path

import numpy as np


def parse_header(path):
    lines = []
    with path.open("rb") as stream:
        while True:
            raw = stream.readline()
            if not raw:
                raise ValueError("truncated PLY header")
            line = raw.decode("ascii").rstrip("\r\n")
            lines.append(line)
            if line == "end_header":
                return lines, stream.tell()


def read_counts(lines):
    vertex_count = None
    face_count = None
    for line in lines:
        if line.startswith("element vertex "):
            vertex_count = int(line.split()[2])
        elif line.startswith("element face "):
            face_count = int(line.split()[2])
    if vertex_count is None or face_count is None:
        raise ValueError("PLY must contain vertex and face elements")
    return vertex_count, face_count


def inspect(path, near_zero_relative):
    lines, payload_offset = parse_header(path)
    if "format binary_little_endian 1.0" not in lines:
        raise ValueError("only binary little-endian PLY is supported")
    vertex_count, face_count = read_counts(lines)
    face_property = next(
        (line for line in lines if line.startswith("property list uchar ")), None
    )
    if face_property not in {
        "property list uchar int vertex_indices",
        "property list uchar int32 vertex_indices",
        "property list uchar uint vertex_indices",
        "property list uchar uint32 vertex_indices",
    }:
        raise ValueError("unsupported PLY face property")

    vertices = np.memmap(
        path,
        dtype=np.dtype("<f4"),
        mode="r",
        offset=payload_offset,
        shape=(vertex_count, 3),
    )
    face_offset = payload_offset + vertex_count * 12
    face_dtype = np.dtype([("corners", "u1"), ("indices", "<u4", (3,))])
    faces = np.memmap(
        path,
        dtype=face_dtype,
        mode="r",
        offset=face_offset,
        shape=(face_count,),
    )
    expected_size = face_offset + face_count * 13

    finite_vertices = np.isfinite(vertices).all(axis=1)
    position_min = np.nanmin(vertices, axis=0)
    position_max = np.nanmax(vertices, axis=0)
    diagonal = float(np.linalg.norm(position_max - position_min))
    near_zero_area = near_zero_relative * diagonal * diagonal

    chunk_faces = 1_000_000
    invalid_corner_count = 0
    invalid_index_count = 0
    repeated_index_count = 0
    zero_area_count = 0
    near_zero_area_count = 0
    used = np.zeros(vertex_count, dtype=np.bool_)
    edge_chunks = []

    for first in range(0, face_count, chunk_faces):
        block = faces[first : first + chunk_faces]
        corners = np.asarray(block["corners"])
        indices = np.asarray(block["indices"])
        invalid_corner_count += int(np.count_nonzero(corners != 3))
        valid_indices = indices < vertex_count
        invalid_index_count += int(indices.size - np.count_nonzero(valid_indices))
        if not valid_indices.all():
            continue
        repeated = (
            (indices[:, 0] == indices[:, 1])
            | (indices[:, 1] == indices[:, 2])
            | (indices[:, 2] == indices[:, 0])
        )
        repeated_index_count += int(np.count_nonzero(repeated))
        used[indices.reshape(-1)] = True

        first_position = np.asarray(vertices[indices[:, 0]], dtype=np.float64)
        second_position = np.asarray(vertices[indices[:, 1]], dtype=np.float64)
        third_position = np.asarray(vertices[indices[:, 2]], dtype=np.float64)
        twice_area = np.linalg.norm(
            np.cross(second_position - first_position, third_position - first_position),
            axis=1,
        )
        zero_area_count += int(np.count_nonzero(twice_area == 0.0))
        near_zero_area_count += int(np.count_nonzero(twice_area <= 2.0 * near_zero_area))

        edges = np.concatenate(
            [
                indices[:, [0, 1]],
                indices[:, [1, 2]],
                indices[:, [2, 0]],
            ],
            axis=0,
        )
        edges.sort(axis=1)
        packed = (edges[:, 0].astype(np.uint64) << np.uint64(32)) | edges[
            :, 1
        ].astype(np.uint64)
        packed.sort()
        edge_chunks.append(packed)

    if edge_chunks:
        all_edges = np.concatenate(edge_chunks)
        all_edges.sort()
        unique_edges, edge_counts = np.unique(all_edges, return_counts=True)
        boundary_edges = int(np.count_nonzero(edge_counts == 1))
        manifold_edges = int(np.count_nonzero(edge_counts == 2))
        nonmanifold_edges = int(np.count_nonzero(edge_counts > 2))
        unique_edge_count = int(unique_edges.size)
    else:
        boundary_edges = manifold_edges = nonmanifold_edges = unique_edge_count = 0

    result = {
        "path": str(path),
        "file_bytes": path.stat().st_size,
        "expected_file_bytes": expected_size,
        "file_size_valid": path.stat().st_size == expected_size,
        "vertex_count": vertex_count,
        "face_count": face_count,
        "nonfinite_vertex_count": int(vertex_count - np.count_nonzero(finite_vertices)),
        "invalid_corner_count": invalid_corner_count,
        "invalid_index_count": invalid_index_count,
        "repeated_index_face_count": repeated_index_count,
        "zero_area_face_count": zero_area_count,
        "near_zero_area_face_count": near_zero_area_count,
        "near_zero_area_relative": near_zero_relative,
        "bbox_diagonal": diagonal,
        "used_vertex_count": int(np.count_nonzero(used)),
        "isolated_vertex_count": int(vertex_count - np.count_nonzero(used)),
        "unique_edge_count": unique_edge_count,
        "boundary_edge_count": boundary_edges,
        "manifold_edge_count": manifold_edges,
        "nonmanifold_edge_count": nonmanifold_edges,
    }
    result["valid"] = all(
        [
            result["file_size_valid"],
            result["nonfinite_vertex_count"] == 0,
            invalid_corner_count == 0,
            invalid_index_count == 0,
            repeated_index_count == 0,
            zero_area_count == 0,
            nonmanifold_edges == 0,
        ]
    )
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mesh", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--near-zero-relative", type=float, default=1.0e-14)
    arguments = parser.parse_args()
    result = inspect(arguments.mesh, arguments.near_zero_relative)
    text = json.dumps(result, indent=2) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
