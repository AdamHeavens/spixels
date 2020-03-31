cmake_minimum_required(VERSION 3.0)
project(spixels C CXX)

include_directories(${CMAKE_SOURCE_DIR}/include)

file(GLOB_RECURSE sources lib/*.c lib/*.cc include/*.h)

add_library(spixels ${sources} )
