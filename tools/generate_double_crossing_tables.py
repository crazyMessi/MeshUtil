#!/usr/bin/env python3
"""Reconstruct and validate the double-crossing symbolic face lookup tables.

Python 3.9+ and the standard library suffice. This is an independent
transcription of Matthias Mueller's *Fast and Robust Tracking of Fluid
Surfaces* (2009), Figures 5/6, selected columns 1, 2, 3, 4, 11, and 12:
https://matthias-research.github.io/pages/publications/surfaceTracking.pdf

These are oriented face paths for at most two crossings per grid edge, not
a three-dimensional triangle table. A yellow marker denotes a turn whose
position (and possible expansion into several vertices) requires geometry.
The generator neither imports external data nor needs the author's code.

Run normally to regenerate the header, or with --check to validate the
construction and compare it byte-for-byte with the existing header.
"""

import argparse
import sys
from pathlib import Path
from typing import Optional


VALID_BIT = 1 << 31
INVALID_CASE = 31
DEFAULT_OUTPUT = (
    Path(__file__).resolve().parents[1]
    / "include/meshutil/detail/double_crossing_tables.hpp"
)

# c0=(0,0), c1=(1,0), c2=(1,1), c3=(0,1). A corner bit of one is inside.
# Edge i runs counterclockwise from c_i to c_(i+1 mod 4).
# eI:J is crossing J in ascending edge parameter order. Seed paths are
# unoriented; y0 is an independent symbolic interior turn, not a coordinate.
# The seeds below are manually transcribed, not obtained from the compact
# boundary-interval rule. Their D4 expansion independently checks that rule.
SEEDS = [
    ("1a", 0, 0, []),
    ("1b", 0, 1, [["e0:0", "y0", "e0:1"]]),
    ("1c", 0, 3, [["e0:1", "e1:0"], ["e1:1", "e0:0"]]),
    ("1d", 0, 5, [["e0:1", "e2:0"], ["e2:1", "e0:0"]]),
    ("1e", 0, 7, [["e0:1", "e1:0"], ["e1:1", "e2:0"], ["e2:1", "e0:0"]]),
    ("1f", 0, 15, [["e0:1", "e1:0"], ["e1:1", "e2:0"], ["e2:1", "e3:0"], ["e3:1", "e0:0"]]),
    ("2a", 1, 0, [["e0:0", "e3:0"]]),
    ("2b", 1, 2, [["e0:0", "e1:0"], ["e1:1", "e3:0"]]),
    ("2c", 1, 6, [["e0:0", "e1:0"], ["e1:1", "e2:0"], ["e2:1", "e3:0"]]),
    ("3a", 3, 0, [["e1:0", "e3:0"]]),
    ("3b", 3, 1, [["e0:0", "y0", "e0:1"], ["e1:0", "e3:0"]]),
    ("3c", 3, 4, [["e1:0", "e2:0"], ["e2:1", "e3:0"]]),
    ("3d", 3, 5, [["e0:0", "y0", "e0:1"], ["e1:0", "e2:0"], ["e2:1", "e3:0"]]),
    ("4a", 5, 0, [["e0:0", "e1:0"], ["e2:0", "e3:0"]]),
    ("11a", 7, 0, [["e2:0", "e3:0"]]),
    ("11b", 7, 1, [["e0:1", "e2:0"], ["e3:0", "e0:0"]]),
    ("11c", 7, 3, [["e0:1", "e1:0"], ["e1:1", "e2:0"], ["e3:0", "e0:0"]]),
    ("12a", 15, 0, []),
    ("12b", 15, 1, [["e0:0", "y0", "e0:1"]]),
    ("12c", 15, 3, [["e0:1", "e1:0"], ["e1:1", "e0:0"]]),
    ("12d", 15, 5, [["e0:1", "e2:0"], ["e2:1", "e0:0"]]),
    ("12e", 15, 7, [["e0:1", "e1:0"], ["e1:1", "e2:0"], ["e2:1", "e0:0"]]),
    ("12f", 15, 15, [["e0:1", "e1:0"], ["e1:1", "e2:0"], ["e2:1", "e3:0"], ["e3:1", "e0:0"]]),
]


def require(condition: bool, message: str) -> None:
    # Do not let Python's -O option disable generator validation.
    if not condition:
        raise ValueError(message)


