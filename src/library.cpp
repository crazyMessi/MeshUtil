#include "meshutil/simplify.hpp"

#include "mesh.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace meshutil {
namespace {

template<typename Value>
Value read_strided(const void *base,
                   const std::size_t stride_bytes,
                   const std::size_t item,
                   const std::size_t component)
{
  Value value;
  const auto *bytes = static_cast<const unsigned char *>(base);
  std::memcpy(&value,
              bytes + item * stride_bytes + component * sizeof(Value),
              sizeof(Value));
  return value;
}

std::size_t effective_target(const std::size_t input_faces,
                             const std::size_t requested_faces) noexcept
{
  if (input_faces == 0 || input_faces <= requested_faces) {
    return requested_faces;
  }
  const float ratio = static_cast<float>(
      static_cast<double>(requested_faces) / static_cast<double>(input_faces));
  return static_cast<std::size_t>(static_cast<float>(input_faces) * ratio);
}

standalone_decimator::InputMesh copy_input(const MeshView mesh)
{
  standalone_decimator::InputMesh input;
  input.vertices.resize(mesh.vertex_count);
  input.faces.resize(mesh.triangle_count);
  for (std::size_t vertex = 0; vertex < mesh.vertex_count; ++vertex) {
    standalone_decimator::Float3 &position = input.vertices[vertex];
    position.x = read_strided<float>(
        mesh.positions, mesh.position_stride_bytes, vertex, 0);
    position.y = read_strided<float>(
        mesh.positions, mesh.position_stride_bytes, vertex, 1);
    position.z = read_strided<float>(
        mesh.positions, mesh.position_stride_bytes, vertex, 2);
  }
  for (std::size_t triangle = 0; triangle < mesh.triangle_count; ++triangle) {
    std::array<standalone_decimator::VertexId, 3> &face = input.faces[triangle];
    for (std::size_t corner = 0; corner < face.size(); ++corner) {
      face[corner] = read_strided<Index>(
          mesh.triangles, mesh.triangle_stride_bytes, triangle, corner);
    }
  }
  return input;
}

standalone_decimator::InputMesh consume_input(Mesh mesh)
{
  validate_mesh(mesh.view());
  standalone_decimator::InputMesh input;
  input.vertices.resize(mesh.vertex_count());
  input.faces.resize(mesh.triangle_count());
  for (std::size_t vertex = 0; vertex < input.vertices.size(); ++vertex) {
    input.vertices[vertex] = {
        mesh.positions[vertex * 3],
        mesh.positions[vertex * 3 + 1],
        mesh.positions[vertex * 3 + 2],
    };
  }
  std::vector<float>().swap(mesh.positions);
  for (std::size_t face = 0; face < input.faces.size(); ++face) {
    input.faces[face] = {
        mesh.triangles[face * 3],
        mesh.triangles[face * 3 + 1],
        mesh.triangles[face * 3 + 2],
    };
  }
  std::vector<Index>().swap(mesh.triangles);
  return input;
}

Mesh copy_output(const standalone_decimator::InputMesh &input)
{
  Mesh output;
  output.positions.resize(input.vertices.size() * 3);
  output.triangles.resize(input.faces.size() * 3);
  for (std::size_t vertex = 0; vertex < input.vertices.size(); ++vertex) {
    output.positions[vertex * 3] = input.vertices[vertex].x;
    output.positions[vertex * 3 + 1] = input.vertices[vertex].y;
    output.positions[vertex * 3 + 2] = input.vertices[vertex].z;
  }
  for (std::size_t face = 0; face < input.faces.size(); ++face) {
    for (std::size_t corner = 0; corner < input.faces[face].size(); ++corner) {
      output.triangles[face * 3 + corner] = input.faces[face][corner];
    }
  }
  return output;
}

SimplifyStats copy_stats(const standalone_decimator::DecimatorStats &stats)
{
  SimplifyStats result;
  result.input_vertices = stats.input_vertices;
  result.input_faces = stats.input_faces;
  result.output_vertices = stats.output_vertices;
  result.output_faces = stats.output_faces;
  result.collapsed_edges = stats.collapsed_edges;
  result.rejected_topology = stats.rejected_topology;
  result.rejected_flip = stats.rejected_flip;
  result.invalid_edges = stats.invalid_edges;
  result.target_reached = stats.target_reached;
  return result;
}

}  // namespace

