#pragma once

#include "meshutil/mesh.hpp"

#include <cstddef>
#include <string>

namespace meshutil {

enum class MemoryMode {
  Balanced,
  Low,
};

struct SimplifyOptions {
  std::size_t target_faces = 0;
  unsigned threads = 1;
  std::size_t partition_dry_run_count = 0;
  std::size_t partition_local_count = 0;
  std::size_t partition_local_target_faces = 0;
  std::size_t partition_local_max_epochs = 1;
  double partition_local_max_normalized_cost = 1.5e-13;
  MemoryMode memory_mode = MemoryMode::Balanced;
  std::string trace_path;
};

struct SimplifyStats {
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
  double input_conversion_seconds = 0.0;
  double initialization_seconds = 0.0;
  double collapse_seconds = 0.0;
  double compact_seconds = 0.0;
  double output_conversion_seconds = 0.0;
  double core_seconds = 0.0;
};

struct SimplifyResult {
  Mesh mesh;
  SimplifyStats stats;
};

class Simplifier {
 public:
  Simplifier();
  Simplifier(const Simplifier &) = delete;
  Simplifier &operator=(const Simplifier &) = delete;
  Simplifier(Simplifier &&) noexcept;
  Simplifier &operator=(Simplifier &&) noexcept;
  ~Simplifier();

  SimplifyResult simplify(MeshView mesh, const SimplifyOptions &options);
  SimplifyResult simplify(Mesh mesh, const SimplifyOptions &options);

 private:
  class Impl;
  Impl *impl_;
};

SimplifyResult simplify(MeshView mesh, const SimplifyOptions &options);
SimplifyResult simplify(Mesh mesh, const SimplifyOptions &options);

}  // namespace meshutil
