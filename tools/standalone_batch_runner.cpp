#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#  include <sys/prctl.h>
#endif

namespace {

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;

volatile sig_atomic_t g_signal = 0;

extern "C" void handle_signal(const int signal_number)
{
  if (g_signal == 0) {
    g_signal = signal_number;
  }
}

struct Arguments {
  std::string binary;
  fs::path manifest;
  std::size_t workers = 0;
  std::uint64_t target_faces = 0;
  std::string target_faces_text;
  fs::path result_jsonl;
  fs::path summary_json;
  std::optional<fs::path> log_dir;
};

struct Job {
  std::uint64_t index = 0;
  fs::path input_path;
  fs::path output_path;
  std::string expected_sha256;
  fs::path log_path;
};

struct RunningJob {
  pid_t pid = -1;
  std::size_t job_position = 0;
  int cpu = -1;
  SystemClock::time_point start_system;
  SteadyClock::time_point start_steady;
};

struct JobResult {
  const Job *job = nullptr;
  SystemClock::time_point start_system;
  SystemClock::time_point end_system;
  double duration_seconds = 0.0;
  int exit_code = 0;
  std::optional<int> term_signal;
  bool launched = false;
  bool success = false;
  std::optional<std::string> output_sha256;
  bool sha_match = false;
  std::optional<std::uint64_t> face_count;
  std::string error;
};

int read_topology_id(const int cpu, const char *name)
{
  const fs::path path = fs::path("/sys/devices/system/cpu") /
                        ("cpu" + std::to_string(cpu)) / "topology" / name;
  std::ifstream input(path);
  int value = -1;
  if (!(input >> value)) {
    throw std::runtime_error("failed to read CPU topology: " + path.string());
  }
  return value;
}

std::vector<int> discover_worker_cpus()
{
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
    throw std::runtime_error("sched_getaffinity failed: " +
                             std::string(std::strerror(errno)));
  }

  std::map<int, std::vector<int>> physical_by_socket;
  std::map<int, std::vector<int>> siblings_by_socket;
  std::set<std::pair<int, int>> seen_cores;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &allowed)) {
      continue;
    }
    const int socket = read_topology_id(cpu, "physical_package_id");
    const std::pair<int, int> core = {socket, read_topology_id(cpu, "core_id")};
    auto &destination = seen_cores.insert(core).second ? physical_by_socket :
                                                         siblings_by_socket;
    destination[socket].push_back(cpu);
  }

  const auto interleave = [](const std::map<int, std::vector<int>> &groups) {
    std::vector<int> result;
    for (std::size_t position = 0;; ++position) {
      bool added = false;
      for (const auto &[socket, cpus] : groups) {
        (void)socket;
        if (position < cpus.size()) {
          result.push_back(cpus[position]);
          added = true;
        }
      }
      if (!added) {
        return result;
      }
    }
  };

  std::vector<int> result = interleave(physical_by_socket);
  std::vector<int> siblings = interleave(siblings_by_socket);
  result.insert(result.end(), siblings.begin(), siblings.end());
  return result;
}

[[noreturn]] void usage_error(const std::string &message)
{
  throw std::invalid_argument(
      message +
      "\nusage: standalone_batch_runner --binary PATH --manifest JOBS.tsv "
      "--workers COUNT --target-faces COUNT --result-jsonl RESULTS.jsonl "
      "--summary-json SUMMARY.json [--log-dir DIR]");
}

std::uint64_t parse_unsigned(const std::string &text, const std::string &name)
{
  if (text.empty() || text.front() == '-') {
    usage_error(name + " must be a non-negative integer");
  }
  std::size_t parsed = 0;
  unsigned long long value = 0;
  try {
    value = std::stoull(text, &parsed, 10);
  }
  catch (const std::exception &) {
    usage_error("invalid " + name + " value: " + text);
  }
  if (parsed != text.size()) {
    usage_error("invalid " + name + " value: " + text);
  }
  return static_cast<std::uint64_t>(value);
}

