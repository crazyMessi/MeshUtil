#include "mesh.hpp"

#include "indexed_min_heap.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace standalone_decimator {

Vec3 &Vec3::operator+=(const Vec3 &other) noexcept
{
  x += other.x;
  y += other.y;
  z += other.z;
  return *this;
}

Vec3 &Vec3::operator*=(const double scalar) noexcept
{
  x *= scalar;
  y *= scalar;
  z *= scalar;
  return *this;
}

Vec3 operator+(Vec3 left, const Vec3 &right) noexcept
{
  left += right;
  return left;
}

Vec3 operator*(Vec3 value, const double scalar) noexcept
{
  value *= scalar;
  return value;
}

double dot(const Vec3 &left, const Vec3 &right) noexcept
{
  return left.x * right.x + left.y * right.y + left.z * right.z;
}

double length_squared(const Vec3 &value) noexcept
{
  return dot(value, value);
}

double length(const Vec3 &value) noexcept
{
  return std::sqrt(length_squared(value));
}

namespace {

constexpr double kBoundaryWeight = 100.0;
constexpr double kOptimizeEpsilon = 1.0e-8;
constexpr double kTopologyFallbackEpsilon = 1.0e-12;
constexpr double kFlipThreshold = 0.01;
constexpr double kInvalidCost = static_cast<double>(FLT_MAX);

Float3 to_float3(const Vec3 &value) noexcept
{
  volatile float x = static_cast<float>(value.x);
  volatile float y = static_cast<float>(value.y);
  volatile float z = static_cast<float>(value.z);
  return {
      x,
      y,
      z,
  };
}

Float3 to_float3(const Float3 &value) noexcept
{
  return value;
}

Vec3 to_vec3(const Float3 &value) noexcept
{
  return {
      static_cast<double>(value.x),
      static_cast<double>(value.y),
      static_cast<double>(value.z),
  };
}

Float3 subtract_float3(const Float3 &left, const Float3 &right) noexcept
{
  return {
      left.x - right.x,
      left.y - right.y,
      left.z - right.z,
  };
}

float dot_float3(const Float3 &left, const Float3 &right) noexcept
{
  return left.x * right.x + left.y * right.y + left.z * right.z;
}

Float3 cross_float3(const Float3 &left, const Float3 &right) noexcept
{
  return {
      left.y * right.z - left.z * right.y,
      left.z * right.x - left.x * right.z,
      left.x * right.y - left.y * right.x,
  };
}

float normalize_float3(Float3 &value) noexcept
{
  float magnitude = dot_float3(value, value);
  if (magnitude > 1.0e-35f) {
    magnitude = std::sqrt(magnitude);
    const float scale = 1.0f / magnitude;
    value.x *= scale;
    value.y *= scale;
    value.z *= scale;
    return magnitude;
  }
  value = {};
  return 0.0f;
}

float safe_acos_float(const float value) noexcept
{
  if (value <= -1.0f) {
    return static_cast<float>(3.14159265358979323846);
  }
  if (value >= 1.0f) {
    return 0.0f;
  }
  return std::acos(value);
}

struct Quadric {
  double xx = 0.0;
  double xy = 0.0;
  double xz = 0.0;
  double xw = 0.0;
  double yy = 0.0;
  double yz = 0.0;
  double yw = 0.0;
  double zz = 0.0;
  double zw = 0.0;
  double ww = 0.0;

  static Quadric from_plane(const double a, const double b, const double c, const double d)
  {
    Quadric result;
    result.xx = a * a;
    result.xy = a * b;
    result.xz = a * c;
    result.xw = a * d;
    result.yy = b * b;
    result.yz = b * c;
    result.yw = b * d;
    result.zz = c * c;
    result.zw = c * d;
    result.ww = d * d;
    return result;
  }

  Quadric &operator+=(const Quadric &other) noexcept
  {
    xx += other.xx;
    xy += other.xy;
    xz += other.xz;
    xw += other.xw;
    yy += other.yy;
    yz += other.yz;
    yw += other.yw;
    zz += other.zz;
    zw += other.zw;
    ww += other.ww;
    return *this;
  }

  Quadric &operator*=(const double scalar) noexcept
  {
    xx *= scalar;
    xy *= scalar;
    xz *= scalar;
    xw *= scalar;
    yy *= scalar;
    yz *= scalar;
    yw *= scalar;
    zz *= scalar;
    zw *= scalar;
    ww *= scalar;
    return *this;
  }

  double evaluate(const Vec3 &point) const noexcept
  {
    const double v00 = point.x * point.x;
    const double v01 = point.x * point.y;
    const double v02 = point.x * point.z;
    const double v11 = point.y * point.y;
    const double v12 = point.y * point.z;
    const double v22 = point.z * point.z;
    return ((xx * v00) + (xy * 2 * v01) + (xz * 2 * v02) + (xw * 2 * point.x) +
            (yy * v11) + (yz * 2 * v12) + (yw * 2 * point.y) +
            (zz * v22) + (zw * 2 * point.z) + ww);
  }

  bool optimize(Vec3 &result, const double epsilon) const noexcept
  {
    const double determinant = (xx * (yy * zz - yz * yz) -
                                xy * (xy * zz - xz * yz) +
                                xz * (xy * yz - xz * yy));
    if (std::fabs(determinant) > epsilon) {
      const double inverse_determinant = 1.0 / determinant;
      const double matrix_00 = (yy * zz - yz * yz) * inverse_determinant;
      const double matrix_10 = (yz * xz - xy * zz) * inverse_determinant;
      const double matrix_20 = (xy * yz - yy * xz) * inverse_determinant;
      const double matrix_01 = (xz * yz - xy * zz) * inverse_determinant;
      const double matrix_11 = (xx * zz - xz * xz) * inverse_determinant;
      const double matrix_21 = (xy * xz - xx * yz) * inverse_determinant;
      const double matrix_02 = (xy * yz - xz * yy) * inverse_determinant;
      const double matrix_12 = (xz * xy - xx * yz) * inverse_determinant;
      const double matrix_22 = (xx * yy - xy * xy) * inverse_determinant;
      result.x = -(matrix_00 * xw + matrix_10 * yw + matrix_20 * zw);
      result.y = -(matrix_01 * xw + matrix_11 * yw + matrix_21 * zw);
      result.z = -(matrix_02 * xw + matrix_12 * yw + matrix_22 * zw);
      return true;
    }
    return false;
  }
};

Quadric operator+(Quadric left, const Quadric &right) noexcept
{
  left += right;
  return left;
}

using LoopId = std::uint32_t;

constexpr EdgeId kInvalidEdgeId = std::numeric_limits<EdgeId>::max();
constexpr LoopId kInvalidLoopId = std::numeric_limits<LoopId>::max();
constexpr LoopId kLoopsPerFace = 3;

LoopId face_first_loop(const FaceId face_id) noexcept
{
  return static_cast<LoopId>(face_id) * kLoopsPerFace;
}

FaceId loop_face(const LoopId loop_id) noexcept
{
  return static_cast<FaceId>(loop_id / kLoopsPerFace);
}

std::size_t loop_corner(const LoopId loop_id) noexcept
{
  return static_cast<std::size_t>(loop_id % kLoopsPerFace);
}

LoopId loop_next(const LoopId loop_id) noexcept
{
  const LoopId first = loop_id - loop_id % kLoopsPerFace;
  return first + (loop_id % kLoopsPerFace + 1) % kLoopsPerFace;
}

LoopId loop_previous(const LoopId loop_id) noexcept
{
  const LoopId first = loop_id - loop_id % kLoopsPerFace;
  return first + (loop_id % kLoopsPerFace + kLoopsPerFace - 1) % kLoopsPerFace;
}

struct Vertex {
  Float3 position;
  Float3 normal;
  Quadric quadric;
  EdgeId disk_head = kInvalidEdgeId;
  bool alive = true;
};

struct DiskLink {
  EdgeId next = kInvalidEdgeId;
  EdgeId previous = kInvalidEdgeId;
};

struct Edge {
  static constexpr std::uint32_t kAliveMask = std::uint32_t{1} << 31;
  static constexpr std::uint32_t kRadialCountMask = kAliveMask - 1;

  VertexId first = 0;
  VertexId second = 0;
  DiskLink first_disk;
  DiskLink second_disk;
  LoopId radial_head = kInvalidLoopId;
  std::uint32_t radial_state = kAliveMask;

  bool alive() const noexcept
  {
    return (radial_state & kAliveMask) != 0;
  }

  std::size_t radial_count() const noexcept
  {
    return radial_state & kRadialCountMask;
  }

  void increment_radial_count()
  {
    if (radial_count() == kRadialCountMask) {
      throw std::runtime_error("edge radial count exceeds 31-bit capacity");
    }
    ++radial_state;
  }

  void decrement_radial_count()
  {
    if (radial_count() == 0) {
      throw std::runtime_error("edge radial count underflow");
    }
    --radial_state;
  }

  void deactivate() noexcept
  {
    radial_state &= kRadialCountMask;
  }
};
static_assert(sizeof(Edge) == 32, "Edge layout must remain compact");

struct Face {
  std::array<VertexId, 3> vertices{};
  bool alive = true;
};

struct Loop {
  EdgeId edge = kInvalidEdgeId;
  LoopId radial_next = kInvalidLoopId;
  LoopId radial_previous = kInvalidLoopId;
};

struct OrderedEdge {
  VertexId low = 0;
  VertexId high = 0;
};

std::uint64_t edge_key(VertexId first, VertexId second) noexcept
{
  if (first > second) {
    std::swap(first, second);
  }
  return (static_cast<std::uint64_t>(first) << 32) | static_cast<std::uint64_t>(second);
}

std::uint64_t blender_ordered_edge_hash(const OrderedEdge &edge) noexcept
{
  return (static_cast<std::uint64_t>(edge.low) << 8) ^
         static_cast<std::uint64_t>(edge.high);
}

std::size_t power_of_two_ceiling(std::size_t value)
{
  if (value <= 1) {
    return 1;
  }
  --value;
  for (std::size_t shift = 1; shift < sizeof(value) * 8; shift <<= 1) {
    value |= value >> shift;
  }
  if (value == std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("initial edge map capacity overflow");
  }
  return value + 1;
}

class BlenderEdgeMap {
 public:
  struct LookupResult {
    EdgeId edge_id = kInvalidEdgeId;
    bool inserted = false;
  };

  explicit BlenderEdgeMap(const std::size_t minimum_usable_slots)
  {
    if (minimum_usable_slots > std::numeric_limits<std::size_t>::max() / 2) {
      throw std::runtime_error("initial edge map capacity overflow");
    }
    const std::size_t minimum_inline_slots = 8;
    const std::size_t total_slots =
        std::max(minimum_inline_slots,
                 power_of_two_ceiling(std::max<std::size_t>(1, minimum_usable_slots * 2)));
    slots_.resize(total_slots);
    mask_ = total_slots - 1;
    usable_slots_ = total_slots / 2;
  }

  LookupResult lookup_or_add(VertexId first,
                             VertexId second,
                             const EdgeId new_edge_id)
  {
    OrderedEdge key;
    key.low = std::min(first, second);
    key.high = std::max(first, second);
    if (occupied_slots_ >= usable_slots_) {
      const EdgeId existing = lookup_ordered(key.low, key.high);
      if (existing != kInvalidEdgeId) {
        return {existing, false};
      }
      ensure_can_add();
    }
    const LookupResult result = lookup_or_add_without_grow(key, new_edge_id);
    if (result.inserted) {
      ++occupied_slots_;
    }
    return result;
  }

  std::size_t size() const noexcept
  {
    return occupied_slots_;
  }

  template<typename AppendEdge> void serialize_edges(AppendEdge append_edge) const
  {
    for (const Slot &slot : slots_) {
      if (slot.edge_id != kInvalidEdgeId) {
        append_edge(slot.key, slot.edge_id);
      }
    }
  }

  EdgeId lookup_ordered(const VertexId low, const VertexId high) const noexcept
  {
    OrderedEdge key;
    key.low = low;
    key.high = high;
    const std::uint64_t initial_hash = blender_ordered_edge_hash(key);
    std::uint64_t hash = initial_hash;
    std::uint64_t perturb = initial_hash;
    while (true) {
      const Slot &slot = slots_[static_cast<std::size_t>(hash & mask_)];
      if (slot.edge_id == kInvalidEdgeId) {
        return kInvalidEdgeId;
      }
      if (slot.key.low == key.low && slot.key.high == key.high) {
        return slot.edge_id;
      }
      perturb >>= 5;
      hash = 5 * hash + 1 + perturb;
    }
  }

