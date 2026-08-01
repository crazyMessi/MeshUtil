#include "meshutil/io.hpp"

#include "mesh.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace meshutil {
namespace {

std::string lowercase(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return text;
}

standalone_decimator::InputMesh to_internal(const MeshView mesh)
{
  validate_mesh(mesh);
  standalone_decimator::InputMesh result;
  result.vertices.resize(mesh.vertex_count);
  result.faces.resize(mesh.triangle_count);
  const auto *position_bytes = reinterpret_cast<const unsigned char *>(mesh.positions);
  const auto *triangle_bytes = reinterpret_cast<const unsigned char *>(mesh.triangles);
  for (std::size_t vertex = 0; vertex < mesh.vertex_count; ++vertex) {
    float position[3];
    std::memcpy(position,
                position_bytes + vertex * mesh.position_stride_bytes,
                sizeof(position));
    result.vertices[vertex] = {position[0], position[1], position[2]};
  }
  for (std::size_t triangle = 0; triangle < mesh.triangle_count; ++triangle) {
    std::array<Index, 3> face;
    std::memcpy(face.data(),
                triangle_bytes + triangle * mesh.triangle_stride_bytes,
                sizeof(face));
    result.faces[triangle] = face;
  }
  return result;
}

Mesh from_internal(standalone_decimator::InputMesh input)
{
  Mesh result;
  result.positions.resize(input.vertices.size() * 3);
  result.triangles.resize(input.faces.size() * 3);
  for (std::size_t vertex = 0; vertex < input.vertices.size(); ++vertex) {
    result.positions[vertex * 3] = input.vertices[vertex].x;
    result.positions[vertex * 3 + 1] = input.vertices[vertex].y;
    result.positions[vertex * 3 + 2] = input.vertices[vertex].z;
  }
  for (std::size_t triangle = 0; triangle < input.faces.size(); ++triangle) {
    for (std::size_t corner = 0; corner < input.faces[triangle].size(); ++corner) {
      result.triangles[triangle * 3 + corner] = input.faces[triangle][corner];
    }
  }
  return result;
}

Index parse_obj_index(const std::string_view token,
                      const std::size_t vertex_count,
                      const std::string &path)
{
  const std::size_t slash = token.find('/');
  const std::string_view index_text = token.substr(0, slash);
  long long raw_index = 0;
  const auto parsed = std::from_chars(
      index_text.data(), index_text.data() + index_text.size(), raw_index);
  if (index_text.empty() || parsed.ec != std::errc() ||
      parsed.ptr != index_text.data() + index_text.size() || raw_index == 0)
  {
    throw std::runtime_error(path + ": invalid OBJ face index");
  }
  const long long resolved =
      raw_index > 0 ? raw_index - 1 : static_cast<long long>(vertex_count) + raw_index;
  if (resolved < 0 || resolved >= static_cast<long long>(vertex_count)) {
    throw std::runtime_error(path + ": OBJ face index is out of range");
  }
  return static_cast<Index>(resolved);
}

Mesh read_obj(const std::string &path)
{
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error(path + ": cannot open input");
  }
  Mesh mesh;
  std::string line;
  while (std::getline(stream, line)) {
    std::istringstream tokens(line);
    std::string keyword;
    tokens >> keyword;
    if (keyword.empty() || keyword.front() == '#') {
      continue;
    }
    if (keyword == "v") {
      float x = 0.0f;
      float y = 0.0f;
      float z = 0.0f;
      if (!(tokens >> x >> y >> z)) {
        throw std::runtime_error(path + ": malformed OBJ vertex");
      }
      mesh.positions.insert(mesh.positions.end(), {x, y, z});
    }
    else if (keyword == "f") {
      std::vector<Index> polygon;
      std::string token;
      while (tokens >> token) {
        polygon.push_back(parse_obj_index(token, mesh.vertex_count(), path));
      }
      if (polygon.size() < 3) {
        throw std::runtime_error(path + ": OBJ face has fewer than three vertices");
      }
      for (std::size_t corner = 1; corner + 1 < polygon.size(); ++corner) {
        mesh.triangles.insert(
            mesh.triangles.end(), {polygon[0], polygon[corner], polygon[corner + 1]});
      }
    }
  }
  if (!stream.eof()) {
    throw std::runtime_error(path + ": failed while reading OBJ");
  }
  validate_mesh(mesh.view());
  return mesh;
}

void write_obj(const std::string &path, const Mesh &mesh)
{
  validate_mesh(mesh.view());
  const std::filesystem::path destination(path);
  if (destination.has_parent_path()) {
    std::filesystem::create_directories(destination.parent_path());
  }
  std::ofstream stream(path, std::ios::trunc);
  if (!stream) {
    throw std::runtime_error(path + ": cannot open output");
  }
  stream << std::setprecision(std::numeric_limits<float>::max_digits10);
  for (std::size_t vertex = 0; vertex < mesh.vertex_count(); ++vertex) {
    stream << "v " << mesh.positions[vertex * 3] << ' '
           << mesh.positions[vertex * 3 + 1] << ' '
           << mesh.positions[vertex * 3 + 2] << '\n';
  }
  for (std::size_t triangle = 0; triangle < mesh.triangle_count(); ++triangle) {
    stream << "f " << mesh.triangles[triangle * 3] + 1 << ' '
           << mesh.triangles[triangle * 3 + 1] + 1 << ' '
           << mesh.triangles[triangle * 3 + 2] + 1 << '\n';
  }
  if (!stream) {
    throw std::runtime_error(path + ": failed while writing OBJ");
  }
}

}  // namespace

MeshFormat detect_mesh_format(const std::string &path)
{
  const std::string extension =
      lowercase(std::filesystem::path(path).extension().string());
  if (extension == ".ply") {
    return MeshFormat::Ply;
  }
  if (extension == ".obj") {
    return MeshFormat::Obj;
  }
  throw std::invalid_argument("cannot detect mesh format from path: " + path);
}

Mesh read_mesh(const std::string &path, MeshFormat format)
{
  if (format == MeshFormat::Auto) {
    format = detect_mesh_format(path);
  }
  if (format == MeshFormat::Ply) {
    return from_internal(standalone_decimator::read_binary_triangle_ply(path));
  }
  if (format == MeshFormat::Obj) {
    return read_obj(path);
  }
  throw std::invalid_argument("unsupported input mesh format");
}

void write_mesh(const std::string &path, const Mesh &mesh, MeshFormat format)
{
  if (format == MeshFormat::Auto) {
    format = detect_mesh_format(path);
  }
  if (format == MeshFormat::Ply) {
    standalone_decimator::write_binary_triangle_ply_atomic(
        path, to_internal(mesh.view()));
    return;
  }
  if (format == MeshFormat::Obj) {
    write_obj(path, mesh);
    return;
  }
  throw std::invalid_argument("unsupported output mesh format");
}

}  // namespace meshutil