Arguments parse_arguments(const int argc, char **argv)
{
  Arguments arguments;
  bool target_faces_set = false;
  for (int argument_index = 1; argument_index < argc; ++argument_index) {
    const std::string option = argv[argument_index];
    if (option == "--help" || option == "-h") {
      std::cout
          << "usage: standalone_batch_runner --binary PATH --manifest JOBS.tsv "
             "--workers COUNT --target-faces COUNT --result-jsonl RESULTS.jsonl "
             "--summary-json SUMMARY.json [--log-dir DIR]\n";
      std::exit(0);
    }
    if (argument_index + 1 >= argc) {
      usage_error("missing value for " + option);
    }
    const std::string value = argv[++argument_index];
    if (option == "--binary") {
      arguments.binary = value;
    }
    else if (option == "--manifest") {
      arguments.manifest = value;
    }
    else if (option == "--workers") {
      const std::uint64_t workers = parse_unsigned(value, "--workers");
      if (workers == 0 || workers > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        usage_error("--workers must be between 1 and INT_MAX");
      }
      arguments.workers = static_cast<std::size_t>(workers);
    }
    else if (option == "--target-faces") {
      arguments.target_faces = parse_unsigned(value, "--target-faces");
      arguments.target_faces_text = value;
      target_faces_set = true;
    }
    else if (option == "--result-jsonl") {
      arguments.result_jsonl = value;
    }
    else if (option == "--summary-json") {
      arguments.summary_json = value;
    }
    else if (option == "--log-dir") {
      arguments.log_dir = fs::path(value);
    }
    else {
      usage_error("unknown option: " + option);
    }
  }

  if (arguments.binary.empty()) {
    usage_error("--binary is required");
  }
  if (arguments.manifest.empty()) {
    usage_error("--manifest is required");
  }
  if (arguments.workers == 0) {
    usage_error("--workers is required");
  }
  if (!target_faces_set) {
    usage_error("--target-faces is required");
  }
  if (arguments.result_jsonl.empty()) {
    usage_error("--result-jsonl is required");
  }
  if (arguments.summary_json.empty()) {
    usage_error("--summary-json is required");
  }
  if (arguments.result_jsonl == arguments.summary_json) {
    usage_error("--result-jsonl and --summary-json must be different paths");
  }
  return arguments;
}

std::vector<std::string> split_tsv(const std::string &line)
{
  std::vector<std::string> fields;
  std::size_t begin = 0;
  while (true) {
    const std::size_t tab = line.find('\t', begin);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(begin));
      return fields;
    }
    fields.push_back(line.substr(begin, tab - begin));
    begin = tab + 1;
  }
}

bool is_sha256(const std::string &text)
{
  if (text.size() != 64) {
    return false;
  }
  return std::all_of(text.begin(), text.end(), [](const unsigned char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
  });
}

std::string lowercase(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return text;
}

std::vector<Job> load_manifest(const fs::path &path)
{
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open manifest: " + path.string());
  }

  std::vector<Job> jobs;
  std::unordered_set<std::uint64_t> seen_indices;
  std::unordered_set<std::string> seen_outputs;
  std::string line;
  std::size_t line_number = 0;
  bool first_data_line = true;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::vector<std::string> fields = split_tsv(line);
    if (first_data_line && fields.size() == 4 && fields[0] == "index" &&
        fields[1] == "input_path" && fields[2] == "output_path" &&
        fields[3] == "expected_sha256") {
      first_data_line = false;
      continue;
    }
    first_data_line = false;
    if (fields.size() != 4) {
      throw std::runtime_error("manifest line " + std::to_string(line_number) +
                               " must contain exactly four tab-separated fields");
    }

    Job job;
    try {
      job.index = parse_unsigned(fields[0], "manifest index");
    }
    catch (const std::invalid_argument &error) {
      throw std::runtime_error("manifest line " + std::to_string(line_number) +
                               ": " + error.what());
    }
    job.input_path = fields[1];
    job.output_path = fields[2];
    if (job.input_path.empty() || job.output_path.empty()) {
      throw std::runtime_error("manifest line " + std::to_string(line_number) +
                               " has an empty input or output path");
    }
    if (!is_sha256(fields[3])) {
      throw std::runtime_error("manifest line " + std::to_string(line_number) +
                               " has an invalid expected SHA-256");
    }
    job.expected_sha256 = lowercase(fields[3]);
    if (!seen_indices.insert(job.index).second) {
      throw std::runtime_error("duplicate manifest index: " + std::to_string(job.index));
    }
    if (!seen_outputs.insert(job.output_path.string()).second) {
      throw std::runtime_error("duplicate manifest output path: " + job.output_path.string());
    }
    jobs.push_back(std::move(job));
  }
  if (!input.eof()) {
    throw std::runtime_error("error while reading manifest: " + path.string());
  }
  if (jobs.empty()) {
    throw std::runtime_error("manifest contains no jobs: " + path.string());
  }
  return jobs;
}