 private:
  struct Slot {
    OrderedEdge key;
    EdgeId edge_id = kInvalidEdgeId;
  };

  LookupResult lookup_or_add_without_grow(const OrderedEdge &key,
                                          const EdgeId new_edge_id)
  {
    const std::uint64_t initial_hash = blender_ordered_edge_hash(key);
    std::uint64_t hash = initial_hash;
    std::uint64_t perturb = initial_hash;
    while (true) {
      Slot &slot = slots_[static_cast<std::size_t>(hash & mask_)];
      if (slot.edge_id == kInvalidEdgeId) {
        slot.key = key;
        slot.edge_id = new_edge_id;
        return {new_edge_id, true};
      }
      if (slot.key.low == key.low && slot.key.high == key.high) {
        return {slot.edge_id, false};
      }
      perturb >>= 5;
      hash = 5 * hash + 1 + perturb;
    }
  }

  void ensure_can_add()
  {
    if (occupied_slots_ < usable_slots_) {
      return;
    }
    if (occupied_slots_ >= std::numeric_limits<std::size_t>::max() / 2) {
      throw std::runtime_error("initial edge map capacity overflow");
    }
    const std::size_t minimum_total_slots = power_of_two_ceiling((occupied_slots_ + 1) * 2);
    const std::size_t new_total_slots = std::max<std::size_t>(8, minimum_total_slots);
    std::vector<Slot> old_slots = std::move(slots_);
    slots_.assign(new_total_slots, Slot{});
    mask_ = new_total_slots - 1;
    usable_slots_ = new_total_slots / 2;
    occupied_slots_ = 0;
    for (const Slot &slot : old_slots) {
      if (slot.edge_id != kInvalidEdgeId) {
        const LookupResult result =
            lookup_or_add_without_grow(slot.key, slot.edge_id);
        if (!result.inserted) {
          throw std::runtime_error("internal error: duplicate edge while growing initial edge map");
        }
        ++occupied_slots_;
      }
    }
  }

  std::vector<Slot> slots_;
  std::uint64_t mask_ = 0;
  std::size_t usable_slots_ = 0;
  std::size_t occupied_slots_ = 0;
};

std::size_t face_vertex_index(const Face &face, const VertexId vertex)
{
  for (std::size_t index = 0; index < face.vertices.size(); ++index) {
    if (face.vertices[index] == vertex) {
      return index;
    }
  }
  throw std::runtime_error("internal error: face does not contain expected vertex");
}

bool face_contains(const Face &face, const VertexId vertex) noexcept
{
  return face.vertices[0] == vertex || face.vertices[1] == vertex || face.vertices[2] == vertex;
}

struct MortonVertex {
  std::uint64_t morton = 0;
  VertexId vertex = 0;
  std::uint32_t face_corner_degree = 0;
};

struct PartitionPlan {
  std::size_t count = 0;
  std::vector<std::uint16_t> owner;
  std::vector<std::uint8_t> boundary_distance;
  std::vector<std::size_t> core_face_counts;
  std::vector<std::size_t> eligible_edge_offsets;
  std::vector<EdgeId> eligible_edges;
};

std::uint32_t quantize_morton_axis(const float value,
                                   const float minimum,
                                   const float maximum) noexcept
{
  constexpr std::uint32_t kMortonAxisMaximum = (std::uint32_t{1} << 21) - 1;
  if (!(maximum > minimum)) {
    return 0;
  }
  const double normalized =
      (static_cast<double>(value) - static_cast<double>(minimum)) /
      (static_cast<double>(maximum) - static_cast<double>(minimum));
  if (normalized <= 0.0) {
    return 0;
  }
  if (normalized >= 1.0) {
    return kMortonAxisMaximum;
  }
  return static_cast<std::uint32_t>(
      normalized * static_cast<double>(kMortonAxisMaximum));
}

std::uint64_t morton_code_21(const Float3 &position,
                             const Float3 &minimum,
                             const Float3 &maximum) noexcept
{
  const auto spread = [](std::uint32_t value) noexcept {
    std::uint64_t result = value & 0x1fffffU;
    result = (result | result << 32) & 0x1f00000000ffffULL;
    result = (result | result << 16) & 0x1f0000ff0000ffULL;
    result = (result | result << 8) & 0x100f00f00f00f00fULL;
    result = (result | result << 4) & 0x10c30c30c30c30c3ULL;
    result = (result | result << 2) & 0x1249249249249249ULL;
    return result;
  };
  const std::uint32_t x =
      quantize_morton_axis(position.x, minimum.x, maximum.x);
  const std::uint32_t y =
      quantize_morton_axis(position.y, minimum.y, maximum.y);
  const std::uint32_t z =
      quantize_morton_axis(position.z, minimum.z, maximum.z);
  return spread(x) | spread(y) << 1 | spread(z) << 2;
}

std::size_t stable_radix_sort_morton(std::vector<MortonVertex> &values)
{
  constexpr std::size_t kRadixBits = 11;
  constexpr std::size_t kRadixSize = std::size_t{1} << kRadixBits;
  constexpr std::uint64_t kRadixMask = kRadixSize - 1;
  constexpr std::size_t kPassCount = 6;
  std::vector<MortonVertex> scratch(values.size());
  std::vector<MortonVertex> *source = &values;
  std::vector<MortonVertex> *destination = &scratch;
  for (std::size_t pass = 0; pass < kPassCount; ++pass) {
    std::array<std::size_t, kRadixSize> offsets{};
    const std::size_t shift = pass * kRadixBits;
    for (const MortonVertex &entry : *source) {
      ++offsets[static_cast<std::size_t>((entry.morton >> shift) & kRadixMask)];
    }
    std::size_t prefix = 0;
    for (std::size_t &offset : offsets) {
      const std::size_t count = offset;
      offset = prefix;
      prefix += count;
    }
    for (const MortonVertex &entry : *source) {
      const std::size_t bucket =
          static_cast<std::size_t>((entry.morton >> shift) & kRadixMask);
      (*destination)[offsets[bucket]++] = entry;
    }
    std::swap(source, destination);
  }
  if (source != &values) {
    values = std::move(*source);
  }
  return scratch.capacity() * sizeof(MortonVertex);
}

double fraction(const std::size_t numerator, const std::size_t denominator) noexcept
{
  return denominator == 0 ?
             0.0 :
             static_cast<double>(numerator) / static_cast<double>(denominator);
}

class TraceWriter {
 public:
  explicit TraceWriter(const std::string &path)
  {
    if (!path.empty()) {
      stream_ = std::make_unique<std::ofstream>(path, std::ios::trunc);
      if (!*stream_) {
        throw std::runtime_error(path + ": cannot open trace output");
      }
      *stream_ << std::setprecision(17);
    }
  }

  template<typename WriteFields> void event(const char *name, WriteFields write_fields)
  {
    if (!stream_) {
      return;
    }
    *stream_ << "{\"event\":\"" << name << "\"";
    write_fields(*stream_);
    *stream_ << "}\n";
    if (!*stream_) {
      throw std::runtime_error("failed while writing trace output");
    }
  }

 private:
  std::unique_ptr<std::ofstream> stream_;
};

}  // namespace

class QemDecimator::Impl {
 public:
  struct WorkerContext {
    WorkerContext() = default;

    explicit WorkerContext(
        IndexedMinHeap<EdgeId>::PositionStorage &shared_positions,
        const IndexedMinHeap<EdgeId>::Position heap_tag,
        const IndexedMinHeap<EdgeId>::Position heap_tag_count)
        : heap(shared_positions, heap_tag, heap_tag_count)
    {}

    IndexedMinHeap<EdgeId> heap;
    std::vector<LoopId> loop_scratch;
    std::vector<std::pair<EdgeId, EdgeId>> splice_edges_scratch;
    std::uint32_t topology_stamp = 0;
    std::uint16_t active_partition = std::numeric_limits<std::uint16_t>::max();
    const std::vector<std::uint16_t> *local_owner = nullptr;
    const std::vector<std::uint8_t> *local_boundary_distance = nullptr;
    std::size_t collapsed_edges = 0;
    std::size_t rejected_topology = 0;
    std::size_t rejected_flip = 0;
    std::size_t invalid_edges = 0;
    std::size_t removed_faces = 0;
    std::size_t stalled_partitions = 0;
    std::size_t heap_entries = 0;
    double heap_build_seconds = 0.0;
    double collapse_seconds = 0.0;
    double worker_seconds = 0.0;
  };

  Impl(InputMesh mesh,
       const MemoryMode memory_mode,
       const bool build_global_heap,
       const unsigned threads)
      : memory_mode_(memory_mode), initialization_threads_(threads)
  {
    if (mesh.vertices.size() > std::numeric_limits<VertexId>::max() ||
        mesh.faces.size() > std::numeric_limits<FaceId>::max() ||
        mesh.faces.size() > static_cast<std::size_t>(kInvalidLoopId) / kLoopsPerFace)
    {
      throw std::runtime_error("mesh exceeds stable 32-bit ID capacity");
    }

    global_worker_.splice_edges_scratch.reserve(2);
    vertices_.reserve(mesh.vertices.size());
    for (const Float3 &position : mesh.vertices) {
      Vertex vertex;
      vertex.position = position;
      vertices_.push_back(std::move(vertex));
    }
    topology_neighbor_stamps_.resize(vertices_.size());

    for (const std::array<VertexId, 3> &face_vertices : mesh.faces) {
      for (const VertexId vertex : face_vertices) {
        if (static_cast<std::size_t>(vertex) >= vertices_.size()) {
          throw std::runtime_error("face index is out of range");
        }
      }
    }

    faces_.reserve(mesh.faces.size());
    for (const std::array<VertexId, 3> &face_vertices : mesh.faces) {
      Face face;
      face.vertices = face_vertices;
      faces_.push_back(face);
    }
    active_faces_ = faces_.size();
    input_vertices_ = vertices_.size();
    input_faces_ = faces_.size();
    std::vector<Float3>().swap(mesh.vertices);
    std::vector<std::array<VertexId, 3>>().swap(mesh.faces);

    {
      loops_.resize(faces_.size() * kLoopsPerFace);
      if (initialization_threads_ > 1 && faces_.size() >= 1000) {
        build_initial_edges_parallel_like_blender();
      }
      else {
        build_initial_edges_like_blender();
      }
      for (FaceId face_id = 0; face_id < faces_.size(); ++face_id) {
        attach_face_to_initial_edges(face_id);
      }
    }
    build_vertex_normals();
    build_quadrics();
    global_worker_.heap.prepare(edges_.size());
    if (build_global_heap) {
      for (EdgeId edge_id = 0; edge_id < edges_.size(); ++edge_id) {
        update_edge_cost(edge_id, global_worker_);
      }
    }
  }

