cmake_minimum_required(VERSION 3.10)
project(raw_control_loop)

set(CMAKE_CXX_STANDARD 11)

find_package(yaml-cpp REQUIRED)
include_directories(${YAML_CPP_INCLUDE_DIR})

add_executable(raw_control_loop main.cpp config.h)
target_link_libraries(raw_control_loop ${YAML_CPP_LIBRARIES} pthread)
