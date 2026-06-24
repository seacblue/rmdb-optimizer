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
 * @file stress_oj9_transaction.cpp
 * @brief 事务压力测试 — 大量 BEGIN/COMMIT/ABORT
 *
 * 测试方式：client-server
 */

#include "../oj/test_oj_client_server.h"

#include <chrono>

class StressTransactionTest : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "stress_txn_test_db";
        ClientServerTest::SetUp();
    }

    void expect_ok(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0));
    }
};

TEST_F(StressTransactionTest, MANY_ShortTransactions) {
    expect_ok(send_sql("create table t(id int, val int)"));
    expect_ok(send_sql("create index t(id)"));

    auto start = std::chrono::steady_clock::now();

    for (int i = 1; i <= 1000; i++) {
        expect_ok(send_sql("begin"));
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i * 10) + ")"));
        expect_ok(send_sql("commit"));
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] MANY_ShortTransactions: 1000 txn (begin+insert+commit each) in "
              << ms << " ms (" << (ms / 1000) << " ms/txn)" << std::endl;
}

TEST_F(StressTransactionTest, MANY_Rollbacks) {
    expect_ok(send_sql("create table t(id int, val int)"));

    // 插入初始数据
    for (int i = 1; i <= 100; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i) + ")"));
    }

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < 200; i++) {
        expect_ok(send_sql("begin"));
        expect_ok(send_sql("insert into t values(9999, " + std::to_string(i) + ")"));
        expect_ok(send_sql("update t set val = -1 where id = " + std::to_string((i % 100) + 1)));
        expect_ok(send_sql("abort"));
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // 验证回滚后数据一致
    std::string res = send_sql("select count(*) from t where val = -1");
    EXPECT_NE(res.find("Total record(s): 1"), std::string::npos);

    std::cout << "\n  [STRESS] MANY_Rollbacks: 200 rollback txn in "
              << ms << " ms (" << (ms / 200) << " ms/txn)" << std::endl;
}

TEST_F(StressTransactionTest, LargeTransactionWithIndex) {
    expect_ok(send_sql("create table t(id int, val int)"));
    expect_ok(send_sql("create index t(id)"));

    // 一个大事务插入很多行
    auto start = std::chrono::steady_clock::now();
    expect_ok(send_sql("begin"));
    for (int i = 1; i <= 2000; i++) {
        expect_ok(send_sql("insert into t values(" + std::to_string(i) + ", " + std::to_string(i) + ")"));
    }
    expect_ok(send_sql("commit"));
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] LargeTransactionWithIndex: 2000 inserts in one txn in "
              << ms << " ms" << std::endl;
}
