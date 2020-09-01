cmake_minimum_required(VERSION 3.10)
project(control_loop)

set(CMAKE_CXX_STANDARD 11)

find_package(yaml-cpp REQUIRED)
include_directories(${YAML_CPP_INCLUDE_DIR})

add_executable(control_loop main.cpp config.h avl_tree.h)
target_link_libraries(control_loop ${YAML_CPP_LIBRARIES} pthread)