void create_parent_directory(const fs::path &path)
{
  const fs::path parent = path.parent_path();
  if (!parent.empty()) {
    fs::create_directories(parent);
  }
}

void prepare_job_paths(std::vector<Job> &jobs, const Arguments &arguments)
{
  const fs::path binary_path = fs::absolute(arguments.binary).lexically_normal();
  const fs::path manifest_path = fs::absolute(arguments.manifest).lexically_normal();
  const fs::path result_path = fs::absolute(arguments.result_jsonl).lexically_normal();
  const fs::path summary_path = fs::absolute(arguments.summary_json).lexically_normal();
  if (arguments.log_dir) {
    fs::create_directories(*arguments.log_dir);
  }
  for (std::size_t job_position = 0; job_position < jobs.size(); ++job_position) {
    Job &job = jobs[job_position];
    const fs::path input_path = fs::absolute(job.input_path).lexically_normal();
    const fs::path output_path = fs::absolute(job.output_path).lexically_normal();
    if (input_path == output_path) {
      throw std::runtime_error("job input and output paths are the same: " +
                               job.output_path.string());
    }
    if (output_path == binary_path || output_path == manifest_path ||
        output_path == result_path || output_path == summary_path) {
      throw std::runtime_error("job output collides with a batch control file: " +
                               job.output_path.string());
    }
    create_parent_directory(job.output_path);
    if (arguments.log_dir) {
      std::ostringstream prefix;
      prefix << std::setw(8) << std::setfill('0') << job_position << '_';
      job.log_path = *arguments.log_dir /
                     (prefix.str() + job.output_path.filename().string() + ".log");
    }
    else {
      job.log_path = fs::path(job.output_path.string() + ".log");
    }
    create_parent_directory(job.log_path);
    const fs::path log_path = fs::absolute(job.log_path).lexically_normal();
    if (log_path == binary_path || log_path == manifest_path ||
        log_path == result_path || log_path == summary_path ||
        log_path == input_path || log_path == output_path) {
      throw std::runtime_error("job log collides with an input, output, or batch control file: " +
                               job.log_path.string());
    }
  }
}

std::string json_escape(const std::string &text)
{
  std::ostringstream output;
  for (const unsigned char character : text) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<int>(character) << std::dec;
        }
        else {
          output << static_cast<char>(character);
        }
    }
  }
  return output.str();
}

std::int64_t unix_milliseconds(const SystemClock::time_point time)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

std::string utc_timestamp(const SystemClock::time_point time)
{
  const std::int64_t milliseconds = unix_milliseconds(time);
  const std::time_t seconds = SystemClock::to_time_t(time);
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
         << std::setw(3) << std::setfill('0') << (milliseconds % 1000) << 'Z';
  return output.str();
}

class Sha256 {
 public:
  void update(const std::uint8_t *bytes, const std::size_t size)
  {
    for (std::size_t index = 0; index < size; ++index) {
      block_[block_size_++] = bytes[index];
      if (block_size_ == block_.size()) {
        transform();
        bit_count_ += 512;
        block_size_ = 0;
      }
    }
  }

