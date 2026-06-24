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
 * @file stress_oj2_sql.cpp
 * @brief SQL 执行压力测试 — 大量 DDL/DML/DQL 操作
 *
 * 测试方式：client-server
 * 覆盖：万条 INSERT、大批量 UPDATE/DELETE、复杂 SELECT
 */

#include "../oj/test_oj_client_server.h"

#include <chrono>

class StressSQLTest : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "stress_sql_test_db";
        ClientServerTest::SetUp();
    }

    void expect_ok(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0));
    }
};

TEST_F(StressSQLTest, INSERT_10000Rows) {
    expect_ok(send_sql("create table t(id int, name char(16), score float)"));

    auto start = std::chrono::steady_clock::now();

    for (int i = 1; i <= 10000; i++) {
        std::string sql = "insert into t values(" + std::to_string(i) + ", 'name_" +
                          std::to_string(i % 1000) + "', " + std::to_string(i * 0.1) + ")";
        expect_ok(send_sql(sql));
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::string res = send_sql("select count(*) from t");
    std::cout << "\n  [STRESS] INSERT_10000Rows: 10000 rows inserted in "
              << ms << " ms (" << (ms / 10) << " us/row)" << std::endl;
    std::cout << "  Result: " << (res.empty() ? "(no output)" : res.substr(0, 80)) << std::endl;
}

TEST_F(StressSQLTest, SELECT_WithConditions) {
    expect_ok(send_sql("create table t(id int, category int, val float)"));

    for (int i = 1; i <= 5000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " +
                           std::to_string(i % 10) + ", " + std::to_string(i * 1.0) + ")"));
    }

    auto start = std::chrono::steady_clock::now();

    for (int c = 0; c < 10; c++) {
        std::string res = send_sql("select * from t where category = " + std::to_string(c));
        EXPECT_NE(res.find("Total record(s): 500"), std::string::npos);
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] SELECT_WithConditions: 10 queries on 5000 rows in "
              << ms << " ms (" << (ms / 10) << " ms/query)" << std::endl;
}

TEST_F(StressSQLTest, UPDATE_5000Rows) {
    expect_ok(send_sql("create table t(id int, val int)"));
    for (int i = 1; i <= 5000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i) + ")"));
    }

    auto start = std::chrono::steady_clock::now();

    expect_ok(send_sql("update t set val = -1 where val > 2500"));

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::string res = send_sql("select count(*) from t where val = -1");
    std::cout << "\n  [STRESS] UPDATE_5000Rows: updated 2500 rows in "
              << ms << " ms" << std::endl;
}

TEST_F(StressSQLTest, DELETE_3000Rows) {
    expect_ok(send_sql("create table t(id int, val int)"));
    for (int i = 1; i <= 5000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i % 3) + ")"));
    }

    auto start = std::chrono::steady_clock::now();

    expect_ok(send_sql("delete from t where val = 0"));

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] DELETE_3000Rows: deleted ~1666 rows in "
              << ms << " ms" << std::endl;
}

TEST_F(StressSQLTest, MIX_ReadWriteHeavy) {
    expect_ok(send_sql("create table t(id int, val int)"));

    // Write phase
    auto wstart = std::chrono::steady_clock::now();
    for (int i = 1; i <= 3000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i) + ")"));
    }
    auto wend = std::chrono::steady_clock::now();
    auto wms = std::chrono::duration_cast<std::chrono::milliseconds>(wend - wstart).count();

    // Read + Update mix phase
    auto rstart = std::chrono::steady_clock::now();
    for (int i = 1; i <= 500; i++) {
        send_sql("select * from t where id = " + std::to_string(i));
        send_sql("update t set val = " + std::to_string(i + 1000) + " where id = " + std::to_string(i));
    }
    auto rend = std::chrono::steady_clock::now();
    auto rms = std::chrono::duration_cast<std::chrono::milliseconds>(rend - rstart).count();

    std::cout << "\n  [STRESS] MIX_ReadWriteHeavy: wrote 3000 rows in " << wms
              << " ms, mixed 1000 ops in " << rms << " ms" << std::endl;
}
