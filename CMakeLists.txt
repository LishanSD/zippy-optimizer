cmake_minimum_required(VERSION 3.16)
project(zippy_topk CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler-specific optimization flags
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
endif()

add_executable(zippy
    src/main.cpp
    src/zippy.cpp
    src/sampler.cpp
    src/group_index.cpp
    src/stratified_sampler.cpp
    src/measure_index.cpp
)

target_include_directories(zippy PRIVATE src/)
