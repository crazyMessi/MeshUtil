cmake_minimum_required(VERSION 3.16)
project(standalone_decimator LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

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

if(BUILD_TESTING)
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

add_executable(
  standalone_decimator
  src/main.cpp
  src/ply_io.cpp
  src/qem_decimator.cpp
)

set(meshutil_executable_targets standalone_decimator)
# The batch runner uses Linux CPU-affinity APIs (sched_getaffinity/cpu_set_t).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(
    standalone_batch_runner
    tools/standalone_batch_runner.cpp
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
