cmake_minimum_required(VERSION 3.16)
project(MeshUtil VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

option(MESHUTIL_BUILD_CLI "Build command-line tools" ON)
option(MESHUTIL_BUILD_PYTHON "Build the pybind11 Python module" OFF)

add_library(
  meshutil
  src/io.cpp
  src/library.cpp
  src/ply_io.cpp
  src/qem_decimator.cpp
)

target_compile_features(meshutil PUBLIC cxx_std_17)
target_include_directories(
  meshutil
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(meshutil PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(meshutil PRIVATE Threads::Threads)

function(meshutil_apply_compile_options target)
  target_compile_options(
    ${target}
    PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-strict-aliasing
  )
endfunction()

meshutil_apply_compile_options(meshutil)

if(MESHUTIL_BUILD_CLI)
  add_executable(meshutil_simplify src/main.cpp)
  target_link_libraries(meshutil_simplify PRIVATE meshutil)
  meshutil_apply_compile_options(meshutil_simplify)

  add_executable(standalone_decimator src/main.cpp)
  target_link_libraries(standalone_decimator PRIVATE meshutil)
  meshutil_apply_compile_options(standalone_decimator)

  add_executable(standalone_batch_runner tools/standalone_batch_runner.cpp)
  target_compile_features(standalone_batch_runner PRIVATE cxx_std_17)
  meshutil_apply_compile_options(standalone_batch_runner)
endif()

if(MESHUTIL_BUILD_PYTHON)
  find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
  find_package(pybind11 CONFIG QUIET)
  if(NOT pybind11_FOUND)
    execute_process(
      COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
      OUTPUT_VARIABLE pybind11_cmake_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE pybind11_cmake_result
    )
    if(NOT pybind11_cmake_result EQUAL 0)
      message(FATAL_ERROR "MESHUTIL_BUILD_PYTHON=ON requires pybind11")
    endif()
    set(pybind11_DIR "${pybind11_cmake_dir}")
    find_package(pybind11 CONFIG REQUIRED)
  endif()
  pybind11_add_module(meshutil_python MODULE python/bindings.cpp)
  target_link_libraries(meshutil_python PRIVATE meshutil)
  set_target_properties(meshutil_python PROPERTIES OUTPUT_NAME meshutil)
  meshutil_apply_compile_options(meshutil_python)
  if(BUILD_TESTING)
    add_test(
      NAME meshutil_python_api
      COMMAND
        "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=$<TARGET_FILE_DIR:meshutil_python>"
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python.py"
    )
  endif()
endif()

if(BUILD_TESTING)
  add_executable(meshutil_api_test tests/test_api.cpp)
  target_link_libraries(meshutil_api_test PRIVATE meshutil)
  meshutil_apply_compile_options(meshutil_api_test)
  add_test(NAME meshutil_api COMMAND meshutil_api_test)
endif()

install(
  TARGETS
    meshutil
  EXPORT MeshUtilTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)

if(MESHUTIL_BUILD_CLI)
  install(
    TARGETS meshutil_simplify standalone_decimator standalone_batch_runner
    RUNTIME DESTINATION bin
  )
endif()

if(MESHUTIL_BUILD_PYTHON)
  install(TARGETS meshutil_python LIBRARY DESTINATION .)
endif()

install(DIRECTORY include/ DESTINATION include)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/MeshUtilConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
  cmake/MeshUtilConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/MeshUtilConfig.cmake"
  INSTALL_DESTINATION lib/cmake/MeshUtil
)
install(
  EXPORT MeshUtilTargets
  NAMESPACE MeshUtil::
  DESTINATION lib/cmake/MeshUtil
)
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/MeshUtilConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/MeshUtilConfigVersion.cmake"
  DESTINATION lib/cmake/MeshUtil
)