void validate_mesh(const MeshView mesh)
{
  if (mesh.vertex_count != 0 && mesh.positions == nullptr) {
    throw std::invalid_argument("positions is null for a non-empty mesh");
  }
  if (mesh.triangle_count != 0 && mesh.triangles == nullptr) {
    throw std::invalid_argument("triangles is null for a non-empty mesh");
  }
  if (mesh.position_stride_bytes < 3 * sizeof(float)) {
    throw std::invalid_argument("position stride is smaller than float32 xyz");
  }
  if (mesh.triangle_stride_bytes < 3 * sizeof(Index)) {
    throw std::invalid_argument("triangle stride is smaller than three uint32 indices");
  }
  if (mesh.vertex_count > std::numeric_limits<Index>::max()) {
    throw std::invalid_argument("mesh exceeds uint32 vertex capacity");
  }
  for (std::size_t vertex = 0; vertex < mesh.vertex_count; ++vertex) {
    for (std::size_t component = 0; component < 3; ++component) {
      const float value = read_strided<float>(
          mesh.positions, mesh.position_stride_bytes, vertex, component);
      if (!std::isfinite(value)) {
        throw std::invalid_argument("mesh contains a non-finite position");
      }
    }
  }
  for (std::size_t triangle = 0; triangle < mesh.triangle_count; ++triangle) {
    Index face[3];
    for (std::size_t corner = 0; corner < 3; ++corner) {
      face[corner] = read_strided<Index>(
          mesh.triangles, mesh.triangle_stride_bytes, triangle, corner);
      if (static_cast<std::size_t>(face[corner]) >= mesh.vertex_count) {
        throw std::invalid_argument("triangle index is out of range");
      }
    }
    if (face[0] == face[1] || face[1] == face[2] || face[2] == face[0]) {
      throw std::invalid_argument("triangle contains a repeated vertex index");
    }
  }
}

class Simplifier::Impl {};

Simplifier::Simplifier() : impl_(new Impl()) {}

Simplifier::Simplifier(Simplifier &&other) noexcept : impl_(other.impl_)
{
  other.impl_ = nullptr;
}

