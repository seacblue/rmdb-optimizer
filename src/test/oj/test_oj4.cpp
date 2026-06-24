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
 * @file test_oj4.cpp
 * @brief OJ Problem 4 — DATETIME 类型支持
 *
 * 测试方式：client-server
 * 覆盖：合法 DATETIME 增删改查、非法值拒绝、边界值
 */

#include "test_oj_client_server.h"

class OJ4Test : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "oj4_datetime_test_db";
        ClientServerTest::SetUp();
    }
};

TEST_F(OJ4Test, CreateTableWithDateTime) {
    std::string res = send_sql("create table t(id int, time datetime)");
    expect_success(res);
}

TEST_F(OJ4Test, InsertAndSelectDateTime) {
    expect_success(send_sql("create table t(id int, time datetime)"));
    expect_success(send_sql("insert into t values(1, '2023-05-18 09:12:19')"));
    expect_success(send_sql("insert into t values(2, '2023-05-30 12:34:32')"));

    std::string res = send_sql("select * from t");
    expect_total_records(res, 2);
    expect_output_contains(res, "2023-05-18");
    expect_output_contains(res, "2023-05-30");
}

TEST_F(OJ4Test, DeleteWhereDateTime) {
    expect_success(send_sql("create table t(id int, time datetime)"));
    expect_success(send_sql("insert into t values(1, '2023-05-18 09:12:19')"));
    expect_success(send_sql("insert into t values(2, '2023-05-30 12:34:32')"));

    expect_success(send_sql("delete from t where time = '2023-05-30 12:34:32'"));

    std::string res = send_sql("select * from t");
    expect_total_records(res, 1);
    expect_output_contains(res, "2023-05-18");
}

TEST_F(OJ4Test, UpdateWhereDateTime) {
    expect_success(send_sql("create table t(id int, time datetime)"));
    expect_success(send_sql("insert into t values(1, '2023-05-18 09:12:19')"));
    expect_success(send_sql("insert into t values(2, '2023-05-30 12:34:32')"));

    expect_success(send_sql("update t set id = 2023 where time = '2023-05-18 09:12:19'"));

    std::string res = send_sql("select * from t");
    expect_total_records(res, 2);
    expect_output_contains(res, "2023");
}

TEST_F(OJ4Test, RejectInvalidDateTime) {
    expect_success(send_sql("create table t(time datetime, temperature float)"));
    expect_success(send_sql("insert into t values('1999-07-07 12:30:00', 36.0)"));

    // 月份13
    expect_error(send_sql("insert into t values('1999-13-07 12:30:00', 36.0)"));
    // 缺少前导零
    expect_error(send_sql("insert into t values('1999-1-07 12:30:00', 36.0)"));
    // 月份0
    expect_error(send_sql("insert into t values('1999-00-07 12:30:00', 36.0)"));
    // 日期0
    expect_error(send_sql("insert into t values('1999-07-00 12:30:00', 36.0)"));
    // 年份过小
    expect_error(send_sql("insert into t values('0001-07-10 12:30:00', 36.0)"));
    // 2月30日
    expect_error(send_sql("insert into t values('1999-02-30 12:30:00', 36.0)"));
    // 秒数61
    expect_error(send_sql("insert into t values('1999-02-28 12:30:61', 36.0)"));

    // 确认原有数据还在
    std::string res = send_sql("select * from t");
    expect_total_records(res, 1);
}

TEST_F(OJ4Test, DateTimeBoundaryValues) {
    expect_success(send_sql("create table t(dt datetime, remark char(8))"));

    // 最小值
    expect_success(send_sql("insert into t values('1000-01-01 00:00:00', 'min')"));
    // 最大值
    expect_success(send_sql("insert into t values('9999-12-31 23:59:59', 'max')"));
    // 闰年
    expect_success(send_sql("insert into t values('2024-02-29 12:00:00', 'leap')"));

    std::string res = send_sql("select * from t");
    expect_total_records(res, 3);
}

TEST_F(OJ4Test, DateTimeWhereComparison) {
    expect_success(send_sql("create table t(time datetime, temperature float)"));
    expect_success(send_sql("insert into t values('1999-07-07 12:30:00', 36.0)"));

    std::string res = send_sql("select * from t where time = '1999-07-07 12:30:00'");
    expect_total_records(res, 1);
    expect_output_contains(res, "36.0");
}
