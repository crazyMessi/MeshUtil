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
  return 0;
}