Simplifier &Simplifier::operator=(Simplifier &&other) noexcept
{
  if (this != &other) {
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

Simplifier::~Simplifier()
{
  delete impl_;
}

SimplifyResult Simplifier::simplify(const MeshView mesh,
                                    const SimplifyOptions &options)
{
  validate_mesh(mesh);
  if (impl_ == nullptr) {
    throw std::logic_error("cannot use a moved-from Simplifier");
  }
  if (options.target_faces > mesh.triangle_count) {
    throw std::invalid_argument("target_faces exceeds the input triangle count");
  }
  if (options.threads != 0 && options.threads != 1) {
    throw std::invalid_argument(
        "the baseline QEM backend currently supports threads=0 or threads=1");
  }

  const auto input_start = std::chrono::steady_clock::now();
  standalone_decimator::InputMesh input = copy_input(mesh);
  const auto input_end = std::chrono::steady_clock::now();
  const auto initialization_start = input_end;
  standalone_decimator::QemDecimator decimator(std::move(input));
  const auto initialization_end = std::chrono::steady_clock::now();
  standalone_decimator::DecimatorOptions internal_options;
  internal_options.target_faces =
      effective_target(mesh.triangle_count, options.target_faces);
  internal_options.trace_path = options.trace_path;
  const auto collapse_start = initialization_end;
  const standalone_decimator::DecimatorStats internal_stats =
      decimator.decimate(internal_options);
  const auto collapse_end = std::chrono::steady_clock::now();
  const auto compact_start = collapse_end;
  standalone_decimator::InputMesh compacted = decimator.compact_mesh();
  const auto compact_end = std::chrono::steady_clock::now();

  SimplifyResult result;
  const auto output_start = compact_end;
  result.mesh = copy_output(compacted);
  const auto output_end = std::chrono::steady_clock::now();
  result.stats = copy_stats(internal_stats);
  result.stats.input_conversion_seconds =
      std::chrono::duration<double>(input_end - input_start).count();
  result.stats.initialization_seconds =
      std::chrono::duration<double>(initialization_end - initialization_start).count();
  result.stats.collapse_seconds =
      std::chrono::duration<double>(collapse_end - collapse_start).count();
  result.stats.compact_seconds =
      std::chrono::duration<double>(compact_end - compact_start).count();
  result.stats.output_conversion_seconds =
      std::chrono::duration<double>(output_end - output_start).count();
  result.stats.core_seconds =
      result.stats.initialization_seconds + result.stats.collapse_seconds +
      result.stats.compact_seconds;
  return result;
}

SimplifyResult Simplifier::simplify(Mesh mesh, const SimplifyOptions &options)
{
  const std::size_t input_faces = mesh.triangle_count();
  if (impl_ == nullptr) {
    throw std::logic_error("cannot use a moved-from Simplifier");
  }
  if (options.target_faces > input_faces) {
    throw std::invalid_argument("target_faces exceeds the input triangle count");
  }
  if (options.threads != 0 && options.threads != 1) {
    throw std::invalid_argument(
        "the baseline QEM backend currently supports threads=0 or threads=1");
  }

  const auto input_start = std::chrono::steady_clock::now();
  standalone_decimator::InputMesh input = consume_input(std::move(mesh));
  const auto input_end = std::chrono::steady_clock::now();
  const auto initialization_start = input_end;
  standalone_decimator::QemDecimator decimator(std::move(input));
  const auto initialization_end = std::chrono::steady_clock::now();
  standalone_decimator::DecimatorOptions internal_options;
  internal_options.target_faces =
      effective_target(input_faces, options.target_faces);
  internal_options.trace_path = options.trace_path;
  const auto collapse_start = initialization_end;
  const standalone_decimator::DecimatorStats internal_stats =
      decimator.decimate(internal_options);
  const auto collapse_end = std::chrono::steady_clock::now();
  const auto compact_start = collapse_end;
  standalone_decimator::InputMesh compacted = decimator.compact_mesh();
  const auto compact_end = std::chrono::steady_clock::now();

  SimplifyResult result;
  const auto output_start = compact_end;
  result.mesh = copy_output(compacted);
  const auto output_end = std::chrono::steady_clock::now();
  result.stats = copy_stats(internal_stats);
  result.stats.input_conversion_seconds =
      std::chrono::duration<double>(input_end - input_start).count();
  result.stats.initialization_seconds =
      std::chrono::duration<double>(initialization_end - initialization_start).count();
  result.stats.collapse_seconds =
      std::chrono::duration<double>(collapse_end - collapse_start).count();
  result.stats.compact_seconds =
      std::chrono::duration<double>(compact_end - compact_start).count();
  result.stats.output_conversion_seconds =
      std::chrono::duration<double>(output_end - output_start).count();
  result.stats.core_seconds =
      result.stats.initialization_seconds + result.stats.collapse_seconds +
      result.stats.compact_seconds;
  return result;
}

SimplifyResult simplify(const MeshView mesh, const SimplifyOptions &options)
{
  Simplifier simplifier;
  return simplifier.simplify(mesh, options);
}

SimplifyResult simplify(Mesh mesh, const SimplifyOptions &options)
{
  Simplifier simplifier;
  return simplifier.simplify(std::move(mesh), options);
}

}  // namespace meshutil
