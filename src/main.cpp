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
  std::size_t target_faces = std::numeric_limits<std::size_t>::max();
};

[[noreturn]] void usage_error(const std::string &message)
{
  throw std::invalid_argument(
      message +
      "\nusage: standalone_decimator --input INPUT.ply --output OUTPUT.ply "
      "--target-faces COUNT [--trace TRACE.jsonl]");
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
             "--target-faces COUNT [--trace TRACE.jsonl]\n";
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
    else if (option == "--trace") {
      arguments.trace = value;
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
    const meshutil::Mesh input = meshutil::read_mesh(arguments.input);
    meshutil::SimplifyOptions options;
    options.target_faces = arguments.target_faces;
    options.trace_path = arguments.trace;
    meshutil::SimplifyResult result = meshutil::simplify(input.view(), options);
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
