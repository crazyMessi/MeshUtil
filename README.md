# MeshUtil

MeshUtil is a C++17 QEM edge-collapse mesh simplification library with C++,
Python, and command-line interfaces. It does not link to Blender and does not
require Blender, `bpy`, or Blender Python at runtime.

The current general-library branch keeps the accepted Strict V4 collapse engine
as its baseline backend while separating the algorithm, file I/O, public API,
Python binding, and CLI.

## C++ API

```cpp
#include <meshutil/simplify.hpp>

meshutil::SimplifyOptions options;
options.target_faces = 1'000'000;
options.memory_mode = meshutil::MemoryMode::Balanced;
meshutil::SimplifyResult result = meshutil::simplify(mesh.view(), options);
```

`MeshView` accepts float32 positions and uint32 triangle indices with explicit
counts and byte strides. The input remains owned by the caller. `SimplifyResult`
owns contiguous output arrays and reports topology decisions together with the
core algorithm wall time.

## Python API

```python
import meshutil

out_vertices, out_faces, stats = meshutil.simplify(
    vertices, faces, target_faces=1_000_000, threads=1, memory_mode="balanced"
)
```

The Python API accepts C-contiguous NumPy arrays with shapes `[N, 3]` and
`[M, 3]`, dtypes `float32` and `uint32`. It releases the GIL during
simplification.

General V2 can run one partition-local epoch before global cleanup:

```python
out_vertices, out_faces, stats = meshutil.simplify(
    vertices,
    faces,
    target_faces=3_000_000,
    threads=32,
    partition_local_count=128,
    partition_local_target_faces=3_000_000,
)
```

`partition_local_count` accepts `16`, `32`, `64`, or `128`.
`partition_local_target_faces` must be between the final target and the input
face count. The local epoch keeps the existing QEM collapse implementation,
protects partition halos, then rebuilds one global heap for exact-target
cleanup. `threads` controls only how fixed partitions are scheduled; partition
count, quotas, local heap order, and output remain fixed. The implementation
uses private heap entries and scratch per worker while sharing one
`EdgeId -> heap position` array, so RSS does not grow with the worker count.
Initial edge maps use at most the same requested thread count.

Validated performance presets for a 3,000,000-face target are:

- about 18M input faces: `partition_local_count=128`,
  `partition_local_target_faces=3_000_000`, `threads=32`;
- about 31M input faces: `partition_local_count=32`,
  `partition_local_target_faces=3_050_000`, `threads=32`.

For deeper reduction of the validated 18M-face case:

- 1,000,000 faces: `partition_local_count=16`,
  `partition_local_target_faces=1_000_000`, `threads=16`;
- 300,000 faces: the same settings with
  `partition_local_max_epochs=2`.

`partition_local_max_epochs` defaults to `1`. Additional epochs rebuild the
Morton ownership and local heaps on the current live topology before the final
global cleanup.

## Implemented

- strict `binary_little_endian 1.0` PLY input and output;
- exactly three `float` vertex properties (`x`, `y`, `z`);
- exactly one `property list uchar int`, `int32`, `uint`, or `uint32`
  `vertex_indices` face property; output uses `uint`;
- triangle-only meshes with finite coordinates and valid signed-int32 indices;
- stable, never-renumbered 32-bit vertex, edge, and face slots internally;
- indexed min-heap with `insert`, `update`, `remove`, and `pop`;
- double-precision symmetric quadrics and optimized contraction targets;
- midpoint fallback when the 3x3 quadric solve is singular at epsilon `1e-8`;
- boundary constraint planes with weight `100`;
- Blender-style near-zero QEM topology fallback at cost epsilon `1e-12`;
- manifold/boundary edge eligibility, Blender-style topology rejection, and
  face flip/near-degeneracy rejection;
- Blender-style float heap costs with cost-only comparisons, including
  equal-cost insertion bubbling and removal behavior;
- Blender 4.0.2 `BKE_mesh_calc_edges` initial edge ordering: `OrderedEdge`
  normalization/hash, 50% reserve capacity, Python probing, and ascending slot
  serialization; meshes below 1000 faces use one map, while larger meshes use
  eight maps bucketed by `v_low & 7` and serialized map-major;
- Blender-compatible BMesh disk/radial ordering, edge splicing, local normal
  interpolation, and heap update order after a collapse;
- duplicate and zero-area input-face behavior compatible with the Blender 4.0.2
  Mesh-to-BMesh path used by the pipeline;
- float/double operation boundaries and QEM expression order matching Blender's
  collapse implementation;
- atomic PLY output through a same-directory temporary file and rename;
- optional JSONL candidate/collapse trace.

## Build and Install

From the repository root, build the Release library and binaries in `build/`:

```bash
./build.sh
```

Or invoke CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

To include the Python module:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DMESHUTIL_BUILD_PYTHON=ON
cmake --build build --parallel
```

To install the C++ package, headers, CLI, and any enabled Python module:

```bash
cmake --install build --prefix "$HOME/.local"
```

Downstream CMake projects can use:

```cmake
find_package(MeshUtil CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE MeshUtil::meshutil)
```

To build and install a Python wheel:

```bash
python3 -m pip install .
```

CMake 3.16 or newer and a C++17 compiler are required. Python builds additionally
require pybind11 and NumPy; wheel builds obtain pybind11 through the declared
build dependencies.

## Run

```bash
./build/meshutil_simplify \
  --input /path/to/input.ply \
  --output /path/to/output.obj \
  --target-faces 3000000 \
  --memory-mode balanced \
  --trace /path/to/output.trace.jsonl
