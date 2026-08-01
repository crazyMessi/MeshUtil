#include "meshutil/io.hpp"
#include "meshutil/simplify.hpp"

#include <iostream>

int main(int argc, char **argv)
{
  if (argc != 4) {
    std::cerr << "usage: meshutil_example INPUT OUTPUT TARGET_FACES\n";
    return 2;
  }
  const meshutil::Mesh input = meshutil::read_mesh(argv[1]);
  meshutil::SimplifyOptions options;
  options.target_faces = std::stoull(argv[3]);
  meshutil::SimplifyResult result = meshutil::simplify(input.view(), options);
  meshutil::write_mesh(argv[2], result.mesh);
  std::cout << result.stats.output_faces << '\n';
  return result.stats.target_reached ? 0 : 3;
}