  std::array<std::uint8_t, 32> finish()
  {
    const std::size_t original_size = block_size_;
    block_[block_size_++] = 0x80;
    if (block_size_ > 56) {
      while (block_size_ < block_.size()) {
        block_[block_size_++] = 0;
      }
      transform();
      block_size_ = 0;
    }
    while (block_size_ < 56) {
      block_[block_size_++] = 0;
    }
    bit_count_ += static_cast<std::uint64_t>(original_size) * 8;
    for (std::size_t index = 0; index < 8; ++index) {
      block_[63 - index] = static_cast<std::uint8_t>(bit_count_ >> (index * 8));
    }
    transform();

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t word = 0; word < state_.size(); ++word) {
      digest[word * 4] = static_cast<std::uint8_t>(state_[word] >> 24);
      digest[word * 4 + 1] = static_cast<std::uint8_t>(state_[word] >> 16);
      digest[word * 4 + 2] = static_cast<std::uint8_t>(state_[word] >> 8);
      digest[word * 4 + 3] = static_cast<std::uint8_t>(state_[word]);
    }
    return digest;
  }

 private:
  static std::uint32_t rotate_right(const std::uint32_t value, const unsigned shift)
  {
    return (value >> shift) | (value << (32 - shift));
  }

  void transform()
  {
    static constexpr std::array<std::uint32_t, 64> constants = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const std::size_t offset = index * 4;
      words[index] = (static_cast<std::uint32_t>(block_[offset]) << 24) |
                     (static_cast<std::uint32_t>(block_[offset + 1]) << 16) |
                     (static_cast<std::uint32_t>(block_[offset + 2]) << 8) |
                     static_cast<std::uint32_t>(block_[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const std::uint32_t sigma0 = rotate_right(words[index - 15], 7) ^
                                   rotate_right(words[index - 15], 18) ^
                                   (words[index - 15] >> 3);
      const std::uint32_t sigma1 = rotate_right(words[index - 2], 17) ^
                                   rotate_right(words[index - 2], 19) ^
                                   (words[index - 2] >> 10);
      words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t sum1 =
          rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temporary1 = h + sum1 + choose + constants[index] + words[index];
      const std::uint32_t sum0 =
          rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667U,
      0xbb67ae85U,
      0x3c6ef372U,
      0xa54ff53aU,
      0x510e527fU,
      0x9b05688cU,
      0x1f83d9abU,
      0x5be0cd19U,
  };
  std::array<std::uint8_t, 64> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t bit_count_ = 0;
};

std::optional<std::string> sha256_file(const fs::path &path, std::string &error)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open output for SHA-256: " + path.string();
    return std::nullopt;
  }
  Sha256 sha256;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    if (g_signal != 0) {
      error = "output SHA-256 interrupted by signal";
      return std::nullopt;
    }
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize bytes_read = input.gcount();
    if (bytes_read > 0) {
      sha256.update(reinterpret_cast<const std::uint8_t *>(buffer.data()),
                    static_cast<std::size_t>(bytes_read));
    }
  }
  if (!input.eof()) {
    error = "error while hashing output: " + path.string();
    return std::nullopt;
  }
  const std::array<std::uint8_t, 32> digest = sha256.finish();
  std::ostringstream hex;
  for (const std::uint8_t byte : digest) {
    hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
  }
  return hex.str();
}

std::optional<std::uint64_t> parse_ply_face_count(const fs::path &path, std::string &error)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open output PLY header: " + path.string();
    return std::nullopt;
  }
  std::string line;
  if (!std::getline(input, line)) {
    error = "output has no PLY header";
    return std::nullopt;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != "ply") {
    error = "output PLY header does not start with 'ply'";
    return std::nullopt;
  }

  bool format_seen = false;
  bool face_seen = false;
  std::uint64_t face_count = 0;
  std::size_t header_bytes = line.size() + 1;
  for (std::size_t line_count = 1; line_count < 4096 && std::getline(input, line); ++line_count) {
    header_bytes += line.size() + 1;
    if (header_bytes > 1024 * 1024) {
      error = "output PLY header exceeds 1 MiB";
      return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line == "format binary_little_endian 1.0") {
      if (format_seen) {
        error = "output PLY header repeats the format line";
        return std::nullopt;
      }
      format_seen = true;
    }
    else if (line.compare(0, 13, "element face ") == 0) {
      if (face_seen) {
        error = "output PLY header repeats the face element";
        return std::nullopt;
      }
      const std::string count_text = line.substr(13);
      if (count_text.empty() || count_text.front() == '-') {
        error = "output PLY face count is invalid";
        return std::nullopt;
      }
      std::size_t parsed = 0;
      try {
        face_count = std::stoull(count_text, &parsed, 10);
      }
      catch (const std::exception &) {
        error = "output PLY face count is invalid";
        return std::nullopt;
      }
      if (parsed != count_text.size()) {
        error = "output PLY face count is invalid";
        return std::nullopt;
      }
      face_seen = true;
    }
    else if (line == "end_header") {
      if (!format_seen || !face_seen) {
        error = "output PLY header is missing binary format or face element";
        return std::nullopt;
      }
      return face_count;
    }
  }
  error = "output PLY header has no end_header";
  return std::nullopt;
}

void append_error(std::string &destination, const std::string &error)
{
  if (!destination.empty()) {
    destination += "; ";
  }
  destination += error;
}

