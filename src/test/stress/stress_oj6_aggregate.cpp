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
 * @file stress_oj6_aggregate.cpp
 * @brief 聚合压力测试 — 大规模聚合计算
 *
 * 测试方式：client-server
 */

#include "../oj/test_oj_client_server.h"

#include <chrono>

class StressAggregateTest : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "stress_aggregate_test_db";
        ClientServerTest::SetUp();
    }

    void expect_ok(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0));
    }
};

TEST_F(StressAggregateTest, SUM_10000Rows) {
    expect_ok(send_sql("create table t(id int, val int)"));

    for (int i = 1; i <= 10000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i) + ")"));
    }

    auto start = std::chrono::steady_clock::now();
    std::string res = send_sql("select SUM(val), MAX(val), MIN(val), COUNT(*) from t");
    auto end = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_NE(res.find("Total record(s): 1"), std::string::npos);
    std::cout << "\n  [STRESS] SUM_10000Rows: 4 aggregates on 10000 rows in "
              << ms << " ms" << std::endl;
}

TEST_F(StressAggregateTest, AggWithGrouping) {
    expect_ok(send_sql("create table sales(category int, amount float)"));

    for (int i = 1; i <= 5000; i++) {
        int cat = i % 5;
        expect_ok(send_sql("insert into sales values(" + std::to_string(cat) + ", " +
                           std::to_string(i * 1.5) + ")"));
    }

    auto start = std::chrono::steady_clock::now();
    std::string res = send_sql("select SUM(amount), MAX(amount), MIN(amount), COUNT(*) from sales where amount > 1000");
    auto end = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] AggWithGrouping: aggregates on 5000 rows (WHERE+aggregates) in "
              << ms << " ms" << std::endl;
}

TEST_F(StressAggregateTest, MultipleAggs) {
    expect_ok(send_sql("create table t(id int, val int)"));

    for (int i = 1; i <= 2000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i % 100) + ")"));
    }

    auto start = std::chrono::steady_clock::now();

    for (int q = 0; q < 50; q++) {
        send_sql("select COUNT(*), SUM(val), AVG(val), MAX(val), MIN(val) from t where id > " + std::to_string(q * 40));
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] MultipleAggs: 50 aggregate queries in "
              << ms << " ms (" << (ms / 50) << " ms/query)" << std::endl;
}
