#include <meshutil/double_crossing.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dc = meshutil::double_crossing;

namespace {

// These checks remain active in Release builds, where assert() is disabled.
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Function>
void require_invalid(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message + ": expected std::invalid_argument");
}

using Path = std::array<unsigned, 3>; // start, end, yellow marker
using Paths = std::vector<Path>;

Paths signature(const dc::FaceConnectivity& face, bool oriented = true) {
    require(face.path_count <= 4, "too many paths in a face");
    Paths paths;
    for (unsigned i = 0; i < face.path_count; ++i) {
        const auto& path = face.paths[i];
        unsigned a = path.start;
        unsigned b = path.end;
        if (!oriented && b < a) std::swap(a, b);
        paths.push_back({a, b, path.has_yellow ? 1u : 0u});
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::array<unsigned, 4> face_counts(unsigned corners, unsigned pairs) {
    std::array<unsigned, 4> counts{};
    for (unsigned e = 0; e < 4; ++e) {
        const bool changes = ((corners >> e) ^ (corners >> ((e + 1) % 4))) & 1u;
        counts[e] = changes ? 1u : 2u * ((pairs >> e) & 1u);
    }
    return counts;
}

bool valid_face(unsigned corners, unsigned pairs) {
    for (unsigned e = 0; e < 4; ++e) {
        if (((pairs >> e) & 1u) &&
            (((corners >> e) ^ (corners >> ((e + 1) % 4))) & 1u)) return false;
    }
    return true;
}

// Independently written literal representatives from the selected cases of
// Figures 5/6. Endpoints are oriented with the inside on the left. Testing
// their eight square symmetries checks connectivity, not merely bit packing.
struct Seed {
    unsigned corners;
    unsigned pairs;
    Paths paths;
};

const std::vector<Seed> kSeeds = {
    {0, 0, {}},
    {0, 1, {{1, 0, 1}}},
    {0, 3, {{1, 2, 0}, {3, 0, 0}}},
    {0, 5, {{1, 4, 0}, {5, 0, 0}}},
    {0, 7, {{1, 2, 0}, {3, 4, 0}, {5, 0, 0}}},
    {0, 15, {{1, 2, 0}, {3, 4, 0}, {5, 6, 0}, {7, 0, 0}}},
    {1, 0, {{0, 6, 0}}},
    {1, 2, {{0, 2, 0}, {3, 6, 0}}},
    {1, 6, {{0, 2, 0}, {3, 4, 0}, {5, 6, 0}}},
    {3, 0, {{2, 6, 0}}},
    {3, 1, {{0, 1, 1}, {2, 6, 0}}},
    {3, 4, {{2, 4, 0}, {5, 6, 0}}},
    {3, 5, {{0, 1, 1}, {2, 4, 0}, {5, 6, 0}}},
    {5, 0, {{0, 2, 0}, {4, 6, 0}}},
    {7, 0, {{4, 6, 0}}},
    {7, 1, {{4, 1, 0}, {0, 6, 0}}},
    {7, 3, {{2, 1, 0}, {4, 3, 0}, {0, 6, 0}}},
    {15, 0, {}},
    {15, 1, {{0, 1, 1}}},
    {15, 3, {{2, 1, 0}, {0, 3, 0}}},
    {15, 5, {{4, 1, 0}, {0, 5, 0}}},
    {15, 7, {{2, 1, 0}, {4, 3, 0}, {0, 5, 0}}},
    {15, 15, {{2, 1, 0}, {4, 3, 0}, {6, 5, 0}, {0, 7, 0}}},
};

void check_face_symmetries() {
    std::array<bool, 256> covered{};
    for (const auto& seed : kSeeds) {
        const auto counts = face_counts(seed.corners, seed.pairs);
        for (unsigned mirror = 0; mirror < 2; ++mirror) {
            for (unsigned rotation = 0; rotation < 4; ++rotation) {
                const auto corner = [=](unsigned i) {
                    return (rotation + (mirror ? 4 - i : i)) % 4;
                };
                std::array<unsigned, 4> edges{};
                std::array<bool, 4> reversed{};
                unsigned corners = 0;
                unsigned pairs = 0;
                for (unsigned i = 0; i < 4; ++i) {
                    const unsigned a = corner(i);
                    const unsigned b = corner((i + 1) % 4);
                    reversed[i] = b != (a + 1) % 4;
                    edges[i] = reversed[i] ? b : a;
                    corners |= ((seed.corners >> i) & 1u) << a;
                    pairs |= ((seed.pairs >> i) & 1u) << edges[i];
                }
                const auto endpoint = [&](unsigned node) {
                    const unsigned e = node / 2;
                    const unsigned slot = node % 2;
                    return 2 * edges[e] + (reversed[e] ? counts[e] - 1 - slot : slot);
                };
                Paths expected;
                for (const auto& path : seed.paths) {
                    unsigned a = endpoint(path[0]);
                    unsigned b = endpoint(path[1]);
                    // A reflection reverses the meaning of left and right.
                    if (mirror) std::swap(a, b);
                    expected.push_back({a, b, path[2]});
                }
                std::sort(expected.begin(), expected.end());
                const unsigned key = corners | (pairs << 4);
                covered[key] = true;
                for (auto storage : {dc::TableStorage::Packed, dc::TableStorage::Symmetry}) {
                    require(signature(dc::lookup_face(corners, pairs, storage)) == expected,
                            "representative/symmetry mismatch at face key " + std::to_string(key));
                }
            }
        }
    }
    require(std::count(covered.begin(), covered.end(), true) == 82,
            "the representative orbits do not cover 82 face states");
}

void check_faces() {
    unsigned valid = 0;
    for (unsigned key = 0; key < 256; ++key) {
        const unsigned corners = key & 15u;
        const unsigned pairs = key >> 4;
        if (!valid_face(corners, pairs)) {
            for (auto storage : {dc::TableStorage::Packed, dc::TableStorage::Symmetry}) {
                require_invalid([=] { dc::lookup_face(corners, pairs, storage); },
                                "invalid face accepted at key " + std::to_string(key));
            }
            continue;
        }
        ++valid;
        const auto counts = face_counts(corners, pairs);
        require(dc::encode_face(corners, counts) == key, "face counts encoded incorrectly");
        const auto packed = dc::lookup_face(corners, pairs, dc::TableStorage::Packed);
        const auto symmetric = dc::lookup_face(corners, pairs, dc::TableStorage::Symmetry);
        require(signature(packed) == signature(symmetric), "face storage formats disagree");
        for (auto storage : {dc::TableStorage::Packed, dc::TableStorage::Symmetry}) {
            require(signature(dc::lookup_face_from_counts(corners, counts, storage)) == signature(packed),
                    "face count-based lookup disagrees with mask lookup");
        }
        std::array<unsigned, 8> uses{};
        for (unsigned i = 0; i < packed.path_count; ++i) {
            const auto& path = packed.paths[i];
            require(path.start < 8 && path.end < 8, "face endpoint out of range");
            require(path.start != path.end, "face path starts and ends at the same crossing");
            ++uses[path.start];
            ++uses[path.end];
            require(path.has_yellow == (path.start / 2 == path.end / 2),
                    "same-edge turn marker missing or attached to unrelated edges");
        }
        for (unsigned e = 0; e < 4; ++e) {
            for (unsigned slot = 0; slot < 2; ++slot) {
                require(uses[2 * e + slot] == (slot < counts[e] ? 1u : 0u),
                        "face crossing missing or duplicated");
            }
        }
    }
    require(valid == 82, "incorrect valid face state count");
    require(signature(dc::lookup_face(0, 1)) == Paths{{1, 0, 1}},
            "a same-edge double crossing must have an interior turn");
    require(signature(dc::lookup_face(0, 5)) == Paths{{1, 4, 0}, {5, 0, 0}},
            "opposite-edge double crossings must retain two separate paths");
    require(signature(dc::lookup_face(5, 0), false) != signature(dc::lookup_face(10, 0), false),
            "inside/outside complementation incorrectly treated as a connectivity symmetry");
}

// Literal coordinate convention, independent of the public constants.
constexpr std::array<std::array<unsigned, 2>, 12> kEdges = {{
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
    {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}
}};
constexpr std::array<std::array<unsigned, 4>, 6> kFaces = {{
    {0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4},
    {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}
}};

bool edge_in_face(unsigned edge, unsigned face) {
    const auto& corners = kFaces[face];
    return std::find(corners.begin(), corners.end(), kEdges[edge][0]) != corners.end() &&
           std::find(corners.begin(), corners.end(), kEdges[edge][1]) != corners.end();
}

using Loops = std::vector<std::vector<unsigned>>;

Loops loop_signature(const dc::CubeConnectivity& cube) {
    Loops loops;
    for (const auto& loop : cube.loops) {
        // Turn IDs must remain stable between the storage implementations.
        // Only the loop's cyclic starting point and direction are arbitrary.
        std::vector<unsigned> nodes(loop.begin(), loop.end());
        std::vector<unsigned> best = nodes;
        for (unsigned direction = 0; direction < 2; ++direction) {
            for (std::size_t shift = 0; shift < nodes.size(); ++shift) {
                std::vector<unsigned> candidate;
                for (std::size_t j = 0; j < nodes.size(); ++j) {
                    candidate.push_back(nodes[(j + shift) % nodes.size()]);
                }
                best = std::min(best, candidate);
            }
            std::reverse(nodes.begin(), nodes.end());
        }
        loops.push_back(std::move(best));
    }
    std::sort(loops.begin(), loops.end());
    return loops;
}

void check_cube_structure(const dc::CubeConnectivity& cube,
                          const std::array<unsigned, 12>& counts) {
    std::array<bool, 48> seen{};
    for (const auto& loop : cube.loops) {
        require(loop.size() >= 3, "cube loop has fewer than three nodes");
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const unsigned id = loop[i];
            require(id < seen.size(), "cube node id out of range");
            require(!seen[id], "cube crossing/turn duplicated within or between loops");
            seen[id] = true;
            const auto decoded = dc::decode_cube_node(id);
            if (id < 24) {
                require(decoded.kind == dc::CubeNode::Kind::EdgeCrossing &&
                        decoded.index == id / 2 && decoded.slot == id % 2,
                        "incorrect cube edge-crossing node decoding");
                require(decoded.slot < counts[decoded.index], "nonexistent cube crossing emitted");
            } else {
                const unsigned face = (id - 24) / 4;
                require(decoded.kind == dc::CubeNode::Kind::FaceTurn &&
                        decoded.index == face && decoded.slot == (id - 24) % 4,
                        "incorrect cube face-turn node decoding");
                const unsigned before = loop[(i + loop.size() - 1) % loop.size()];
                const unsigned after = loop[(i + 1) % loop.size()];
                require(before < 24 && after < 24, "turn must connect two edge crossings");
                require(before / 2 == after / 2 && edge_in_face(before / 2, face),
                        "turn assigned to the wrong face or wrong edge pair");
            }
        }
    }
    for (unsigned e = 0; e < 12; ++e) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            require(seen[2 * e + slot] == (slot < counts[e]),
                    "cube edge-crossing coverage differs from its input counts");
        }
    }
}

