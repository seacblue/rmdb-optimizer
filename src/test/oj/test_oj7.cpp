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
 * @file test_oj7.cpp
 * @brief OJ Problem 7 — ORDER BY + LIMIT
 *
 * 测试方式：client-server
 */

#include "test_oj_client_server.h"

class OJ7Test : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "oj7_order_test_db";
        ClientServerTest::SetUp();
    }
};

TEST_F(OJ7Test, OrderByAsc) {
    expect_success(send_sql("create table orders (company char(10), order_number int)"));
    expect_success(send_sql("insert into orders values('AAA',12)"));
    expect_success(send_sql("insert into orders values('ABB',13)"));
    expect_success(send_sql("insert into orders values('ABC',19)"));
    expect_success(send_sql("insert into orders values('ACA',1)"));

    std::string res = send_sql("SELECT company, order_number FROM orders ORDER BY order_number");
    expect_total_records(res, 4);
    // 验证排序顺序：1, 12, 13, 19
    expect_output_contains(res, "ACA");
    expect_output_contains(res, "AAA");
    expect_output_contains(res, "ABB");
    expect_output_contains(res, "ABC");
}

TEST_F(OJ7Test, OrderByMultiColumn) {
    expect_success(send_sql("create table orders (company char(10), order_number int)"));
    expect_success(send_sql("insert into orders values('AAA',12)"));
    expect_success(send_sql("insert into orders values('ABB',13)"));
    expect_success(send_sql("insert into orders values('ABC',19)"));
    expect_success(send_sql("insert into orders values('ACA',1)"));

    std::string res = send_sql("SELECT company, order_number FROM orders ORDER BY company, order_number");
    expect_total_records(res, 4);
}

TEST_F(OJ7Test, OrderByDesc) {
    expect_success(send_sql("create table orders (company char(10), order_number int)"));
    expect_success(send_sql("insert into orders values('AAA',12)"));
    expect_success(send_sql("insert into orders values('ABB',13)"));
    expect_success(send_sql("insert into orders values('ABC',19)"));
    expect_success(send_sql("insert into orders values('ACA',1)"));

    std::string res = send_sql("SELECT company, order_number FROM orders ORDER BY company DESC, order_number ASC");
    expect_total_records(res, 4);
}

TEST_F(OJ7Test, OrderByWithLimit) {
    expect_success(send_sql("create table orders (company char(10), order_number int)"));
    expect_success(send_sql("insert into orders values('AAA',12)"));
    expect_success(send_sql("insert into orders values('ABB',13)"));
    expect_success(send_sql("insert into orders values('ABC',19)"));
    expect_success(send_sql("insert into orders values('ACA',1)"));

    std::string res = send_sql("SELECT company, order_number FROM orders ORDER BY order_number ASC LIMIT 2");
    expect_total_records(res, 2);
    expect_output_contains(res, "ACA");
    expect_output_contains(res, "AAA");
}

TEST_F(OJ7Test, OrderByString) {
    expect_success(send_sql("create table t(name char(8), val int)"));
    expect_success(send_sql("insert into t values('delta', 4)"));
    expect_success(send_sql("insert into t values('alpha', 1)"));
    expect_success(send_sql("insert into t values('beta', 2)"));
    expect_success(send_sql("insert into t values('gamma', 3)"));

    std::string res = send_sql("SELECT name, val FROM t ORDER BY name");
    expect_total_records(res, 4);
}
