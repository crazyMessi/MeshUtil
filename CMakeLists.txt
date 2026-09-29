cmake_minimum_required(VERSION 3.16)
project(MeshUtil LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(meshutil_is_top_level OFF)
set(meshutil_default_tests OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(meshutil_is_top_level ON)
  include(CTest)
  set(meshutil_default_tests "${BUILD_TESTING}")
endif()

option(MESHUTIL_BUILD_TOOLS "Build standalone mesh command-line tools" ${meshutil_is_top_level})
option(MESHUTIL_BUILD_TESTS "Build and register MeshUtil tests" ${meshutil_default_tests})

add_library(meshutil_double_crossing INTERFACE)
add_library(MeshUtil::double_crossing ALIAS meshutil_double_crossing)
set_target_properties(meshutil_double_crossing PROPERTIES EXPORT_NAME double_crossing)
target_compile_features(meshutil_double_crossing INTERFACE cxx_std_17)
target_include_directories(
  meshutil_double_crossing
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

option(MESHUTIL_BUILD_EXAMPLES "Build mesh connectivity examples" OFF)
if(MESHUTIL_BUILD_EXAMPLES)
  add_executable(double_crossing_example examples/double_crossing.cpp)
  target_link_libraries(double_crossing_example PRIVATE MeshUtil::double_crossing)
endif()

# Respect the conventional top-level switch, including when our own option was
# cached ON. Embedded builds opt in explicitly, independently of parent tests.
if(MESHUTIL_BUILD_TESTS AND (NOT meshutil_is_top_level OR BUILD_TESTING))
  enable_testing()
  add_executable(double_crossing_test tests/double_crossing_test.cpp)
  target_link_libraries(double_crossing_test PRIVATE MeshUtil::double_crossing)
  target_compile_options(double_crossing_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME double_crossing_connectivity COMMAND double_crossing_test)
  set_tests_properties(double_crossing_connectivity PROPERTIES RUN_SERIAL TRUE)

  find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME double_crossing_tables_generated
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_double_crossing_tables.py" --check
    )
    set_tests_properties(double_crossing_tables_generated PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

if(MESHUTIL_BUILD_TOOLS)
  add_executable(
    standalone_decimator
    apps/standalone_decimator.cpp
    src/decimate/ply_io.cpp
    src/decimate/qem_decimator.cpp
  )
  target_include_directories(
    standalone_decimator PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/decimate"
  )

  set(meshutil_executable_targets standalone_decimator)
  # The batch runner uses Linux CPU-affinity APIs (sched_getaffinity/cpu_set_t).
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(
      standalone_batch_runner
      apps/standalone_batch_runner.cpp
    )
    list(APPEND meshutil_executable_targets standalone_batch_runner)
  endif()

  foreach(target IN LISTS meshutil_executable_targets)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(
      ${target}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -ffp-contract=off
        -fno-strict-aliasing
    )
  endforeach()

  install(
    TARGETS ${meshutil_executable_targets}
    RUNTIME DESTINATION bin
  )
endif()

install(TARGETS meshutil_double_crossing EXPORT MeshUtilTargets)
install(DIRECTORY include/meshutil DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(
  EXPORT MeshUtilTargets
  FILE MeshUtilTargets.cmake
  NAMESPACE MeshUtil::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MeshUtil"
)
configure_package_config_file(
  cmake/MeshUtilConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/MeshUtilConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MeshUtil"
)
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/MeshUtilConfig.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MeshUtil"
)
