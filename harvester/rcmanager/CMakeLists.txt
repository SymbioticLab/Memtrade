cmake_minimum_required(VERSION 2.8)
project(rcmanager)

set(CMAKE_CXX_STANDARD 14)

find_package(Threads)
set(CMAKE_CXX_FLAGS "-pthread -std=c++11")

add_executable(rcmanager rcmanager.cpp)
