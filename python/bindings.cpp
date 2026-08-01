#include "meshutil/simplify.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <cstring>
#include <stdexcept>

namespace py = pybind11;

namespace {

void require_matrix(const py::array &array,
                    const py::dtype &dtype,
                    const char *name)
{
  if (!array.dtype().is(dtype)) {
    throw py::type_error(std::string(name) + " has an unsupported dtype");
  }
  if (array.ndim() != 2 || array.shape(1) != 3) {
    throw py::value_error(std::string(name) + " must have shape [N, 3]");
  }
  if ((array.flags() & py::array::c_style) == 0) {
    throw py::value_error(std::string(name) + " must be C-contiguous");
  }
}

py::dict stats_dict(const meshutil::SimplifyStats &stats)
{
  py::dict result;
  result["input_vertices"] = stats.input_vertices;
  result["input_faces"] = stats.input_faces;
  result["output_vertices"] = stats.output_vertices;
  result["output_faces"] = stats.output_faces;
  result["collapsed_edges"] = stats.collapsed_edges;
  result["rejected_topology"] = stats.rejected_topology;
  result["rejected_flip"] = stats.rejected_flip;
  result["invalid_edges"] = stats.invalid_edges;
  result["target_reached"] = stats.target_reached;
  result["input_conversion_seconds"] = stats.input_conversion_seconds;
  result["initialization_seconds"] = stats.initialization_seconds;
  result["collapse_seconds"] = stats.collapse_seconds;
  result["compact_seconds"] = stats.compact_seconds;
  result["output_conversion_seconds"] = stats.output_conversion_seconds;
  result["core_seconds"] = stats.core_seconds;
  return result;
}

py::tuple simplify_arrays(const py::array &positions,
                          const py::array &triangles,
                          const std::size_t target_faces,
                          const unsigned threads)
{
  require_matrix(positions, py::dtype::of<float>(), "positions");
  require_matrix(triangles, py::dtype::of<meshutil::Index>(), "triangles");

  meshutil::MeshView view;
  view.positions = static_cast<const float *>(positions.data());
  view.vertex_count = static_cast<std::size_t>(positions.shape(0));
  view.position_stride_bytes = static_cast<std::size_t>(positions.strides(0));
  view.triangles = static_cast<const meshutil::Index *>(triangles.data());
  view.triangle_count = static_cast<std::size_t>(triangles.shape(0));
  view.triangle_stride_bytes = static_cast<std::size_t>(triangles.strides(0));

  meshutil::SimplifyOptions options;
  options.target_faces = target_faces;
  options.threads = threads;

  meshutil::SimplifyResult result;
  {
    py::gil_scoped_release release;
    result = meshutil::simplify(view, options);
  }

  py::array_t<float> output_positions(
      {static_cast<py::ssize_t>(result.mesh.vertex_count()), py::ssize_t(3)});
  py::array_t<meshutil::Index> output_triangles(
      {static_cast<py::ssize_t>(result.mesh.triangle_count()), py::ssize_t(3)});
  std::memcpy(output_positions.mutable_data(),
              result.mesh.positions.data(),
              result.mesh.positions.size() * sizeof(float));
  std::memcpy(output_triangles.mutable_data(),
              result.mesh.triangles.data(),
              result.mesh.triangles.size() * sizeof(meshutil::Index));
  return py::make_tuple(
      std::move(output_positions), std::move(output_triangles), stats_dict(result.stats));
}

}  // namespace

PYBIND11_MODULE(meshutil, module)
{
  module.doc() = "General-purpose QEM edge-collapse mesh simplification";
  module.def(
      "simplify",
      &simplify_arrays,
      py::arg("positions"),
      py::arg("triangles"),
      py::arg("target_faces"),
      py::arg("threads") = 1,
      "Simplify float32 positions and uint32 triangle indices.");
}
