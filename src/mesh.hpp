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
  unsigned threads = 1;
  std::size_t partition_local_count = 0;
  std::size_t partition_local_target_faces = 0;
  std::size_t partition_local_max_epochs = 1;
  double partition_local_max_normalized_cost = 0.0;
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
  std::size_t partition_dry_run_count = 0;
  std::size_t partition_alive_vertices = 0;
  std::size_t partition_alive_edges = 0;
  std::size_t partition_face_corner_load_min = 0;
  double partition_face_corner_load_mean = 0.0;
  std::size_t partition_face_corner_load_max = 0;
  double partition_face_corner_load_max_over_mean = 0.0;
  std::size_t partition_cross_edge_count = 0;
  double partition_cross_edge_fraction = 0.0;
  std::size_t partition_halo_b0_vertex_count = 0;
  double partition_halo_b0_vertex_fraction = 0.0;
  std::size_t partition_halo_b1_vertex_count = 0;
  double partition_halo_b1_vertex_fraction = 0.0;
  std::size_t partition_halo_b2_vertex_count = 0;
  double partition_halo_b2_vertex_fraction = 0.0;
  std::size_t partition_halo_face_count = 0;
  double partition_halo_face_fraction = 0.0;
  std::size_t partition_eligible_edge_count = 0;
  double partition_eligible_edge_fraction = 0.0;
  double partition_wall_seconds = 0.0;
  std::size_t partition_transient_bytes = 0;
  std::size_t partition_local_count = 0;
  std::size_t partition_local_target_faces = 0;
  std::size_t partition_local_epoch_count = 0;
  double partition_local_max_normalized_cost = 0.0;
  double partition_local_effective_max_cost = 0.0;
  std::size_t partition_local_output_faces = 0;
  std::size_t partition_local_collapsed_edges = 0;
  std::size_t partition_local_stalled_count = 0;
  std::size_t partition_local_heap_entries = 0;
  double partition_local_plan_seconds = 0.0;
  double partition_local_heap_build_seconds = 0.0;
  double partition_local_collapse_seconds = 0.0;
  unsigned partition_local_workers = 0;
  double partition_local_parallel_seconds = 0.0;
  double partition_local_worker_seconds = 0.0;
  double partition_local_worker_max_seconds = 0.0;
  std::size_t global_cleanup_input_faces = 0;
  std::size_t global_cleanup_collapsed_edges = 0;
  double global_heap_rebuild_seconds = 0.0;
  double global_cleanup_seconds = 0.0;
  bool target_reached = false;
};

class QemDecimator {
 public:
  QemDecimator(InputMesh mesh,
               MemoryMode memory_mode,
               bool build_global_heap,
               unsigned threads);
  QemDecimator(const QemDecimator &) = delete;
  QemDecimator &operator=(const QemDecimator &) = delete;
  ~QemDecimator();

  void partition_dry_run(std::size_t partition_count);
  DecimatorStats decimate(const DecimatorOptions &options);
  InputMesh compact_mesh() const;

 private:
  class Impl;
  Impl *impl_;
};

}  // namespace standalone_decimator
