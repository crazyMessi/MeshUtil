#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

import numpy as np

try:
    from numba import njit
except ImportError:
    njit = None


def _count_components_union_find(first_vertices, second_vertices, used):
    parent = np.full(used.size, -1, dtype=np.int32)
    sizes = np.zeros(used.size, dtype=np.int32)
    for vertex in range(used.size):
        if used[vertex]:
            parent[vertex] = vertex
            sizes[vertex] = 1

    for edge in range(first_vertices.size):
        first = first_vertices[edge]
        second = second_vertices[edge]
        first_root = first
        while parent[first_root] != first_root:
            parent[first_root] = parent[parent[first_root]]
            first_root = parent[first_root]
        second_root = second
        while parent[second_root] != second_root:
            parent[second_root] = parent[parent[second_root]]
            second_root = parent[second_root]
        if first_root == second_root:
            continue
        if sizes[first_root] < sizes[second_root]:
            first_root, second_root = second_root, first_root
        parent[second_root] = first_root
        sizes[first_root] += sizes[second_root]

    component_count = 0
    for vertex in range(used.size):
        if parent[vertex] == vertex:
            component_count += 1
    return component_count


if njit is not None:
    _count_components_union_find = njit(cache=False)(
        _count_components_union_find
    )


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


def count_connected_components(unique_edges, used):
    first = (unique_edges >> np.uint64(32)).astype(np.uint32)
    second = (unique_edges & np.uint64(0xFFFFFFFF)).astype(np.uint32)
    if njit is not None:
        return (
            int(_count_components_union_find(first, second, used)),
            "numba_union_find",
        )

    try:
        from scipy.sparse import coo_matrix
        from scipy.sparse.csgraph import connected_components
    except ImportError:
        return (
            int(_count_components_union_find(first, second, used)),
            "python_union_find",
        )

    graph = coo_matrix(
        (np.ones(unique_edges.size, dtype=np.bool_), (first, second)),
        shape=(used.size, used.size),
    )
    all_components = connected_components(
        graph, directed=False, return_labels=False
    )
    return int(all_components - np.count_nonzero(~used)), "scipy_sparse"


def find_roots(parent):
    roots = np.empty(parent.size, dtype=np.int64)
    for vertex in range(parent.size):
        root = vertex
        while parent[root] != root:
            root = parent[root]
        roots[vertex] = root
        while parent[vertex] != vertex:
            next_vertex = parent[vertex]
            parent[vertex] = root
            vertex = next_vertex
    return roots


def inspect_boundary(unique_edges, edge_counts, vertices, diagonal):
    boundary_edges = unique_edges[edge_counts == 1]
    boundary_edge_count = int(boundary_edges.size)
    if boundary_edge_count == 0:
        return {
            "boundary_edge_count": 0,
            "boundary_vertex_count": 0,
            "boundary_component_count": 0,
            "boundary_loop_count": 0,
            "boundary_open_chain_count": 0,
            "boundary_branched_component_count": 0,
            "boundary_endpoint_vertex_count": 0,
            "boundary_branch_vertex_count": 0,
            "boundary_total_length": 0.0,
            "boundary_total_length_normalized": 0.0,
        }

    endpoints = np.empty((boundary_edge_count, 2), dtype=np.uint32)
    endpoints[:, 0] = (boundary_edges >> np.uint64(32)).astype(np.uint32)
    endpoints[:, 1] = (
        boundary_edges & np.uint64(0xFFFFFFFF)
    ).astype(np.uint32)
    boundary_vertices, inverse, degrees = np.unique(
        endpoints.reshape(-1), return_inverse=True, return_counts=True
    )
    local_edges = inverse.reshape(-1, 2)

    parent = np.arange(boundary_vertices.size, dtype=np.int64)
    sizes = np.ones(boundary_vertices.size, dtype=np.int64)
    for first, second in local_edges:
        first_root = first
        while parent[first_root] != first_root:
            parent[first_root] = parent[parent[first_root]]
            first_root = parent[first_root]
        second_root = second
        while parent[second_root] != second_root:
            parent[second_root] = parent[parent[second_root]]
            second_root = parent[second_root]
        if first_root == second_root:
            continue
        if sizes[first_root] < sizes[second_root]:
            first_root, second_root = second_root, first_root
        parent[second_root] = first_root
        sizes[first_root] += sizes[second_root]

    roots = find_roots(parent)
    _, component_ids = np.unique(roots, return_inverse=True)
    component_count = int(component_ids.max()) + 1
    component_vertex_counts = np.bincount(
        component_ids, minlength=component_count
    )
    component_edge_counts = np.bincount(
        component_ids[local_edges[:, 0]], minlength=component_count
    )
    component_endpoint_counts = np.bincount(
        component_ids, weights=degrees == 1, minlength=component_count
    )
    component_branch_counts = np.bincount(
        component_ids, weights=degrees > 2, minlength=component_count
    )
    loops = (
        (component_edge_counts == component_vertex_counts)
        & (component_endpoint_counts == 0)
        & (component_branch_counts == 0)
    )
    open_chains = (
        (component_edge_counts + 1 == component_vertex_counts)
        & (component_endpoint_counts == 2)
        & (component_branch_counts == 0)
    )

    boundary_total_length = 0.0
    chunk_edges = 1_000_000
    for first in range(0, boundary_edge_count, chunk_edges):
        block = endpoints[first : first + chunk_edges]
        first_positions = np.asarray(vertices[block[:, 0]], dtype=np.float64)
        second_positions = np.asarray(vertices[block[:, 1]], dtype=np.float64)
        boundary_total_length += float(
            np.linalg.norm(second_positions - first_positions, axis=1).sum()
        )

    return {
        "boundary_edge_count": boundary_edge_count,
        "boundary_vertex_count": int(boundary_vertices.size),
        "boundary_component_count": component_count,
        "boundary_loop_count": int(np.count_nonzero(loops)),
        "boundary_open_chain_count": int(np.count_nonzero(open_chains)),
        "boundary_branched_component_count": int(
            component_count - np.count_nonzero(loops) - np.count_nonzero(open_chains)
        ),
        "boundary_endpoint_vertex_count": int(np.count_nonzero(degrees == 1)),
        "boundary_branch_vertex_count": int(np.count_nonzero(degrees > 2)),
        "boundary_total_length": boundary_total_length,
        "boundary_total_length_normalized": (
            boundary_total_length / diagonal if diagonal > 0.0 else 0.0
        ),
    }