void check_cubes() {
    for (unsigned e = 0; e < 12; ++e) {
        for (unsigned i = 0; i < 2; ++i) {
            require(dc::kCubeEdges[e][i] == kEdges[e][i], "cube edge coordinate convention changed");
        }
    }
    for (unsigned f = 0; f < 6; ++f) {
        for (unsigned i = 0; i < 4; ++i) {
            require(dc::kCubeFaces[f][i] == kFaces[f][i], "cube face coordinate convention changed");
        }
    }
    unsigned configurations = 0;
    for (unsigned corners = 0; corners < 256; ++corners) {
        unsigned same_mask = 0;
        for (unsigned e = 0; e < 12; ++e) {
            if ((((corners >> kEdges[e][0]) ^ (corners >> kEdges[e][1])) & 1u) == 0) {
                same_mask |= 1u << e;
            }
        }
        unsigned pairs = same_mask;
        for (;;) {
            std::array<unsigned, 12> counts{};
            for (unsigned e = 0; e < 12; ++e) {
                counts[e] = (same_mask & (1u << e)) ? 2u * ((pairs >> e) & 1u) : 1u;
            }
            const auto packed = dc::cube_boundary_loops(corners, pairs, dc::TableStorage::Packed);
            const auto symmetric = dc::cube_boundary_loops(corners, pairs, dc::TableStorage::Symmetry);
            check_cube_structure(packed, counts);
            check_cube_structure(symmetric, counts);
            const auto expected = loop_signature(packed);
            require(loop_signature(symmetric) == expected, "cube storage formats disagree");
            require(dc::encode_cube(corners, counts) == (corners | (pairs << 8)),
                    "cube counts encoded incorrectly");
            for (auto storage : {dc::TableStorage::Packed, dc::TableStorage::Symmetry}) {
                require(loop_signature(dc::cube_boundary_loops_from_counts(corners, counts, storage)) == expected,
                        "cube count-based lookup disagrees with mask lookup");
            }
            ++configurations;
            if (pairs == 0) break;
            pairs = (pairs - 1) & same_mask;
        }
    }
    require(configurations == 36450, "incorrect number of parity-compatible cube configurations");
    require(dc::cube_boundary_loops(0, 0).loops.empty(), "empty outside cube produced loops");
    require(dc::cube_boundary_loops(255, 0).loops.empty(), "empty inside cube produced loops");

    // A thin slab crosses all four x-directed edges twice. It must remain two
    // separate four-node sheets even though every cube corner is outside.
    const auto slab = dc::cube_boundary_loops(0, 0x00f);
    require(slab.loops.size() == 2, "thin slab lost or merged one of its two sheets");
    std::array<bool, 2> used_slots{};
    for (const auto& loop : slab.loops) {
        require(loop.size() == 4, "thin slab should have two quadrilateral boundary loops");
        const unsigned slot = loop.front() % 2;
        require(!used_slots[slot], "thin slab duplicates the same side");
        used_slots[slot] = true;
        std::array<bool, 4> edges{};
        for (unsigned id : loop) {
            require(id < 8 && id % 2 == slot, "thin slab connects opposite sheets");
            edges[id / 2] = true;
        }
        require(std::all_of(edges.begin(), edges.end(), [](bool value) { return value; }),
                "thin slab misses an x-directed edge");
    }
}