def crossing_counts(corners: int, pairs: int) -> Optional[list[int]]:
    counts = []
    for edge in range(4):
        different = ((corners >> edge) ^ (corners >> ((edge + 1) & 3))) & 1
        pair = (pairs >> edge) & 1
        if different and pair:
            return None
        counts.append(1 if different else 2 * pair)
    return counts


def signature(paths: list[list[str]], oriented: bool = False) -> tuple:
    normalized = []
    for path in paths:
        tokens = tuple("Y" if node.startswith("y") else node for node in path)
        normalized.append(tokens if oriented else min(tokens, tokens[::-1]))
    return tuple(sorted(normalized))


def transform(corners: int, pairs: int, paths: list[list[str]], code: int,
              oriented: bool = False) -> tuple[int, list[list[str]]]:
    """Apply a canonical-to-input D4 transform: reflect, then rotate.

    code & 3 is a counterclockwise rotation in quarter turns; code & 4
    reflects corner i to -i. Reflection reverses crossing slots on an edge.
    For oriented paths it also reverses path direction to keep inside left.
    """
    rotation, mirror = code & 3, bool(code & 4)

    def corner(index: int) -> int:
        return ((-index if mirror else index) + rotation) & 3

    def edge(index: int) -> tuple[int, bool]:
        a, b = corner(index), corner((index + 1) & 3)
        return (a, False) if b == ((a + 1) & 3) else (b, True)

    counts = crossing_counts(corners, pairs)
    require(counts is not None, "Cannot transform an invalid face state")
    new_corners = sum(((corners >> i) & 1) << corner(i) for i in range(4))
    new_pairs = sum(((pairs >> i) & 1) << edge(i)[0] for i in range(4))

    def node(name: str) -> str:
        if name.startswith("y"):
            return name
        index, slot = map(int, name[1:].split(":"))
        target, reverse = edge(index)
        return f"e{target}:{counts[index] - 1 - slot if reverse else slot}"

    result = [[node(name) for name in path] for path in paths]
    if oriented and mirror:
        result = [path[::-1] for path in result]
    return new_corners | (new_pairs << 4), result


def interval_paths(corners: int, pairs: int) -> list[list[str]]:
    """Infer the selected templates by pairing intervals on the face rim.

    With zero, one, or two inside corners, connect the endpoints of outside
    boundary intervals. With three or four, connect inside intervals. This
    is the selected asymmetric ambiguity policy, not a topology guarantee.
    A path joining hits on the same edge requires an interior yellow turn.
    """
    counts = crossing_counts(corners, pairs)
    require(counts is not None, "Cannot pair an invalid face state")
    hits, interval_states = [], []
    state = corners & 1
    for edge, count in enumerate(counts):
        require(state == ((corners >> edge) & 1), "Corner parity mismatch")
        for slot in range(count):
            hits.append(f"e{edge}:{slot}")
            state ^= 1
            interval_states.append(state)
    require(state == (corners & 1), "Boundary walk did not close")
    target = int(bin(corners).count("1") >= 3)
    paths, yellow = [], 0
    for index, start in enumerate(hits):
        if interval_states[index] != target:
            continue
        end = hits[(index + 1) % len(hits)]
        path = [start, end]
        if start.split(":")[0] == end.split(":")[0]:
            path.insert(1, f"y{yellow}")
            yellow += 1
        if target:
            path.reverse()
        paths.append(path)
    return paths


def boundary_id(name: str) -> int:
    require(name.startswith("e"), f"Expected a boundary crossing: {name}")
    edge, slot = map(int, name[1:].split(":"))
    require(0 <= edge < 4 and 0 <= slot < 2, f"Invalid crossing: {name}")
    return 2 * edge + slot


def boundary_name(index: int) -> str:
    return f"e{index // 2}:{index % 2}"


def pack(paths: list[list[str]]) -> int:
    require(len(paths) <= 4, "More than four face paths")
    word = VALID_BIT | (len(paths) << 28)
    yellow_names = []
    for index, path in enumerate(paths):
        require(len(path) in (2, 3), f"Invalid face path: {path}")
        start, end = boundary_id(path[0]), boundary_id(path[-1])
        if len(path) == 3:
            require(path[1].startswith("y"), f"Expected a yellow turn: {path}")
            yellow_names.append(path[1])
        payload = start | (end << 3) | (int(len(path) == 3) << 6)
        word |= payload << (7 * index)
    require(len(yellow_names) == len(set(yellow_names)), "Shared yellow turns cannot be packed")
    return word


