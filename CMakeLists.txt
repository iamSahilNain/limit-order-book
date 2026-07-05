cmake_minimum_required(VERSION 3.16)
project(limit_order_book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# -march=native matters for the cache/vectorization story. Drop it if you need the
# binary to run on a different CPU than the one you build on.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

add_library(lob
  src/map_order_book.cpp
  src/array_order_book.cpp)
target_include_directories(lob PUBLIC include)

add_executable(bench bench/benchmark.cpp)
target_include_directories(bench PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench PRIVATE lob)

enable_testing()
add_executable(tests tests/test_order_book.cpp)
target_link_libraries(tests PRIVATE lob)
add_test(NAME unit COMMAND tests)
