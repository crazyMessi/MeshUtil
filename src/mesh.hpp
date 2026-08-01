#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace standalone_decimator {

enum class MemoryMode {
  Balanced,
  Low,
};

using VertexId = std::uint32_t;
using EdgeId = std::uint32_t;
using FaceId = std::uint32_t;

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;

  Vec3 &operator+=(const Vec3 &other) noexcept;
  Vec3 &operator*=(double scalar) noexcept;
};

Vec3 operator+(Vec3 left, const Vec3 &right) noexcept;
Vec3 operator*(Vec3 value, double scalar) noexcept;
double dot(const Vec3 &left, const Vec3 &right) noexcept;
double length_squared(const Vec3 &value) noexcept;
double length(const Vec3 &value) noexcept;

struct Float3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct InputMesh {
  std::vector<Float3> vertices;
  std::vector<std::array<VertexId, 3>> faces;
};

InputMesh read_binary_triangle_ply(const std::string &path);
void write_binary_triangle_ply_atomic(const std::string &path, const InputMesh &mesh);

struct DecimatorOptions {
  std::size_t target_faces = 0;
  MemoryMode memory_mode = MemoryMode::Balanced;
  std::string trace_path;
};

struct DecimatorStats {
  std::size_t input_vertices = 0;
  std::size_t input_faces = 0;
  std::size_t output_vertices = 0;
  std::size_t output_faces = 0;
  std::size_t collapsed_edges = 0;
  std::size_t rejected_topology = 0;
  std::size_t rejected_flip = 0;
  std::size_t invalid_edges = 0;
  bool target_reached = false;
};

class QemDecimator {
 public:
  QemDecimator(InputMesh mesh, MemoryMode memory_mode);
  QemDecimator(const QemDecimator &) = delete;
  QemDecimator &operator=(const QemDecimator &) = delete;
  ~QemDecimator();

  DecimatorStats decimate(const DecimatorOptions &options);
  InputMesh compact_mesh() const;

 private:
  class Impl;
  Impl *impl_;
};

}  // namespace standalone_decimator
