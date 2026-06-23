/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "analyze/analyze.h"
#include "common/config.h"
#include "execution/execution_manager.h"
#include "index/ix.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parser.h"
#include "portal.h"
#include "recovery/log_manager.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "system/sm.h"
#include "system/sm_manager.h"
#include "test_oj_sql_helper.h"
#include "transaction/concurrency/lock_manager.h"
#include "transaction/transaction_manager.h"

namespace {
const std::string TEST_DIR = "oj_index_test_dir";
void EnterTestDir(DiskManager *dm) {
    if (dm->is_dir(TEST_DIR)) {
        if (chdir("..") < 0) {}
        dm->destroy_dir(TEST_DIR);
    }
    dm->create_dir(TEST_DIR);
    ASSERT_TRUE(dm->is_dir(TEST_DIR));
    if (chdir(TEST_DIR.c_str()) < 0) {
        perror("chdir");
        FAIL();
    }
}
void LeaveTestDir(DiskManager *dm) {
    if (chdir("..") < 0) {
        perror("chdir");
    }
    if (dm->is_dir(TEST_DIR)) {
        dm->destroy_dir(TEST_DIR);
    }
}
}  // namespace

class IndexOjTest : public ::testing::Test {
   protected:
    static const std::string TEST_DB;
    DiskManager *disk_manager_;
    BufferPoolManager *bpm_;
    RmManager *rm_manager_;
    IxManager *ix_manager_;
    SmManager *sm_manager_;
    LockManager *lock_manager_;
    TransactionManager *txn_manager_;
    QlManager *ql_manager_;
    LogManager *log_manager_;
    Planner *planner_;
    Optimizer *optimizer_;
    Portal *portal_;
    Analyze *analyze_;
    SqlHelper sql_helper_;

    void SetUp() override {
        disk_manager_ = new DiskManager();
        EnterTestDir(disk_manager_);
        bpm_ = new BufferPoolManager(BUFFER_POOL_SIZE, disk_manager_);
        rm_manager_ = new RmManager(disk_manager_, bpm_);
        ix_manager_ = new IxManager(disk_manager_, bpm_);
        sm_manager_ = new SmManager(disk_manager_, bpm_, rm_manager_, ix_manager_);
        lock_manager_ = new LockManager();
        txn_manager_ = new TransactionManager(lock_manager_, sm_manager_);
        ql_manager_ = new QlManager(sm_manager_, txn_manager_);
        log_manager_ = new LogManager(disk_manager_);
        planner_ = new Planner(sm_manager_);
        optimizer_ = new Optimizer(sm_manager_, planner_);
        portal_ = new Portal(sm_manager_);
        analyze_ = new Analyze(sm_manager_);
        sql_helper_.init(sm_manager_, lock_manager_, txn_manager_, ql_manager_, log_manager_,
                         planner_, optimizer_, portal_, analyze_);
        sm_manager_->create_db(TEST_DB);
        sm_manager_->open_db(TEST_DB);
    }

    void TearDown() override {
        sm_manager_->close_db();
        sm_manager_->drop_db(TEST_DB);
        delete analyze_;
        delete portal_;
        delete optimizer_;
        delete planner_;
        delete log_manager_;
        delete ql_manager_;
        delete txn_manager_;
        delete lock_manager_;
        delete sm_manager_;
        delete ix_manager_;
        delete rm_manager_;
        delete bpm_;
        delete disk_manager_;
        LeaveTestDir(disk_manager_);
    }

    void expect_success(const std::string &res) {
        EXPECT_TRUE(res.empty() || res.find("Error: ") != 0) << res;
    }

    void expect_error(const std::string &res) {
        EXPECT_TRUE(res.find("Error: ") == 0) << res;
    }

    void expect_total_records(const std::string &res, int n) {
        std::string expected = "Total record(s): " + std::to_string(n);
        EXPECT_NE(res.find(expected), std::string::npos) << res;
    }
};

const std::string IndexOjTest::TEST_DB = "index_oj_test_db";

TEST_F(IndexOjTest, ShowCreateDropIndex) {
    expect_success(sql_helper_.execute_sql("create table warehouse (id int, name char(8));"));
    expect_success(sql_helper_.execute_sql("create index warehouse (id);"));
    std::string res = sql_helper_.execute_sql("show index from warehouse;");
    EXPECT_NE(res.find("| warehouse | unique | (id) |"), std::string::npos);

    expect_success(sql_helper_.execute_sql("create index warehouse (id,name);"));
    res = sql_helper_.execute_sql("show index from warehouse;");
    EXPECT_NE(res.find("| warehouse | unique | (id) |"), std::string::npos);
    EXPECT_NE(res.find("| warehouse | unique | (id,name) |"), std::string::npos);

    expect_success(sql_helper_.execute_sql("drop index warehouse (id);"));
    expect_success(sql_helper_.execute_sql("drop index warehouse (id,name);"));
    res = sql_helper_.execute_sql("show index from warehouse;");
    EXPECT_TRUE(res.empty());
}

