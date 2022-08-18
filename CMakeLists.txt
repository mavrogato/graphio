
cmake_minimum_required(VERSION 3.14)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_COMPILER icpx)

project(graphio)
find_package(IntelDPCPP REQUIRED)

add_executable(graphio
  main.cc)

target_compile_options(graphio
  PRIVATE
  -std=c++2b)

target_link_libraries(graphio
  PRIVATE
  wayland-client)

add_custom_target(run
  DEPENDS graphio
  COMMAND WAYLAND_DEBUG=1 ./graphio)
