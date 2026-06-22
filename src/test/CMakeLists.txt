# ============================================================
# RMDB 模块级单元测试
# 规则：每个底层模块对应一个独立的测试文件 + 可执行目标
# 命名：test_<module>.cpp  →  test_<缩写>
# ============================================================

# 统一的头文件搜索路径
set(TEST_INCLUDES ${CMAKE_SOURCE_DIR}/src)
# 统一的链接库：所有模块测试共享 gtest
set(TEST_LIBS gtest_main)

# ============================================================
# CTest 注册 + 测试目标
# 每个 add_executable 后紧跟 add_test 以注册到 CTest。
# CI 通过 ctest --output-on-failure 批量运行全部测试。
# ============================================================

# -------------------- LRUReplacer 测试 --------------------
# 对应源代码: src/replacer/lru_replacer.cpp
# 编译 & 运行: make test_lru -j$(nproc) && ./bin/test_lru
add_executable(test_lru test_lru_replacer.cpp)
target_link_libraries(test_lru lru_replacer ${TEST_LIBS})
target_include_directories(test_lru PRIVATE ${TEST_INCLUDES})
add_test(NAME test_lru COMMAND test_lru
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- DiskManager 测试 --------------------
# 对应源代码: src/storage/disk_manager.cpp
# 编译 & 运行: make test_dm -j$(nproc) && ./bin/test_dm
add_executable(test_dm test_disk_manager.cpp)
target_link_libraries(test_dm storage ${TEST_LIBS})
target_include_directories(test_dm PRIVATE ${TEST_INCLUDES})
add_test(NAME test_dm COMMAND test_dm
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- SmManager 测试 --------------------
# 对应源代码: src/system/sm_manager.cpp
# 依赖: system → index → storage, record → system(storage,transaction)
# 编译 & 运行: make test_sm -j$(nproc) && ./bin/test_sm
add_executable(test_sm test_system.cpp)
target_link_libraries(test_sm system index record storage lru_replacer ${TEST_LIBS})
target_include_directories(test_sm PRIVATE ${TEST_INCLUDES})
# 静态库循环依赖（record↔system）通过 --start-group 解决
target_link_options(test_sm PRIVATE -Wl,--start-group)
target_link_options(test_sm PRIVATE -Wl,--end-group)
add_test(NAME test_sm COMMAND test_sm
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- BufferPoolManager 测试 --------------------
# 对应源代码: src/storage/buffer_pool_manager.cpp
# 依赖: storage(含 disk_manager + lru_replacer)
# 编译 & 运行: make test_bpm -j$(nproc) && ./bin/test_bpm
add_executable(test_bpm test_buffer_pool_manager.cpp)
target_link_libraries(test_bpm storage lru_replacer ${TEST_LIBS})
target_include_directories(test_bpm PRIVATE ${TEST_INCLUDES})
add_test(NAME test_bpm COMMAND test_bpm
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Record 测试 (RmFileHandle + RmScan) --------------------
# 对应源代码: src/record/rm_file_handle.cpp + rm_scan.cpp
# 依赖: record → system, transaction, storage; 需 start-group 解循环
# 编译 & 运行: make test_record -j$(nproc) && ./bin/test_record
add_executable(test_record test_record.cpp)
target_link_libraries(test_record record storage lru_replacer ${TEST_LIBS})
target_include_directories(test_record PRIVATE ${TEST_INCLUDES})
# 静态库循环依赖（record↔system）通过 --start-group 解决
target_link_options(test_record PRIVATE -Wl,--start-group)
target_link_options(test_record PRIVATE -Wl,--end-group)
add_test(NAME test_record COMMAND test_record
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ============================================================
# 新模块测试添加模板
#
# add_executable(test_ix test_ix_index_handle.cpp)
# target_link_libraries(test_ix index storage lru_replacer ${TEST_LIBS})
# target_include_directories(test_ix PRIVATE ${TEST_INCLUDES})
# add_test(NAME test_ix COMMAND test_ix
#     WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
#
# add_executable(test_lock test_lock_manager.cpp)
# target_link_libraries(test_lock transaction ${TEST_LIBS})
# target_include_directories(test_lock PRIVATE ${TEST_INCLUDES})
# add_test(NAME test_lock COMMAND test_lock
#     WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
# ============================================================

# -------------------- Execution 测试 --------------------
# 对应源代码: src/execution/ 全部 8 种 Executor
# 依赖: execution → system, record, index, storage, transaction
# 编译 & 运行: make test_execution -j$(nproc) && ./bin/test_execution
add_executable(test_execution test_execution.cpp)
target_link_libraries(test_execution execution system record index storage lru_replacer ${TEST_LIBS})
target_include_directories(test_execution PRIVATE ${TEST_INCLUDES})
target_link_options(test_execution PRIVATE -Wl,--start-group)
target_link_options(test_execution PRIVATE -Wl,--end-group)
add_test(NAME test_execution COMMAND test_execution
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- 批量运行脚本 ---
# 不编译任何东西，只提供一个便捷 target 名
# 用法: make run_all_tests
# 或在构建目录中: ctest --output-on-failure
add_custom_target(run_all_tests
    COMMAND ctest --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all module tests via CTest...")
