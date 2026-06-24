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
 * @file test_oj8.cpp
 * @brief OJ Problem 8 — 块嵌套循环连接（Block Nested-Loop Join）
 *
 * 测试方式：client-server
 * 覆盖：等值连接、非等值连接、大表连接
 */

#include "test_oj_client_server.h"

class OJ8Test : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "oj8_bnlj_test_db";
        ClientServerTest::SetUp();
    }
};

TEST_F(OJ8Test, EquiJoin) {
    expect_success(send_sql("create table t1(id int, val int)"));
    expect_success(send_sql("create table t2(t_id int, descr char(8))"));
    expect_success(send_sql("insert into t1 values(1, 100)"));
    expect_success(send_sql("insert into t1 values(2, 200)"));
    expect_success(send_sql("insert into t2 values(1, 'desc1')"));
    expect_success(send_sql("insert into t2 values(2, 'desc2')"));

    std::string res = send_sql("select * from t1, t2 where t1.id = t2.t_id order by t1.id");
    expect_total_records(res, 2);
}

TEST_F(OJ8Test, NonEquiJoin) {
    expect_success(send_sql("create table t1(id int, val int)"));
    expect_success(send_sql("create table t2(t_id int, descr char(8))"));
    expect_success(send_sql("insert into t1 values(1, 10)"));
    expect_success(send_sql("insert into t1 values(2, 20)"));
    expect_success(send_sql("insert into t2 values(1, 'a')"));
    expect_success(send_sql("insert into t2 values(5, 'b')"));

    std::string res = send_sql("select * from t1, t2 where t1.id < t2.t_id and t2.t_id < 1000");
    expect_total_records(res, 2);
}

TEST_F(OJ8Test, JoinThreeTables) {
    expect_success(send_sql("create table a(id int)"));
    expect_success(send_sql("create table b(a_id int)"));
    expect_success(send_sql("create table c(b_id int)"));
    expect_success(send_sql("insert into a values(1)"));
    expect_success(send_sql("insert into a values(2)"));
    expect_success(send_sql("insert into b values(1)"));
    expect_success(send_sql("insert into b values(2)"));
    expect_success(send_sql("insert into c values(1)"));
    expect_success(send_sql("insert into c values(2)"));

    std::string res = send_sql("select * from a, b, c where a.id = b.a_id and b.a_id = c.b_id");
    expect_total_records(res, 2);
}

TEST_F(OJ8Test, JoinWithWhere) {
    expect_success(send_sql("create table products(pid int, pname char(8), price float)"));
    expect_success(send_sql("create table orders(oid int, pid int, qty int)"));
    expect_success(send_sql("insert into products values(1, 'widget', 10.0)"));
    expect_success(send_sql("insert into products values(2, 'gadget', 20.0)"));
    expect_success(send_sql("insert into orders values(101, 1, 5)"));
    expect_success(send_sql("insert into orders values(102, 2, 3)"));

    std::string res = send_sql("select pname, qty from products, orders where products.pid = orders.pid and qty > 3");
    expect_total_records(res, 1);
    expect_output_contains(res, "widget");
}

TEST_F(OJ8Test, LargeJoin) {
    expect_success(send_sql("create table big1(id int, val int)"));
    expect_success(send_sql("create table big2(id int, descr char(8))"));

    // Insert 200 rows into each table
    for (int i = 1; i <= 200; i++) {
        expect_success(send_sql("insert into big1 values(" + std::to_string(i) + ", " + std::to_string(i * 10) + ")"));
        expect_success(send_sql("insert into big2 values(" + std::to_string(i) + ", 'item')"));
    }

    std::string res = send_sql("select * from big1, big2 where big1.id = big2.id");
    expect_total_records(res, 200);
}
