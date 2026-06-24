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
 * @file test_oj6.cpp
 * @brief OJ Problem 6 — 聚合函数（SUM, MAX, MIN, COUNT）
 *
 * 测试方式：client-server
 */

#include "test_oj_client_server.h"

class OJ6Test : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "oj6_aggregate_test_db";
        ClientServerTest::SetUp();
    }
};

TEST_F(OJ6Test, SumInt) {
    expect_success(send_sql("create table aggregate (id int,val float)"));
    expect_success(send_sql("insert into aggregate values(1,5.5)"));
    expect_success(send_sql("insert into aggregate values(3,4.5)"));
    expect_success(send_sql("insert into aggregate values(5,10.0)"));

    std::string res = send_sql("select SUM(id) as sum_id from aggregate");
    expect_total_records(res, 1);
    expect_output_contains(res, "9");
}

TEST_F(OJ6Test, SumFloat) {
    expect_success(send_sql("create table aggregate (id int,val float)"));
    expect_success(send_sql("insert into aggregate values(1,5.5)"));
    expect_success(send_sql("insert into aggregate values(3,4.5)"));
    expect_success(send_sql("insert into aggregate values(5,10.0)"));

    std::string res = send_sql("select SUM(val) as sum_val from aggregate");
    expect_total_records(res, 1);
    expect_output_contains(res, "20.000000");
}

TEST_F(OJ6Test, Max) {
    expect_success(send_sql("create table aggregate (id int,val float)"));
    expect_success(send_sql("insert into aggregate values(1,5.5)"));
    expect_success(send_sql("insert into aggregate values(3,4.5)"));
    expect_success(send_sql("insert into aggregate values(5,10.0)"));

    std::string res = send_sql("select MAX(id) as max_id from aggregate");
    expect_total_records(res, 1);
    expect_output_contains(res, "5");
}

TEST_F(OJ6Test, Min) {
    expect_success(send_sql("create table aggregate (id int,val float)"));
    expect_success(send_sql("insert into aggregate values(1,5.5)"));
    expect_success(send_sql("insert into aggregate values(3,4.5)"));
    expect_success(send_sql("insert into aggregate values(5,10.0)"));

    std::string res = send_sql("select MIN(val) as min_val from aggregate");
    expect_total_records(res, 1);
    expect_output_contains(res, "4.500000");
}

TEST_F(OJ6Test, CountStar) {
    expect_success(send_sql("create table aggregate (id int,name char(8),val float)"));
    expect_success(send_sql("insert into aggregate values (1,'qwerasdf',1.0)"));
    expect_success(send_sql("insert into aggregate values (2,'qwerasdf',2.0)"));
    expect_success(send_sql("insert into aggregate values (3,'uiophjkl',2.0)"));

    std::string res = send_sql("select COUNT(*) as count_row from aggregate");
    expect_total_records(res, 1);
    expect_output_contains(res, "3");
}

TEST_F(OJ6Test, CountColumn) {
    expect_success(send_sql("create table aggregate (id int,name char(8),val float)"));
    expect_success(send_sql("insert into aggregate values (1,'qwerasdf',1.0)"));
    expect_success(send_sql("insert into aggregate values (2,'qwerasdf',2.0)"));
    expect_success(send_sql("insert into aggregate values (3,'uiophjkl',2.0)"));

    std::string res = send_sql("select COUNT(id) as count_id from aggregate");
    expect_total_records(res, 1);
    expect_output_contains(res, "3");

    res = send_sql("select COUNT(name) as count_name from aggregate where val = 2.0");
    expect_total_records(res, 1);
    expect_output_contains(res, "2");
}

TEST_F(OJ6Test, AggWithWhere) {
    expect_success(send_sql("create table t(val int)"));
    expect_success(send_sql("insert into t values(10)"));
    expect_success(send_sql("insert into t values(20)"));
    expect_success(send_sql("insert into t values(30)"));

    std::string res = send_sql("select SUM(val) from t where val > 15");
    expect_total_records(res, 1);
    expect_output_contains(res, "50");
}
