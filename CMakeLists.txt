cmake_minimum_required(VERSION 3.16)
project(standalone_decimator LANGUAGES CXX)

add_executable(
  standalone_decimator
  src/main.cpp
  src/ply_io.cpp
  src/qem_decimator.cpp
)

add_executable(
  standalone_batch_runner
  tools/standalone_batch_runner.cpp
)

foreach(target standalone_decimator standalone_batch_runner)
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
  TARGETS
    standalone_decimator
    standalone_batch_runner
  RUNTIME DESTINATION bin
)
