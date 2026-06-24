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
 * @file stress_oj3_bigint.cpp
 * @brief BIGINT 压力测试 — 大量大数运算
 *
 * 测试方式：client-server
 */

#include "../oj/test_oj_client_server.h"

#include <chrono>

class StressBigIntTest : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "stress_bigint_test_db";
        ClientServerTest::SetUp();
    }

    void expect_ok(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0));
    }
};

TEST_F(StressBigIntTest, InsertAndSumBigInt) {
    expect_ok(send_sql("create table t(id int, big_val bigint)"));

    auto start = std::chrono::steady_clock::now();

    for (int64_t i = 1; i <= 5000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i * 1000000) + ")"));
    }

    std::string res = send_sql("select SUM(big_val) from t");
    EXPECT_NE(res.find("Total record(s): 1"), std::string::npos);

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] InsertAndSumBigInt: 5000 bigint rows + SUM in "
              << ms << " ms" << std::endl;
}

TEST_F(StressBigIntTest, BigIntWhere) {
    expect_ok(send_sql("create table t(id int, big_val bigint)"));

    for (int64_t i = 1; i <= 3000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i * 1000000) + ")"));
    }

    auto start = std::chrono::steady_clock::now();

    for (int q = 0; q < 100; q++) {
        int64_t target = (q + 1) * 1000000;
        send_sql("select * from t where big_val = " + std::to_string(target));
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] BigIntWhere: 100 queries on 3000 bigint rows in "
              << ms << " ms" << std::endl;
}

TEST_F(StressBigIntTest, BigIntBoundaryMix) {
    expect_ok(send_sql("create table t(id int, big_val bigint)"));

    auto start = std::chrono::steady_clock::now();

    expect_ok(send_sql("insert into t values(1, 9223372036854775807)"));   // MAX
    expect_ok(send_sql("insert into t values(2, -9223372036854775808)"));   // MIN
    expect_ok(send_sql("insert into t values(3, 0)"));
    expect_ok(send_sql("insert into t values(4, 2147483647)"));             // INT MAX
    expect_ok(send_sql("insert into t values(5, 2147483648)"));             // INT MAX + 1

    // 混合 INT 与 BIGINT 比较
    std::string res = send_sql("select * from t where big_val > 0");
    EXPECT_NE(res.find("Total record(s): 3"), std::string::npos);

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] BigIntBoundaryMix: boundary tests in " << ms << " ms" << std::endl;
}
