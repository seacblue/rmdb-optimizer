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
 * @file test_oj5.cpp
 * @brief OJ Problem 5 — B+树索引（CREATE/DROP/SHOW INDEX + 索引查询 + 索引维护）
 *
 * 测试方式：client-server
 */

#include "test_oj_client_server.h"

class OJ5Test : public ClientServerTest {
   protected:
    void SetUp() override {
        db_name_ = "oj5_index_test_db";
        ClientServerTest::SetUp();
    }
};

TEST_F(OJ5Test, ShowCreateDropIndex) {
    expect_success(send_sql("create table warehouse (id int, name char(8))"));
    expect_success(send_sql("create index warehouse (id)"));
    std::string res = send_sql("show index from warehouse");
    expect_output_contains(res, "warehouse");

    expect_success(send_sql("create index warehouse (id,name)"));
    res = send_sql("show index from warehouse");
    expect_output_contains(res, "warehouse");
    expect_output_contains(res, "warehouse");

    expect_success(send_sql("drop index warehouse (id)"));
    expect_success(send_sql("drop index warehouse (id,name)"));
    res = send_sql("show index from warehouse");
    EXPECT_TRUE(res.empty());
}

TEST_F(OJ5Test, IndexPointQuery) {
    expect_success(send_sql("create table warehouse (w_id int, name char(8))"));
    expect_success(send_sql("insert into warehouse values (10 , 'qweruiop')"));
    expect_success(send_sql("insert into warehouse values (534, 'asdfhjkl')"));
    expect_success(send_sql("insert into warehouse values (100, 'qwerghjk')"));
    expect_success(send_sql("insert into warehouse values (500, 'bgtyhnmj')"));
    expect_success(send_sql("create index warehouse(w_id)"));

    std::string res = send_sql("select * from warehouse where w_id = 10");
    expect_total_records(res, 1);
    expect_output_contains(res, "qweruiop");
}

TEST_F(OJ5Test, IndexRangeQuery) {
    expect_success(send_sql("create table warehouse (w_id int, name char(8))"));
    expect_success(send_sql("insert into warehouse values (10 , 'qweruiop')"));
    expect_success(send_sql("insert into warehouse values (534, 'asdfhjkl')"));
    expect_success(send_sql("insert into warehouse values (100, 'qwerghjk')"));
    expect_success(send_sql("insert into warehouse values (500, 'bgtyhnmj')"));
    expect_success(send_sql("create index warehouse(w_id)"));

    std::string res = send_sql("select * from warehouse where w_id < 534 and w_id > 100");
    expect_total_records(res, 1);
    expect_output_contains(res, "bgtyhnmj");
}

TEST_F(OJ5Test, IndexStringQuery) {
    expect_success(send_sql("create table warehouse (w_id int, name char(8))"));
    expect_success(send_sql("insert into warehouse values (10 , 'qweruiop')"));
    expect_success(send_sql("insert into warehouse values (534, 'asdfhjkl')"));
    expect_success(send_sql("insert into warehouse values (100, 'qwerghjk')"));
    expect_success(send_sql("insert into warehouse values (500, 'bgtyhnmj')"));
    expect_success(send_sql("create index warehouse(name)"));

    std::string res = send_sql("select * from warehouse where name = 'qweruiop'");
    expect_total_records(res, 1);

    res = send_sql("select * from warehouse where name > 'qwerghjk'");
    expect_total_records(res, 1);

    res = send_sql("select * from warehouse where name > 'aszdefgh' and name < 'qweraaaa'");
    expect_total_records(res, 1);
}

TEST_F(OJ5Test, IndexMultiColumn) {
    expect_success(send_sql("create table warehouse (w_id int, name char(8))"));
    expect_success(send_sql("insert into warehouse values (10 , 'qweruiop')"));
    expect_success(send_sql("insert into warehouse values (534, 'asdfhjkl')"));
    expect_success(send_sql("insert into warehouse values (100, 'qwerghjk')"));
    expect_success(send_sql("insert into warehouse values (500, 'bgtyhnmj')"));
    expect_success(send_sql("create index warehouse(w_id,name)"));

    std::string res = send_sql("select * from warehouse where w_id = 100 and name = 'qwerghjk'");
    expect_total_records(res, 1);

    res = send_sql("select * from warehouse where w_id < 600 and name > 'bztyhnmj'");
    expect_total_records(res, 2);
}

TEST_F(OJ5Test, IndexMaintenance) {
    expect_success(send_sql("create table warehouse (w_id int, name char(8))"));
    expect_success(send_sql("insert into warehouse values (10 , 'qweruiop')"));
    expect_success(send_sql("insert into warehouse values (534, 'asdfhjkl')"));
    expect_success(send_sql("create index warehouse(w_id)"));

    expect_success(send_sql("insert into warehouse values (500, 'lastdanc')"));
    expect_success(send_sql("update warehouse set name = 'bgtyhnmj' where w_id = 500"));

    std::string res = send_sql("select * from warehouse where w_id = 10");
    expect_total_records(res, 1);
    expect_output_contains(res, "qweruiop");

    res = send_sql("select * from warehouse where w_id < 534 and w_id > 100");
    expect_total_records(res, 1);
    expect_output_contains(res, "bgtyhnmj");
}

TEST_F(OJ5Test, IndexUniquenessConstraint) {
    expect_success(send_sql("create table t(id int, name char(8))"));
    expect_success(send_sql("create index t(id)"));
    expect_success(send_sql("insert into t values(1, 'first')"));

    // 插入重复值应报错（唯一索引）
    std::string res = send_sql("insert into t values(1, 'dup')");
    expect_error(res);
}