void inspect_output(JobResult &result)
{
  std::string sha_error;
  result.output_sha256 = sha256_file(result.job->output_path, sha_error);
  if (!result.output_sha256) {
    append_error(result.error, sha_error);
  }
  else {
    result.sha_match = *result.output_sha256 == result.job->expected_sha256;
  }

  std::string ply_error;
  result.face_count = parse_ply_face_count(result.job->output_path, ply_error);
  if (!result.face_count) {
    append_error(result.error, ply_error);
  }
  result.success = result.exit_code == 0 && result.output_sha256.has_value() &&
                   result.face_count.has_value();
}

std::string result_json(const JobResult &result)
{
  std::ostringstream output;
  output << std::fixed << std::setprecision(6)
         << "{\"index\":" << result.job->index
         << ",\"input_path\":\"" << json_escape(result.job->input_path.string()) << '"'
         << ",\"output_path\":\"" << json_escape(result.job->output_path.string()) << '"'
         << ",\"log_path\":\"" << json_escape(result.job->log_path.string()) << '"'
         << ",\"start_time_utc\":\"" << utc_timestamp(result.start_system) << '"'
         << ",\"end_time_utc\":\"" << utc_timestamp(result.end_system) << '"'
         << ",\"start_unix_ms\":" << unix_milliseconds(result.start_system)
         << ",\"end_unix_ms\":" << unix_milliseconds(result.end_system)
         << ",\"duration_seconds\":" << result.duration_seconds
         << ",\"launched\":" << (result.launched ? "true" : "false")
         << ",\"exit_code\":" << result.exit_code
         << ",\"term_signal\":";
  if (result.term_signal) {
    output << *result.term_signal;
  }
  else {
    output << "null";
  }
  output << ",\"success\":" << (result.success ? "true" : "false")
         << ",\"output_sha256\":";
  if (result.output_sha256) {
    output << '"' << *result.output_sha256 << '"';
  }
  else {
    output << "null";
  }
  output << ",\"expected_sha256\":\"" << result.job->expected_sha256 << '"'
         << ",\"sha_match\":" << (result.sha_match ? "true" : "false")
         << ",\"face_count\":";
  if (result.face_count) {
    output << *result.face_count;
  }
  else {
    output << "null";
  }
  output << ",\"error\":";
  if (result.error.empty()) {
    output << "null";
  }
  else {
    output << '"' << json_escape(result.error) << '"';
  }
  output << '}';
  return output.str();
}

void write_result(std::ofstream &stream, const JobResult &result)
{
  stream << result_json(result) << '\n';
  stream.flush();
  if (!stream) {
    throw std::runtime_error("failed to write result JSONL");
  }
}

void send_process_group_signal(const pid_t pid, const int signal_number)
{
  if (::kill(-pid, signal_number) != 0 && errno == ESRCH) {
    ::kill(pid, signal_number);
  }
}

pid_t launch_job(const Arguments &arguments,
                 const Job &job,
                 const int cpu,
                 const SystemClock::time_point start_system,
                 const SteadyClock::time_point start_steady,
                 const std::size_t job_position,
                 std::vector<RunningJob> &running,
                 std::string &error)
{
  std::error_code remove_error;
  fs::remove(job.output_path, remove_error);
  if (remove_error) {
    error = "cannot remove existing output: " + remove_error.message();
    return -1;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    error = "fork failed: " + std::string(std::strerror(errno));
    return -1;
  }
  if (pid == 0) {
#ifdef __linux__
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() == 1) {
      _exit(126);
    }
#endif
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    if (::sigaction(SIGINT, &default_action, nullptr) != 0 ||
        ::sigaction(SIGTERM, &default_action, nullptr) != 0) {
      _exit(126);
    }
    if (::setpgid(0, 0) != 0) {
      _exit(126);
    }
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    if (::sched_setaffinity(0, sizeof(affinity), &affinity) != 0) {
      _exit(126);
    }
    const int log_fd = ::open(job.log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) {
      ::dprintf(STDERR_FILENO,
                "cannot open child log %s: %s\n",
                job.log_path.c_str(),
                std::strerror(errno));
      _exit(126);
    }
    if (::dup2(log_fd, STDOUT_FILENO) < 0 || ::dup2(log_fd, STDERR_FILENO) < 0) {
      _exit(126);
    }
    if (log_fd > STDERR_FILENO) {
      ::close(log_fd);
    }

    std::array<char *, 8> child_arguments = {
        const_cast<char *>(arguments.binary.c_str()),
        const_cast<char *>("--input"),
        const_cast<char *>(job.input_path.c_str()),
        const_cast<char *>("--output"),
        const_cast<char *>(job.output_path.c_str()),
        const_cast<char *>("--target-faces"),
        const_cast<char *>(arguments.target_faces_text.c_str()),
        nullptr,
    };
    ::execvp(child_arguments[0], child_arguments.data());
    ::dprintf(STDERR_FILENO,
              "exec failed for %s: %s\n",
              arguments.binary.c_str(),
              std::strerror(errno));
    _exit(127);
  }

  if (::setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) {
    const int setpgid_error = errno;
    send_process_group_signal(pid, SIGKILL);
    int ignored_status = 0;
    while (::waitpid(pid, &ignored_status, 0) < 0 && errno == EINTR) {
    }
    error = "setpgid failed: " + std::string(std::strerror(setpgid_error));
    return -1;
  }
  running.push_back(RunningJob{pid, job_position, cpu, start_system, start_steady});
  return pid;
}

