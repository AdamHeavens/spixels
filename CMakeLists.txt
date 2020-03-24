cmake_minimum_required(VERSION 3.0)
project(spixels C CXX)

file(GLOB_RECURSE sources lib/*.c include/*.h)

add_library(spixels ${sources} )
