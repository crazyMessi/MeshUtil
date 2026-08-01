#include "meshutil/io.hpp"
#include "meshutil/simplify.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

struct Arguments {
  std::string input;
  std::string output;
  std::string trace;
  meshutil::MemoryMode memory_mode = meshutil::MemoryMode::Balanced;
  std::size_t target_faces = std::numeric_limits<std::size_t>::max();
  std::size_t partition_dry_run_count = 0;
  std::size_t partition_local_count = 0;
  std::size_t partition_local_target_faces = 0;
};

[[noreturn]] void usage_error(const std::string &message)
{
  throw std::invalid_argument(
      message +
      "\nusage: standalone_decimator --input INPUT.ply --output OUTPUT.ply "
      "--target-faces COUNT [--partition-dry-run-count 16|32|64|128] "
      "[--partition-local-count 16|32|64|128 "
      "--partition-local-target-faces COUNT] "
      "[--memory-mode balanced|low] [--trace TRACE.jsonl]");
}

std::size_t parse_count(const std::string &text)
{
  if (text.empty() || text.front() == '-') {
    usage_error("--target-faces must be a non-negative integer");
  }
  std::size_t parsed = 0;
  unsigned long long value = 0;
  try {
    value = std::stoull(text, &parsed, 10);
  }
  catch (const std::exception &) {
    usage_error("invalid --target-faces value: " + text);
  }
  if (parsed != text.size() || value > std::numeric_limits<std::size_t>::max()) {
    usage_error("invalid --target-faces value: " + text);
  }
  return static_cast<std::size_t>(value);
}

Arguments parse_arguments(const int argc, char **argv)
{
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--help" || option == "-h") {
      std::cout
          << "usage: standalone_decimator --input INPUT.ply --output OUTPUT.ply "
             "--target-faces COUNT [--partition-dry-run-count 16|32|64|128] "
             "[--partition-local-count 16|32|64|128 "
             "--partition-local-target-faces COUNT] "
             "[--memory-mode balanced|low] [--trace TRACE.jsonl]\n";
      std::exit(0);
    }
    if (index + 1 >= argc) {
      usage_error("missing value for " + option);
    }
    const std::string value = argv[++index];
    if (option == "--input") {
      arguments.input = value;
    }
    else if (option == "--output") {
      arguments.output = value;
    }
    else if (option == "--target-faces") {
      arguments.target_faces = parse_count(value);
    }
    else if (option == "--partition-dry-run-count") {
      arguments.partition_dry_run_count = parse_count(value);
    }
    else if (option == "--partition-local-count") {
      arguments.partition_local_count = parse_count(value);
    }
    else if (option == "--partition-local-target-faces") {
      arguments.partition_local_target_faces = parse_count(value);
    }
    else if (option == "--trace") {
      arguments.trace = value;
    }
    else if (option == "--memory-mode") {
      if (value == "balanced") {
        arguments.memory_mode = meshutil::MemoryMode::Balanced;
      }
      else if (value == "low") {
        arguments.memory_mode = meshutil::MemoryMode::Low;
      }
      else {
        usage_error("--memory-mode must be balanced or low");
      }
    }
    else {
      usage_error("unknown option: " + option);
    }
  }

  if (arguments.input.empty()) {
    usage_error("--input is required");
  }
  if (arguments.output.empty()) {
    usage_error("--output is required");
  }
  if (arguments.target_faces == std::numeric_limits<std::size_t>::max()) {
    usage_error("--target-faces is required");
  }
  if (arguments.input == arguments.output) {
    usage_error("--input and --output must be different paths");
  }
  return arguments;
}

}  // namespace

