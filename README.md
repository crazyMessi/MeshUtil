# MeshUtil

Self-contained C++17 utilities for triangle meshes and grid-based surface
reconstruction. Each module has a specific role and can be used independently.

## Modules

| Module | Purpose | Entry point | Documentation |
| --- | --- | --- | --- |
| Triangle QEM decimator | Reduce strict binary triangle PLY meshes using the validated Blender 4.0.2 collapse behavior | `standalone_decimator` CLI; Linux batch runner | [Usage](doc/decimate/README.md), [development history](doc/decimate/DEVELOPMENT.md) |
| Double-crossing connectivity | Preserve known pairs of crossings that corner-sign-only Marching Cubes can miss on coarse grid edges | Header-only `MeshUtil::double_crossing` | [Guide](doc/double_crossing/README.md), [中文说明](doc/double_crossing/README.zh-CN.md) |

The decimator operates on existing triangle meshes. The connectivity module
sits between mesh/grid intersection and triangulation: it accepts corner signs
and 0–2 distinct crossings per edge and returns face paths or cube boundary
loops. Callers supply intersection positions, yellow-turn geometry, and
triangulation. More than two crossings are rejected.

The double-crossing module includes [23 illustrated cube examples](doc/double_crossing/cases/README.md),
[7 complement-pairing comparisons](doc/double_crossing/conflicts/README.md),
and a [21-page vector PDF atlas](output/pdf/MeshUtil_double_crossing_cases_and_conflicts.pdf).
The examples also have individual SVG diagrams, overviews, and an offline interactive gallery.

## Build

Requires CMake 3.16+ and a C++17 compiler. The C++ modules have no external
runtime dependencies. Python 3.9+ is optional for regenerating/checking the
committed connectivity tables.

```bash
cmake -S . -B temp_output/build -DCMAKE_BUILD_TYPE=Release
cmake --build temp_output/build --parallel
ctest --test-dir temp_output/build --output-on-failure -j 1
```

`./build.sh` remains a shortcut for a Release build in `build/`; pass a first
argument to choose another directory, for example `./build.sh temp_output/build`.

| CMake option | Standalone default | When included by another project |
| --- | --- | --- |
| `MESHUTIL_BUILD_TOOLS` | `ON` | `OFF` |
| `MESHUTIL_BUILD_EXAMPLES` | `OFF` | `OFF` |
| `MESHUTIL_BUILD_TESTS` | Follows `BUILD_TESTING` on first configure | `OFF` |

In a standalone build, `BUILD_TESTING=OFF` also disables MeshUtil tests.
The batch runner is available only on Linux because it uses CPU-affinity APIs.
The decimator and connectivity module build on macOS as well; the repository's
Blender compatibility results refer to the fixed workload in the decimator guide.

Enable the connectivity example with `-DMESHUTIL_BUILD_EXAMPLES=ON`, then run
`temp_output/build/double_crossing_example`. It prints symbolic boundary loops.

## Use the connectivity library

With MeshUtil as a source dependency:

```cmake
add_subdirectory(path/to/MeshUtil)
target_link_libraries(my_application PRIVATE MeshUtil::double_crossing)
```

By default this adds the header-only target without building MeshUtil tools,
examples, or tests. Include `<meshutil/double_crossing.hpp>` in your C++ source.

For an installed package:

```bash
cmake --install temp_output/build --prefix /your/install/prefix
```

```cmake
find_package(MeshUtil CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE MeshUtil::double_crossing)
```

Set `CMAKE_PREFIX_PATH` to the install prefix. Configure with
`-DMESHUTIL_BUILD_TOOLS=OFF -DBUILD_TESTING=OFF` for a headers/package-only
installation. The [connectivity guide](doc/double_crossing/README.md) documents
the API, 1,024 B / 348 B table formats, provenance, and geometric limitations.

## Repository layout

```text
apps/                    Command-line entry points
src/decimate/            Private decimator implementation and PLY I/O
include/meshutil/         Public C++ headers and generated connectivity tables
examples/                Small usage examples
tests/                   Connectivity checks
tools/                   Offline generators
cmake/                   Installed-package configuration
doc/                    Module guides and development history
temp_output/             Ignored build, validation, and experiment outputs
```

Generated tables are committed so normal C++ builds do not need Python:

```bash
python3 tools/generate_double_crossing_tables.py --check
```

Edit the generator when changing templates, then regenerate its header.
