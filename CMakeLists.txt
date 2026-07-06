cmake_minimum_required(VERSION 3.20)
project(lob_pipeline CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(reconstruct oracle/reconstruct.cpp)

# --- Rung 1: fast parser + book core (file replay) --------------------------
add_executable(rung1
    src/rung1_replay.cpp
    src/parser.cpp
    src/book.cpp
    src/tsc.cpp)
target_include_directories(rung1 PRIVATE third_party)
target_compile_options(rung1 PRIVATE -O2 -march=native)
