#include "meshutil/io.hpp"
#include "meshutil/simplify.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

meshutil::Mesh cube()
{
  meshutil::Mesh mesh;
  mesh.positions = {
      -1.0f, -1.0f, -1.0f,
      1.0f, -1.0f, -1.0f,
      1.0f, 1.0f, -1.0f,
      -1.0f, 1.0f, -1.0f,
      -1.0f, -1.0f, 1.0f,
      1.0f, -1.0f, 1.0f,
      1.0f, 1.0f, 1.0f,
      -1.0f, 1.0f, 1.0f,
  };
  mesh.triangles = {
      0, 2, 1, 0, 3, 2,
      4, 5, 6, 4, 6, 7,
      0, 1, 5, 0, 5, 4,
      1, 2, 6, 1, 6, 5,
      2, 3, 7, 2, 7, 6,
      3, 0, 4, 3, 4, 7,
  };
  return mesh;
}

meshutil::Mesh grid(const std::size_t side)
{
  meshutil::Mesh mesh;
  mesh.positions.reserve(side * side * 3);
  for (std::size_t y = 0; y < side; ++y) {
    for (std::size_t x = 0; x < side; ++x) {
      mesh.positions.push_back(static_cast<float>(x));
      mesh.positions.push_back(static_cast<float>(y));
      mesh.positions.push_back(0.0f);
    }
  }
  mesh.triangles.reserve((side - 1) * (side - 1) * 6);
  for (std::size_t y = 0; y + 1 < side; ++y) {
    for (std::size_t x = 0; x + 1 < side; ++x) {
      const meshutil::Index first =
          static_cast<meshutil::Index>(y * side + x);
      const meshutil::Index second = first + 1;
      const meshutil::Index third =
          static_cast<meshutil::Index>((y + 1) * side + x);
      const meshutil::Index fourth = third + 1;
      mesh.triangles.insert(
          mesh.triangles.end(),
          {first, second, fourth, first, fourth, third});
    }
  }
  return mesh;
}

void require(const bool condition, const std::string &message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main()
{
  const meshutil::Mesh input = cube();
  meshutil::SimplifyOptions options;
  options.target_faces = 6;
  const meshutil::SimplifyResult result = meshutil::simplify(input.view(), options);
  require(result.stats.input_vertices == 8, "unexpected input vertex count");
  require(result.stats.input_faces == 12, "unexpected input face count");
  require(result.stats.target_reached, "target was not reached");
  require(result.mesh.triangle_count() <= 6, "target face count was exceeded");
  meshutil::validate_mesh(result.mesh.view());

  const meshutil::SimplifyResult owning_result =
      meshutil::simplify(meshutil::Mesh(input), options);
  require(
      owning_result.mesh.positions == result.mesh.positions,
      "owning positions changed the result");
  require(
      owning_result.mesh.triangles == result.mesh.triangles,
      "owning triangles changed the result");

  meshutil::SimplifyOptions low_memory_options = options;
  low_memory_options.memory_mode = meshutil::MemoryMode::Low;
  const meshutil::SimplifyResult low_memory_result =
      meshutil::simplify(input.view(), low_memory_options);
  require(low_memory_result.stats.target_reached, "low-memory target was not reached");
  require(
      low_memory_result.mesh.triangle_count() <= options.target_faces,
      "low-memory target face count was exceeded");
  meshutil::validate_mesh(low_memory_result.mesh.view());

  const std::filesystem::path output =
      std::filesystem::temp_directory_path() / "meshutil_api_test.obj";
  meshutil::write_mesh(output.string(), result.mesh);
  const meshutil::Mesh roundtrip = meshutil::read_mesh(output.string());
  std::filesystem::remove(output);
  require(roundtrip.positions == result.mesh.positions, "OBJ positions changed");
  require(roundtrip.triangles == result.mesh.triangles, "OBJ triangles changed");

  struct PaddedPosition {
    float x;
    float y;
    float z;
    float padding;
  };
  struct PaddedTriangle {
    meshutil::Index a;
    meshutil::Index b;
    meshutil::Index c;
    meshutil::Index padding;
  };
  std::vector<PaddedPosition> padded_positions(input.vertex_count());
  std::vector<PaddedTriangle> padded_triangles(input.triangle_count());
  for (std::size_t vertex = 0; vertex < input.vertex_count(); ++vertex) {
    padded_positions[vertex] = {
        input.positions[vertex * 3],
        input.positions[vertex * 3 + 1],
        input.positions[vertex * 3 + 2],
        123.0f,
    };
  }
  for (std::size_t triangle = 0; triangle < input.triangle_count(); ++triangle) {
    padded_triangles[triangle] = {
        input.triangles[triangle * 3],
        input.triangles[triangle * 3 + 1],
        input.triangles[triangle * 3 + 2],
        std::numeric_limits<meshutil::Index>::max(),
    };
  }
  const meshutil::MeshView padded_view = {
      &padded_positions.front().x,
      padded_positions.size(),
      sizeof(PaddedPosition),
      &padded_triangles.front().a,
      padded_triangles.size(),
      sizeof(PaddedTriangle),
  };
  const meshutil::SimplifyResult padded_result =
      meshutil::simplify(padded_view, options);
  require(
      padded_result.mesh.positions == result.mesh.positions,
      "strided positions changed the result");
  require(
      padded_result.mesh.triangles == result.mesh.triangles,
      "strided triangles changed the result");

  const meshutil::Mesh grid_input = grid(64);
  meshutil::SimplifyOptions partition_options;
  partition_options.target_faces = 2000;
  partition_options.partition_local_count = 16;
  partition_options.partition_local_target_faces = 2400;
  const meshutil::SimplifyResult partition_result =
      meshutil::simplify(grid_input.view(), partition_options);
  require(partition_result.stats.target_reached, "partition target was not reached");
  require(
      partition_result.stats.partition_local_count == 16,
      "partition count was not reported");
  require(
      partition_result.stats.partition_local_collapsed_edges > 0,
      "partition-local stage did not collapse any edge");
  require(
      partition_result.stats.global_cleanup_input_faces <
          partition_result.stats.input_faces,
      "global cleanup did not receive partition-local output");
  require(
      partition_result.mesh.triangle_count() <= partition_options.target_faces,
      "partition path exceeded the target face count");
  meshutil::validate_mesh(partition_result.mesh.view());

  meshutil::SimplifyOptions parallel_partition_options = partition_options;
  parallel_partition_options.threads = 4;
  const meshutil::SimplifyResult parallel_partition_result =
      meshutil::simplify(grid_input.view(), parallel_partition_options);
  require(
      parallel_partition_result.stats.partition_local_workers == 4,
      "partition-local worker count was not reported");
  require(
      parallel_partition_result.mesh.positions == partition_result.mesh.positions,
      "parallel partition-local positions are not deterministic");
  require(
      parallel_partition_result.mesh.triangles == partition_result.mesh.triangles,
      "parallel partition-local triangles are not deterministic");

  meshutil::SimplifyOptions skipped_partition_options = options;
  skipped_partition_options.partition_local_count = 16;
  skipped_partition_options.partition_local_target_faces =
      input.triangle_count();
  const meshutil::SimplifyResult skipped_partition_result =
      meshutil::simplify(input.view(), skipped_partition_options);
  require(
      skipped_partition_result.stats.target_reached,
      "skipped partition-local path did not reach the target");
  require(
      skipped_partition_result.mesh.positions == result.mesh.positions,
      "skipped partition-local positions changed the default result");
  require(
      skipped_partition_result.mesh.triangles == result.mesh.triangles,
      "skipped partition-local triangles changed the default result");
  return 0;
}
