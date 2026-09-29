#!/usr/bin/env python3
"""Geometry for the double-crossing case illustrations (standard library only).

Face connectivity comes exclusively from generate_double_crossing_tables.
Coordinates are illustrative: each 2D state is repeated at z=0 and z=1,
with no crossings on the four z-directed edges. Yellow turns are placed
0.20 units inside the corresponding face, not computed from input geometry.
"""

from collections import Counter

import generate_double_crossing_tables as tables


CUBE_EDGES = (
    (0, 1), (2, 3), (4, 5), (6, 7),
    (0, 2), (1, 3), (4, 6), (5, 7),
    (0, 4), (1, 5), (2, 6), (3, 7),
)
CUBE_FACES = (
    (0, 1, 3, 2), (4, 5, 7, 6), (0, 1, 5, 4),
    (2, 3, 7, 6), (0, 2, 6, 4), (1, 3, 7, 5),
)
_FACE_EDGES = (
    (0, 5, 1, 4), (2, 7, 3, 6), (0, 9, 2, 8),
    (1, 11, 3, 10), (4, 10, 6, 8), (5, 11, 7, 9),
)
_FACE_CORNERS = ((0, 0), (1, 0), (1, 1), (0, 1))
_TURN_POINTS = ((0.5, 0.2), (0.8, 0.5), (0.5, 0.8), (0.2, 0.5))


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _corner(index):
    return [index & 1, (index >> 1) & 1, (index >> 2) & 1]


def _point_between(a, b, t):
    return [x + t * (y - x) for x, y in zip(a, b)]


def _crossing_parameter(count, slot):
    _require(0 <= slot < count <= 2, "Invalid crossing slot")
    return 0.5 if count == 1 else (0.32, 0.68)[slot]


def _local_crossing(token, counts):
    edge, slot = map(int, token[1:].split(":"))
    return _point_between(
        _FACE_CORNERS[edge], _FACE_CORNERS[(edge + 1) % 4],
        _crossing_parameter(counts[edge], slot),
    )


def _face_point(face, uv):
    origin = _corner(CUBE_FACES[face][0])
    u_end = _corner(CUBE_FACES[face][1])
    v_end = _corner(CUBE_FACES[face][3])
    return [origin[i] + uv[0] * (u_end[i] - origin[i])
            + uv[1] * (v_end[i] - origin[i]) for i in range(3)]


def _sorted_paths(word):
    paths = tables.unpack(word)
    _require(paths is not None, "A cube face has an invalid table key")
    return sorted(paths, key=lambda path: (
        tables.boundary_id(path[0]), tables.boundary_id(path[-1]), len(path) == 3))


def _coordinate_edge(a, b):
    return tuple(sorted((tuple(round(x, 12) for x in a),
                         tuple(round(x, 12) for x in b))))


def _patches_match_boundary(patches, face_paths, nodes):
    edges = Counter()
    for patch in patches:
        for i, point in enumerate(patch):
            edges[_coordinate_edge(point, patch[(i + 1) % len(patch)])] += 1
    if any(count > 2 for count in edges.values()):
        return False
    boundary = Counter({edge: count for edge, count in edges.items() if count == 1})
    expected = Counter()
    for path in face_paths:
        for a, b in zip(path["nodes"], path["nodes"][1:]):
            expected[_coordinate_edge(nodes[str(a)], nodes[str(b)])] += 1
    return boundary == expected


