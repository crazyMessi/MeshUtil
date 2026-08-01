#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace meshutil {

using Index = std::uint32_t;

struct MeshView {
  const float *positions = nullptr;
  std::size_t vertex_count = 0;
  std::size_t position_stride_bytes = 3 * sizeof(float);
  const Index *triangles = nullptr;
  std::size_t triangle_count = 0;
  std::size_t triangle_stride_bytes = 3 * sizeof(Index);
};

struct Mesh {
  std::vector<float> positions;
  std::vector<Index> triangles;

  std::size_t vertex_count() const noexcept
  {
    return positions.size() / 3;
  }

  std::size_t triangle_count() const noexcept
  {
    return triangles.size() / 3;
  }

  MeshView view() const noexcept
  {
    return {
        positions.data(),
        vertex_count(),
        3 * sizeof(float),
        triangles.data(),
        triangle_count(),
        3 * sizeof(Index),
    };
  }
};

void validate_mesh(MeshView mesh);

}  // namespace meshutil