TEST_F(IndexOjTest, IndexQueries) {
    expect_success(sql_helper_.execute_sql("create table warehouse (w_id int, name char(8));"));
    expect_success(sql_helper_.execute_sql("insert into warehouse values (10 , 'qweruiop');"));
    expect_success(sql_helper_.execute_sql("insert into warehouse values (534, 'asdfhjkl');"));
    expect_success(sql_helper_.execute_sql("insert into warehouse values (100, 'qwerghjk');"));
    expect_success(sql_helper_.execute_sql("insert into warehouse values (500, 'bgtyhnmj');"));

    expect_success(sql_helper_.execute_sql("create index warehouse(w_id);"));
    std::string res = sql_helper_.execute_sql("select * from warehouse where w_id = 10;");
    expect_total_records(res, 1);
    EXPECT_NE(res.find("qweruiop"), std::string::npos);
    res = sql_helper_.execute_sql("select * from warehouse where w_id < 534 and w_id > 100;");
    expect_total_records(res, 1);
    EXPECT_NE(res.find("bgtyhnmj"), std::string::npos);

    expect_success(sql_helper_.execute_sql("drop index warehouse(w_id);"));
    expect_success(sql_helper_.execute_sql("create index warehouse(name);"));
    res = sql_helper_.execute_sql("select * from warehouse where name = 'qweruiop';");
    expect_total_records(res, 1);
    EXPECT_NE(res.find("10"), std::string::npos);
    res = sql_helper_.execute_sql("select * from warehouse where name > 'qwerghjk';");
    expect_total_records(res, 1);
    EXPECT_NE(res.find("qweruiop"), std::string::npos);
    res = sql_helper_.execute_sql("select * from warehouse where name > 'aszdefgh' and name < 'qweraaaa';");
    expect_total_records(res, 1);
    EXPECT_NE(res.find("bgtyhnmj"), std::string::npos);

    expect_success(sql_helper_.execute_sql("drop index warehouse(name);"));
    expect_success(sql_helper_.execute_sql("create index warehouse(w_id,name);"));
    res = sql_helper_.execute_sql("select * from warehouse where w_id = 100 and name = 'qwerghjk';");
    expect_total_records(res, 1);
    EXPECT_NE(res.find("qwerghjk"), std::string::npos);
    res = sql_helper_.execute_sql("select * from warehouse where w_id < 600 and name > 'bztyhnmj';");
    expect_total_records(res, 2);
}

TEST_F(IndexOjTest, IndexMaintenanceAndUniqueness) {
    expect_success(sql_helper_.execute_sql("create table warehouse (w_id int, name char(8));"));
    expect_success(sql_helper_.execute_sql("insert into warehouse values (10 , 'qweruiop');"));
    expect_success(sql_helper_.execute_sql("insert into warehouse values (534, 'asdfhjkl');"));

    std::string res = sql_helper_.execute_sql("select * from warehouse where w_id = 10;");
    expect_total_records(res, 1);
    res = sql_helper_.execute_sql("select * from warehouse where w_id < 534 and w_id > 100;");
    expect_total_records(res, 0);

    expect_success(sql_helper_.execute_sql("create index warehouse(w_id);"));
    expect_success(sql_helper_.execute_sql("insert into warehouse values (500, 'lastdanc');"));
    expect_success(sql_helper_.execute_sql("update warehouse set w_id = 507 where w_id = 534;"));
    res = sql_helper_.execute_sql("select * from warehouse where w_id = 10;");
    expect_total_records(res, 1);
    res = sql_helper_.execute_sql("select * from warehouse where w_id < 534 and w_id > 100;");
    expect_total_records(res, 2);
    EXPECT_NE(res.find("lastdanc"), std::string::npos);
    EXPECT_NE(res.find("asdfhjkl"), std::string::npos);

    expect_error(sql_helper_.execute_sql("insert into warehouse values (10, 'dup');"));
    expect_error(sql_helper_.execute_sql("update warehouse set w_id = 10 where w_id = 500;"));
}
