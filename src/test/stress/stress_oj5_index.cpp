/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

/**
 * @file stress_oj5_index.cpp
 * @brief 索引压力测试 — 大量数据索引创建、查询、维护
 *
 * 测试方式：client-server
 * 报告执行时间 + 对比有无索引的性能差异
 */

#include "../oj/test_oj_client_server.h"

#include <chrono>

class StressIndexTest : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "stress_index_test_db";
        ClientServerTest::SetUp();
    }

    void expect_ok(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0));
    }
};

TEST_F(StressIndexTest, CreateIndexOnLargeTable) {
    expect_ok(send_sql("create table t(id int, val int, name char(16))"));

    for (int i = 1; i <= 10000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " +
                           std::to_string(i % 100) + ", 'item_" + std::to_string(i % 500) + "')"));
    }

    auto start = std::chrono::steady_clock::now();
    expect_ok(send_sql("create index t(id)"));
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] CreateIndexOnLargeTable: created index on 10000 rows in "
              << ms << " ms" << std::endl;
}

TEST_F(StressIndexTest, IndexQueryVsFullScan) {
    expect_ok(send_sql("create table t(id int, val int)"));

    for (int i = 1; i <= 8000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " +
                           std::to_string(i * 2) + ")"));
    }

    // 无索引查询
    double without_idx = 0;
    {
        auto start = std::chrono::steady_clock::now();
        for (int q = 0; q < 100; q++) {
            int target = 100 + q;
            send_sql("select * from t where id = " + std::to_string(target));
        }
        auto end = std::chrono::steady_clock::now();
        without_idx = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    }

    // 建索引
    expect_ok(send_sql("create index t(id)"));

    // 有索引查询
    double with_idx = 0;
    {
        auto start = std::chrono::steady_clock::now();
        for (int q = 0; q < 100; q++) {
            int target = 100 + q;
            send_sql("select * from t where id = " + std::to_string(target));
        }
        auto end = std::chrono::steady_clock::now();
        with_idx = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    }

    std::cout << "\n  [STRESS] IndexQueryVsFullScan: 100 point queries" << std::endl;
    std::cout << "    Without index: " << without_idx << " ms" << std::endl;
    std::cout << "    With index:    " << with_idx << " ms" << std::endl;
    if (without_idx > 0) {
        double ratio = (with_idx / without_idx) * 100;
        std::cout << "    Index speed:   " << ratio << "% of full-scan time" << std::endl;
    }
}

TEST_F(StressIndexTest, RangeQueryWithIndex) {
    expect_ok(send_sql("create table t(id int, val int)"));

    for (int i = 1; i <= 10000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " +
                           std::to_string(i % 1000) + ")"));
    }

    expect_ok(send_sql("create index t(id)"));

    auto start = std::chrono::steady_clock::now();

    // 范围查询
    for (int r = 0; r < 50; r++) {
        int lo = r * 100 + 1;
        int hi = (r + 1) * 100;
        send_sql("select * from t where id >= " + std::to_string(lo) + " and id <= " + std::to_string(hi));
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] RangeQueryWithIndex: 50 range queries on 10000 rows in "
              << ms << " ms (" << (ms / 50) << " ms/query)" << std::endl;
}

TEST_F(StressIndexTest, IndexMaintenanceHeavy) {
    expect_ok(send_sql("create table t(id int, val int)"));
    expect_ok(send_sql("create index t(id)"));

    // 大量插入
    auto start = std::chrono::steady_clock::now();
    for (int i = 1; i <= 5000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i) + ")"));
    }
    auto mid = std::chrono::steady_clock::now();

    // 大量更新
    for (int i = 1; i <= 2000; i++) {
        expect_ok(send_sql("update t set val = " + std::to_string(i * 10) + " where id = " + std::to_string(i)));
    }
    auto mid2 = std::chrono::steady_clock::now();

    // 大量删除
    for (int i = 3001; i <= 5000; i++) {
        expect_ok(send_sql("delete from t where id = " + std::to_string(i)));
    }

    auto end = std::chrono::steady_clock::now();

    auto insert_ms = std::chrono::duration_cast<std::chrono::milliseconds>(mid - start).count();
    auto update_ms = std::chrono::duration_cast<std::chrono::milliseconds>(mid2 - mid).count();
    auto delete_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - mid2).count();

    std::cout << "\n  [STRESS] IndexMaintenanceHeavy:" << std::endl;
    std::cout << "    Insert 5000 rows: " << insert_ms << " ms" << std::endl;
    std::cout << "    Update 2000 rows: " << update_ms << " ms" << std::endl;
    std::cout << "    Delete 2000 rows: " << delete_ms << " ms" << std::endl;
}
