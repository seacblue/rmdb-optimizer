# ============================================================
# EnableCoverage.cmake
# 用途：在启用 ENABLE_COVERAGE=ON 时添加 gcov 覆盖率编译标志
# 用法：在根 CMakeLists.txt 中：
#   option(ENABLE_COVERAGE "Enable coverage profiling" OFF)
#   if(ENABLE_COVERAGE)
#       include(cmake/EnableCoverage.cmake)
#   endif()
# ============================================================

# 只在 GCC/Clang 上支持
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "Coverage profiling ENABLED")

    # 全局编译标志
    add_compile_options(-g -O0 -fprofile-arcs -ftest-coverage)
    # 全局链接标志
    add_link_options(-fprofile-arcs -ftest-coverage)

    message(STATUS "  Compile flags: -g -O0 -fprofile-arcs -ftest-coverage")
    message(STATUS "  Link flags:    -fprofile-arcs -ftest-coverage")
else()
    message(WARNING "Coverage not supported for compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()
