#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "meshutil/detail/double_crossing_tables.hpp"

// Symbolic connectivity for regular-grid edges with zero, one, or two crossings.
// Independently reconstructed from Muller (2009), Figs. 5/6; not author code.
// Unlike a corner-sign-only marching-cubes case, a same-sign edge can retain a
// pair of crossings belonging to a thin feature. These tables connect known
// crossings; they do not discover crossings or guarantee input-mesh topology.
//
// This is NOT a triangle table or a complete surface reconstruction algorithm.
// Mesh/grid intersection, duplicate/tangent/endpoint handling, yellow-turn
// coordinates, shared geometry between cells, and triangulation are caller work.
// A yellow turn may need to expand to several geometric vertices. More than two
// crossings on one edge are rejected, never silently averaged or discarded.
namespace meshutil::double_crossing {

enum class TableStorage { Packed, Symmetry };

// Face corners are counterclockwise: (0,0), (1,0), (1,1), (0,1).
// Edge e goes from corner e to corner (e+1)%4. Boundary node 2*e+slot
// identifies a crossing in increasing parameter order along that directed edge.
// Paths have inside on the left in these local face coordinates.
struct FacePath {
  std::uint8_t start = 0;
  std::uint8_t end = 0;
  bool has_yellow = false;
};

struct FaceConnectivity {
  std::array<FacePath, 4> paths{};
  std::uint8_t path_count = 0;
};

// Cube corner i has coordinates (i&1, (i>>1)&1, (i>>2)&1).
// All cube edges point along a positive coordinate axis. Crossing slot zero
// therefore always precedes slot one in that direction, independently of face.
inline constexpr std::array<std::array<std::uint8_t, 2>, 12> kCubeEdges{{
    {{0, 1}}, {{2, 3}}, {{4, 5}}, {{6, 7}},
    {{0, 2}}, {{1, 3}}, {{4, 6}}, {{5, 7}},
    {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
}};

// Face order: z=0, z=1, y=0, y=1, x=0, x=1. These are local cycles,
// not a set of consistently outward-facing cycles.
inline constexpr std::array<std::array<std::uint8_t, 4>, 6> kCubeFaces{{
    {{0, 1, 3, 2}}, {{4, 5, 7, 6}}, {{0, 1, 5, 4}},
    {{2, 3, 7, 6}}, {{0, 2, 6, 4}}, {{1, 3, 7, 5}},
}};

struct CubeConnectivity {
  // Node IDs: 2*edge+slot in [0,24), or 24+4*face+yellow_slot in [24,48).
  // Each loop starts at its smallest node and visits its smaller neighbor first.
  // The starting node is not repeated at the end. Loop order follows their
  // starting nodes. This deterministic orientation does not specify normals.
  std::vector<std::vector<std::uint8_t>> loops;
};

struct CubeNode {
  enum class Kind { EdgeCrossing, FaceTurn };
  Kind kind;
  std::uint8_t index;  // Cube edge index, or cube face index.
  std::uint8_t slot;   // Crossing slot, or face-local yellow-turn slot.
};

// Decode a node's numeric format; this does not assert that it occurs in a
// particular cube. Face-turn slots enumerate yellow paths in sorted face order.
inline CubeNode decode_cube_node(unsigned node) {
  if (node >= 48u) {
    throw std::invalid_argument("double_crossing: cube node must be below 48");
  }
  if (node < 24u) {
    return {CubeNode::Kind::EdgeCrossing,
            static_cast<std::uint8_t>(node / 2u),
            static_cast<std::uint8_t>(node % 2u)};
  }
  return {CubeNode::Kind::FaceTurn,
          static_cast<std::uint8_t>((node - 24u) / 4u),
          static_cast<std::uint8_t>((node - 24u) % 4u)};
}

namespace detail {

inline unsigned edge_parity(unsigned corners, unsigned a, unsigned b) {
  return ((corners >> a) ^ (corners >> b)) & 1u;
}

inline void validate_face_masks(unsigned corners, unsigned pairs) {
  if (corners > 15u || pairs > 15u) {
    throw std::invalid_argument("double_crossing: face masks must fit four bits");
  }
  for (unsigned edge = 0; edge < 4u; ++edge) {
    if (((pairs >> edge) & 1u) &&
        edge_parity(corners, edge, (edge + 1u) % 4u)) {
      throw std::invalid_argument(
          "double_crossing: a crossing pair requires same-sign endpoints");
    }
  }
}

inline void validate_cube_masks(unsigned corners, unsigned pairs) {
  if (corners > 255u || pairs > 4095u) {
    throw std::invalid_argument(
        "double_crossing: cube masks must fit eight and twelve bits");
  }
  for (unsigned edge = 0; edge < kCubeEdges.size(); ++edge) {
    if (((pairs >> edge) & 1u) &&
        edge_parity(corners, kCubeEdges[edge][0], kCubeEdges[edge][1])) {
      throw std::invalid_argument(
          "double_crossing: a crossing pair requires same-sign endpoints");
    }
  }
}

inline void validate_count(unsigned count, unsigned parity) {
  if (count > 2u) {
    throw std::invalid_argument(
        "double_crossing: at most two crossings per edge are supported");
  }
  if ((count & 1u) != parity) {
    throw std::invalid_argument(
        "double_crossing: crossing count disagrees with endpoint signs");
  }
}

inline FaceConnectivity unpack_face(std::uint32_t word) {
  if ((word & 0x80000000u) == 0u) {
    throw std::logic_error("double_crossing: missing valid face table entry");
  }
  FaceConnectivity result;
  result.path_count = static_cast<std::uint8_t>((word >> 28u) & 7u);
  if (result.path_count > result.paths.size()) {
    throw std::logic_error("double_crossing: invalid face table path count");
  }
  for (unsigned i = 0; i < result.path_count; ++i) {
    const unsigned payload = (word >> (7u * i)) & 127u;
    result.paths[i] = {static_cast<std::uint8_t>(payload & 7u),
                       static_cast<std::uint8_t>((payload >> 3u) & 7u),
                       ((payload >> 6u) & 1u) != 0u};
  }
  return result;
}

inline std::uint8_t transform_boundary(std::uint8_t vertex, unsigned corners,
                                       unsigned pairs, unsigned transform) {
  const unsigned edge = vertex / 2u;
  unsigned slot = vertex % 2u;
  const unsigned rotation = transform & 3u;
  const bool mirror = (transform & 4u) != 0u;
  const unsigned target =
      (mirror ? rotation + 3u - edge : rotation + edge) & 3u;
  const unsigned parity = edge_parity(corners, target, (target + 1u) % 4u);
  const unsigned count = parity ? 1u : 2u * ((pairs >> target) & 1u);
  if (slot >= count) {
    throw std::logic_error("double_crossing: transformed crossing is absent");
  }
  if (mirror) {
    slot = count - 1u - slot;
  }
  return static_cast<std::uint8_t>(2u * target + slot);
}

inline bool path_less(const FacePath &a, const FacePath &b) {
  if (a.start != b.start) return a.start < b.start;
  if (a.end != b.end) return a.end < b.end;
  return a.has_yellow < b.has_yellow;
}

// Sorting at most four paths removes the arbitrary representative-table order,
// giving both storage modes the same public path and yellow-turn numbering.
inline void sort_paths(FaceConnectivity &face) {
  for (unsigned i = 1; i < face.path_count; ++i) {
    const FacePath path = face.paths[i];
    unsigned j = i;
    while (j > 0u && path_less(path, face.paths[j - 1u])) {
      face.paths[j] = face.paths[j - 1u];
      --j;
    }
    face.paths[j] = path;
  }
}

inline constexpr std::array<std::array<std::uint8_t, 4>, 6> kCubeFaceEdges{{
    {{0, 5, 1, 4}}, {{2, 7, 3, 6}}, {{0, 9, 2, 8}},
    {{1, 11, 3, 10}}, {{4, 10, 6, 8}}, {{5, 11, 7, 9}},
}};

}  // namespace detail

// A corner bit of one denotes inside. Counts must be 0/2 for same-sign
// endpoints and 1 for opposite signs. The returned key is corners|(pairs<<4).
inline std::uint8_t encode_face(unsigned corner_mask,
                                const std::array<unsigned, 4> &counts) {
  detail::validate_face_masks(corner_mask, 0u);
  unsigned pairs = 0u;
  for (unsigned edge = 0; edge < counts.size(); ++edge) {
    detail::validate_count(counts[edge], detail::edge_parity(
        corner_mask, edge, (edge + 1u) % 4u));
    if (counts[edge] == 2u) pairs |= 1u << edge;
  }
  return static_cast<std::uint8_t>(corner_mask | (pairs << 4u));
}

// A pair bit denotes two crossings on a same-sign edge. Opposite-sign edges
// implicitly have one crossing and must have a zero pair bit. Invalid masks
// throw std::invalid_argument before any narrowing or table access.
inline FaceConnectivity lookup_face(
    unsigned corner_mask, unsigned pair_mask,
    TableStorage storage = TableStorage::Packed) {
  detail::validate_face_masks(corner_mask, pair_mask);
  const unsigned key = corner_mask | (pair_mask << 4u);
  FaceConnectivity result;
  switch (storage) {
    case TableStorage::Packed:
      result = detail::unpack_face(detail::kPackedFaces[key]);
      break;
    case TableStorage::Symmetry: {
      const unsigned mapping = detail::kFaceCaseMap[key];
      const unsigned case_id = mapping & 31u;
      const unsigned transform = mapping >> 5u;
      if (case_id >= 23u) {
        throw std::logic_error("double_crossing: missing symmetry case");
      }
      result = detail::unpack_face(detail::kCanonicalFaces[case_id]);
      for (unsigned i = 0; i < result.path_count; ++i) {
        const auto start = detail::transform_boundary(
            result.paths[i].start, corner_mask, pair_mask, transform);
        const auto end = detail::transform_boundary(
            result.paths[i].end, corner_mask, pair_mask, transform);
        // A reflection reverses face handedness; reverse each path too so
        // inside remains on its left after restoring the input coordinates.
        result.paths[i].start = (transform & 4u) ? end : start;
        result.paths[i].end = (transform & 4u) ? start : end;
      }
      break;
    }
    default:
      throw std::invalid_argument("double_crossing: unknown table storage");
  }
  detail::sort_paths(result);
  return result;
}

inline FaceConnectivity lookup_face_from_counts(
    unsigned corner_mask, const std::array<unsigned, 4> &counts,
    TableStorage storage = TableStorage::Packed) {
  const auto key = encode_face(corner_mask, counts);
  return lookup_face(key & 15u, key >> 4u, storage);
}

// Return corners|(pairs<<8), a 20-bit key. Crossing counts are ordered by
// kCubeEdges; no truncation or simplification of unsupported counts occurs.
inline std::uint32_t encode_cube(unsigned corner_mask,
                                 const std::array<unsigned, 12> &counts) {
  detail::validate_cube_masks(corner_mask, 0u);
  unsigned pairs = 0u;
  for (unsigned edge = 0; edge < counts.size(); ++edge) {
    detail::validate_count(counts[edge], detail::edge_parity(
        corner_mask, kCubeEdges[edge][0], kCubeEdges[edge][1]));
    if (counts[edge] == 2u) pairs |= 1u << edge;
  }
  return static_cast<std::uint32_t>(corner_mask | (pairs << 8u));
}

// Compose six face lookups into undirected symbolic boundary loops. Valid
// parity only establishes a combinatorial state; it does not guarantee that
// geometry is realizable, that triangulation is intersection-free, or that an
// input component entirely inside the cell can be represented.
inline CubeConnectivity cube_boundary_loops(
    unsigned corner_mask, unsigned pair_mask,
    TableStorage storage = TableStorage::Packed) {
  detail::validate_cube_masks(corner_mask, pair_mask);
  std::array<std::array<std::uint8_t, 2>, 48> neighbors{};
  std::array<std::uint8_t, 48> degree{};
  std::array<unsigned, 12> counts{};
  for (unsigned edge = 0; edge < counts.size(); ++edge) {
    const unsigned parity = detail::edge_parity(
        corner_mask, kCubeEdges[edge][0], kCubeEdges[edge][1]);
    counts[edge] = parity ? 1u : 2u * ((pair_mask >> edge) & 1u);
  }

  const auto add_segment = [&](unsigned a, unsigned b) {
    if (a >= neighbors.size() || b >= neighbors.size() || a == b ||
        degree[a] >= 2u || degree[b] >= 2u) {
      throw std::logic_error("double_crossing: invalid cube boundary graph");
    }
    for (unsigned i = 0; i < degree[a]; ++i) {
      if (neighbors[a][i] == b) {
        throw std::logic_error("double_crossing: duplicate boundary segment");
      }
    }
    neighbors[a][degree[a]++] = static_cast<std::uint8_t>(b);
    neighbors[b][degree[b]++] = static_cast<std::uint8_t>(a);
  };

  for (unsigned face = 0; face < kCubeFaces.size(); ++face) {
    unsigned corners = 0u;
    unsigned pairs = 0u;
    for (unsigned i = 0; i < 4u; ++i) {
      corners |= ((corner_mask >> kCubeFaces[face][i]) & 1u) << i;
      pairs |= ((pair_mask >> detail::kCubeFaceEdges[face][i]) & 1u) << i;
    }
    const FaceConnectivity connectivity = lookup_face(corners, pairs, storage);
    const auto cube_vertex = [&](std::uint8_t vertex) {
      const unsigned local_edge = vertex / 2u;
      const unsigned edge = detail::kCubeFaceEdges[face][local_edge];
      unsigned slot = vertex % 2u;
      if (slot >= counts[edge]) {
        throw std::logic_error("double_crossing: face uses an absent crossing");
      }
      if (kCubeEdges[edge][0] != kCubeFaces[face][local_edge]) {
        slot = counts[edge] - 1u - slot;
      }
      return 2u * edge + slot;
    };
    unsigned yellow_slot = 0u;
    for (unsigned i = 0; i < connectivity.path_count; ++i) {
      const FacePath &path = connectivity.paths[i];
      const unsigned start = cube_vertex(path.start);
      const unsigned end = cube_vertex(path.end);
      if (path.has_yellow) {
        const unsigned yellow = 24u + 4u * face + yellow_slot++;
        add_segment(start, yellow);
        add_segment(yellow, end);
      } else {
        add_segment(start, end);
      }
    }
  }

  for (unsigned node = 0; node < degree.size(); ++node) {
    if (degree[node] != 0u && degree[node] != 2u) {
      throw std::logic_error("double_crossing: boundary node degree is not two");
    }
    if (node < 24u &&
        ((degree[node] != 0u) != ((node % 2u) < counts[node / 2u]))) {
      throw std::logic_error("double_crossing: cube crossing coverage mismatch");
    }
  }

  CubeConnectivity result;
  std::array<bool, 48> visited{};
  for (unsigned start = 0; start < degree.size(); ++start) {
    if (degree[start] == 0u || visited[start]) continue;
    std::vector<std::uint8_t> loop{static_cast<std::uint8_t>(start)};
    visited[start] = true;
    unsigned previous = start;
    unsigned current = neighbors[start][0] < neighbors[start][1]
                           ? neighbors[start][0] : neighbors[start][1];
    while (current != start) {
      if (current >= visited.size() || visited[current]) {
        throw std::logic_error("double_crossing: boundary loop repeats a node");
      }
      visited[current] = true;
      loop.push_back(static_cast<std::uint8_t>(current));
      unsigned next;
      if (neighbors[current][0] == previous) {
        next = neighbors[current][1];
      } else if (neighbors[current][1] == previous) {
        next = neighbors[current][0];
      } else {
        throw std::logic_error("double_crossing: broken boundary adjacency");
      }
      previous = current;
      current = next;
    }
    if (loop.size() < 3u) {
      throw std::logic_error("double_crossing: boundary loop is too short");
    }
    result.loops.push_back(std::move(loop));
  }
  return result;
}

inline CubeConnectivity cube_boundary_loops_from_counts(
    unsigned corner_mask, const std::array<unsigned, 12> &counts,
    TableStorage storage = TableStorage::Packed) {
  const auto key = encode_cube(corner_mask, counts);
  return cube_boundary_loops(key & 255u, key >> 8u, storage);
}

}  // namespace meshutil::double_crossing