def inspect(path, near_zero_relative, scale_diagonal=None):
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
    quality_scale_diagonal = (
        diagonal if scale_diagonal is None else float(scale_diagonal)
    )
    near_zero_area = (
        near_zero_relative * quality_scale_diagonal * quality_scale_diagonal
    )

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
        valid_faces = valid_indices.all(axis=1)
        if not valid_faces.any():
            continue
        indices = indices[valid_faces]
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
        manifold_edges = int(np.count_nonzero(edge_counts == 2))
        nonmanifold_edges = int(np.count_nonzero(edge_counts > 2))
        unique_edge_count = int(unique_edges.size)
    else:
        unique_edges = np.empty(0, dtype=np.uint64)
        edge_counts = np.empty(0, dtype=np.int64)
        manifold_edges = nonmanifold_edges = unique_edge_count = 0

    connected_component_count, component_backend = count_connected_components(
        unique_edges, used
    )
    boundary = inspect_boundary(
        unique_edges, edge_counts, vertices, quality_scale_diagonal
    )

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
        "quality_scale_diagonal": quality_scale_diagonal,
        "used_vertex_count": int(np.count_nonzero(used)),
        "isolated_vertex_count": int(vertex_count - np.count_nonzero(used)),
        "connected_component_count": connected_component_count,
        "connected_component_definition": "used vertices connected by mesh edges",
        "connected_component_backend": component_backend,
        "unique_edge_count": unique_edge_count,
        "manifold_edge_count": manifold_edges,
        "nonmanifold_edge_count": nonmanifold_edges,
    }
    result.update(boundary)
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


COUNT_GATES = (
    "nonfinite_vertex_count",
    "invalid_corner_count",
    "invalid_index_count",
    "repeated_index_face_count",
    "zero_area_face_count",
    "near_zero_area_face_count",
    "isolated_vertex_count",
    "nonmanifold_edge_count",
    "connected_component_count",
    "boundary_edge_count",
    "boundary_vertex_count",
    "boundary_component_count",
    "boundary_loop_count",
    "boundary_open_chain_count",
    "boundary_branched_component_count",
    "boundary_endpoint_vertex_count",
    "boundary_branch_vertex_count",
)


def compare_inspections(reference, output, near_zero_relative):
    deltas = {}
    for metric in COUNT_GATES:
        reference_value = reference[metric]
        output_value = output[metric]
        deltas[metric] = {
            "input": reference_value,
            "output": output_value,
            "delta": output_value - reference_value,
            "passed": output_value <= reference_value,
        }

    length_metric = "boundary_total_length"
    reference_length = reference[length_metric]
    output_length = output[length_metric]
    length_tolerance = max(
        1.0e-12 * max(reference_length, 1.0),
        1.0e-12 * max(reference["quality_scale_diagonal"], 1.0),
    )
    deltas[length_metric] = {
        "input": reference_length,
        "output": output_length,
        "delta": output_length - reference_length,
        "tolerance": length_tolerance,
        "passed": output_length <= reference_length + length_tolerance,
    }

    format_gates = {
        "output_file_size_valid": output["file_size_valid"],
    }
    passed = all(entry["passed"] for entry in deltas.values()) and all(
        format_gates.values()
    )
    return {
        "protocol": {
            "rule": "every topology or degeneracy metric must satisfy output <= input",
            "connected_component_definition": reference[
                "connected_component_definition"
            ],
            "near_zero_area_relative": near_zero_relative,
            "near_zero_area_scale": "input bbox diagonal squared",
            "boundary_length_tolerance": "floating-point summation only",
        },
        "input": reference,
        "output": output,
        "deltas": deltas,
        "format_gates": format_gates,
        "passed": passed,
    }


def compare(reference_path, output_path, near_zero_relative):
    reference = inspect(reference_path, near_zero_relative)
    output = inspect(
        output_path,
        near_zero_relative,
        scale_diagonal=reference["quality_scale_diagonal"],
    )
    return compare_inspections(reference, output, near_zero_relative)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mesh", type=Path)
    parser.add_argument(
        "--reference",
        type=Path,
        help="input mesh for output<=input topology and degeneracy gates",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--near-zero-relative", type=float, default=1.0e-14)
    arguments = parser.parse_args()
    if arguments.reference:
        result = compare(
            arguments.reference, arguments.mesh, arguments.near_zero_relative
        )
    else:
        result = inspect(arguments.mesh, arguments.near_zero_relative)
    text = json.dumps(result, indent=2) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
