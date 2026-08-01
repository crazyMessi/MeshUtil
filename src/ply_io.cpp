#include "mesh.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace standalone_decimator {
namespace {

enum class FaceIndexType {
  SignedInt32,
  UnsignedInt32,
};

constexpr std::size_t kIoBufferSize = 4 * 1024 * 1024;
constexpr std::size_t kVertexRecordSize = 3 * sizeof(std::uint32_t);
constexpr std::size_t kFaceRecordSize =
    sizeof(std::uint8_t) + 3 * sizeof(std::uint32_t);

[[noreturn]] void fail(const std::string &path, const std::string &message)
{
  throw std::runtime_error(path + ": " + message);
}

std::string read_header_line(std::istream &stream, const std::string &path)
{
  std::string line;
  if (!std::getline(stream, line)) {
    fail(path, "unexpected end of PLY header");
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

std::size_t parse_count(const std::string &text, const std::string &path)
{
  if (text.empty()) {
    fail(path, "empty PLY element count");
  }
  std::size_t parsed = 0;
  unsigned long long value = 0;
  try {
    value = std::stoull(text, &parsed, 10);
  }
  catch (const std::exception &) {
    fail(path, "invalid PLY element count: " + text);
  }
  if (parsed != text.size() || value > std::numeric_limits<std::uint32_t>::max()) {
    fail(path, "unsupported PLY element count: " + text);
  }
  return static_cast<std::size_t>(value);
}

void read_bytes(std::istream &stream,
                void *data,
                const std::size_t size,
                const std::string &path)
{
  stream.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
  if (!stream) {
    fail(path, "truncated binary PLY payload");
  }
}

std::uint32_t decode_little_uint32(const unsigned char *bytes) noexcept
{
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

float decode_float32(const unsigned char *bytes, const std::string &path)
{
  const std::uint32_t bits = decode_little_uint32(bytes);
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&value, &bits, sizeof(value));
  if (!std::isfinite(value)) {
    fail(path, "vertex coordinate is not finite");
  }
  return value;
}

void write_bytes(std::ostream &stream,
                 const void *data,
                 const std::size_t size,
                 const std::string &path)
{
  stream.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
  if (!stream) {
    fail(path, "failed while writing binary PLY");
  }
}

void encode_little_uint32(unsigned char *bytes, const std::uint32_t value) noexcept
{
  bytes[0] = static_cast<unsigned char>(value & 0xffu);
  bytes[1] = static_cast<unsigned char>((value >> 8) & 0xffu);
  bytes[2] = static_cast<unsigned char>((value >> 16) & 0xffu);
  bytes[3] = static_cast<unsigned char>((value >> 24) & 0xffu);
}

void encode_float32(unsigned char *bytes, const double source, const std::string &path)
{
  const float value = static_cast<float>(source);
  if (!std::isfinite(value)) {
    fail(path, "vertex coordinate cannot be represented as finite float32");
  }
  if (value == 0.0f) {
    encode_little_uint32(bytes, 0);
    return;
  }
  std::uint32_t bits = 0;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&bits, &value, sizeof(value));
  encode_little_uint32(bytes, bits);
}

class TemporaryFile {
 public:
  explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}

  ~TemporaryFile()
  {
    if (!committed_) {
      std::error_code error;
      std::filesystem::remove(path_, error);
    }
  }

  const std::filesystem::path &path() const noexcept
  {
    return path_;
  }

  void commit() noexcept
  {
    committed_ = true;
  }

 private:
  std::filesystem::path path_;
  bool committed_ = false;
};

}  // namespace

