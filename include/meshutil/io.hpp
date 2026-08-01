#pragma once

#include "meshutil/mesh.hpp"

#include <string>

namespace meshutil {

enum class MeshFormat {
  Auto,
  Ply,
  Obj,
};

MeshFormat detect_mesh_format(const std::string &path);
Mesh read_mesh(const std::string &path, MeshFormat format = MeshFormat::Auto);
void write_mesh(const std::string &path,
                const Mesh &mesh,
                MeshFormat format = MeshFormat::Auto);

}  // namespace meshutil