std::size_t find_running_job(const std::vector<RunningJob> &running, const pid_t pid)
{
  for (std::size_t index = 0; index < running.size(); ++index) {
    if (running[index].pid == pid) {
      return index;
    }
  }
  throw std::runtime_error("reaped an unknown child process");
}

double percentile(std::vector<double> values, const double fraction)
{
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const std::size_t rank = static_cast<std::size_t>(
      std::ceil(fraction * static_cast<double>(values.size())));
  return values[std::max<std::size_t>(1, rank) - 1];
}

std::optional<long> peak_parent_rss_kb()
{
  struct rusage usage {};
  if (::getrusage(RUSAGE_SELF, &usage) != 0) {
    return std::nullopt;
  }
  return usage.ru_maxrss;
}

void write_all(const int file_descriptor, const std::string &contents)
{
  std::size_t written = 0;
  while (written < contents.size()) {
    const ssize_t result =
        ::write(file_descriptor, contents.data() + written, contents.size() - written);
    if (result > 0) {
      written += static_cast<std::size_t>(result);
    }
    else if (result < 0 && errno == EINTR) {
      continue;
    }
    else {
      throw std::runtime_error("failed to write atomic summary");
    }
  }
}

void write_atomic(const fs::path &path, const std::string &contents)
{
  create_parent_directory(path);
  const fs::path temporary =
      fs::path(path.string() + ".tmp." + std::to_string(static_cast<long long>(::getpid())));
  const int file_descriptor =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (file_descriptor < 0) {
    throw std::runtime_error("cannot create summary temporary file: " +
                             std::string(std::strerror(errno)));
  }
  try {
    write_all(file_descriptor, contents);
    if (::fsync(file_descriptor) != 0) {
      throw std::runtime_error("cannot fsync summary temporary file: " +
                               std::string(std::strerror(errno)));
    }
    if (::close(file_descriptor) != 0) {
      throw std::runtime_error("cannot close summary temporary file: " +
                               std::string(std::strerror(errno)));
    }
  }
  catch (...) {
    ::close(file_descriptor);
    ::unlink(temporary.c_str());
    throw;
  }
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    const std::string error = std::strerror(errno);
    ::unlink(temporary.c_str());
    throw std::runtime_error("cannot atomically rename summary: " + error);
  }
}

void install_signal_handlers()
{
  struct sigaction action {};
  action.sa_handler = handle_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  if (::sigaction(SIGINT, &action, nullptr) != 0 ||
      ::sigaction(SIGTERM, &action, nullptr) != 0) {
    throw std::runtime_error("cannot install signal handlers");
  }
}

