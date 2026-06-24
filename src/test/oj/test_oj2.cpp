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
 * @file test_oj2.cpp
 * @brief OJ Problem 2 — SQL 查询执行（DDL + DML + DQL + JOIN）
 *
 * 测试方式：client-server（通过 TCP socket 连接 rmdb server）
 *
 * 覆盖：
 *   测试点1: DDL（CREATE / DROP TABLE, SHOW TABLES）
 *   测试点2: 单表插入与条件查询
 *   测试点3: 单表更新与条件查询
 *   测试点4: 单表删除与条件查询
 *   测试点5: 连接查询 + 浮点数精度
 */

#include "test_oj_client_server.h"

class OJ2Test : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "oj2_test_db";
        ClientServerTest::SetUp();
    }
};

TEST_F(OJ2Test, DDLShowTables) {
    expect_success(send_sql("create table t1(id int,name char(4))"));
    std::string res = send_sql("show tables");
    expect_output_contains(res, "t1");
    expect_success(send_sql("create table t2(id int)"));
    res = send_sql("show tables");
    expect_output_contains(res, "t1");
    expect_output_contains(res, "t2");
    expect_success(send_sql("drop table t1"));
    res = send_sql("show tables");
    expect_output_contains(res, "t2");
    expect_output_not_contains(res, "t1");
    expect_success(send_sql("drop table t2"));
    res = send_sql("show tables");
    // After dropping all tables, show tables may return a formatted table
    // with just the header, no actual table names. Both outcomes are OK.
    EXPECT_TRUE(res.find("t1") == std::string::npos && res.find("t2") == std::string::npos)
        << "Expected no table names after dropping all tables, got: [" << res << "]";
}

TEST_F(OJ2Test, SingleTableInsertAndSelect) {
    expect_success(send_sql("create table grade (name char(4),id int,score float)"));
    expect_success(send_sql("insert into grade values ('Data', 1, 90.5)"));
    expect_success(send_sql("insert into grade values ('Data', 2, 95.0)"));
    expect_success(send_sql("insert into grade values ('Calc', 2, 92.0)"));
    expect_success(send_sql("insert into grade values ('Calc', 1, 88.5)"));

    std::string res = send_sql("select * from grade");
    expect_total_records(res, 4);

    res = send_sql("select score,name,id from grade where score > 90");
    expect_total_records(res, 3);

    res = send_sql("select id from grade where name = 'Data'");
    expect_total_records(res, 2);
    expect_output_contains(res, "1");
    expect_output_contains(res, "2");

    res = send_sql("select name from grade where id = 2 and score > 90");
    expect_total_records(res, 2);
}

TEST_F(OJ2Test, UpdateWithWhere) {
    expect_success(send_sql("create table grade (name char(4),id int,score float)"));
    expect_success(send_sql("insert into grade values ('Data', 1, 90.5)"));
    expect_success(send_sql("insert into grade values ('Data', 2, 95.0)"));
    expect_success(send_sql("insert into grade values ('Calc', 2, 92.0)"));
    expect_success(send_sql("insert into grade values ('Calc', 1, 88.5)"));

    expect_success(send_sql("update grade set score = 99.0 where name = 'Calc'"));
    std::string res = send_sql("select * from grade");
    expect_total_records(res, 4);
    expect_output_contains(res, "99.0");

    expect_success(send_sql("update grade set name = 'test' where name > 'A'"));
    res = send_sql("select * from grade");
    expect_total_records(res, 4);

    expect_success(send_sql("update grade set name = 'test', id = -1, score = 0 where name = 'test' and score > 90"));
    res = send_sql("select * from grade");
    expect_total_records(res, 4);
}

TEST_F(OJ2Test, DeleteWithWhere) {
    expect_success(send_sql("create table grade (name char(4),id int,score float)"));
    expect_success(send_sql("insert into grade values ('Data', 1, 90.5)"));
    expect_success(send_sql("insert into grade values ('Data', 2, 95.0)"));
    expect_success(send_sql("insert into grade values ('Calc', 2, 92.0)"));
    expect_success(send_sql("insert into grade values ('Calc', 1, 88.5)"));

    expect_success(send_sql("delete from grade where name = 'Calc'"));
    std::string res = send_sql("select * from grade");
    expect_total_records(res, 2);

    expect_success(send_sql("delete from grade where id = 1"));
    res = send_sql("select * from grade");
    expect_total_records(res, 1);
}

TEST_F(OJ2Test, JoinQuery) {
    expect_success(send_sql("create table t1(id int, name char(8))"));
    expect_success(send_sql("create table t2(t1_id int, descr char(8))"));
    expect_success(send_sql("insert into t1 values (1, 'aaa')"));
    expect_success(send_sql("insert into t1 values (2, 'bbb')"));
    expect_success(send_sql("insert into t2 values (1, 'desc1')"));
    expect_success(send_sql("insert into t2 values (2, 'desc2')"));

    std::string res = send_sql("select * from t1, t2 where t1.id = t2.t1_id");
    expect_total_records(res, 2);
}

TEST_F(OJ2Test, InvalidSqlFailure) {
    expect_success(send_sql("create table t(id int)"));
    // 删除不存在的表
    std::string res = send_sql("drop table nonexist");
    expect_error(res);

    // 创建已存在的表
    res = send_sql("create table t(id int)");
    expect_error(res);

    // where 条件中出现不存在的字段
    expect_success(send_sql("insert into t values (1)"));
    res = send_sql("select * from t where no_such_col = 1");
    expect_error(res);
}

TEST_F(OJ2Test, FloatPrecision) {
    expect_success(send_sql("create table nums(a float, b float)"));
    expect_success(send_sql("insert into nums values (1.0, 2.0)"));
    expect_success(send_sql("insert into nums values (3.0, 4.0)"));

    std::string res = send_sql("select * from nums where a > 0.5");
    expect_total_records(res, 2);

    res = send_sql("select * from nums where a = 1.0");
    expect_total_records(res, 1);
}