void check_invalid_inputs() {
    const std::array<unsigned, 4> no_face_hits{};
    const std::array<unsigned, 12> no_cube_hits{};
    require_invalid([] { dc::lookup_face(16, 0); }, "face corner mask overflow");
    require_invalid([] { dc::lookup_face(0, 16); }, "face pair mask overflow");
    require_invalid([&] { dc::encode_face(16, no_face_hits); }, "encoded face mask overflow");
    require_invalid([&] { dc::encode_face(1, no_face_hits); }, "face sign/count parity mismatch");
    for (unsigned count : {3u, 4u}) {
        auto face_counts_overflow = no_face_hits;
        face_counts_overflow[0] = count;
        require_invalid([&] { dc::lookup_face_from_counts(0, face_counts_overflow); },
                        "face silently reduces excessive intersection counts");
        auto cube_counts_overflow = no_cube_hits;
        cube_counts_overflow[11] = count;
        require_invalid([&] { dc::cube_boundary_loops_from_counts(0, cube_counts_overflow); },
                        "cube silently reduces excessive intersection counts");
    }
    require_invalid([] { dc::cube_boundary_loops(256, 0); }, "cube corner mask overflow");
    require_invalid([] { dc::cube_boundary_loops(0, 4096); }, "cube pair mask overflow");
    require_invalid([] { dc::cube_boundary_loops(1, 1); }, "cube pair on a sign-changing edge");
    require_invalid([&] { dc::encode_cube(256, no_cube_hits); }, "encoded cube mask overflow");
    require_invalid([&] { dc::encode_cube(1, no_cube_hits); }, "cube sign/count parity mismatch");
    require_invalid([] { dc::decode_cube_node(48); }, "invalid cube node accepted");
}

} // namespace

int main() {
    try {
        check_face_symmetries();
        check_faces();
        check_cubes();
        check_invalid_inputs();
        std::cout << "PASS: 23 representatives and their square symmetries; 256 face codes; "
                     "36,450 valid cube configurations; both storage formats; invalid inputs.\n";
        std::cout << "These are symbolic connectivity checks, not geometric reconstruction tests.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
