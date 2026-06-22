cmake_minimum_required(VERSION 3.16)
project(RMDB)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "-Wall -O0 -g -ggdb3")
# set(CMAKE_CXX_FLAGS "-Wall -O3")

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g")

# ============================================================
# 覆盖率支持（仅 CI 启用）
# 本地构建默认关闭： cmake .. -DENABLE_COVERAGE=OFF
# CI 构建：          cmake .. -DENABLE_COVERAGE=ON
# ============================================================
option(ENABLE_COVERAGE "Enable gcov coverage profiling" OFF)
if(ENABLE_COVERAGE)
    include(cmake/EnableCoverage.cmake)
endif()

enable_testing()
add_subdirectory(src)
add_subdirectory(deps)