def _build_case(name, corners, pairs, seed_paths, words):
    face_key = corners | (pairs << 4)
    counts = tables.crossing_counts(corners, pairs)
    _require(counts is not None, "Invalid source face state")
    _require(tables.signature(seed_paths) == tables.signature(tables.unpack(words[face_key])),
             "Source seed does not match generated face connectivity")

    # c2/c3 differ from the cube's binary-coordinate ordering.
    cube_corners = sum(((corners >> local) & 1) << vertex
                       for face in CUBE_FACES[:2]
                       for local, vertex in enumerate(face))
    cube_counts = [counts[0], counts[2], counts[0], counts[2],
                   counts[3], counts[1], counts[3], counts[1], 0, 0, 0, 0]
    cube_pairs = sum((count == 2) << edge for edge, count in enumerate(cube_counts))
    for edge, (a, b) in enumerate(CUBE_EDGES):
        parity = ((cube_corners >> a) ^ (cube_corners >> b)) & 1
        _require(cube_counts[edge] % 2 == parity, "Cube crossing parity mismatch")

    nodes = {}
    for edge, count in enumerate(cube_counts):
        a, b = CUBE_EDGES[edge]
        for slot in range(count):
            nodes[str(2 * edge + slot)] = _point_between(
                _corner(a), _corner(b), _crossing_parameter(count, slot))

    adjacency = {}
    face_paths = []
    for face, face_vertices in enumerate(CUBE_FACES):
        local_corners = sum(((cube_corners >> vertex) & 1) << local
                            for local, vertex in enumerate(face_vertices))
        local_pairs = sum(((cube_pairs >> edge) & 1) << local
                          for local, edge in enumerate(_FACE_EDGES[face]))
        if face < 2:
            _require((local_corners, local_pairs) == (corners, pairs),
                     "Seed-to-cube face mapping mismatch")
        paths = _sorted_paths(words[local_corners | (local_pairs << 4)])

        def cube_node(token):
            local_edge, slot = map(int, token[1:].split(":"))
            edge = _FACE_EDGES[face][local_edge]
            count = cube_counts[edge]
            _require(slot < count, "Face references an absent crossing")
            if CUBE_EDGES[edge][0] != face_vertices[local_edge]:
                slot = count - 1 - slot
            return 2 * edge + slot

        yellow_slot = 0
        for path in paths:
            ids = [cube_node(path[0])]
            if len(path) == 3:
                local_edge = tables.boundary_id(path[0]) // 2
                _require(local_edge == tables.boundary_id(path[-1]) // 2,
                         "A yellow path must return to the same edge")
                yellow = 24 + 4 * face + yellow_slot
                yellow_slot += 1
                nodes[str(yellow)] = _face_point(face, _TURN_POINTS[local_edge])
                ids.append(yellow)
            ids.append(cube_node(path[-1]))
            face_paths.append({"face": face, "nodes": ids})
            for a, b in zip(ids, ids[1:]):
                _require(a != b and b not in adjacency.get(a, []),
                         "Duplicate or degenerate cube boundary segment")
                adjacency.setdefault(a, []).append(b)
                adjacency.setdefault(b, []).append(a)

    _require(all(len(neighbors) == 2 for neighbors in adjacency.values()),
             "Cube boundary node degree must be two")
    expected_crossings = {2 * edge + slot for edge, count in enumerate(cube_counts)
                          for slot in range(count)}
    _require({node for node in adjacency if node < 24} == expected_crossings,
             "Cube boundary crossing coverage mismatch")
    _require({int(node) for node in nodes} == set(adjacency),
             "Illustration coordinates do not cover the boundary graph")

    loops, visited = [], set()
    for start in sorted(adjacency):
        if start in visited:
            continue
        loop = [start]
        visited.add(start)
        previous, current = start, min(adjacency[start])
        while current != start:
            _require(current not in visited, "Cube loop repeats a node")
            visited.add(current)
            loop.append(current)
            neighbors = adjacency[current]
            _require(previous in neighbors, "Broken cube boundary adjacency")
            previous, current = current, next(node for node in neighbors if node != previous)
        _require(len(loop) >= 3, "Cube boundary loop is too short")
        loops.append(loop)

    patches = []
    for path in seed_paths:
        points = []
        for token in path:
            if token.startswith("y"):
                edge = tables.boundary_id(path[0]) // 2
                points.append(list(_TURN_POINTS[edge]))
            else:
                points.append(_local_crossing(token, counts))
        for a, b in zip(points, points[1:]):
            patches.append([a + [0.0], b + [0.0], b + [1.0], a + [1.0]])

    result = {
        "name": name,
        "face_key": face_key,
        "face_corners": corners,
        "face_pairs": pairs,
        "face_counts": counts,
        "cube_corners": cube_corners,
        "cube_pairs": cube_pairs,
        "cube_counts": cube_counts,
        "nodes": nodes,
        "loops": loops,
        "face_paths": face_paths,
        "seed_paths": [list(path) for path in seed_paths],
    }
    # These illustrative ruled patches are exposed only if their complete
    # boundary agrees with the six-face public API, including yellow turns.
    if _patches_match_boundary(patches, face_paths, nodes):
        result["patches"] = patches
    return result


def build_cases(include_all_faces=False):
    """Return 23 named seed examples, or all 82 compatible face examples.

    The default preserves SEEDS order/names. The expanded version is ordered
    by face key, retains seed names for exact seed keys, and names other states
    ``face-XX`` using their hexadecimal key. All node IDs follow the C++ API;
    dictionary keys are strings so the result can be serialized as JSON.
    """
    words, _, _ = tables.build_tables()
    if not include_all_faces:
        seeds = tables.SEEDS
    else:
        names = {corners | (pairs << 4): name
                 for name, corners, pairs, _ in tables.SEEDS}
        seeds = [(names.get(key, f"face-{key:02X}"), key & 15, key >> 4,
                  tables.unpack(word))
                 for key, word in enumerate(words) if word & tables.VALID_BIT]
    cases = [_build_case(name, corners, pairs, paths, words)
             for name, corners, pairs, paths in seeds]
    _require(len(cases) == (82 if include_all_faces else 23),
             "Unexpected number of illustrated cases")
    return cases
