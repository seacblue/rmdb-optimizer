/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

/**
 * @file test_oj_execution.cpp
 * @brief OJ test for execution module (Problem 2)
 */

#include <gtest/gtest.h>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "analyze/analyze.h"
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
#include "transaction/concurrency/lock_manager.h"
#include "transaction/transaction_manager.h"
#include "common/config.h"
#include "test_oj_sql_helper.h"

namespace {
const std::string TEST_DIR = "oj_execution_test_dir";
void EnterTestDir(DiskManager *dm) {
    if (dm->is_dir(TEST_DIR)) { if (chdir("..") < 0) {} dm->destroy_dir(TEST_DIR); }
    dm->create_dir(TEST_DIR);
    ASSERT_TRUE(dm->is_dir(TEST_DIR));
    if (chdir(TEST_DIR.c_str()) < 0) { perror("chdir"); FAIL(); }
}
void LeaveTestDir(DiskManager *dm) {
    if (chdir("..") < 0) { perror("chdir"); }
    if (dm->is_dir(TEST_DIR)) { dm->destroy_dir(TEST_DIR); }
}
}

class ExecOjTest : public ::testing::Test {
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
        sql_helper_.init(sm_manager_, lock_manager_, txn_manager_, ql_manager_, log_manager_, planner_, optimizer_, portal_, analyze_);
        sm_manager_->create_db(TEST_DB);
        sm_manager_->open_db(TEST_DB);
    }

    void TearDown() override {
        sm_manager_->close_db();
        sm_manager_->drop_db(TEST_DB);
        delete analyze_; delete portal_; delete optimizer_; delete planner_;
        delete log_manager_; delete ql_manager_; delete txn_manager_; delete lock_manager_;
        delete sm_manager_; delete ix_manager_; delete rm_manager_; delete bpm_; delete disk_manager_;
        LeaveTestDir(disk_manager_);
    }

    void expect_success(const std::string &res) {
        EXPECT_TRUE(res.empty() || res.find("Error: ") != 0);
    }

    void expect_total_records(const std::string &res, int n) {
        std::string exp = "Total record(s): " + std::to_string(n);
        EXPECT_NE(res.find(exp), std::string::npos)
            << "Expected \"" << exp << "\", got: [" << res << "]";
    }
};
const std::string ExecOjTest::TEST_DB = "exec_oj_test_db";

TEST_F(ExecOjTest, CreateTableAndInsert) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE student (id INT, name CHAR(16), score FLOAT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (1, 'Alice', 95.5);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (2, 'Bob', 87.0);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (3, 'Charlie', 72.5);"));
    std::string res = sql_helper_.execute_sql("SELECT * FROM student;");
    expect_total_records(res, 3);
}

TEST_F(ExecOjTest, SelectWithWhere) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE student (id INT, name CHAR(16), score FLOAT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (1, 'Alice', 95.5);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (2, 'Bob', 87.0);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (3, 'Charlie', 72.5);"));

    std::string res = sql_helper_.execute_sql("SELECT * FROM student WHERE score >= 80.0;");
    expect_total_records(res, 2);
}

TEST_F(ExecOjTest, UpdateWithWhere) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE student (id INT, name CHAR(16), score FLOAT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (1, 'Alice', 95.5);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (2, 'Bob', 87.0);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (3, 'Charlie', 72.5);"));
    expect_success(sql_helper_.execute_sql("UPDATE student SET score = 99.0 WHERE id = 2;"));
    std::string res = sql_helper_.execute_sql("SELECT * FROM student;");
    expect_total_records(res, 3);
}

TEST_F(ExecOjTest, DeleteWithWhere) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE student (id INT, name CHAR(16), score FLOAT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (1, 'Alice', 95.5);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (2, 'Bob', 87.0);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO student VALUES (3, 'Charlie', 72.5);"));
    expect_success(sql_helper_.execute_sql("DELETE FROM student WHERE score < 80.0;"));
    std::string res = sql_helper_.execute_sql("SELECT * FROM student;");
    expect_total_records(res, 2);
    res = sql_helper_.execute_sql("SELECT * FROM student WHERE id = 3;");
    expect_total_records(res, 0);
}

TEST_F(ExecOjTest, JoinQuery) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE t (id INT, t_name CHAR(8));"));
    expect_success(sql_helper_.execute_sql("CREATE TABLE d (d_name CHAR(8), ref INT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO t VALUES (1, \'aaa\');"));
    expect_success(sql_helper_.execute_sql("INSERT INTO t VALUES (2, \'baa\');"));
    expect_success(sql_helper_.execute_sql("INSERT INTO d VALUES (\'X\', 1);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO d VALUES (\'Y\', 2);"));
    std::string res = sql_helper_.execute_sql("SELECT * FROM t, d;");
    expect_total_records(res, 4);
    res = sql_helper_.execute_sql("SELECT * FROM t, d WHERE t.id = d.ref;");
    expect_total_records(res, 2);
}

TEST_F(ExecOjTest, DDLStatements) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE t1 (id INT);"));
    expect_success(sql_helper_.execute_sql("CREATE TABLE t2 (name CHAR(8));"));
    std::string res = sql_helper_.execute_sql("DESC t1;");
    expect_success(res);
    expect_success(sql_helper_.execute_sql("DROP TABLE t1;"));
    res = sql_helper_.execute_sql("SELECT * FROM t2;");
    expect_total_records(res, 0);
    res = sql_helper_.execute_sql("DROP TABLE t1;");
    EXPECT_TRUE(res.find("Error:") != std::string::npos);
}

TEST_F(ExecOjTest, MultiTypeProjection) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE alltypes (id INT, score FLOAT, info CHAR(16));"));
    expect_success(sql_helper_.execute_sql("INSERT INTO alltypes VALUES (42, 3.14, \'hello\');"));
    std::string res = sql_helper_.execute_sql("SELECT id, info FROM alltypes;");
    expect_total_records(res, 1);
    res = sql_helper_.execute_sql("SELECT score FROM alltypes WHERE id = 42;");
    expect_total_records(res, 1);
}
