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
 * @file test_oj9.cpp
 * @brief OJ Problem 9 — 事务控制（BEGIN / COMMIT / ABORT）
 *
 * 测试方式：client-server
 * 覆盖：无索引事务、有索引事务
 */

#include "test_oj_client_server.h"

class OJ9Test : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "oj9_transaction_test_db";
        ClientServerTest::SetUp();
    }
};

TEST_F(OJ9Test, AbortWithoutIndex) {
    expect_success(send_sql("create table student (id int, name char(8), score float)"));
    expect_success(send_sql("insert into student values (1, 'xiaohong', 90.0)"));

    expect_success(send_sql("begin"));
    expect_success(send_sql("insert into student values (2, 'xiaoming', 99.0)"));
    expect_success(send_sql("delete from student where id = 2"));
    expect_success(send_sql("abort"));

    std::string res = send_sql("select * from student");
    expect_total_records(res, 1);
    expect_output_contains(res, "xiaohong");
    expect_output_not_contains(res, "xiaoming");
}

TEST_F(OJ9Test, CommitWithoutIndex) {
    expect_success(send_sql("create table student (id int, name char(8), score float)"));
    expect_success(send_sql("insert into student values (1, 'alice', 85.0)"));

    expect_success(send_sql("begin"));
    expect_success(send_sql("insert into student values (2, 'bob', 92.0)"));
    expect_success(send_sql("commit"));

    std::string res = send_sql("select * from student");
    expect_total_records(res, 2);
    expect_output_contains(res, "alice");
    expect_output_contains(res, "bob");
}

TEST_F(OJ9Test, AbortWithIndex) {
    expect_success(send_sql("create table stock (s_id int, qty int, dist char(8))"));
    for (int i = 1; i <= 20; i++) {
        expect_success(send_sql("insert into stock values (" + std::to_string(i) + ", " +
                                std::to_string(i * 10) + ", 'base')"));
    }
    expect_success(send_sql("create index stock(s_id)"));

    expect_success(send_sql("begin"));
    expect_success(send_sql("insert into stock values (100, 1000, 'newone')"));
    expect_success(send_sql("update stock set qty = 777 where s_id = 10"));
    expect_success(send_sql("delete from stock where s_id = 20"));
    expect_success(send_sql("abort"));

    std::string res = send_sql("select * from stock where s_id = 100");
    expect_total_records(res, 0);

    res = send_sql("select * from stock where s_id = 10");
    expect_total_records(res, 1);
    expect_output_contains(res, "100");
    expect_output_not_contains(res, "777");

    res = send_sql("select * from stock where s_id = 20");
    expect_total_records(res, 1);
}

TEST_F(OJ9Test, CommitWithIndex) {
    expect_success(send_sql("create table t(id int, val int)"));
    expect_success(send_sql("create index t(id)"));
    expect_success(send_sql("insert into t values(1, 10)"));

    expect_success(send_sql("begin"));
    expect_success(send_sql("insert into t values(2, 20)"));
    expect_success(send_sql("commit"));

    std::string res = send_sql("select * from t where id = 2");
    expect_total_records(res, 1);
}

TEST_F(OJ9Test, RollbackKeepsOriginalState) {
    expect_success(send_sql("create table t(id int, name char(8))"));
    expect_success(send_sql("insert into t values(1, 'original')"));

    expect_success(send_sql("begin"));
    expect_success(send_sql("insert into t values(2, 'temp')"));
    expect_success(send_sql("abort"));

    // Should only have the original row
    std::string res = send_sql("select * from t");
    expect_total_records(res, 1);
    expect_output_contains(res, "original");
}

TEST_F(OJ9Test, MultipleTransactions) {
    expect_success(send_sql("create table t(id int, val int)"));

    expect_success(send_sql("begin"));
    expect_success(send_sql("insert into t values(1, 100)"));
    expect_success(send_sql("commit"));

    expect_success(send_sql("begin"));
    expect_success(send_sql("insert into t values(2, 200)"));
    expect_success(send_sql("abort"));

    expect_success(send_sql("begin"));
    expect_success(send_sql("insert into t values(3, 300)"));
    expect_success(send_sql("commit"));

    std::string res = send_sql("select * from t");
    expect_total_records(res, 2);
    expect_output_contains(res, "100");
    expect_output_contains(res, "300");
    expect_output_not_contains(res, "200");
}