def unpack(word: int) -> Optional[list[list[str]]]:
    if not word & VALID_BIT:
        require(word == 0, "An invalid packed word must be zero")
        return None
    count = (word >> 28) & 7
    require(count <= 4, "Invalid packed path count")
    paths, yellow = [], 0
    for index in range(count):
        payload = (word >> (7 * index)) & 127
        path = [boundary_name(payload & 7)]
        if payload & 64:
            path.append(f"y{yellow}")
            yellow += 1
        path.append(boundary_name((payload >> 3) & 7))
        paths.append(path)
    return paths


def decode_symmetry(key: int, canonical: list[int], case_map: list[int]) -> Optional[list[list[str]]]:
    """Model the runtime decoder independently of the full D4 transform."""
    mapping = case_map[key]
    case_id, code = mapping & 31, mapping >> 5
    if case_id == INVALID_CASE:
        return None
    paths = unpack(canonical[case_id])
    require(paths is not None, "Canonical case cannot be invalid")
    rotation, mirror = code & 3, bool(code & 4)

    def node(name: str) -> str:
        edge, slot = divmod(boundary_id(name), 2)
        target = (rotation - edge - 1 if mirror else rotation + edge) & 3
        if mirror:
            different = ((key >> target) ^ (key >> ((target + 1) & 3))) & 1
            slot = (1 if different else 2) - 1 - slot
        return boundary_name(2 * target + slot)

    result = []
    for path in paths:
        start, end = node(path[0]), node(path[-1])
        if mirror:
            start, end = end, start
        result.append([start] + path[1:-1] + [end])
    return result


def build_tables() -> tuple[list[int], list[int], list[int]]:
    expanded = {}
    require(len(SEEDS) == 23, "Expected 23 independently transcribed seed cases")
    for name, corners, pairs, paths in SEEDS:
        for code in range(8):
            key, transformed = transform(corners, pairs, paths, code)
            if key in expanded:
                require(signature(expanded[key]) == signature(transformed),
                        f"Inconsistent seed overlap at {key}, seed {name}")
            else:
                expanded[key] = transformed

    valid = {}
    for key in range(256):
        counts = crossing_counts(key & 15, key >> 4)
        require((counts is not None) == (key in expanded), f"Seed coverage mismatch at {key}")
        if counts is None:
            continue
        paths = interval_paths(key & 15, key >> 4)
        require(signature(paths) == signature(expanded[key]), f"Inferred rule mismatch at {key}")
        endpoints = [boundary_id(node) for path in paths for node in (path[0], path[-1])]
        expected = [2 * edge + slot for edge, count in enumerate(counts) for slot in range(count)]
        require(sorted(endpoints) == expected, f"Crossings must each occur once at {key}")
        valid[key] = paths
    require(len(valid) == 82, f"Expected 82 valid states; got {len(valid)}")
    words = [pack(valid[key]) if key in valid else 0 for key in range(256)]

    canonical_key_for, transform_for = {}, {}
    for key, paths in valid.items():
        orbit = []
        for code in range(8):
            other_key, other_paths = transform(key & 15, key >> 4, paths, code, oriented=True)
            require(other_key in valid, f"Invalid transformed state {key}, transform {code}")
            require(signature(other_paths, True) == signature(valid[other_key], True),
                    f"Orientation mismatch at {key}, transform {code}")
            orbit.append(other_key)
        canonical_key = min(orbit)
        canonical_key_for[key] = canonical_key
        for code in range(8):
            other_key, other_paths = transform(
                canonical_key & 15, canonical_key >> 4, valid[canonical_key], code, oriented=True)
            if other_key == key:
                require(signature(other_paths, True) == signature(paths, True),
                        f"Canonical orientation mismatch at {key}")
                transform_for[key] = code
                break
        require(key in transform_for, f"No inverse transform for {key}")

    canonical_keys = sorted(set(canonical_key_for.values()))
    require(len(canonical_keys) == 23, "Expected 23 D4 canonical cases")
    canonical_ids = {key: index for index, key in enumerate(canonical_keys)}
    canonical = [words[key] for key in canonical_keys]
    case_map = [
        canonical_ids[canonical_key_for[key]] | (transform_for[key] << 5)
        if key in valid else INVALID_CASE for key in range(256)
    ]

    # Exhaustively round-trip both representations, including invalid and
    # valid-empty states. Yellow names and path-list order are not identities.
    for key in range(256):
        direct = unpack(words[key])
        compressed = decode_symmetry(key, canonical, case_map)
        if key not in valid:
            require(direct is None and compressed is None, f"Invalid state decoded at {key}")
            continue
        require(direct is not None and compressed is not None, f"Valid state missing at {key}")
        for paths in (direct, compressed):
            require(signature(paths, True) == signature(valid[key], True),
                    f"Packed representation mismatch at {key}")
        require(pack(direct) == words[key], f"Packed word round-trip mismatch at {key}")
    return words, canonical, case_map


