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
 * @file stress_oj8_join.cpp
 * @brief 连接压力测试 — BNLJ 大表连接性能
 *
 * 测试方式：client-server
 */

#include "../oj/test_oj_client_server.h"

#include <chrono>

class StressJoinTest : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "stress_join_test_db";
        ClientServerTest::SetUp();
    }

    void expect_ok(const std::string &res) {
        EXPECT_TRUE(res.empty() || (res.find("Error: ") != 0 && res.find("failure") != 0));
    }
};

TEST_F(StressJoinTest, EquiJoinLargeTables) {
    expect_ok(send_sql("create table t1(id int, val int)"));
    expect_ok(send_sql("create table t2(t_id int, descr char(16))"));

    for (int i = 1; i <= 2000; i++) {
        expect_ok(send_sql("insert into t1 values(" + std::to_string(i) + ", " + std::to_string(i * 10) + ")"));
        expect_ok(send_sql("insert into t2 values(" + std::to_string(i) + ", 'desc_" + std::to_string(i) + "')"));
    }

    auto start = std::chrono::steady_clock::now();
    std::string res = send_sql("select * from t1, t2 where t1.id = t2.t_id");
    auto end = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_NE(res.find("Total record(s): 2000"), std::string::npos);
    std::cout << "\n  [STRESS] EquiJoinLargeTables: 2000x2000 equi-join in "
              << ms << " ms" << std::endl;
}

TEST_F(StressJoinTest, NonEquiJoinLarge) {
    expect_ok(send_sql("create table t1(id int, val int)"));
    expect_ok(send_sql("create table t2(t_id int, descr char(16))"));

    for (int i = 1; i <= 1000; i++) {
        expect_ok(send_sql("insert into t1 values(" + std::to_string(i) + ", " + std::to_string(i) + ")"));
        expect_ok(send_sql("insert into t2 values(" + std::to_string(i * 2) + ", 'item" + std::to_string(i) + "')"));
    }

    auto start = std::chrono::steady_clock::now();
    std::string res = send_sql("select * from t1, t2 where t1.id < t2.t_id and t2.t_id < 300");
    auto end = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] NonEquiJoinLarge: 1000x1000 non-equi join in "
              << ms << " ms" << std::endl;
}

TEST_F(StressJoinTest, MultiJoinWithOrderBy) {
    expect_ok(send_sql("create table a(id int, name char(8))"));
    expect_ok(send_sql("create table b(a_id int, val int)"));
    expect_ok(send_sql("create table c(b_id int, tag char(8))"));

    for (int i = 1; i <= 500; i++) {
        expect_ok(send_sql("insert into a values(" + std::to_string(i) + ", 'a" + std::to_string(i) + "')"));
        expect_ok(send_sql("insert into b values(" + std::to_string(i) + ", " + std::to_string(i * 10) + ")"));
        expect_ok(send_sql("insert into c values(" + std::to_string(i) + ", 't" + std::to_string(i) + "')"));
    }

    auto start = std::chrono::steady_clock::now();
    std::string res = send_sql("select a.name, b.val, c.tag from a, b, c where a.id = b.a_id and b.a_id = c.b_id order by a.id");
    auto end = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_NE(res.find("Total record(s): 500"), std::string::npos);
    std::cout << "\n  [STRESS] MultiJoinWithOrderBy: 3-table join (500 rows each) in "
              << ms << " ms" << std::endl;
}