```

`--trace` is optional. The program prints one JSON summary to stdout. Exit codes
are `0` for reaching the requested face count, `1` for processing/I/O failures,
`2` for invalid CLI arguments, and `3` when no valid collapse remains before
the target.

The current format layer supports strict binary little-endian triangle PLY and
geometry-only OBJ. OBJ polygon faces are triangulated with a fan. The legacy
`standalone_decimator` executable remains available as a compatibility alias for
`meshutil_simplify`.

`memory_mode` and `--memory-mode` accept:

- `balanced` (default): preserve the larger initial edge-map reserve for the
  best single-thread throughput;
- `low`: use a tighter triangle-mesh edge estimate. On the validated
  17.9-million-face case this reduced peak RSS from 4.14 GiB to 3.39 GiB, with
  a small initialization-time trade-off.

## Continuous Batch Runner

`standalone_batch_runner` keeps a fixed-size process pool full until a TSV
manifest is exhausted. Each child is launched directly with `fork` and
`execvp`, without a shell, so spaces in paths are safe. The manifest has four
tab-separated fields:

```text
index	input_path	output_path	expected_sha256
0	/path with spaces/source.ply	/path with spaces/result.ply	0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

The header is optional. Blank lines and lines beginning with `#` are ignored.
Indices and output paths must be unique, and `expected_sha256` must contain 64
hexadecimal characters. Tabs and newlines cannot occur inside a field.

```bash
./build/standalone_batch_runner \
  --binary ./build/standalone_decimator \
  --manifest /path/to/jobs.tsv \
  --workers 16 \
  --target-faces 3000000 \
  --result-jsonl /path/to/results.jsonl \
  --summary-json /path/to/summary.json \
  --log-dir /path/to/logs
```

`--log-dir` is optional. Without it, each child log is written beside its output
as `<output_path>.log`. Child stdout and stderr share that regular file, so the
parent never blocks on a pipe. An existing output is removed before launch to
prevent a failed job from validating stale data. The runner rejects input/output
identity and collisions between job outputs/logs and its binary, manifest, or
result files.

The JSONL contains one durable, flushed record per completed or unstarted job:
timestamps, duration, exit code or terminating signal, output and expected
SHA-256, SHA match status, strict PLY-header face count, log path, and any
inspection error. The summary is written through a same-directory temporary
file and atomically renamed. It separates:

- `processing_wall_seconds`: continuous mesh-processing wall time, ending when
  the last decimator child exits;
- `validation_wall_seconds`: later SHA-256 and PLY-header validation time;
- `total_wall_seconds`: both phases combined.

Jobs/hour is based on processing wall time. The runner pins one worker process
per physical core, interleaving CPU sockets before using SMT siblings. The
summary also includes successful jobs, SHA matches, fully validated jobs,
p50/p95 child duration, selected CPUs, worker count, and peak parent RSS when
`getrusage` provides it.

A job is fully validated only when the child exits `0`, its output can be hashed
and parsed as binary little-endian PLY, and its SHA-256 matches the manifest.
Individual failures do not stop later jobs, but any non-validated job makes the
runner exit nonzero. On `SIGINT` or `SIGTERM`, the runner stops launching work,
sends the signal to every child process group, escalates to `SIGKILL` after five
seconds, reaps all children, records unstarted jobs, atomically writes the
summary, and exits nonzero. On Linux, children also request a parent-death
`SIGTERM` so an unexpectedly lost runner does not leave the decimator running.

## Validated Compatibility

The reader rejects ASCII or big-endian PLY, extra elements/properties, alternate
numeric property types other than the four supported 32-bit face-index aliases,
non-triangles, negative signed indices, out-of-range indices, repeated vertices
within a face, non-finite coordinates, and trailing payload bytes. Duplicate
faces and zero-area faces are accepted because the reference Blender pipeline
accepts them.

The frozen Version 4 release was exported from accepted commit
`f681987c3e6615790499297245f4a8ace8501290` and validated against Blender 4.0.2
commit `9be62e85b7270d3d2e5bcc846420b91bab3988f9`.

The fixed acceptance set contains 128 meshes, each independently reduced to
3,000,000 requested faces. Outputs contain 2,999,999 to 3,000,000 faces,
matching Blender's float-ratio behavior. Version 4 results were:

| Workers | Processing wall | Blender-matching SHA-256 | OOM kills |
| ---: | ---: | ---: | ---: |
| 8 | 74.48 s | 128/128 | 0 |
| 16 | 38.25 s | 128/128 | 0 |
| 32 | 40.73 s | 128/128 | 0 |

Sixteen workers are recommended for this validated workload. SHA validation is
reported as a separate post-processing phase and is not included in processing
wall time. The fixed tiny differential corpus also matches 12/12 final PLY
SHA-256.

## Scope

- UVs, normals, colors, materials, custom data, vertex groups, shape keys,
  symmetry, delimiters, triangulation/rejoining, and vertex weights are absent.
- Input and output are geometry-only strict binary triangle PLY files.
- Compatibility is pinned to Blender 4.0.2 and the tested modifier/export
  settings; a different Blender revision requires repeating differential
  validation.
- There is no in-mesh parallel collapse. Parallelism is expected across
  independent process-level mesh jobs.
- Trace output itself is not atomic; only the requested output PLY is atomic.