def format_array(name: str, ctype: str, values: list[int], digits: int, width: int) -> str:
    lines = [f"inline constexpr std::{ctype} {name}[{len(values)}] = {{"]
    for start in range(0, len(values), width):
        row = ", ".join(f"0x{value:0{digits}x}u" for value in values[start:start + width])
        lines.append(f"    {row},")
    lines.append("};")
    return "\n".join(lines)


def generate_header() -> str:
    words, canonical, case_map = build_tables()
    preamble = """// Generated by tools/generate_double_crossing_tables.py; do not edit by hand.
// Independently reconstructed from Matthias Mueller (2009),
// Fast and Robust Tracking of Fluid Surfaces, Figs. 5/6,
// selected columns 1, 2, 3, 4, 11, 12. Not the author's original code.
// https://matthias-research.github.io/pages/publications/surfaceTracking.pdf
//
// Symbolic face connectivity for at most TWO crossings per grid edge.
// This is not a 3-D triangulation table. Yellow turns need input geometry.
// Parity compatibility alone does not guarantee geometric realizability.
// key = corner_mask | (pair_mask << 4); corner bit 1 means inside.
// c0=(0,0), c1=(1,0), c2=(1,1), c3=(0,1); e_i=c_i -> c_(i+1 mod 4).
// Crossing ids are 2*edge+slot; slots ascend along the oriented edge.
// Paths are oriented with inside on the left in the local face plane.
//
// Packed word: bit 31=valid; bits 28..30=path count (0..4).
// Bits 0..27 hold four 7-bit payloads, starting with the lowest bits:
// bits 0..2=start crossing, bits 3..5=end crossing, bit 6=yellow turn.
// Invalid words are zero; a valid empty face has word 0x80000000.
#pragma once

#include <cstdint>

namespace meshutil::double_crossing::detail {

// Direct representation: 256 * 4 = 1024 bytes.
"""
    parts = [preamble, format_array("kPackedFaces", "uint32_t", words, 8, 4),
             "\n\n// D4 representation: 23 * 4 + 256 = 348 bytes.\n"
             "// No inside/outside complement symmetry is assumed.\n",
             format_array("kCanonicalFaces", "uint32_t", canonical, 8, 4),
             "\n\n// Map low 5 bits: canonical index (31=invalid). High 3 bits:\n"
             "// transform = rotation_quarters | (mirror ? 4 : 0).\n"
             "// Mirror i -> -i first, then rotate; this maps canonical to input.\n"
             "// Reflection reverses double-hit slots AND path orientation.\n",
             format_array("kFaceCaseMap", "uint8_t", case_map, 2, 16),
             "\n\n} // namespace meshutil::double_crossing::detail\n"]
    return "".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                        help="Generated C++17 header (default: repository include directory)")
    parser.add_argument("--check", action="store_true",
                        help="Validate all cases and compare the existing header without writing")
    args = parser.parse_args()
    try:
        expected = generate_header().encode("utf-8")
        if args.check:
            actual = args.output.read_bytes()
            if actual != expected:
                print(f"Generated table differs: {args.output}", file=sys.stderr)
                return 1
            print(f"Verified {args.output}: 256 encodings, 82 valid, 23 canonical; "
                  "all round trips and 82 x 8 oriented transforms passed.")
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(expected)
            print(f"Generated {args.output}: 256 encodings, 82 valid, 23 canonical; "
                  "all round trips and 82 x 8 oriented transforms passed.")
    except (OSError, ValueError) as error:
        print(f"Table generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
