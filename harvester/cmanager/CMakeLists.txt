cmake_minimum_required(VERSION 2.8)
project(cmanager)

set(CMAKE_CXX_STANDARD 14)

find_package(Threads)
set(CMAKE_CXX_FLAGS "-pthread -std=c++11")

add_executable(cmanager cmanager.cpp)