InputMesh read_binary_triangle_ply(const std::string &path)
{
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    fail(path, "cannot open input");
  }
  if (read_header_line(stream, path) != "ply") {
    fail(path, "missing PLY magic");
  }
  if (read_header_line(stream, path) != "format binary_little_endian 1.0") {
    fail(path, "only binary_little_endian PLY 1.0 is accepted");
  }

  enum class HeaderSection {
    None,
    Vertex,
    Face,
  };
  HeaderSection section = HeaderSection::None;
  std::size_t vertex_count = 0;
  std::size_t face_count = 0;
  std::vector<std::string> vertex_properties;
  std::vector<std::string> face_properties;
  bool saw_vertex = false;
  bool saw_face = false;

  while (true) {
    const std::string line = read_header_line(stream, path);
    if (line == "end_header") {
      break;
    }
    if (line.empty() || line.rfind("comment ", 0) == 0 || line.rfind("obj_info ", 0) == 0) {
      continue;
    }

    std::istringstream tokens(line);
    std::string keyword;
    tokens >> keyword;
    if (keyword == "element") {
      std::string name;
      std::string count;
      std::string extra;
      if (!(tokens >> name >> count) || (tokens >> extra)) {
        fail(path, "malformed element declaration: " + line);
      }
      if (name == "vertex" && !saw_vertex && !saw_face) {
        vertex_count = parse_count(count, path);
        saw_vertex = true;
        section = HeaderSection::Vertex;
      }
      else if (name == "face" && saw_vertex && !saw_face) {
        face_count = parse_count(count, path);
        saw_face = true;
        section = HeaderSection::Face;
      }
      else {
        fail(path, "only one vertex element followed by one face element is accepted");
      }
    }
    else if (keyword == "property") {
      if (section == HeaderSection::None) {
        fail(path, "property appears before an element");
      }
      if (section == HeaderSection::Vertex) {
        vertex_properties.push_back(line);
      }
      else {
        face_properties.push_back(line);
      }
    }
    else {
      fail(path, "unsupported PLY header directive: " + keyword);
    }
  }

  const std::vector<std::string> expected_vertex_properties = {
      "property float x", "property float y", "property float z"};
  if (!saw_vertex || !saw_face || vertex_properties != expected_vertex_properties ||
      face_properties.size() != 1)
  {
    fail(path,
         "strict schema requires float x/y/z vertices and "
         "list uchar int/int32/uint/uint32 vertex_indices faces");
  }

  FaceIndexType face_index_type = FaceIndexType::SignedInt32;
  if (face_properties[0] == "property list uchar int vertex_indices" ||
      face_properties[0] == "property list uchar int32 vertex_indices")
  {
    face_index_type = FaceIndexType::SignedInt32;
  }
  else if (face_properties[0] == "property list uchar uint vertex_indices" ||
           face_properties[0] == "property list uchar uint32 vertex_indices")
  {
    face_index_type = FaceIndexType::UnsignedInt32;
  }
  else {
    fail(path,
         "strict face schema requires "
         "property list uchar int/int32/uint/uint32 vertex_indices");
  }

  InputMesh mesh;
  mesh.vertices.resize(vertex_count);
  mesh.faces.resize(face_count);
  std::vector<unsigned char> buffer(kIoBufferSize);

  constexpr std::size_t vertices_per_chunk = kIoBufferSize / kVertexRecordSize;
  for (std::size_t first = 0; first < vertex_count;) {
    const std::size_t chunk_count = std::min(vertices_per_chunk, vertex_count - first);
    const std::size_t chunk_size = chunk_count * kVertexRecordSize;
    read_bytes(stream, buffer.data(), chunk_size, path);

    const unsigned char *cursor = buffer.data();
    for (std::size_t index = 0; index < chunk_count; ++index) {
      Vec3 &vertex = mesh.vertices[first + index];
      vertex.x = decode_float32(cursor, path);
      cursor += sizeof(std::uint32_t);
      vertex.y = decode_float32(cursor, path);
      cursor += sizeof(std::uint32_t);
      vertex.z = decode_float32(cursor, path);
      cursor += sizeof(std::uint32_t);
    }
    first += chunk_count;
  }

  constexpr std::size_t faces_per_chunk = kIoBufferSize / kFaceRecordSize;
  for (std::size_t first = 0; first < face_count;) {
    const std::size_t chunk_count = std::min(faces_per_chunk, face_count - first);
    const std::size_t chunk_size = chunk_count * kFaceRecordSize;
    read_bytes(stream, buffer.data(), chunk_size, path);

    const unsigned char *cursor = buffer.data();
    for (std::size_t index = 0; index < chunk_count; ++index) {
      std::array<VertexId, 3> &face = mesh.faces[first + index];
      const std::uint8_t corners = *cursor++;
      if (corners != 3) {
        fail(path, "all faces must contain exactly three vertices");
      }
      for (VertexId &vertex : face) {
        const std::uint32_t raw_index = decode_little_uint32(cursor);
        cursor += sizeof(std::uint32_t);
        if (face_index_type == FaceIndexType::SignedInt32 &&
            raw_index > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        {
          fail(path, "face vertex index is negative");
        }
        if (static_cast<std::size_t>(raw_index) >= vertex_count) {
          fail(path, "face vertex index is out of range");
        }
        vertex = static_cast<VertexId>(raw_index);
      }
      if (face[0] == face[1] || face[1] == face[2] || face[2] == face[0]) {
        fail(path, "degenerate input face contains a repeated vertex");
      }
    }
    first += chunk_count;
  }

  char trailing = 0;
  if (stream.read(&trailing, 1)) {
    fail(path, "unexpected trailing bytes after strict PLY payload");
  }
  if (!stream.eof()) {
    fail(path, "I/O error after reading PLY payload");
  }
  return mesh;
}

