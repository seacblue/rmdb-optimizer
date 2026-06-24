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

# ccache support (faster rebuilds)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    message(STATUS "ccache found: ${CCACHE_PROGRAM}")
endif()

enable_testing()
add_subdirectory(src)
add_subdirectory(deps)

# ============================================================
# 打包可独立编译的生产子集（不含测试/文档/CI）
#   make package_src                 → 生成 rmdb-src.zip
#   make package_src ZIP=submit.zip  → 指定输出路径
#   ZIP=submit.zip make package_src  → 环境变量方式（等效）
# ============================================================
add_custom_target(package_src
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/package_src.sh" $(ZIP)
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Packaging production subset (excl. tests/docs/CI) ..."
    USES_TERMINAL
)
