cmake_minimum_required(VERSION 3.20)
project(lob_pipeline CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(reconstruct oracle/reconstruct.cpp)

# --- Rung 1: fast parser + book core (file replay) --------------------------
add_executable(rung1
    src/rung1_replay.cpp
        src/tsc.cpp)
target_include_directories(rung1 PRIVATE third_party)
target_compile_options(rung1 PRIVATE -O2 -march=native)

add_executable(rung1_bench src/rung1_replay.cpp src/tsc.cpp)
target_compile_definitions(rung1_bench PRIVATE MEASURE NDEBUG)
target_include_directories(rung1_bench PRIVATE third_party)
target_compile_options(rung1_bench PRIVATE -O2 -march=native)

# --- Rung 2: MoldUDP64 over loopback, blocking recv --------------------------
add_executable(rung2_sender src/rung2_sender.cpp)
target_compile_options(rung2_sender PRIVATE -O2 -march=native)

add_executable(rung2 src/rung2_replay.cpp src/tsc.cpp)
target_include_directories(rung2 PRIVATE third_party)
target_compile_options(rung2 PRIVATE -O2 -march=native)

add_executable(rung2_bench src/rung2_replay.cpp src/tsc.cpp)
target_compile_definitions(rung2_bench PRIVATE MEASURE NDEBUG)
target_include_directories(rung2_bench PRIVATE third_party)
target_compile_options(rung2_bench PRIVATE -O2 -march=native)

# Lightweight queueing-latency probe (SO_TIMESTAMPNS) for the stress chart (Fig 5a).
add_executable(rung2_qbench src/rung2_replay.cpp src/tsc.cpp)
target_compile_definitions(rung2_qbench PRIVATE QTIME NDEBUG)
target_include_directories(rung2_qbench PRIVATE third_party)
target_compile_options(rung2_qbench PRIVATE -O2 -march=native)

# --- Rung 3: epoll readiness + recvmmsg (two syscalls per batch) -------------
add_executable(rung3 src/rung3_replay.cpp src/tsc.cpp)
target_include_directories(rung3 PRIVATE third_party)
target_compile_options(rung3 PRIVATE -O2 -march=native)

add_executable(rung3_bench src/rung3_replay.cpp src/tsc.cpp)
target_compile_definitions(rung3_bench PRIVATE MEASURE NDEBUG)
target_include_directories(rung3_bench PRIVATE third_party)
target_compile_options(rung3_bench PRIVATE -O2 -march=native)

add_executable(rung3_qbench src/rung3_replay.cpp src/tsc.cpp)
target_compile_definitions(rung3_qbench PRIVATE QTIME NDEBUG)
target_include_directories(rung3_qbench PRIVATE third_party)
target_compile_options(rung3_qbench PRIVATE -O2 -march=native)

# --- Rung 4A: SO_BUSY_POLL + spinning recvmmsg(MSG_DONTWAIT) ------------------
add_executable(rung4a src/rung4a_replay.cpp src/tsc.cpp)
target_include_directories(rung4a PRIVATE third_party)
target_compile_options(rung4a PRIVATE -O2 -march=native)

add_executable(rung4a_bench src/rung4a_replay.cpp src/tsc.cpp)
target_compile_definitions(rung4a_bench PRIVATE MEASURE NDEBUG)
target_include_directories(rung4a_bench PRIVATE third_party)
target_compile_options(rung4a_bench PRIVATE -O2 -march=native)

add_executable(rung4a_qbench src/rung4a_replay.cpp src/tsc.cpp)
target_compile_definitions(rung4a_qbench PRIVATE QTIME NDEBUG)
target_include_directories(rung4a_qbench PRIVATE third_party)
target_compile_options(rung4a_qbench PRIVATE -O2 -march=native)

# --- Rung 4B: io_uring multishot recv + provided buffer ring (+ SQPOLL) ------
add_executable(rung4b src/rung4b_replay.cpp src/tsc.cpp)
target_include_directories(rung4b PRIVATE third_party)
target_compile_options(rung4b PRIVATE -O2 -march=native)
target_link_libraries(rung4b PRIVATE uring)

add_executable(rung4b_bench src/rung4b_replay.cpp src/tsc.cpp)
target_compile_definitions(rung4b_bench PRIVATE MEASURE NDEBUG)
target_include_directories(rung4b_bench PRIVATE third_party)
target_compile_options(rung4b_bench PRIVATE -O2 -march=native)
target_link_libraries(rung4b_bench PRIVATE uring)

# --- Combined QTIME+MEASURE builds: heavier consumer (~5M/s cap) to expose the
#     queueing knee within the sender's achievable load range (Fig 5a knee demo).
add_executable(rung2_qm src/rung2_replay.cpp src/tsc.cpp)
target_compile_definitions(rung2_qm PRIVATE QTIME MEASURE NDEBUG)
target_include_directories(rung2_qm PRIVATE third_party)
target_compile_options(rung2_qm PRIVATE -O2 -march=native)

add_executable(rung3_qm src/rung3_replay.cpp src/tsc.cpp)
target_compile_definitions(rung3_qm PRIVATE QTIME MEASURE NDEBUG)
target_include_directories(rung3_qm PRIVATE third_party)
target_compile_options(rung3_qm PRIVATE -O2 -march=native)

add_executable(rung4a_qm src/rung4a_replay.cpp src/tsc.cpp)
target_compile_definitions(rung4a_qm PRIVATE QTIME MEASURE NDEBUG)
target_include_directories(rung4a_qm PRIVATE third_party)
target_compile_options(rung4a_qm PRIVATE -O2 -march=native)
