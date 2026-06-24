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
 * @file test_oj3.cpp
 * @brief OJ Problem 3 — BIGINT 类型支持
 *
 * 测试方式：client-server
 * 覆盖：CREATE/INSERT/SELECT BIGINT, 边界值, 超范围拒绝
 */

#include "test_oj_client_server.h"

class OJ3Test : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "oj3_bigint_test_db";
        ClientServerTest::SetUp();
    }
};

TEST_F(OJ3Test, CreateTableWithBigInt) {
    std::string res = send_sql("create table t(bid bigint, sid int)");
    expect_success(res);
}

TEST_F(OJ3Test, InsertAndSelectBigInt) {
    expect_success(send_sql("create table t(bid bigint, sid int)"));
    expect_success(send_sql("insert into t values(372036854775807, 233421)"));
    expect_success(send_sql("insert into t values(-922337203685477580, 124332)"));

    std::string res = send_sql("select * from t");
    expect_total_records(res, 2);
    expect_output_contains(res, "372036854775807");
    expect_output_contains(res, "233421");
    expect_output_contains(res, "124332");
}

TEST_F(OJ3Test, IntAutoPromoteToBigInt) {
    expect_success(send_sql("create table t(bid bigint, sid int)"));
    expect_success(send_sql("insert into t values(42, 100)"));

    std::string res = send_sql("select * from t");
    expect_total_records(res, 1);
    expect_output_contains(res, "42");
}

TEST_F(OJ3Test, RejectOutOfRangeBigInt) {
    expect_success(send_sql("create table t(bid bigint, sid int)"));
    expect_success(send_sql("insert into t values(372036854775807, 233421)"));
    expect_success(send_sql("insert into t values(-922337203685477580, 124332)"));

    // 超出 BIGINT 范围的值应被拒绝
    std::string res = send_sql("insert into t values(9223372036854775809, 12345)");
    expect_error(res);

    // 确认原有数据未受影响
    res = send_sql("select * from t");
    expect_total_records(res, 2);
}

TEST_F(OJ3Test, BigIntBoundaryValues) {
    expect_success(send_sql("create table t(bid bigint)"));
    expect_success(send_sql("insert into t values(0)"));
    expect_success(send_sql("insert into t values(1)"));
    expect_success(send_sql("insert into t values(-1)"));
    expect_success(send_sql("insert into t values(9223372036854775807)"));
    expect_success(send_sql("insert into t values(-9223372036854775808)"));

    std::string res = send_sql("select * from t");
    expect_total_records(res, 5);
    expect_output_contains(res, "0");
    expect_output_contains(res, "1");
    expect_output_contains(res, "-1");
}

TEST_F(OJ3Test, WhereConditionBigInt) {
    expect_success(send_sql("create table t(bid bigint, val int)"));
    expect_success(send_sql("insert into t values(100, 1)"));
    expect_success(send_sql("insert into t values(200, 2)"));
    expect_success(send_sql("insert into t values(300, 3)"));

    std::string res = send_sql("select * from t where bid > 150");
    expect_total_records(res, 2);

    res = send_sql("select * from t where bid = 100");
    expect_total_records(res, 1);
}

TEST_F(OJ3Test, BigIntAndIntMix) {
    expect_success(send_sql("create table t(bid bigint, sid int)"));
    expect_success(send_sql("insert into t values(9999999999, 123)"));
    expect_success(send_sql("insert into t values(42, 456)"));

    std::string res = send_sql("select * from t where bid > 100");
    expect_total_records(res, 1);
    expect_output_contains(res, "9999999999");
}
