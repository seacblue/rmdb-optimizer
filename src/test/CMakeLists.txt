# ============================================================
# RMDB 测试结构
#
# 测试按类别分为两个目录：
#   oj/      —— OJ 接口兼容性测试（兼冒烟测试）
#              覆盖题目描述中明确列出的全部接口
#              编译快、运行快，CI 入口首选
#   stress/  —— 压力/性能测试
#              大数据量、并发、边界压力场景
#              需要较长的运行时间
# ============================================================

# 统一的头文件搜索路径
set(TEST_INCLUDES ${CMAKE_SOURCE_DIR}/src)
# 统一的链接库
set(TEST_LIBS gtest_main)
set(CS_TEST_LIBS gtest_main pthread)  # client-server tests

# ============================================================
# CTest 注册 + 测试目标
# 每个 add_executable 后紧跟 add_test 以注册到 CTest。
# CI 通过 ctest --output-on-failure 批量运行全部测试。
# ============================================================

# ==================== OJ/冒烟测试 (oj/) ====================

# -------------------- Problem 1: 存储管理 OJ 测试 --------------------
# 使用直接 API 调用（无需 server）
add_executable(test_oj1 oj/test_oj1.cpp)
target_link_libraries(test_oj1 record storage lru_replacer ${TEST_LIBS})
target_include_directories(test_oj1 PRIVATE ${TEST_INCLUDES})
target_link_options(test_oj1 PRIVATE -Wl,--start-group)
target_link_options(test_oj1 PRIVATE -Wl,--end-group)
add_test(NAME test_oj1 COMMAND test_oj1
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Problem 2: SQL 执行 OJ 测试 --------------------
# 使用 client-server 方式
add_executable(test_oj2 oj/test_oj2.cpp)
target_link_libraries(test_oj2 ${CS_TEST_LIBS})
target_include_directories(test_oj2 PRIVATE ${TEST_INCLUDES})
add_test(NAME test_oj2 COMMAND test_oj2
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Problem 3: BIGINT 类型 OJ 测试 --------------------
add_executable(test_oj3 oj/test_oj3.cpp)
target_link_libraries(test_oj3 ${CS_TEST_LIBS})
target_include_directories(test_oj3 PRIVATE ${TEST_INCLUDES})
add_test(NAME test_oj3 COMMAND test_oj3
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Problem 4: DATETIME 类型 OJ 测试 --------------------
add_executable(test_oj4 oj/test_oj4.cpp)
target_link_libraries(test_oj4 ${CS_TEST_LIBS})
target_include_directories(test_oj4 PRIVATE ${TEST_INCLUDES})
add_test(NAME test_oj4 COMMAND test_oj4
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Problem 5: B+Tree 索引 OJ 测试 --------------------
add_executable(test_oj5 oj/test_oj5.cpp)
target_link_libraries(test_oj5 ${CS_TEST_LIBS})
target_include_directories(test_oj5 PRIVATE ${TEST_INCLUDES})
add_test(NAME test_oj5 COMMAND test_oj5
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Problem 6: 聚合函数 OJ 测试 --------------------
add_executable(test_oj6 oj/test_oj6.cpp)
target_link_libraries(test_oj6 ${CS_TEST_LIBS})
target_include_directories(test_oj6 PRIVATE ${TEST_INCLUDES})
add_test(NAME test_oj6 COMMAND test_oj6
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Problem 7: ORDER BY + LIMIT OJ 测试 --------------------
add_executable(test_oj7 oj/test_oj7.cpp)
target_link_libraries(test_oj7 ${CS_TEST_LIBS})
target_include_directories(test_oj7 PRIVATE ${TEST_INCLUDES})
add_test(NAME test_oj7 COMMAND test_oj7
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Problem 8: 块嵌套循环连接 OJ 测试 --------------------
add_executable(test_oj8 oj/test_oj8.cpp)
target_link_libraries(test_oj8 ${CS_TEST_LIBS})
target_include_directories(test_oj8 PRIVATE ${TEST_INCLUDES})
add_test(NAME test_oj8 COMMAND test_oj8
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Problem 9: 事务控制 OJ 测试 --------------------
add_executable(test_oj9 oj/test_oj9.cpp)
target_link_libraries(test_oj9 ${CS_TEST_LIBS})
target_include_directories(test_oj9 PRIVATE ${TEST_INCLUDES})
add_test(NAME test_oj9 COMMAND test_oj9
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Problem 10: 并发控制 OJ 测试 --------------------
add_executable(test_oj10 oj/test_oj10.cpp)
target_link_libraries(test_oj10 ${CS_TEST_LIBS})
target_include_directories(test_oj10 PRIVATE ${TEST_INCLUDES})
add_test(NAME test_oj10 COMMAND test_oj10
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- Problem 11: 故障恢复 OJ 测试 --------------------
add_executable(test_oj11 oj/test_oj11.cpp)
target_link_libraries(test_oj11 ${CS_TEST_LIBS})
target_include_directories(test_oj11 PRIVATE ${TEST_INCLUDES})
add_test(NAME test_oj11 COMMAND test_oj11
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ==================== 压力测试 (stress/) ====================

# 注：压力测试需要较长时间，默认不加入快速 CI 检查。
# 在 CI 中可以通过 ctest -R stress -j1 单独运行。

# -------------------- 存储模块压力测试 --------------------
add_executable(stress_oj1_storage stress/stress_oj1_storage.cpp)
target_link_libraries(stress_oj1_storage storage lru_replacer ${TEST_LIBS})
target_include_directories(stress_oj1_storage PRIVATE ${TEST_INCLUDES})
target_link_options(stress_oj1_storage PRIVATE -Wl,--start-group)
target_link_options(stress_oj1_storage PRIVATE -Wl,--end-group)
add_test(NAME stress_oj1_storage COMMAND stress_oj1_storage
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- SQL 执行压力测试 --------------------
add_executable(stress_oj2_sql stress/stress_oj2_sql.cpp)
target_link_libraries(stress_oj2_sql ${CS_TEST_LIBS})
target_include_directories(stress_oj2_sql PRIVATE ${TEST_INCLUDES})
add_test(NAME stress_oj2_sql COMMAND stress_oj2_sql
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- BIGINT 压力测试 --------------------
add_executable(stress_oj3_bigint stress/stress_oj3_bigint.cpp)
target_link_libraries(stress_oj3_bigint ${CS_TEST_LIBS})
target_include_directories(stress_oj3_bigint PRIVATE ${TEST_INCLUDES})
add_test(NAME stress_oj3_bigint COMMAND stress_oj3_bigint
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- DATETIME 压力测试 --------------------
add_executable(stress_oj4_datetime stress/stress_oj4_datetime.cpp)
target_link_libraries(stress_oj4_datetime ${CS_TEST_LIBS})
target_include_directories(stress_oj4_datetime PRIVATE ${TEST_INCLUDES})
add_test(NAME stress_oj4_datetime COMMAND stress_oj4_datetime
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- 索引压力测试 --------------------
add_executable(stress_oj5_index stress/stress_oj5_index.cpp)
target_link_libraries(stress_oj5_index ${CS_TEST_LIBS})
target_include_directories(stress_oj5_index PRIVATE ${TEST_INCLUDES})
add_test(NAME stress_oj5_index COMMAND stress_oj5_index
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- 聚合函数压力测试 --------------------
add_executable(stress_oj6_aggregate stress/stress_oj6_aggregate.cpp)
target_link_libraries(stress_oj6_aggregate ${CS_TEST_LIBS})
target_include_directories(stress_oj6_aggregate PRIVATE ${TEST_INCLUDES})
add_test(NAME stress_oj6_aggregate COMMAND stress_oj6_aggregate
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- ORDER BY 压力测试 --------------------
add_executable(stress_oj7_orderby stress/stress_oj7_orderby.cpp)
target_link_libraries(stress_oj7_orderby ${CS_TEST_LIBS})
target_include_directories(stress_oj7_orderby PRIVATE ${TEST_INCLUDES})
add_test(NAME stress_oj7_orderby COMMAND stress_oj7_orderby
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- 连接压力测试 --------------------
add_executable(stress_oj8_join stress/stress_oj8_join.cpp)
target_link_libraries(stress_oj8_join ${CS_TEST_LIBS})
target_include_directories(stress_oj8_join PRIVATE ${TEST_INCLUDES})
add_test(NAME stress_oj8_join COMMAND stress_oj8_join
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- 事务压力测试 --------------------
add_executable(stress_oj9_transaction stress/stress_oj9_transaction.cpp)
target_link_libraries(stress_oj9_transaction ${CS_TEST_LIBS})
target_include_directories(stress_oj9_transaction PRIVATE ${TEST_INCLUDES})
add_test(NAME stress_oj9_transaction COMMAND stress_oj9_transaction
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- 并发压力测试 --------------------
add_executable(stress_oj10_concurrency stress/stress_oj10_concurrency.cpp)
target_link_libraries(stress_oj10_concurrency ${CS_TEST_LIBS})
target_include_directories(stress_oj10_concurrency PRIVATE ${TEST_INCLUDES})
add_test(NAME stress_oj10_concurrency COMMAND stress_oj10_concurrency
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# -------------------- 故障恢复压力测试 --------------------
add_executable(stress_oj11_recovery stress/stress_oj11_recovery.cpp)
target_link_libraries(stress_oj11_recovery ${CS_TEST_LIBS})
target_include_directories(stress_oj11_recovery PRIVATE ${TEST_INCLUDES})
add_test(NAME stress_oj11_recovery COMMAND stress_oj11_recovery
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})


# ============================================================
# 便捷运行目标
# 用法: make run_oj_tests      — 运行所有 OJ 测试
#       make run_stress_tests  — 运行所有压力测试
#       ctest --output-on-failure   — 运行全部
# ============================================================

add_custom_target(run_oj_tests
    COMMAND ctest -R "^test_oj" --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all OJ tests via CTest...")

add_custom_target(run_stress_tests
    COMMAND ctest -R "^stress" --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all stress tests via CTest...")

add_custom_target(run_all_tests
    COMMAND ctest --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all tests via CTest...")
