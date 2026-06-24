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
 * @file stress_oj7_orderby.cpp
 * @brief ORDER BY 压力测试 — 大量数据排序
 *
 * 测试方式：client-server
 */

#include "../oj/test_oj_client_server.h"

#include <chrono>

class StressOrderByTest : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "stress_orderby_test_db";
        ClientServerTest::SetUp();
    }

    void expect_ok(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0));
    }
};

TEST_F(StressOrderByTest, OrderByLargeINT) {
    expect_ok(send_sql("create table t(id int, val int)"));

    for (int i = 1; i <= 5000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " +
                           std::to_string((i * 7) % 5000) + ")"));
    }

    auto start = std::chrono::steady_clock::now();
    std::string res = send_sql("select * from t order by val");
    auto end = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_NE(res.find("Total record(s): 5000"), std::string::npos);
    std::cout << "\n  [STRESS] OrderByLargeINT: sort 5000 rows in "
              << ms << " ms" << std::endl;
}

TEST_F(StressOrderByTest, OrderByWithLimitOffset) {
    expect_ok(send_sql("create table t(id int, val int)"));

    for (int i = 1; i <= 3000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " +
                           std::to_string((i * 13) % 3000) + ")"));
    }

    auto start = std::chrono::steady_clock::now();

    for (int q = 0; q < 50; q++) {
        send_sql("select * from t order by val limit 100");
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] OrderByWithLimitOffset: 50 ORDER BY LIMIT queries in "
              << ms << " ms" << std::endl;
}

TEST_F(StressOrderByTest, OrderByMultiColumnLarge) {
    expect_ok(send_sql("create table t(cat int, val int, name char(8))"));

    for (int i = 1; i <= 3000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i % 10) + ", " +
                           std::to_string((i * 17) % 3000) + ", 'n" + std::to_string(i % 100) + "')"));
    }

    auto start = std::chrono::steady_clock::now();
    std::string res = send_sql("select * from t order by cat, val");
    auto end = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_NE(res.find("Total record(s): 3000"), std::string::npos);
    std::cout << "\n  [STRESS] OrderByMultiColumnLarge: multi-column sort 3000 rows in "
              << ms << " ms" << std::endl;
}