void kill_and_reap(std::vector<RunningJob> &running)
{
  for (const RunningJob &child : running) {
    send_process_group_signal(child.pid, SIGKILL);
  }
  for (const RunningJob &child : running) {
    int status = 0;
    while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
  running.clear();
}

int run(const Arguments &arguments)
{
  std::vector<Job> jobs = load_manifest(arguments.manifest);
  const std::vector<int> worker_cpus = discover_worker_cpus();
  if (worker_cpus.size() < arguments.workers) {
    throw std::runtime_error("not enough allowed CPUs for requested workers");
  }
  prepare_job_paths(jobs, arguments);
  create_parent_directory(arguments.result_jsonl);
  create_parent_directory(arguments.summary_json);

  std::ofstream result_stream(arguments.result_jsonl, std::ios::trunc);
  if (!result_stream) {
    throw std::runtime_error("cannot open result JSONL: " + arguments.result_jsonl.string());
  }

  install_signal_handlers();
  const SystemClock::time_point batch_start_system = SystemClock::now();
  const SteadyClock::time_point batch_start_steady = SteadyClock::now();
  std::vector<RunningJob> running;
  running.reserve(arguments.workers);
  std::vector<JobResult> completed_results;
  completed_results.reserve(jobs.size());
  std::vector<double> durations;
  durations.reserve(jobs.size());
  std::size_t next_job = 0;
  std::size_t launched_count = 0;
  std::size_t reaped_count = 0;
  std::size_t result_count = 0;
  std::size_t success_count = 0;
  std::size_t sha_matches = 0;
  std::size_t validated_count = 0;
  bool termination_sent = false;
  bool force_kill_sent = false;
  SteadyClock::time_point force_kill_at;

  const auto fill_worker_pool = [&]() {
    while (!termination_sent && g_signal == 0 && next_job < jobs.size() &&
           running.size() < arguments.workers)
    {
      const Job &job = jobs[next_job];
      int cpu = -1;
      for (std::size_t worker = 0; worker < arguments.workers; ++worker) {
        const int candidate = worker_cpus[worker];
        const bool used = std::any_of(
            running.begin(), running.end(), [&](const RunningJob &child) {
              return child.cpu == candidate;
            });
        if (!used) {
          cpu = candidate;
          break;
        }
      }
      if (cpu < 0) {
        throw std::runtime_error("failed to assign an unused worker CPU");
      }
      const SystemClock::time_point start_system = SystemClock::now();
      const SteadyClock::time_point start_steady = SteadyClock::now();
      std::string launch_error;
      const pid_t pid = launch_job(arguments,
                                   job,
                                   cpu,
                                   start_system,
                                   start_steady,
                                   next_job,
                                   running,
                                   launch_error);
      ++next_job;
      if (pid >= 0) {
        ++launched_count;
        continue;
      }

      const SystemClock::time_point end_system = SystemClock::now();
      JobResult result;
      result.job = &job;
      result.start_system = start_system;
      result.end_system = end_system;
      result.duration_seconds =
          std::chrono::duration<double>(SteadyClock::now() - start_steady).count();
      result.exit_code = 125;
      result.error = launch_error;
      completed_results.push_back(std::move(result));
      ++result_count;
    }
  };

  try {
    while (next_job < jobs.size() || !running.empty()) {
      if (g_signal != 0 && !termination_sent) {
        termination_sent = true;
        force_kill_at = SteadyClock::now() + std::chrono::seconds(5);
        for (const RunningJob &child : running) {
          send_process_group_signal(child.pid, SIGTERM);
        }
      }
      if (termination_sent && running.empty()) {
        break;
      }

      fill_worker_pool();

      if (running.empty()) {
        continue;
      }

      int status = 0;
      const pid_t reaped_pid = ::waitpid(-1, &status, WNOHANG);
      if (reaped_pid > 0) {
        const std::size_t running_index = find_running_job(running, reaped_pid);
        const RunningJob child = running[running_index];
        running[running_index] = running.back();
        running.pop_back();

        JobResult result;
        result.job = &jobs[child.job_position];
        result.start_system = child.start_system;
        result.end_system = SystemClock::now();
        result.duration_seconds =
            std::chrono::duration<double>(SteadyClock::now() - child.start_steady).count();
        result.launched = true;
        if (WIFEXITED(status)) {
          result.exit_code = WEXITSTATUS(status);
        }
        else if (WIFSIGNALED(status)) {
          result.term_signal = WTERMSIG(status);
          result.exit_code = 128 + *result.term_signal;
          result.error = "child terminated by signal " + std::to_string(*result.term_signal);
        }
        else {
          result.exit_code = 125;
          result.error = "child ended with an unsupported wait status";
        }
        /* Refill the process pool before reading and hashing the completed
         * output, so validation I/O does not leave a worker slot idle. */
        fill_worker_pool();
        completed_results.push_back(std::move(result));
        durations.push_back(result.duration_seconds);
        ++reaped_count;
        ++result_count;
        continue;
      }
      if (reaped_pid < 0 && errno != EINTR) {
        throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
      }

      if (termination_sent && !force_kill_sent && SteadyClock::now() >= force_kill_at) {
        force_kill_sent = true;
        for (const RunningJob &child : running) {
          send_process_group_signal(child.pid, SIGKILL);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  catch (...) {
    kill_and_reap(running);
    throw;
  }

  if (termination_sent) {
    while (next_job < jobs.size()) {
      const SystemClock::time_point now = SystemClock::now();
      JobResult result;
      result.job = &jobs[next_job++];
      result.start_system = now;
      result.end_system = now;
      result.exit_code = 128 + static_cast<int>(g_signal);
      result.term_signal = static_cast<int>(g_signal);
      result.error = "not started because the batch runner received a signal";
      completed_results.push_back(std::move(result));
      ++result_count;
    }
  }

  const SteadyClock::time_point processing_end_steady = SteadyClock::now();
  const double processing_wall_seconds =
      std::chrono::duration<double>(processing_end_steady - batch_start_steady).count();
  const SteadyClock::time_point validation_start_steady = SteadyClock::now();
  for (JobResult &result : completed_results) {
    if (result.launched) {
      inspect_output(result);
    }
    write_result(result_stream, result);
    if (result.success) {
      ++success_count;
    }
    if (result.sha_match) {
      ++sha_matches;
    }
    if (result.success && result.sha_match) {
      ++validated_count;
    }
  }
  const double validation_wall_seconds =
      std::chrono::duration<double>(SteadyClock::now() - validation_start_steady).count();
  const SystemClock::time_point batch_end_system = SystemClock::now();
  const double total_wall_seconds =
      std::chrono::duration<double>(SteadyClock::now() - batch_start_steady).count();
  const double jobs_per_hour =
      processing_wall_seconds > 0.0 ?
          static_cast<double>(success_count) * 3600.0 / processing_wall_seconds :
          0.0;
  const std::optional<long> peak_rss = peak_parent_rss_kb();

  std::ostringstream summary;
  summary << std::fixed << std::setprecision(6)
          << "{\"manifest\":\"" << json_escape(arguments.manifest.string()) << '"'
          << ",\"batch_start_time_utc\":\"" << utc_timestamp(batch_start_system) << '"'
          << ",\"batch_end_time_utc\":\"" << utc_timestamp(batch_end_system) << '"'
          << ",\"wall_seconds\":" << processing_wall_seconds
          << ",\"processing_wall_seconds\":" << processing_wall_seconds
          << ",\"validation_wall_seconds\":" << validation_wall_seconds
          << ",\"total_wall_seconds\":" << total_wall_seconds
          << ",\"total_count\":" << jobs.size()
          << ",\"launched_count\":" << launched_count
          << ",\"reaped_count\":" << reaped_count
          << ",\"result_count\":" << result_count
          << ",\"success_count\":" << success_count
          << ",\"sha_matches\":" << sha_matches
          << ",\"validated_count\":" << validated_count
          << ",\"failure_count\":" << (jobs.size() - validated_count)
          << ",\"jobs_per_hour\":" << jobs_per_hour
          << ",\"duration_p50_seconds\":" << percentile(durations, 0.50)
          << ",\"duration_p95_seconds\":" << percentile(durations, 0.95)
          << ",\"workers\":" << arguments.workers
          << ",\"cpus\":[";
  for (std::size_t index = 0; index < arguments.workers; ++index) {
    if (index != 0) {
      summary << ',';
    }
    summary << worker_cpus[index];
  }
  summary << ']'
          << ",\"target_faces\":" << arguments.target_faces
          << ",\"peak_parent_rss_kb\":";
  if (peak_rss) {
    summary << *peak_rss;
  }
  else {
    summary << "null";
  }
  summary << ",\"interrupted\":" << (g_signal != 0 ? "true" : "false")
          << ",\"signal\":";
  if (g_signal != 0) {
    summary << static_cast<int>(g_signal);
  }
  else {
    summary << "null";
  }
  summary << "}\n";
  write_atomic(arguments.summary_json, summary.str());

  if (g_signal != 0) {
    return 128 + static_cast<int>(g_signal);
  }
  return validated_count == jobs.size() ? 0 : 1;
}

}  // namespace

int main(const int argc, char **argv)
{
  try {
    return run(parse_arguments(argc, argv));
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