int main(const int argc, char **argv)
{
  try {
    const Arguments arguments = parse_arguments(argc, argv);
    meshutil::Mesh input = meshutil::read_mesh(arguments.input);
    meshutil::SimplifyOptions options;
    options.target_faces = arguments.target_faces;
    options.partition_dry_run_count = arguments.partition_dry_run_count;
    options.partition_local_count = arguments.partition_local_count;
    options.partition_local_target_faces =
        arguments.partition_local_target_faces;
    options.memory_mode = arguments.memory_mode;
    options.trace_path = arguments.trace;
    meshutil::SimplifyResult result = meshutil::simplify(std::move(input), options);
    meshutil::write_mesh(arguments.output, result.mesh);
    const meshutil::SimplifyStats &stats = result.stats;

    std::cout << "{\"success\":true"
              << ",\"input_vertices\":" << stats.input_vertices
              << ",\"input_faces\":" << stats.input_faces
              << ",\"output_vertices\":" << stats.output_vertices
              << ",\"output_faces\":" << stats.output_faces
              << ",\"collapsed_edges\":" << stats.collapsed_edges
              << ",\"rejected_topology\":" << stats.rejected_topology
              << ",\"rejected_flip\":" << stats.rejected_flip
              << ",\"invalid_edges\":" << stats.invalid_edges
              << ",\"partition_dry_run_count\":" << stats.partition_dry_run_count
              << ",\"partition_alive_vertices\":" << stats.partition_alive_vertices
              << ",\"partition_alive_edges\":" << stats.partition_alive_edges
              << ",\"partition_face_corner_load_min\":"
              << stats.partition_face_corner_load_min
              << ",\"partition_face_corner_load_mean\":"
              << stats.partition_face_corner_load_mean
              << ",\"partition_face_corner_load_max\":"
              << stats.partition_face_corner_load_max
              << ",\"partition_face_corner_load_max_over_mean\":"
              << stats.partition_face_corner_load_max_over_mean
              << ",\"partition_cross_edge_count\":" << stats.partition_cross_edge_count
              << ",\"partition_cross_edge_fraction\":"
              << stats.partition_cross_edge_fraction
              << ",\"partition_halo_b0_vertex_count\":"
              << stats.partition_halo_b0_vertex_count
              << ",\"partition_halo_b0_vertex_fraction\":"
              << stats.partition_halo_b0_vertex_fraction
              << ",\"partition_halo_b1_vertex_count\":"
              << stats.partition_halo_b1_vertex_count
              << ",\"partition_halo_b1_vertex_fraction\":"
              << stats.partition_halo_b1_vertex_fraction
              << ",\"partition_halo_b2_vertex_count\":"
              << stats.partition_halo_b2_vertex_count
              << ",\"partition_halo_b2_vertex_fraction\":"
              << stats.partition_halo_b2_vertex_fraction
              << ",\"partition_halo_face_count\":" << stats.partition_halo_face_count
              << ",\"partition_halo_face_fraction\":" << stats.partition_halo_face_fraction
              << ",\"partition_eligible_edge_count\":"
              << stats.partition_eligible_edge_count
              << ",\"partition_eligible_edge_fraction\":"
              << stats.partition_eligible_edge_fraction
              << ",\"partition_wall_seconds\":" << stats.partition_wall_seconds
              << ",\"partition_transient_bytes\":" << stats.partition_transient_bytes
              << ",\"partition_local_count\":" << stats.partition_local_count
              << ",\"partition_local_target_faces\":"
              << stats.partition_local_target_faces
              << ",\"partition_local_output_faces\":"
              << stats.partition_local_output_faces
              << ",\"partition_local_collapsed_edges\":"
              << stats.partition_local_collapsed_edges
              << ",\"partition_local_stalled_count\":"
              << stats.partition_local_stalled_count
              << ",\"partition_local_heap_entries\":"
              << stats.partition_local_heap_entries
              << ",\"partition_local_plan_seconds\":"
              << stats.partition_local_plan_seconds
              << ",\"partition_local_heap_build_seconds\":"
              << stats.partition_local_heap_build_seconds
              << ",\"partition_local_collapse_seconds\":"
              << stats.partition_local_collapse_seconds
              << ",\"global_cleanup_input_faces\":"
              << stats.global_cleanup_input_faces
              << ",\"global_cleanup_collapsed_edges\":"
              << stats.global_cleanup_collapsed_edges
              << ",\"global_heap_rebuild_seconds\":"
              << stats.global_heap_rebuild_seconds
              << ",\"global_cleanup_seconds\":" << stats.global_cleanup_seconds
              << ",\"input_conversion_seconds\":" << stats.input_conversion_seconds
              << ",\"initialization_seconds\":" << stats.initialization_seconds
              << ",\"collapse_seconds\":" << stats.collapse_seconds
              << ",\"compact_seconds\":" << stats.compact_seconds
              << ",\"output_conversion_seconds\":" << stats.output_conversion_seconds
              << ",\"core_seconds\":" << stats.core_seconds
              << ",\"target_reached\":" << (stats.target_reached ? "true" : "false") << "}\n";
    return stats.target_reached ? 0 : 3;
  }
  catch (const std::invalid_argument &error) {
    std::cerr << "argument error: " << error.what() << '\n';
    return 2;
  }
  catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
