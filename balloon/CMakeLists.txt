cmake_minimum_required(VERSION 3.0)
project(Balloon)

set(CMAKE_CXX_STANDARD 14)

find_package(Curses REQUIRED)
find_package(Threads)
include_directories(${CURSES_INCLUDE_DIR})

set(CMAKE_CXX_FLAGS -pthread)

add_executable(Balloon balloon.cpp curses.cpp handlers.cpp balloon.h)
target_link_libraries(Balloon ${CURSES_LIBRARIES})