  void partition_dry_run(const std::size_t partition_count)
  {
    if (partition_count == 0) {
      return;
    }
    const auto start = std::chrono::steady_clock::now();
    stats_.partition_alive_vertices = 0;
    stats_.partition_alive_edges = 0;
    stats_.partition_face_corner_load_min = 0;
    stats_.partition_face_corner_load_mean = 0.0;
    stats_.partition_face_corner_load_max = 0;
    stats_.partition_face_corner_load_max_over_mean = 0.0;
    stats_.partition_cross_edge_count = 0;
    stats_.partition_cross_edge_fraction = 0.0;
    stats_.partition_halo_b0_vertex_count = 0;
    stats_.partition_halo_b0_vertex_fraction = 0.0;
    stats_.partition_halo_b1_vertex_count = 0;
    stats_.partition_halo_b1_vertex_fraction = 0.0;
    stats_.partition_halo_b2_vertex_count = 0;
    stats_.partition_halo_b2_vertex_fraction = 0.0;
    stats_.partition_halo_face_count = 0;
    stats_.partition_halo_face_fraction = 0.0;
    stats_.partition_eligible_edge_count = 0;
    stats_.partition_eligible_edge_fraction = 0.0;
    stats_.partition_wall_seconds = 0.0;
    stats_.partition_transient_bytes = 0;
    stats_.partition_dry_run_count = partition_count;

    Float3 bounds_minimum;
    Float3 bounds_maximum;
    bool have_bounds = false;
    std::size_t alive_vertices = 0;
    for (const Vertex &vertex : vertices_) {
      if (!vertex.alive) {
        continue;
      }
      ++alive_vertices;
      if (!have_bounds) {
        bounds_minimum = vertex.position;
        bounds_maximum = vertex.position;
        have_bounds = true;
        continue;
      }
      bounds_minimum.x = std::min(bounds_minimum.x, vertex.position.x);
      bounds_minimum.y = std::min(bounds_minimum.y, vertex.position.y);
      bounds_minimum.z = std::min(bounds_minimum.z, vertex.position.z);
      bounds_maximum.x = std::max(bounds_maximum.x, vertex.position.x);
      bounds_maximum.y = std::max(bounds_maximum.y, vertex.position.y);
      bounds_maximum.z = std::max(bounds_maximum.z, vertex.position.z);
    }
    stats_.partition_alive_vertices = alive_vertices;

    std::fill(
        topology_neighbor_stamps_.begin(), topology_neighbor_stamps_.end(), 0);
    for (const Face &face : faces_) {
      if (!face.alive) {
        continue;
      }
      for (const VertexId vertex : face.vertices) {
        if (topology_neighbor_stamps_[vertex] ==
            std::numeric_limits<std::uint32_t>::max())
        {
          throw std::runtime_error("vertex face-corner degree exceeds uint32 capacity");
        }
        ++topology_neighbor_stamps_[vertex];
      }
    }
    std::vector<MortonVertex> morton_vertices;
    morton_vertices.reserve(alive_vertices);
    std::uint64_t total_face_corner_load = 0;
    for (VertexId vertex_id = 0; vertex_id < vertices_.size(); ++vertex_id) {
      const Vertex &vertex = vertices_[vertex_id];
      if (!vertex.alive) {
        continue;
      }
      const std::uint32_t degree = topology_neighbor_stamps_[vertex_id];
      morton_vertices.push_back({
          morton_code_21(vertex.position, bounds_minimum, bounds_maximum),
          vertex_id,
          degree,
      });
      total_face_corner_load += degree;
    }
    const std::size_t radix_transient_bytes =
        stable_radix_sort_morton(morton_vertices);
    std::fill(
        topology_neighbor_stamps_.begin(), topology_neighbor_stamps_.end(), 0);

    const std::uint16_t invalid_owner = std::numeric_limits<std::uint16_t>::max();
    std::vector<std::uint16_t> owner(vertices_.size(), invalid_owner);
    std::vector<std::size_t> partition_loads(partition_count, 0);
    std::uint64_t load_prefix = 0;
    for (std::size_t index = 0; index < morton_vertices.size(); ++index) {
      const MortonVertex &entry = morton_vertices[index];
      std::size_t partition = 0;
      if (total_face_corner_load != 0) {
        const std::uint64_t midpoint_twice =
            load_prefix * 2 + entry.face_corner_degree;
        partition = static_cast<std::size_t>(
            midpoint_twice * partition_count / (total_face_corner_load * 2));
        partition = std::min(partition, partition_count - 1);
      }
      else {
        partition = index * partition_count /
                    std::max<std::size_t>(1, morton_vertices.size());
      }
      owner[entry.vertex] = static_cast<std::uint16_t>(partition);
      partition_loads[partition] += entry.face_corner_degree;
      load_prefix += entry.face_corner_degree;
    }

    const auto load_bounds =
        std::minmax_element(partition_loads.begin(), partition_loads.end());
    stats_.partition_face_corner_load_min = *load_bounds.first;
    stats_.partition_face_corner_load_mean =
        static_cast<double>(total_face_corner_load) /
        static_cast<double>(partition_count);
    stats_.partition_face_corner_load_max = *load_bounds.second;
    stats_.partition_face_corner_load_max_over_mean =
        stats_.partition_face_corner_load_mean == 0.0 ?
            0.0 :
            static_cast<double>(stats_.partition_face_corner_load_max) /
                stats_.partition_face_corner_load_mean;

    constexpr std::uint8_t kOutsideHalo = std::numeric_limits<std::uint8_t>::max();
    std::vector<std::uint8_t> boundary_distance(vertices_.size(), kOutsideHalo);
    std::size_t alive_edges = 0;
    std::size_t cross_edges = 0;
    for (const Edge &edge : edges_) {
      if (!edge.alive() || edge.radial_head == kInvalidLoopId) {
        continue;
      }
      ++alive_edges;
      if (owner[edge.first] != owner[edge.second]) {
        ++cross_edges;
        boundary_distance[edge.first] = 0;
        boundary_distance[edge.second] = 0;
      }
    }
    stats_.partition_alive_edges = alive_edges;
    stats_.partition_cross_edge_count = cross_edges;
    stats_.partition_cross_edge_fraction = fraction(cross_edges, alive_edges);

    for (std::uint8_t ring = 1; ring <= 4; ++ring) {
      for (const Edge &edge : edges_) {
        if (!edge.alive() || edge.radial_head == kInvalidLoopId) {
          continue;
        }
        const bool first_frontier = boundary_distance[edge.first] == ring - 1;
        const bool second_frontier = boundary_distance[edge.second] == ring - 1;
        if (first_frontier && boundary_distance[edge.second] > ring) {
          boundary_distance[edge.second] = ring;
        }
        if (second_frontier && boundary_distance[edge.first] > ring) {
          boundary_distance[edge.first] = ring;
        }
      }
    }

    for (VertexId vertex_id = 0; vertex_id < vertices_.size(); ++vertex_id) {
      if (!vertices_[vertex_id].alive) {
        continue;
      }
      stats_.partition_halo_b0_vertex_count += boundary_distance[vertex_id] <= 0 ? 1 : 0;
      stats_.partition_halo_b1_vertex_count += boundary_distance[vertex_id] <= 1 ? 1 : 0;
      stats_.partition_halo_b2_vertex_count += boundary_distance[vertex_id] <= 2 ? 1 : 0;
    }
    stats_.partition_halo_b0_vertex_fraction =
        fraction(stats_.partition_halo_b0_vertex_count, alive_vertices);
    stats_.partition_halo_b1_vertex_fraction =
        fraction(stats_.partition_halo_b1_vertex_count, alive_vertices);
    stats_.partition_halo_b2_vertex_fraction =
        fraction(stats_.partition_halo_b2_vertex_count, alive_vertices);

    for (const Face &face : faces_) {
      if (!face.alive) {
        continue;
      }
      if (boundary_distance[face.vertices[0]] <= 2 ||
          boundary_distance[face.vertices[1]] <= 2 ||
          boundary_distance[face.vertices[2]] <= 2)
      {
        ++stats_.partition_halo_face_count;
      }
    }
    stats_.partition_halo_face_fraction =
        fraction(stats_.partition_halo_face_count, active_faces_);

    for (const Edge &edge : edges_) {
      if (!edge.alive() || edge.radial_head == kInvalidLoopId) {
        continue;
      }
      if (owner[edge.first] == owner[edge.second] &&
          boundary_distance[edge.first] > 4 &&
          boundary_distance[edge.second] > 4)
      {
        ++stats_.partition_eligible_edge_count;
      }
    }
    stats_.partition_eligible_edge_fraction =
        fraction(stats_.partition_eligible_edge_count, alive_edges);
    stats_.partition_transient_bytes =
        morton_vertices.capacity() * sizeof(MortonVertex) +
        radix_transient_bytes +
        owner.capacity() * sizeof(std::uint16_t) +
        boundary_distance.capacity() * sizeof(std::uint8_t) +
        partition_loads.capacity() * sizeof(std::size_t);
    stats_.partition_wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  }

