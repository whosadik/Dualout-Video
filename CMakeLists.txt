cmake_minimum_required(VERSION 3.20)
project(dualout LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(dualout-core)
add_subdirectory(dualout-video)