void write_binary_triangle_ply_atomic(const std::string &path, const InputMesh &mesh)
{
  if (mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max()) {
    fail(path, "too many vertices for uint32 PLY indices");
  }

  const std::filesystem::path destination(path);
  if (destination.has_parent_path()) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
      fail(path, "cannot create output directory: " + error.message());
    }
  }

  const std::filesystem::path temporary_path =
      destination.string() + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
  TemporaryFile temporary(temporary_path);
  std::ofstream stream(temporary.path(), std::ios::binary | std::ios::trunc);
  if (!stream) {
    fail(temporary.path().string(), "cannot open temporary output");
  }
  const std::string temporary_path_string = temporary.path().string();

  stream << "ply\n"
         << "format binary_little_endian 1.0\n"
         << "comment Created in Blender version 4.0.2\n"
         << "element vertex " << mesh.vertices.size() << "\n"
         << "property float x\n"
         << "property float y\n"
         << "property float z\n"
         << "element face " << mesh.faces.size() << "\n"
         << "property list uchar uint vertex_indices\n"
         << "end_header\n";
  if (!stream) {
    fail(temporary_path_string, "failed while writing PLY header");
  }

  std::vector<unsigned char> buffer(kIoBufferSize);

  constexpr std::size_t vertices_per_chunk = kIoBufferSize / kVertexRecordSize;
  for (std::size_t first = 0; first < mesh.vertices.size();) {
    const std::size_t chunk_count =
        std::min(vertices_per_chunk, mesh.vertices.size() - first);
    unsigned char *cursor = buffer.data();
    for (std::size_t index = 0; index < chunk_count; ++index) {
      const Vec3 &vertex = mesh.vertices[first + index];
      encode_float32(cursor, vertex.x, temporary_path_string);
      cursor += sizeof(std::uint32_t);
      encode_float32(cursor, vertex.y, temporary_path_string);
      cursor += sizeof(std::uint32_t);
      encode_float32(cursor, vertex.z, temporary_path_string);
      cursor += sizeof(std::uint32_t);
    }
    write_bytes(stream, buffer.data(), chunk_count * kVertexRecordSize, temporary_path_string);
    first += chunk_count;
  }

  constexpr std::size_t faces_per_chunk = kIoBufferSize / kFaceRecordSize;
  for (std::size_t first = 0; first < mesh.faces.size();) {
    const std::size_t chunk_count = std::min(faces_per_chunk, mesh.faces.size() - first);
    unsigned char *cursor = buffer.data();
    for (std::size_t index = 0; index < chunk_count; ++index) {
      const std::array<VertexId, 3> &face = mesh.faces[first + index];
      *cursor++ = 3;
      for (const VertexId vertex : face) {
        if (static_cast<std::size_t>(vertex) >= mesh.vertices.size()) {
          fail(path, "output face vertex index is out of range");
        }
        encode_little_uint32(cursor, vertex);
        cursor += sizeof(std::uint32_t);
      }
    }
    write_bytes(stream, buffer.data(), chunk_count * kFaceRecordSize, temporary_path_string);
    first += chunk_count;
  }

  stream.flush();
  if (!stream) {
    fail(temporary_path_string, "failed to flush temporary output");
  }
  stream.close();
  if (!stream) {
    fail(temporary_path_string, "failed to close temporary output");
  }

  std::error_code error;
  std::filesystem::rename(temporary.path(), destination, error);
  if (error) {
    fail(path, "atomic rename failed: " + error.message());
  }
  temporary.commit();
}

}  // namespace standalone_decimator