  PartitionPlan build_partition_plan(const std::size_t partition_count)
  {
    PartitionPlan plan;
    plan.count = partition_count;
    const std::uint16_t invalid_owner = std::numeric_limits<std::uint16_t>::max();
    const std::uint8_t outside_halo = std::numeric_limits<std::uint8_t>::max();
    plan.owner.assign(vertices_.size(), invalid_owner);
    plan.boundary_distance.assign(vertices_.size(), outside_halo);
    plan.core_face_counts.assign(partition_count, 0);

    Float3 bounds_minimum;
    Float3 bounds_maximum;
    bool have_bounds = false;
    std::size_t alive_vertices = 0;
    for (const Vertex &vertex : vertices_) {
      if (!vertex.alive) {
        continue;
      }
      ++alive_vertices;
      if (!have_bounds) {
        bounds_minimum = vertex.position;
        bounds_maximum = vertex.position;
        have_bounds = true;
        continue;
      }
      bounds_minimum.x = std::min(bounds_minimum.x, vertex.position.x);
      bounds_minimum.y = std::min(bounds_minimum.y, vertex.position.y);
      bounds_minimum.z = std::min(bounds_minimum.z, vertex.position.z);
      bounds_maximum.x = std::max(bounds_maximum.x, vertex.position.x);
      bounds_maximum.y = std::max(bounds_maximum.y, vertex.position.y);
      bounds_maximum.z = std::max(bounds_maximum.z, vertex.position.z);
    }

    std::fill(
        topology_neighbor_stamps_.begin(), topology_neighbor_stamps_.end(), 0);
    for (const Face &face : faces_) {
      if (!face.alive) {
        continue;
      }
      for (const VertexId vertex : face.vertices) {
        if (topology_neighbor_stamps_[vertex] ==
            std::numeric_limits<std::uint32_t>::max())
        {
          throw std::runtime_error("vertex face-corner degree exceeds uint32 capacity");
        }
        ++topology_neighbor_stamps_[vertex];
      }
    }
    std::vector<MortonVertex> morton_vertices;
    morton_vertices.reserve(alive_vertices);
    std::uint64_t total_face_corner_load = 0;
    for (VertexId vertex_id = 0; vertex_id < vertices_.size(); ++vertex_id) {
      const Vertex &vertex = vertices_[vertex_id];
      if (!vertex.alive) {
        continue;
      }
      const std::uint32_t degree = topology_neighbor_stamps_[vertex_id];
      morton_vertices.push_back({
          morton_code_21(vertex.position, bounds_minimum, bounds_maximum),
          vertex_id,
          degree,
      });
      total_face_corner_load += degree;
    }
    stable_radix_sort_morton(morton_vertices);
    std::fill(
        topology_neighbor_stamps_.begin(), topology_neighbor_stamps_.end(), 0);

    std::uint64_t load_prefix = 0;
    for (std::size_t index = 0; index < morton_vertices.size(); ++index) {
      const MortonVertex &entry = morton_vertices[index];
      std::size_t partition = 0;
      if (total_face_corner_load != 0) {
        const std::uint64_t midpoint_twice =
            load_prefix * 2 + entry.face_corner_degree;
        partition = static_cast<std::size_t>(
            midpoint_twice * partition_count / (total_face_corner_load * 2));
        partition = std::min(partition, partition_count - 1);
      }
      else {
        partition = index * partition_count /
                    std::max<std::size_t>(1, morton_vertices.size());
      }
      plan.owner[entry.vertex] = static_cast<std::uint16_t>(partition);
      load_prefix += entry.face_corner_degree;
    }

    for (const Edge &edge : edges_) {
      if (!edge.alive() || edge.radial_head == kInvalidLoopId) {
        continue;
      }
      if (plan.owner[edge.first] != plan.owner[edge.second]) {
        plan.boundary_distance[edge.first] = 0;
        plan.boundary_distance[edge.second] = 0;
      }
    }
    for (std::uint8_t ring = 1; ring <= 4; ++ring) {
      for (const Edge &edge : edges_) {
        if (!edge.alive() || edge.radial_head == kInvalidLoopId) {
          continue;
        }
        const bool first_frontier =
            plan.boundary_distance[edge.first] == ring - 1;
        const bool second_frontier =
            plan.boundary_distance[edge.second] == ring - 1;
        if (first_frontier && plan.boundary_distance[edge.second] > ring) {
          plan.boundary_distance[edge.second] = ring;
        }
        if (second_frontier && plan.boundary_distance[edge.first] > ring) {
          plan.boundary_distance[edge.first] = ring;
        }
      }
    }

    for (const Face &face : faces_) {
      if (!face.alive) {
        continue;
      }
      const std::uint16_t owner = plan.owner[face.vertices[0]];
      if (owner != invalid_owner &&
          plan.owner[face.vertices[1]] == owner &&
          plan.owner[face.vertices[2]] == owner &&
          plan.boundary_distance[face.vertices[0]] > 2 &&
          plan.boundary_distance[face.vertices[1]] > 2 &&
          plan.boundary_distance[face.vertices[2]] > 2)
      {
        ++plan.core_face_counts[owner];
      }
    }

    plan.eligible_edge_offsets.assign(partition_count + 1, 0);
    for (const Edge &edge : edges_) {
      if (!edge.alive() || edge.radial_head == kInvalidLoopId) {
        continue;
      }
      const std::uint16_t owner = plan.owner[edge.first];
      if (owner != invalid_owner && plan.owner[edge.second] == owner &&
          plan.boundary_distance[edge.first] > 4 &&
          plan.boundary_distance[edge.second] > 4)
      {
        ++plan.eligible_edge_offsets[static_cast<std::size_t>(owner) + 1];
      }
    }
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
      plan.eligible_edge_offsets[partition + 1] +=
          plan.eligible_edge_offsets[partition];
    }
    plan.eligible_edges.resize(plan.eligible_edge_offsets.back());
    std::vector<std::size_t> write_offsets = plan.eligible_edge_offsets;
    for (EdgeId edge_id = 0; edge_id < edges_.size(); ++edge_id) {
      const Edge &edge = edges_[edge_id];
      if (!edge.alive() || edge.radial_head == kInvalidLoopId) {
        continue;
      }
      const std::uint16_t owner = plan.owner[edge.first];
      if (owner != invalid_owner && plan.owner[edge.second] == owner &&
          plan.boundary_distance[edge.first] > 4 &&
          plan.boundary_distance[edge.second] > 4)
      {
        plan.eligible_edges[write_offsets[owner]++] = edge_id;
      }
    }
    return plan;
  }

  std::vector<std::size_t> allocate_partition_removals(
      const PartitionPlan &plan,
      const std::size_t total_removals) const
  {
    std::vector<std::size_t> result(plan.count, 0);
    std::size_t total_weight = 0;
    for (const std::size_t weight : plan.core_face_counts) {
      total_weight += weight;
    }
    if (total_weight == 0 || total_removals == 0) {
      return result;
    }

    struct Remainder {
      std::uint64_t value = 0;
      std::size_t partition = 0;
    };
    std::vector<Remainder> remainders;
    remainders.reserve(plan.count);
    std::size_t allocated = 0;
    for (std::size_t partition = 0; partition < plan.count; ++partition) {
      const std::uint64_t product =
          static_cast<std::uint64_t>(total_removals) *
          static_cast<std::uint64_t>(plan.core_face_counts[partition]);
      result[partition] = static_cast<std::size_t>(product / total_weight);
      allocated += result[partition];
      remainders.push_back({
          product % static_cast<std::uint64_t>(total_weight),
          partition,
      });
    }
    std::sort(
        remainders.begin(),
        remainders.end(),
        [](const Remainder &left, const Remainder &right) {
          if (left.value != right.value) {
            return left.value > right.value;
          }
          return left.partition < right.partition;
        });
    for (std::size_t index = 0;
         allocated < total_removals && index < remainders.size();
         ++index, ++allocated)
    {
      ++result[remainders[index].partition];
    }
    return result;
  }

  bool local_vertex_allowed(const VertexId vertex,
                            const WorkerContext &worker) const noexcept
  {
    return worker.local_owner != nullptr &&
           worker.local_boundary_distance != nullptr &&
           vertex < worker.local_owner->size() &&
           (*worker.local_owner)[vertex] == worker.active_partition &&
           (*worker.local_boundary_distance)[vertex] > 2;
  }

  bool local_edge_static_allowed(const EdgeId edge_id,
                                 const WorkerContext &worker) const noexcept
  {
    if (worker.local_owner == nullptr) {
      return true;
    }
    if (edge_id >= edges_.size()) {
      return false;
    }
    const Edge &edge = edges_[edge_id];
    return edge.alive() &&
           (*worker.local_owner)[edge.first] == worker.active_partition &&
           (*worker.local_owner)[edge.second] == worker.active_partition &&
           (*worker.local_boundary_distance)[edge.first] > 4 &&
           (*worker.local_boundary_distance)[edge.second] > 4;
  }

  bool local_edge_dynamic_allowed(const EdgeId edge_id,
                                  const WorkerContext &worker) const
  {
    if (worker.local_owner == nullptr) {
      return true;
    }
    if (!local_edge_static_allowed(edge_id, worker)) {
      return false;
    }
    const Edge &edge = edges_[edge_id];
    for (const VertexId endpoint : {edge.first, edge.second}) {
      if (!local_vertex_allowed(endpoint, worker)) {
        return false;
      }
      const EdgeId disk_head = vertices_[endpoint].disk_head;
      if (disk_head == kInvalidEdgeId) {
        return false;
      }
      EdgeId incident_id = disk_head;
      do {
        const Edge &incident = edges_[incident_id];
        const VertexId neighbor =
            incident.first == endpoint ? incident.second : incident.first;
        if (!local_vertex_allowed(neighbor, worker)) {
          return false;
        }
        const EdgeId neighbor_head = vertices_[neighbor].disk_head;
        if (neighbor_head == kInvalidEdgeId) {
          return false;
        }
        EdgeId neighbor_edge_id = neighbor_head;
        do {
          const Edge &neighbor_edge = edges_[neighbor_edge_id];
          const VertexId second_neighbor =
              neighbor_edge.first == neighbor ?
                  neighbor_edge.second :
                  neighbor_edge.first;
          if (!local_vertex_allowed(second_neighbor, worker)) {
            return false;
          }
          neighbor_edge_id = disk_edge_next(neighbor_edge_id, neighbor);
        } while (neighbor_edge_id != neighbor_head);
        incident_id = disk_edge_next(incident_id, endpoint);
      } while (incident_id != disk_head);
    }
    return true;
  }

  std::size_t collapse_from_active_heap(WorkerContext &worker,
                                        const std::size_t target_faces,
                                        const std::size_t removal_limit,
                                        TraceWriter *trace)
  {
    const std::size_t start_removed_faces = worker.removed_faces;
    const bool local = worker.local_owner != nullptr;
    while ((local || active_faces_ > target_faces) &&
           worker.removed_faces - start_removed_faces < removal_limit &&
           !worker.heap.empty())
    {
      const IndexedMinHeap<EdgeId>::Entry candidate = worker.heap.pop();
      if (candidate.key >= kInvalidCost) {
        if (trace != nullptr) {
          trace->event("stopped", [&](std::ostream &stream) {
            stream << ",\"reason\":\"no_valid_edges\",\"faces\":" << active_faces_;
          });
        }
        break;
      }
      if (candidate.value >= edges_.size() || !edges_[candidate.value].alive()) {
        continue;
      }

      Edge &edge = edges_[candidate.value];
      const std::size_t faces_removed = edge_face_count(candidate.value);
      if (faces_removed == 0 || faces_removed > 2) {
        ++worker.invalid_edges;
        set_invalid_edge(candidate.value, worker);
        continue;
      }
      const std::size_t removed_so_far =
          worker.removed_faces - start_removed_faces;
      if (faces_removed > removal_limit - std::min(removal_limit, removed_so_far)) {
        continue;
      }
      if (!local_edge_dynamic_allowed(candidate.value, worker)) {
        continue;
      }

      const Vec3 target = calculate_collapse_target(edge);
      if (trace != nullptr) {
        trace->event("candidate", [&](std::ostream &stream) {
          stream << ",\"edge\":" << candidate.value
                 << ",\"v_keep\":" << edge.first
                 << ",\"v_remove\":" << edge.second
                 << ",\"cost\":" << candidate.key
                 << ",\"target\":[" << target.x << ',' << target.y << ',' << target.z << ']'
                 << ",\"faces_before\":" << active_faces_;
        });
      }

      if (collapse_has_degenerate_topology(candidate.value, worker)) {
        ++worker.rejected_topology;
        set_invalid_edge(candidate.value, worker);
        if (trace != nullptr) {
          trace->event("reject", [&](std::ostream &stream) {
            stream << ",\"edge\":" << candidate.value
                   << ",\"reason\":\"degenerate_topology\"";
          });
        }
        continue;
      }
      if (collapse_has_flip(candidate.value, target, worker)) {
        ++worker.rejected_flip;
        set_invalid_edge(candidate.value, worker);
        if (trace != nullptr) {
          trace->event("reject", [&](std::ostream &stream) {
            stream << ",\"edge\":" << candidate.value
                   << ",\"reason\":\"degenerate_flip\"";
          });
        }
        continue;
      }

      collapse_edge(candidate.value, target, worker);
      ++worker.collapsed_edges;
      if (!local) {
        active_faces_ -= faces_removed;
      }
      if (trace != nullptr) {
        trace->event("collapse", [&](std::ostream &stream) {
          stream << ",\"edge\":" << candidate.value
                 << ",\"removed_faces\":" << faces_removed
                 << ",\"faces_after\":" << active_faces_;
        });
      }
    }
    return worker.removed_faces - start_removed_faces;
  }

  void run_partition_local_stage(const DecimatorOptions &options,
                                 TraceWriter *trace)
  {
    stats_.partition_local_count = options.partition_local_count;
    stats_.partition_local_target_faces = options.partition_local_target_faces;
    const auto plan_start = std::chrono::steady_clock::now();
    PartitionPlan plan = build_partition_plan(options.partition_local_count);
    const std::size_t total_removals =
        active_faces_ > options.partition_local_target_faces ?
            active_faces_ - options.partition_local_target_faces :
            0;
    const std::vector<std::size_t> quotas =
        allocate_partition_removals(plan, total_removals);
    stats_.partition_local_plan_seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - plan_start)
            .count();

    global_worker_.heap.clear();
    const unsigned worker_count = static_cast<unsigned>(
        std::min<std::size_t>(std::max(1U, options.threads), plan.count));
    stats_.partition_local_workers = worker_count;
    std::size_t max_partition_edges = 0;
    for (std::size_t partition = 0; partition < plan.count; ++partition) {
      max_partition_edges = std::max(
          max_partition_edges,
          plan.eligible_edge_offsets[partition + 1] -
              plan.eligible_edge_offsets[partition]);
    }

    using WorkerPtr = std::unique_ptr<WorkerContext>;
    std::vector<WorkerPtr> workers;
    workers.reserve(worker_count);
    IndexedMinHeap<EdgeId>::PositionStorage &shared_positions =
        global_worker_.heap.position_storage();
    for (unsigned worker_index = 0; worker_index < worker_count; ++worker_index) {
      workers.push_back(std::make_unique<WorkerContext>(
          shared_positions,
          static_cast<IndexedMinHeap<EdgeId>::Position>(worker_index),
          static_cast<IndexedMinHeap<EdgeId>::Position>(worker_count)));
      workers.back()->heap.reserve_entries(max_partition_edges);
      workers.back()->local_owner = &plan.owner;
      workers.back()->local_boundary_distance = &plan.boundary_distance;
    }

    std::atomic<std::size_t> next_partition{0};
    std::atomic<bool> stop{false};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const auto parallel_start = std::chrono::steady_clock::now();
    auto run_worker = [&](WorkerContext &worker) {
      const auto worker_start = std::chrono::steady_clock::now();
      try {
        while (!stop.load(std::memory_order_relaxed)) {
          const std::size_t partition =
              next_partition.fetch_add(1, std::memory_order_relaxed);
          if (partition >= plan.count) {
            break;
          }
          worker.active_partition = static_cast<std::uint16_t>(partition);
          const auto build_start = std::chrono::steady_clock::now();
          for (std::size_t index = plan.eligible_edge_offsets[partition];
               index < plan.eligible_edge_offsets[partition + 1];
               ++index)
          {
            update_edge_cost(plan.eligible_edges[index], worker);
          }
          worker.heap_entries += worker.heap.size();
          worker.heap_build_seconds +=
              std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - build_start)
                  .count();

          const auto collapse_start = std::chrono::steady_clock::now();
          const std::size_t removed = collapse_from_active_heap(
              worker,
              options.partition_local_target_faces,
              quotas[partition],
              worker_count == 1 ? trace : nullptr);
          worker.collapse_seconds +=
              std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - collapse_start)
                  .count();
          if (removed < quotas[partition]) {
            ++worker.stalled_partitions;
          }
          worker.heap.clear();
        }
      }
      catch (...) {
        stop.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(failure_mutex);
        if (failure == nullptr) {
          failure = std::current_exception();
        }
      }
      worker.worker_seconds =
          std::chrono::duration<double>(
              std::chrono::steady_clock::now() - worker_start)
              .count();
    };

    std::vector<std::thread> threads;
    threads.reserve(worker_count > 0 ? worker_count - 1 : 0);
    try {
      for (unsigned worker_index = 1; worker_index < worker_count; ++worker_index) {
        threads.emplace_back(run_worker, std::ref(*workers[worker_index]));
      }
    }
    catch (...) {
      stop.store(true, std::memory_order_relaxed);
      for (std::thread &thread : threads) {
        thread.join();
      }
      throw;
    }
    run_worker(*workers.front());
    for (std::thread &thread : threads) {
      thread.join();
    }
    stats_.partition_local_parallel_seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - parallel_start)
            .count();
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }

    std::size_t total_removed_faces = 0;
    for (const WorkerPtr &worker : workers) {
      total_removed_faces += worker->removed_faces;
      stats_.collapsed_edges += worker->collapsed_edges;
      stats_.rejected_topology += worker->rejected_topology;
      stats_.rejected_flip += worker->rejected_flip;
      stats_.invalid_edges += worker->invalid_edges;
      stats_.partition_local_stalled_count += worker->stalled_partitions;
      stats_.partition_local_heap_entries += worker->heap_entries;
      stats_.partition_local_heap_build_seconds += worker->heap_build_seconds;
      stats_.partition_local_collapse_seconds += worker->collapse_seconds;
      stats_.partition_local_worker_seconds += worker->worker_seconds;
      stats_.partition_local_worker_max_seconds = std::max(
          stats_.partition_local_worker_max_seconds, worker->worker_seconds);
    }
    if (total_removed_faces > active_faces_) {
      throw std::runtime_error("partition-local removed-face count overflow");
    }
    active_faces_ -= total_removed_faces;
    stats_.partition_local_output_faces = active_faces_;
    stats_.partition_local_collapsed_edges = stats_.collapsed_edges;

    std::fill(
        topology_neighbor_stamps_.begin(), topology_neighbor_stamps_.end(), 0);
    global_worker_.topology_stamp = 0;
    const auto rebuild_start = std::chrono::steady_clock::now();
    for (EdgeId edge_id = 0; edge_id < edges_.size(); ++edge_id) {
      update_edge_cost(edge_id, global_worker_);
    }
    stats_.global_heap_rebuild_seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - rebuild_start)
            .count();
  }

  DecimatorStats decimate(const DecimatorOptions &options)
  {
    if (options.target_faces > active_faces_) {
      throw std::runtime_error("--target-faces cannot exceed the input face count");
    }
    TraceWriter trace(options.trace_path);
    trace.event("header", [&](std::ostream &stream) {
      stream << ",\"input_vertices\":" << input_vertices_ << ",\"input_faces\":" << input_faces_
             << ",\"target_faces\":" << options.target_faces
             << ",\"boundary_weight\":" << kBoundaryWeight
             << ",\"optimize_epsilon\":" << kOptimizeEpsilon
             << ",\"topology_fallback_epsilon\":" << kTopologyFallbackEpsilon;
    });

    if (options.partition_local_count != 0 &&
        active_faces_ > options.partition_local_target_faces)
    {
      run_partition_local_stage(options, &trace);
    }
    stats_.global_cleanup_input_faces = active_faces_;
    const std::size_t cleanup_start_collapses = global_worker_.collapsed_edges;
    const auto cleanup_start = std::chrono::steady_clock::now();
    collapse_from_active_heap(
        global_worker_,
        options.target_faces,
        std::numeric_limits<std::size_t>::max(),
        &trace);
    stats_.global_cleanup_seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - cleanup_start)
            .count();
    stats_.global_cleanup_collapsed_edges =
        global_worker_.collapsed_edges - cleanup_start_collapses;
    stats_.collapsed_edges += global_worker_.collapsed_edges;
    stats_.rejected_topology += global_worker_.rejected_topology;
    stats_.rejected_flip += global_worker_.rejected_flip;
    stats_.invalid_edges += global_worker_.invalid_edges;

    stats_.input_vertices = input_vertices_;
    stats_.input_faces = input_faces_;
    stats_.output_faces = active_faces_;
    stats_.output_vertices = alive_vertex_count();
    stats_.target_reached = active_faces_ <= options.target_faces;
    trace.event("summary", [&](std::ostream &stream) {
      stream << ",\"output_vertices\":" << stats_.output_vertices
             << ",\"output_faces\":" << stats_.output_faces
             << ",\"collapsed_edges\":" << stats_.collapsed_edges
             << ",\"rejected_topology\":" << stats_.rejected_topology
             << ",\"rejected_flip\":" << stats_.rejected_flip
             << ",\"invalid_edges\":" << stats_.invalid_edges
             << ",\"target_reached\":" << (stats_.target_reached ? "true" : "false");
    });
    return stats_;
  }

  InputMesh compact_mesh() const
  {
    InputMesh result;
    std::vector<VertexId> remap(vertices_.size(), std::numeric_limits<VertexId>::max());
    for (VertexId vertex_id = 0; vertex_id < vertices_.size(); ++vertex_id) {
      if (!vertices_[vertex_id].alive || !vertex_has_faces(vertex_id)) {
        continue;
      }
      remap[vertex_id] = static_cast<VertexId>(result.vertices.size());
      result.vertices.push_back(vertices_[vertex_id].position);
    }

    result.faces.reserve(active_faces_);
    for (const Face &face : faces_) {
      if (!face.alive) {
        continue;
      }
      std::array<VertexId, 3> compact_face{};
      for (std::size_t corner = 0; corner < compact_face.size(); ++corner) {
        compact_face[corner] = remap[face.vertices[corner]];
        if (compact_face[corner] == std::numeric_limits<VertexId>::max()) {
          throw std::runtime_error("internal error: live face references a removed vertex");
        }
      }
      result.faces.push_back(compact_face);
    }
    return result;
  }

 private:
  Float3 calculate_face_normal(const Face &face) const noexcept
  {
    const Float3 first = to_float3(vertices_[face.vertices[0]].position);
    const Float3 second = to_float3(vertices_[face.vertices[1]].position);
    const Float3 third = to_float3(vertices_[face.vertices[2]].position);
    Float3 normal = cross_float3(subtract_float3(first, second),
                                 subtract_float3(second, third));
    normalize_float3(normal);
    return normal;
  }

  void build_vertex_normals()
  {
    std::vector<Float3> normals(vertices_.size());
    for (const Face &face : faces_) {
      if (!face.alive) {
        continue;
      }

      std::array<Float3, 3> positions;
      for (std::size_t corner = 0; corner < positions.size(); ++corner) {
        positions[corner] = to_float3(vertices_[face.vertices[corner]].position);
      }

      Float3 face_normal;
      Float3 previous = positions.back();
      for (const Float3 &current : positions) {
        face_normal.x +=
            (previous.y - current.y) * (previous.z + current.z);
        face_normal.y +=
            (previous.z - current.z) * (previous.x + current.x);
        face_normal.z +=
            (previous.x - current.x) * (previous.y + current.y);
        previous = current;
      }
      if (normalize_float3(face_normal) == 0.0f) {
        face_normal.z = 1.0f;
      }

      Float3 edge_previous = subtract_float3(positions[1], positions[2]);
      normalize_float3(edge_previous);
      const Float3 edge_end = edge_previous;
      Float3 current = positions[2];
      for (std::size_t next_index = 0, current_index = 2;
           next_index < positions.size();
           current_index = next_index++)
      {
        Float3 edge_next;
        if (next_index != 2) {
          edge_next = subtract_float3(current, positions[next_index]);
          normalize_float3(edge_next);
        }
        else {
          edge_next = edge_end;
        }

        const float factor = safe_acos_float(-dot_float3(edge_previous, edge_next));
        Float3 &normal = normals[face.vertices[current_index]];
        normal.x += face_normal.x * factor;
        normal.y += face_normal.y * factor;
        normal.z += face_normal.z * factor;
        current = positions[next_index];
        edge_previous = edge_next;
      }
    }

    for (VertexId vertex_id = 0; vertex_id < vertices_.size(); ++vertex_id) {
      Float3 &normal = normals[vertex_id];
      if (normalize_float3(normal) == 0.0f) {
        normal = to_float3(vertices_[vertex_id].position);
        normalize_float3(normal);
      }
      vertices_[vertex_id].normal = normal;
    }
  }

  void build_quadrics()
  {
    for (const Face &face : faces_) {
      Float3 center;
      for (const VertexId vertex : face.vertices) {
        const Float3 position = to_float3(vertices_[vertex].position);
        center.x += position.x;
        center.y += position.y;
        center.z += position.z;
      }
      const float center_scale = 1.0f / static_cast<float>(face.vertices.size());
      center.x *= center_scale;
      center.y *= center_scale;
      center.z *= center_scale;

      const Float3 face_normal = calculate_face_normal(face);
      const double normal_x = static_cast<double>(face_normal.x);
      const double normal_y = static_cast<double>(face_normal.y);
      const double normal_z = static_cast<double>(face_normal.z);
      const double plane_offset =
          -(normal_x * static_cast<double>(center.x) +
            normal_y * static_cast<double>(center.y) +
            normal_z * static_cast<double>(center.z));
      const Quadric quadric =
          Quadric::from_plane(normal_x, normal_y, normal_z, plane_offset);
      for (const VertexId vertex : face.vertices) {
        vertices_[vertex].quadric += quadric;
      }
    }

    for (EdgeId edge_id = 0; edge_id < edges_.size(); ++edge_id) {
      const Edge &edge = edges_[edge_id];
      if (!edge.alive() || edge_face_count(edge_id) != 1) {
        continue;
      }
      const Float3 face_normal =
          calculate_face_normal(faces_[loop_face(edge.radial_head)]);
      const Float3 first_position = to_float3(vertices_[edge.first].position);
      const Float3 second_position = to_float3(vertices_[edge.second].position);
      const Float3 edge_vector = subtract_float3(second_position, first_position);
      const Float3 edge_plane =
          cross_float3(edge_vector, face_normal);
      Vec3 boundary_normal = to_vec3(edge_plane);
      const double boundary_length = length(boundary_normal);
      if (!(boundary_length > static_cast<double>(FLT_EPSILON))) {
        continue;
      }
      boundary_normal *= 1.0 / boundary_length;
      const Float3 midpoint = {
          (first_position.x + second_position.x) * 0.5f,
          (first_position.y + second_position.y) * 0.5f,
          (first_position.z + second_position.z) * 0.5f,
      };
      const double boundary_offset =
          -(boundary_normal.x * static_cast<double>(midpoint.x) +
            boundary_normal.y * static_cast<double>(midpoint.y) +
            boundary_normal.z * static_cast<double>(midpoint.z));
      Quadric boundary = Quadric::from_plane(boundary_normal.x,
                                             boundary_normal.y,
                                             boundary_normal.z,
                                             boundary_offset);
      boundary *= kBoundaryWeight;
      vertices_[edge.first].quadric += boundary;
      vertices_[edge.second].quadric += boundary;
    }
  }

  DiskLink &disk_link(Edge &edge, const VertexId vertex)
  {
    if (edge.first == vertex) {
      return edge.first_disk;
    }
    if (edge.second == vertex) {
      return edge.second_disk;
    }
    throw std::runtime_error("internal error: disk vertex is not an edge endpoint");
  }

  const DiskLink &disk_link(const Edge &edge, const VertexId vertex) const
  {
    if (edge.first == vertex) {
      return edge.first_disk;
    }
    if (edge.second == vertex) {
      return edge.second_disk;
    }
    throw std::runtime_error("internal error: disk vertex is not an edge endpoint");
  }

  EdgeId disk_edge_next(const EdgeId edge_id, const VertexId vertex) const
  {
    return disk_link(edges_[edge_id], vertex).next;
  }

  std::size_t edge_face_count(const EdgeId edge_id) const noexcept
  {
    return edges_[edge_id].radial_count();
  }

  bool vertex_has_faces(const VertexId vertex) const noexcept
  {
    const EdgeId disk_head = vertices_[vertex].disk_head;
    if (disk_head == kInvalidEdgeId) {
      return false;
    }
    EdgeId edge_id = disk_head;
    do {
      const LoopId radial_head = edges_[edge_id].radial_head;
      if (radial_head != kInvalidLoopId && radial_contains_vertex(radial_head, vertex)) {
        return true;
      }
      edge_id = disk_edge_next(edge_id, vertex);
    } while (edge_id != disk_head);
    return false;
  }

  void disk_edge_append(const EdgeId edge_id, const VertexId vertex)
  {
    Vertex &disk_vertex = vertices_[vertex];
    DiskLink &link = disk_link(edges_[edge_id], vertex);
    if (disk_vertex.disk_head == kInvalidEdgeId) {
      disk_vertex.disk_head = edge_id;
      link.next = edge_id;
      link.previous = edge_id;
    }
    else {
      const EdgeId head_id = disk_vertex.disk_head;
      DiskLink &head_link = disk_link(edges_[head_id], vertex);
      const EdgeId tail_id = head_link.previous;
      DiskLink &tail_link = disk_link(edges_[tail_id], vertex);
      link.next = head_id;
      link.previous = tail_id;
      head_link.previous = edge_id;
      tail_link.next = edge_id;
    }
  }

  void disk_edge_remove(const EdgeId edge_id, const VertexId vertex)
  {
    Vertex &disk_vertex = vertices_[vertex];
    DiskLink &link = disk_link(edges_[edge_id], vertex);
    if (link.previous == kInvalidEdgeId || link.next == kInvalidEdgeId) {
      throw std::runtime_error("internal error: removing an edge outside the disk cycle");
    }
    disk_link(edges_[link.previous], vertex).next = link.next;
    disk_link(edges_[link.next], vertex).previous = link.previous;
    if (disk_vertex.disk_head == edge_id) {
      disk_vertex.disk_head = edge_id != link.next ? link.next : kInvalidEdgeId;
    }
    link.next = kInvalidEdgeId;
    link.previous = kInvalidEdgeId;
  }

  void radial_loop_append(const EdgeId edge_id, const LoopId loop_id)
  {
    Edge &edge = edges_[edge_id];
    Loop &loop = loops_[loop_id];
    if ((edge.radial_head == kInvalidLoopId) != (edge.radial_count() == 0)) {
      throw std::runtime_error("internal error: edge radial count is inconsistent");
    }
    if (edge.radial_head == kInvalidLoopId) {
      edge.radial_head = loop_id;
      loop.radial_next = loop_id;
      loop.radial_previous = loop_id;
    }
    else {
      const LoopId head_id = edge.radial_head;
      Loop &head = loops_[head_id];
      const LoopId next_id = head.radial_next;
      loop.radial_previous = head_id;
      loop.radial_next = next_id;
      loops_[next_id].radial_previous = loop_id;
      head.radial_next = loop_id;
      edge.radial_head = loop_id;
    }
    loop.edge = edge_id;
    edge.increment_radial_count();
  }

  void radial_loop_remove(const EdgeId edge_id, const LoopId loop_id)
  {
    Edge &edge = edges_[edge_id];
    Loop &loop = loops_[loop_id];
    if (edge.radial_count() == 0 || edge.radial_head == kInvalidLoopId) {
      throw std::runtime_error("internal error: removing from an empty radial cycle");
    }
    if (loop.radial_next != loop_id) {
      if (edge.radial_head == loop_id) {
        edge.radial_head = loop.radial_next;
      }
      loops_[loop.radial_next].radial_previous = loop.radial_previous;
      loops_[loop.radial_previous].radial_next = loop.radial_next;
    }
    else if (edge.radial_head == loop_id) {
      edge.radial_head = kInvalidLoopId;
    }
    else {
      throw std::runtime_error("internal error: radial loop head is inconsistent");
    }
    loop.radial_next = kInvalidLoopId;
    loop.radial_previous = kInvalidLoopId;
    edge.decrement_radial_count();
    if ((edge.radial_head == kInvalidLoopId) != (edge.radial_count() == 0)) {
      throw std::runtime_error("internal error: edge radial count is inconsistent");
    }
  }

  void kill_face(const FaceId face_id)
  {
    Face &face = faces_[face_id];
    LoopId loop_id = face_first_loop(face_id);
    for (std::size_t corner = 0; corner < face.vertices.size(); ++corner) {
      Loop &loop = loops_[loop_id];
      const LoopId next_id = loop_next(loop_id);
      radial_loop_remove(loop.edge, loop_id);
      loop_id = next_id;
    }
    face.alive = false;
  }

  void replace_vertex_in_edge_loops(const EdgeId edge_id,
                                    const VertexId destination,
                                    const VertexId source)
  {
    const LoopId radial_head = edges_[edge_id].radial_head;
    if (radial_head == kInvalidLoopId) {
      return;
    }
    LoopId loop_id = radial_head;
    do {
      const FaceId face_id = loop_face(loop_id);
      Face &face = faces_[face_id];
      const std::size_t corner = loop_corner(loop_id);
      if (face.vertices[corner] == source) {
        face.vertices[corner] = destination;
      }
      else {
        const LoopId next_id = loop_next(loop_id);
        const std::size_t next_corner = loop_corner(next_id);
        if (face.vertices[next_corner] == source) {
          face.vertices[next_corner] = destination;
        }
        else if (face.vertices[loop_corner(loop_previous(loop_id))] == source) {
          std::ostringstream message;
          message << "internal error: edge radial loop lost source vertex"
                  << " edge=" << edge_id << " face=" << face_id << " loop=" << loop_id
                  << " source=" << source << " destination=" << destination
                  << " previous_vertex="
                  << face.vertices[loop_corner(loop_previous(loop_id))]
                  << " loop_vertex=" << face.vertices[corner]
                  << " next_vertex=" << face.vertices[next_corner];
          throw std::runtime_error(message.str());
        }
      }
      loop_id = loops_[loop_id].radial_next;
    } while (loop_id != radial_head);
  }

  void splice_vertex(const VertexId destination,
                     const VertexId source,
                     const std::vector<std::pair<EdgeId, EdgeId>> &splice_edges)
  {
    while (vertices_[source].disk_head != kInvalidEdgeId) {
      const EdgeId edge_id = vertices_[source].disk_head;
      Edge &edge = edges_[edge_id];
      replace_vertex_in_edge_loops(edge_id, destination, source);
      disk_edge_remove(edge_id, source);
      if (edge.first == source) {
        edge.first = destination;
      }
      else if (edge.second == source) {
        edge.second = destination;
      }
      else {
        throw std::runtime_error("internal error: source disk contains a foreign edge");
      }

      EdgeId duplicate_id = kInvalidEdgeId;
      const EdgeId destination_head = vertices_[destination].disk_head;
      if (destination_head != kInvalidEdgeId) {
        EdgeId destination_edge_id = destination_head;
        do {
          const Edge &destination_edge = edges_[destination_edge_id];
          if (destination_edge.alive() &&
              edge_key(destination_edge.first, destination_edge.second) ==
                  edge_key(edge.first, edge.second))
          {
            duplicate_id = destination_edge_id;
            break;
          }
          destination_edge_id = disk_edge_next(destination_edge_id, destination);
        } while (destination_edge_id != destination_head);
      }
      if (duplicate_id != kInvalidEdgeId) {
        bool expected_duplicate = false;
        for (const std::pair<EdgeId, EdgeId> &splice : splice_edges) {
          if (splice.first == edge_id && splice.second == duplicate_id) {
            expected_duplicate = true;
            break;
          }
        }
        if (!expected_duplicate) {
          throw std::runtime_error("internal error: vertex splice created an unexpected edge");
        }
      }
      disk_edge_append(edge_id, destination);
    }
  }

  void splice_edge(const EdgeId destination,
                   const EdgeId source,
                   WorkerContext &worker)
  {
    Edge &source_edge = edges_[source];
    Edge &destination_edge = edges_[destination];
    if (edge_key(source_edge.first, source_edge.second) !=
        edge_key(destination_edge.first, destination_edge.second))
    {
      throw std::runtime_error("internal error: edge splice endpoints are inconsistent");
    }
    while (source_edge.radial_head != kInvalidLoopId) {
      const LoopId loop_id = source_edge.radial_head;
      radial_loop_remove(source, loop_id);
      radial_loop_append(destination, loop_id);
    }
    deactivate_edge(source, worker);
  }

  LoopId disk_faceloop_find_first(const VertexId vertex) const
  {
    const EdgeId disk_head = vertices_[vertex].disk_head;
    if (disk_head == kInvalidEdgeId) {
      return kInvalidLoopId;
    }
    EdgeId edge_id = disk_head;
    do {
      const LoopId radial_head = edges_[edge_id].radial_head;
      if (radial_head != kInvalidLoopId) {
        return faces_[loop_face(radial_head)].vertices[loop_corner(radial_head)] == vertex ?
                   radial_head :
                   loop_next(radial_head);
      }
      edge_id = disk_edge_next(edge_id, vertex);
    } while (edge_id != disk_head);
    return kInvalidLoopId;
  }

  bool radial_contains_vertex(const LoopId radial_head, const VertexId vertex) const
  {
    LoopId loop_id = radial_head;
    do {
      if (faces_[loop_face(loop_id)].vertices[loop_corner(loop_id)] == vertex) {
        return true;
      }
      loop_id = loops_[loop_id].radial_next;
    } while (loop_id != radial_head);
    return false;
  }

  LoopId radial_faceloop_find_first(const LoopId radial_head,
                                    const VertexId vertex) const
  {
    LoopId loop_id = radial_head;
    do {
      if (faces_[loop_face(loop_id)].vertices[loop_corner(loop_id)] == vertex) {
        return loop_id;
      }
      loop_id = loops_[loop_id].radial_next;
    } while (loop_id != radial_head);
    throw std::runtime_error("internal error: radial cycle does not contain vertex");
  }

  LoopId radial_faceloop_find_next(const LoopId loop_id, const VertexId vertex) const
  {
    LoopId next_id = loops_[loop_id].radial_next;
    while (next_id != loop_id) {
      if (faces_[loop_face(next_id)].vertices[loop_corner(next_id)] == vertex) {
        return next_id;
      }
      next_id = loops_[next_id].radial_next;
    }
    return loop_id;
  }

  EdgeId disk_faceedge_find_next(const EdgeId edge_id, const VertexId vertex) const
  {
    EdgeId next_id = disk_edge_next(edge_id, vertex);
    while (next_id != edge_id) {
      const LoopId radial_head = edges_[next_id].radial_head;
      if (radial_head != kInvalidLoopId && radial_contains_vertex(radial_head, vertex)) {
        return next_id;
      }
      next_id = disk_edge_next(next_id, vertex);
    }
    return edge_id;
  }

  std::size_t vertex_face_count(const VertexId vertex) const noexcept
  {
    const EdgeId disk_head = vertices_[vertex].disk_head;
    if (disk_head == kInvalidEdgeId) {
      return 0;
    }
    std::size_t result = 0;
    EdgeId edge_id = disk_head;
    do {
      const LoopId radial_head = edges_[edge_id].radial_head;
      if (radial_head != kInvalidLoopId) {
        LoopId loop_id = radial_head;
        do {
          result +=
              faces_[loop_face(loop_id)].vertices[loop_corner(loop_id)] == vertex ? 1 : 0;
          loop_id = loops_[loop_id].radial_next;
        } while (loop_id != radial_head);
      }
      edge_id = disk_edge_next(edge_id, vertex);
    } while (edge_id != disk_head);
    return result;
  }

  void collect_loops_of_vertex(const VertexId vertex, std::vector<LoopId> &result) const
  {
    result.clear();
    const std::size_t face_count = vertex_face_count(vertex);
    result.reserve(face_count);
    LoopId loop_first = disk_faceloop_find_first(vertex);
    if (loop_first == kInvalidLoopId) {
      return;
    }
    EdgeId edge_next = loops_[loop_first].edge;
    LoopId loop_next = loop_first;
    while (result.size() < face_count) {
      const LoopId loop_current = loop_next;
      result.push_back(loop_current);
      loop_next = radial_faceloop_find_next(loop_next, vertex);
      if (loop_next == loop_first) {
        edge_next = disk_faceedge_find_next(edge_next, vertex);
        loop_first = radial_faceloop_find_first(edges_[edge_next].radial_head, vertex);
        loop_next = loop_first;
      }
    }
  }

  void build_initial_edges_like_blender()
  {
    constexpr std::size_t kParallelMapThreshold = 1000;
    constexpr std::size_t kParallelMapCount = 8;
    const std::size_t map_count =
        faces_.size() < kParallelMapThreshold ? 1 : kParallelMapCount;
    const std::size_t map_mask = map_count - 1;
    if (faces_.size() > std::numeric_limits<std::size_t>::max() / 3) {
      throw std::runtime_error("initial edge map capacity overflow");
    }
    std::size_t unique_edge_guess = 0;
    if (memory_mode_ == MemoryMode::Low) {
      const std::size_t edge_references = faces_.size() * 3;
      const std::size_t shard_divisor = 2 * map_count;
      unique_edge_guess =
          edge_references / shard_divisor +
          (edge_references % shard_divisor != 0 ? 1 : 0);
    }
    else {
      if (faces_.size() > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::runtime_error("initial edge map capacity overflow");
      }
      unique_edge_guess = faces_.size() * 2 / map_count;
    }
    std::vector<BlenderEdgeMap> edge_maps;
    edge_maps.reserve(map_count);
    for (std::size_t map_index = 0; map_index < map_count; ++map_index) {
      edge_maps.emplace_back(unique_edge_guess);
    }

    EdgeId temporary_edge_count = 0;
    for (FaceId face_id = 0; face_id < faces_.size(); ++face_id) {
      const Face &face = faces_[face_id];
      VertexId previous = face.vertices.back();
      std::size_t previous_corner = face.vertices.size() - 1;
      for (std::size_t current_corner = 0;
           current_corner < face.vertices.size();
           ++current_corner)
      {
        const VertexId current = face.vertices[current_corner];
        if (previous != current) {
          const VertexId low = std::min(previous, current);
          const std::size_t map_index = static_cast<std::size_t>(low) & map_mask;
          const BlenderEdgeMap::LookupResult result =
              edge_maps[map_index].lookup_or_add(
                  previous, current, temporary_edge_count);
          loops_[face_first_loop(face_id) +
                 static_cast<LoopId>(previous_corner)]
              .edge = result.edge_id;
          if (result.inserted) {
            if (temporary_edge_count == kInvalidEdgeId - 1) {
              throw std::runtime_error("mesh exceeds stable 32-bit edge ID capacity");
            }
            ++temporary_edge_count;
          }
        }
        previous = current;
        previous_corner = current_corner;
      }
    }

    std::size_t edge_count = 0;
    for (const BlenderEdgeMap &edge_map : edge_maps) {
      if (edge_map.size() > std::numeric_limits<std::size_t>::max() - edge_count) {
        throw std::runtime_error("initial edge count overflow");
      }
      edge_count += edge_map.size();
    }
    if (edge_count > std::numeric_limits<EdgeId>::max()) {
      throw std::runtime_error("mesh exceeds stable 32-bit edge ID capacity");
    }
    if (edge_count != temporary_edge_count) {
      throw std::runtime_error("internal error: temporary edge count is inconsistent");
    }
    std::vector<EdgeId> temporary_to_final(edge_count, kInvalidEdgeId);
    edges_.reserve(edge_count);
    for (const BlenderEdgeMap &edge_map : edge_maps) {
      edge_map.serialize_edges([&](const OrderedEdge &ordered_edge,
                                   const EdgeId temporary_edge_id) {
        const EdgeId edge_id = static_cast<EdgeId>(edges_.size());
        temporary_to_final[temporary_edge_id] = edge_id;
        Edge edge;
        edge.first = ordered_edge.low;
        edge.second = ordered_edge.high;
        edges_.push_back(std::move(edge));
        disk_edge_append(edge_id, ordered_edge.low);
        disk_edge_append(edge_id, ordered_edge.high);
      });
    }
    for (Loop &loop : loops_) {
      if (loop.edge == kInvalidEdgeId ||
          loop.edge >= temporary_to_final.size() ||
          temporary_to_final[loop.edge] == kInvalidEdgeId)
      {
        throw std::runtime_error("internal error: initial Blender edge map lost a face edge");
      }
      loop.edge = temporary_to_final[loop.edge];
    }
  }

  void build_initial_edges_parallel_like_blender()
  {
    constexpr std::size_t kMapCount = 8;
    constexpr std::size_t kMapMask = kMapCount - 1;
    if (faces_.size() > std::numeric_limits<std::size_t>::max() / 2) {
      throw std::runtime_error("initial edge map capacity overflow");
    }
    const std::size_t unique_edge_guess =
        memory_mode_ == MemoryMode::Low ?
            (faces_.size() * 3 + 2 * kMapCount - 1) / (2 * kMapCount) :
            faces_.size() * 2 / kMapCount;
    std::vector<BlenderEdgeMap> edge_maps;
    edge_maps.reserve(kMapCount);
    for (std::size_t map_index = 0; map_index < kMapCount; ++map_index) {
      edge_maps.emplace_back(unique_edge_guess);
    }
    std::array<EdgeId, kMapCount> temporary_edge_counts{};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const auto build_map = [&](const std::size_t map_index) {
      try {
        EdgeId &temporary_edge_count = temporary_edge_counts[map_index];
        BlenderEdgeMap &edge_map = edge_maps[map_index];
        for (FaceId face_id = 0; face_id < faces_.size(); ++face_id) {
          const Face &face = faces_[face_id];
          VertexId previous = face.vertices.back();
          std::size_t previous_corner = face.vertices.size() - 1;
          for (std::size_t current_corner = 0;
               current_corner < face.vertices.size();
               ++current_corner)
          {
            const VertexId current = face.vertices[current_corner];
            if (previous != current) {
              const VertexId low = std::min(previous, current);
              if ((static_cast<std::size_t>(low) & kMapMask) == map_index) {
                const BlenderEdgeMap::LookupResult result =
                    edge_map.lookup_or_add(previous, current, temporary_edge_count);
                loops_[face_first_loop(face_id) +
                       static_cast<LoopId>(previous_corner)]
                    .edge = result.edge_id;
                if (result.inserted) {
                  if (temporary_edge_count == kInvalidEdgeId - 1) {
                    throw std::runtime_error(
                        "mesh exceeds stable 32-bit edge ID capacity");
                  }
                  ++temporary_edge_count;
                }
              }
            }
            previous = current;
            previous_corner = current_corner;
          }
        }
      }
      catch (...) {
        std::lock_guard<std::mutex> lock(failure_mutex);
        if (failure == nullptr) {
          failure = std::current_exception();
        }
      }
    };

    std::vector<std::thread> threads;
    threads.reserve(kMapCount - 1);
    try {
      for (std::size_t map_index = 1; map_index < kMapCount; ++map_index) {
        threads.emplace_back(build_map, map_index);
      }
    }
    catch (...) {
      for (std::thread &thread : threads) {
        thread.join();
      }
      throw;
    }
    build_map(0);
    for (std::thread &thread : threads) {
      thread.join();
    }
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }

    std::size_t edge_count = 0;
    for (const BlenderEdgeMap &edge_map : edge_maps) {
      if (edge_map.size() > std::numeric_limits<std::size_t>::max() - edge_count) {
        throw std::runtime_error("initial edge count overflow");
      }
      edge_count += edge_map.size();
    }
    if (edge_count > std::numeric_limits<EdgeId>::max()) {
      throw std::runtime_error("mesh exceeds stable 32-bit edge ID capacity");
    }
    std::array<std::vector<EdgeId>, kMapCount> temporary_to_final;
    edges_.reserve(edge_count);
    for (std::size_t map_index = 0; map_index < kMapCount; ++map_index) {
      temporary_to_final[map_index].assign(
          temporary_edge_counts[map_index], kInvalidEdgeId);
      edge_maps[map_index].serialize_edges(
          [&](const OrderedEdge &ordered_edge, const EdgeId temporary_edge_id) {
            const EdgeId edge_id = static_cast<EdgeId>(edges_.size());
            temporary_to_final[map_index][temporary_edge_id] = edge_id;
            Edge edge;
            edge.first = ordered_edge.low;
            edge.second = ordered_edge.high;
            edges_.push_back(std::move(edge));
            disk_edge_append(edge_id, ordered_edge.low);
            disk_edge_append(edge_id, ordered_edge.high);
          });
    }
    for (FaceId face_id = 0; face_id < faces_.size(); ++face_id) {
      const Face &face = faces_[face_id];
      for (std::size_t corner = 0; corner < face.vertices.size(); ++corner) {
        const VertexId low =
            std::min(face.vertices[corner],
                     face.vertices[(corner + 1) % face.vertices.size()]);
        const std::size_t map_index = static_cast<std::size_t>(low) & kMapMask;
        Loop &loop = loops_[face_first_loop(face_id) + static_cast<LoopId>(corner)];
        if (loop.edge == kInvalidEdgeId ||
            loop.edge >= temporary_to_final[map_index].size() ||
            temporary_to_final[map_index][loop.edge] == kInvalidEdgeId)
        {
          throw std::runtime_error(
              "internal error: parallel Blender edge map lost a face edge");
        }
        loop.edge = temporary_to_final[map_index][loop.edge];
      }
    }
  }

  void attach_face_to_initial_edges(const FaceId face_id)
  {
    const LoopId first_loop = face_first_loop(face_id);
    for (std::size_t corner = 0; corner < faces_[face_id].vertices.size(); ++corner) {
      const LoopId loop_id = first_loop + static_cast<LoopId>(corner);
      radial_loop_append(loops_[loop_id].edge, loop_id);
    }
  }

  void deactivate_edge(const EdgeId edge_id, WorkerContext &worker)
  {
    Edge &edge = edges_[edge_id];
    if (!edge.alive()) {
      return;
    }
    worker.heap.remove(edge_id);
    disk_edge_remove(edge_id, edge.first);
    disk_edge_remove(edge_id, edge.second);
    if (edge.radial_count() != 0 || edge.radial_head != kInvalidLoopId) {
      throw std::runtime_error("internal error: deactivating an edge with radial users");
    }
    edge.radial_head = kInvalidLoopId;
    edge.deactivate();
  }

  Vec3 calculate_target(const Edge &edge) const noexcept
  {
    const Quadric combined =
        vertices_[edge.first].quadric + vertices_[edge.second].quadric;
    Vec3 target;
    if (combined.optimize(target, kOptimizeEpsilon)) {
      return target;
    }
    const Float3 &first = vertices_[edge.first].position;
    const Float3 &second = vertices_[edge.second].position;
    return {
        (static_cast<double>(first.x) + static_cast<double>(second.x)) * 0.5,
        (static_cast<double>(first.y) + static_cast<double>(second.y)) * 0.5,
        (static_cast<double>(first.z) + static_cast<double>(second.z)) * 0.5,
    };
  }

  Vec3 calculate_collapse_target(const Edge &edge) const noexcept
  {
    return to_vec3(to_float3(calculate_target(edge)));
  }

  float calculate_edge_cost(const EdgeId edge_id) const noexcept
  {
    const Edge &edge = edges_[edge_id];
    const std::size_t face_count = edge_face_count(edge_id);
    if (!edge.alive() || face_count == 0 || face_count > 2) {
      return static_cast<float>(kInvalidCost);
    }
    const Vec3 target = calculate_target(edge);
    float cost = static_cast<float>(vertices_[edge.first].quadric.evaluate(target) +
                                    vertices_[edge.second].quadric.evaluate(target));
    cost = std::fabs(cost);
    if (!std::isfinite(cost)) {
      return static_cast<float>(kInvalidCost);
    }
    if (cost < static_cast<float>(kTopologyFallbackEpsilon)) {
      const Float3 &first_position = vertices_[edge.first].position;
      const Float3 &second_position = vertices_[edge.second].position;
      const float delta_x = first_position.x - second_position.x;
      const float delta_y = first_position.y - second_position.y;
      const float delta_z = first_position.z - second_position.z;
      const float squared_edge_length =
          delta_x * delta_x + delta_y * delta_y + delta_z * delta_z;
      const float denominator = std::min(-squared_edge_length, -FLT_EPSILON);
      const Float3 &first_normal = vertices_[edge.first].normal;
      const Float3 &second_normal = vertices_[edge.second].normal;
      const float normal_alignment = std::fabs(
          first_normal.x * second_normal.x +
          first_normal.y * second_normal.y +
          first_normal.z * second_normal.z);
      cost = normal_alignment / denominator - cost;
    }
    return cost;
  }

  void update_edge_cost(const EdgeId edge_id, WorkerContext &worker)
  {
    if (edge_id >= edges_.size() || !edges_[edge_id].alive()) {
      if (worker.local_owner == nullptr) {
        worker.heap.remove(edge_id);
      }
      return;
    }
    if (!local_edge_static_allowed(edge_id, worker)) {
      return;
    }
    const std::size_t face_count = edge_face_count(edge_id);
    if (face_count == 0 || face_count > 2) {
      worker.heap.remove(edge_id);
      return;
    }
    const float cost = calculate_edge_cost(edge_id);
    if (cost >= kInvalidCost) {
      ++worker.invalid_edges;
    }
    worker.heap.update(edge_id, cost);
  }

  void set_invalid_edge(const EdgeId edge_id, WorkerContext &worker)
  {
    if (edge_id < edges_.size() && edges_[edge_id].alive()) {
      worker.heap.update(edge_id, static_cast<float>(kInvalidCost));
    }
  }

  bool collapse_has_degenerate_topology(const EdgeId edge_id,
                                        WorkerContext &worker)
  {
    const Edge &edge = edges_[edge_id];
    const std::size_t face_count = edge_face_count(edge_id);
    if (!edge.alive() || face_count == 0 || face_count > 2) {
      return true;
    }
    for (const VertexId endpoint : {edge.first, edge.second}) {
      const EdgeId disk_head = vertices_[endpoint].disk_head;
      if (disk_head == kInvalidEdgeId) {
        return true;
      }
      EdgeId incident_id = disk_head;
      do {
        const Edge &incident = edges_[incident_id];
        const std::size_t incident_face_count = edge_face_count(incident_id);
        if (!incident.alive() || incident_face_count == 0 || incident_face_count > 2) {
          return true;
        }
        incident_id = disk_edge_next(incident_id, endpoint);
      } while (incident_id != disk_head);
    }

    if (worker.topology_stamp > std::numeric_limits<std::uint32_t>::max() - 3) {
      throw std::runtime_error("partition-local topology stamp capacity exceeded");
    }
    const std::uint32_t first_neighbor_stamp = ++worker.topology_stamp;
    const std::uint32_t shared_neighbor_stamp = ++worker.topology_stamp;
    const std::uint32_t opposite_vertex_stamp = ++worker.topology_stamp;

    const EdgeId first_disk_head = vertices_[edge.first].disk_head;
    EdgeId first_incident_id = first_disk_head;
    do {
      const Edge &incident = edges_[first_incident_id];
      const VertexId neighbor =
          incident.first == edge.first ? incident.second : incident.first;
      if (neighbor != edge.second) {
        topology_neighbor_stamps_[neighbor] = first_neighbor_stamp;
      }
      first_incident_id = disk_edge_next(first_incident_id, edge.first);
    } while (first_incident_id != first_disk_head);

    std::size_t unmatched_shared_neighbors = 0;
    const EdgeId second_disk_head = vertices_[edge.second].disk_head;
    EdgeId second_incident_id = second_disk_head;
    do {
      const Edge &incident = edges_[second_incident_id];
      const VertexId neighbor =
          incident.first == edge.second ? incident.second : incident.first;
      std::uint32_t &neighbor_stamp = topology_neighbor_stamps_[neighbor];
      if (neighbor != edge.first && neighbor_stamp == first_neighbor_stamp) {
        neighbor_stamp = shared_neighbor_stamp;
        ++unmatched_shared_neighbors;
      }
      second_incident_id = disk_edge_next(second_incident_id, edge.second);
    } while (second_incident_id != second_disk_head);

    LoopId loop_id = edge.radial_head;
    do {
      const FaceId face_id = loop_face(loop_id);
      const Face &face = faces_[face_id];
      for (const VertexId vertex : face.vertices) {
        if (vertex != edge.first && vertex != edge.second) {
          std::uint32_t &vertex_stamp = topology_neighbor_stamps_[vertex];
          if (vertex_stamp == shared_neighbor_stamp) {
            vertex_stamp = opposite_vertex_stamp;
            --unmatched_shared_neighbors;
          }
          else if (vertex_stamp != opposite_vertex_stamp) {
            return true;
          }
        }
      }
      loop_id = loops_[loop_id].radial_next;
    } while (loop_id != edge.radial_head);
    return unmatched_shared_neighbors != 0;
  }

  bool collapse_has_flip(const EdgeId edge_id,
                         const Vec3 &target,
                         WorkerContext &worker)
  {
    const Edge &edge = edges_[edge_id];
    const Float3 optimized_position = to_float3(target);
    for (const VertexId endpoint : {edge.first, edge.second}) {
      collect_loops_of_vertex(endpoint, worker.loop_scratch);
      for (const LoopId loop_id : worker.loop_scratch) {
        const FaceId face_id = loop_face(loop_id);
        const Face &face = faces_[face_id];
        if (!face.alive ||
            (face_contains(face, edge.first) && face_contains(face, edge.second)))
        {
          continue;
        }
        const std::size_t index = face_vertex_index(face, endpoint);
        const Float3 previous =
            to_float3(vertices_[face.vertices[(index + 2) % 3]].position);
        const Float3 next =
            to_float3(vertices_[face.vertices[(index + 1) % 3]].position);
        const Float3 other = subtract_float3(previous, next);
        const Float3 existing =
            subtract_float3(previous, to_float3(vertices_[endpoint].position));
        const Float3 optimized = subtract_float3(previous, optimized_position);
        const Float3 existing_cross = cross_float3(other, existing);
        const Float3 optimized_cross = cross_float3(other, optimized);
        if (dot_float3(existing_cross, optimized_cross) <=
            (dot_float3(existing_cross, existing_cross) +
             dot_float3(optimized_cross, optimized_cross)) *
                static_cast<float>(kFlipThreshold))
        {
          return true;
        }
      }
    }
    return false;
  }

  void collapse_edge(const EdgeId edge_id,
                     const Vec3 &target,
                     WorkerContext &worker)
  {
    const Edge edge_before = edges_[edge_id];
    const VertexId keep = edge_before.first;
    const VertexId remove = edge_before.second;
    const Float3 keep_position = vertices_[keep].position;
    const Float3 remove_position = vertices_[remove].position;
    const Float3 optimized_position = to_float3(target);
    const Float3 keep_normal = vertices_[keep].normal;
    const Float3 remove_normal = vertices_[remove].normal;
    if (edge_before.radial_head == kInvalidLoopId) {
      throw std::runtime_error("internal error: collapse edge has no radial users");
    }

    float customdata_factor = 0.5f;
    if (!(std::fabs(keep_position.x - remove_position.x) <= FLT_EPSILON &&
          std::fabs(keep_position.y - remove_position.y) <= FLT_EPSILON &&
          std::fabs(keep_position.z - remove_position.z) <= FLT_EPSILON))
    {
      const Float3 direction = subtract_float3(remove_position, keep_position);
      const Float3 offset = subtract_float3(optimized_position, keep_position);
      customdata_factor = dot_float3(direction, offset) /
                          dot_float3(direction, direction);
    }

    worker.loop_scratch.clear();
    LoopId collapse_loop = edge_before.radial_head;
    do {
      worker.loop_scratch.push_back(collapse_loop);
      collapse_loop = loops_[collapse_loop].radial_next;
    } while (collapse_loop != edge_before.radial_head);

    worker.splice_edges_scratch.clear();
    worker.splice_edges_scratch.reserve(worker.loop_scratch.size());
    for (const LoopId loop_id : worker.loop_scratch) {
      const Face &face = faces_[loop_face(loop_id)];
      const Loop &previous = loops_[loop_previous(loop_id)];
      const Loop &next = loops_[loop_next(loop_id)];
      if (face.vertices[loop_corner(loop_id)] == remove) {
        worker.splice_edges_scratch.emplace_back(previous.edge, next.edge);
      }
      else {
        worker.splice_edges_scratch.emplace_back(next.edge, previous.edge);
      }
    }

    for (const LoopId loop_id : worker.loop_scratch) {
      kill_face(loop_face(loop_id));
    }
    worker.removed_faces += worker.loop_scratch.size();
    deactivate_edge(edge_id, worker);

    splice_vertex(keep, remove, worker.splice_edges_scratch);

    for (const std::pair<EdgeId, EdgeId> &splice : worker.splice_edges_scratch) {
      splice_edge(splice.second, splice.first, worker);
    }

    vertices_[keep].position = optimized_position;
    vertices_[keep].quadric += vertices_[remove].quadric;
    vertices_[remove].alive = false;

    const float inverse_factor = 1.0f - customdata_factor;
    Float3 interpolated_normal = {
        inverse_factor * keep_normal.x + customdata_factor * remove_normal.x,
        inverse_factor * keep_normal.y + customdata_factor * remove_normal.y,
        inverse_factor * keep_normal.z + customdata_factor * remove_normal.z,
    };
    normalize_float3(interpolated_normal);
    vertices_[keep].normal = interpolated_normal;

    const EdgeId disk_head = vertices_[keep].disk_head;
    if (disk_head != kInvalidEdgeId) {
      EdgeId incident_id = disk_head;
      do {
        update_edge_cost(incident_id, worker);
        incident_id = disk_edge_next(incident_id, keep);
      } while (incident_id != disk_head);
    }

    collect_loops_of_vertex(keep, worker.loop_scratch);
    for (const LoopId loop_id : worker.loop_scratch) {
      const VertexId loop_vertex =
          faces_[loop_face(loop_id)].vertices[loop_corner(loop_id)];
      const Edge &previous_edge = edges_[loops_[loop_previous(loop_id)].edge];
      const EdgeId outer_id =
          previous_edge.first == loop_vertex || previous_edge.second == loop_vertex ?
              loops_[loop_next(loop_id)].edge :
              loops_[loop_previous(loop_id)].edge;
      if (edges_[outer_id].first == keep || edges_[outer_id].second == keep) {
        throw std::runtime_error("internal error: face fan outer edge contains keep vertex");
      }
      update_edge_cost(outer_id, worker);
    }
  }

  std::size_t alive_vertex_count() const noexcept
  {
    std::size_t result = 0;
    for (VertexId vertex_id = 0; vertex_id < vertices_.size(); ++vertex_id) {
      result += vertices_[vertex_id].alive && vertex_has_faces(vertex_id) ? 1 : 0;
    }
    return result;
  }

  std::vector<Vertex> vertices_;
  std::vector<Edge> edges_;
  std::vector<Face> faces_;
  std::vector<Loop> loops_;
  std::vector<std::uint32_t> topology_neighbor_stamps_;
  WorkerContext global_worker_;
  std::size_t active_faces_ = 0;
  std::size_t input_vertices_ = 0;
  std::size_t input_faces_ = 0;
  MemoryMode memory_mode_ = MemoryMode::Balanced;
  unsigned initialization_threads_ = 1;
  DecimatorStats stats_;
};

QemDecimator::QemDecimator(InputMesh mesh,
                           const MemoryMode memory_mode,
                           const bool build_global_heap,
                           const unsigned threads)
    : impl_(new Impl(std::move(mesh), memory_mode, build_global_heap, threads))
{}

QemDecimator::~QemDecimator()
{
  delete impl_;
}

void QemDecimator::partition_dry_run(const std::size_t partition_count)
{
  impl_->partition_dry_run(partition_count);
}

DecimatorStats QemDecimator::decimate(const DecimatorOptions &options)
{
  return impl_->decimate(options);
}

InputMesh QemDecimator::compact_mesh() const
{
  return impl_->compact_mesh();
}

}  // namespace standalone_decimator
