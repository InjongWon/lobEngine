cmake_minimum_required(VERSION 3.20)
project(LobEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler flags ────────────────────────────────────────────────────────────
# -O3 -march=native: maximise auto-vectorisation and cache-friendly codegen
# -fno-omit-frame-pointer: required for `perf` stack unwinding
# -Wall -Wextra: catch latent bugs early
add_compile_options(
    -O3
    -march=native
    -fno-omit-frame-pointer
    -Wall
    -Wextra
    -Wpedantic
)

# ── Include directories ───────────────────────────────────────────────────────
include_directories(include)

# ── Main simulation executable ────────────────────────────────────────────────
add_executable(lob_sim
    src/main.cpp
    src/matching_engine.cpp
    src/coinbase_l2_book.cpp
)

# ── Google Benchmark ──────────────────────────────────────────────────────────
# Install: brew install google-benchmark  OR  apt install libbenchmark-dev
find_package(benchmark QUIET)
if(benchmark_FOUND)
    add_executable(lob_bench
        bench/bench_engine.cpp
        src/matching_engine.cpp
        src/coinbase_l2_book.cpp
    )
    target_link_libraries(lob_bench benchmark::benchmark)
    message(STATUS "Google Benchmark found — lob_bench target enabled")
else()
    message(WARNING "Google Benchmark not found — skipping lob_bench. "
                    "Install with: brew install google-benchmark")
endif()

# ── perf integration (Linux only) ─────────────────────────────────────────────
# To profile the match path:
#   perf stat -e L1-dcache-load-misses,L1-dcache-loads ./lob_sim
#   perf record -g ./lob_sim && perf report
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Linux detected — perf profiling available")
    message(STATUS "  perf stat -e L1-dcache-load-misses,L1-dcache-loads ./lob_sim")
endif()