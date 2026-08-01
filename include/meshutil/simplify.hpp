#pragma once

#include "meshutil/mesh.hpp"

#include <cstddef>
#include <string>

namespace meshutil {

struct SimplifyOptions {
  std::size_t target_faces = 0;
  unsigned threads = 1;
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
