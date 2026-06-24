/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

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

constexpr const char *TEST_DIR = "oj_transaction_test_dir";
constexpr const char *TEST_DB = "oj_transaction_test_db";

void enter_test_dir(DiskManager *disk_manager) {
    if (disk_manager->is_dir(TEST_DIR)) {
        disk_manager->destroy_dir(TEST_DIR);
    }
    disk_manager->create_dir(TEST_DIR);
    ASSERT_TRUE(disk_manager->is_dir(TEST_DIR));
    if (chdir(TEST_DIR) < 0) {
        perror("chdir");
        FAIL() << "Cannot enter transaction test directory";
    }
}

class TransactionOjTest : public ::testing::Test {
   protected:
    void SetUp() override {
        dir_disk_manager_ = std::make_unique<DiskManager>();
        char cwd[4096];
        ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
        base_dir_ = cwd;
        enter_test_dir(dir_disk_manager_.get());

        disk_manager_ = std::make_unique<DiskManager>();
        bpm_ = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager_.get());
        rm_manager_ = std::make_unique<RmManager>(disk_manager_.get(), bpm_.get());
        ix_manager_ = std::make_unique<IxManager>(disk_manager_.get(), bpm_.get());
        sm_manager_ = std::make_unique<SmManager>(disk_manager_.get(), bpm_.get(), rm_manager_.get(), ix_manager_.get());
        lock_manager_ = std::make_unique<LockManager>();
        txn_manager_ = std::make_unique<TransactionManager>(lock_manager_.get(), sm_manager_.get());
        ql_manager_ = std::make_unique<QlManager>(sm_manager_.get(), txn_manager_.get());
        log_manager_ = std::make_unique<LogManager>(disk_manager_.get());
        planner_ = std::make_unique<Planner>(sm_manager_.get());
        optimizer_ = std::make_unique<Optimizer>(sm_manager_.get(), planner_.get());
        portal_ = std::make_unique<Portal>(sm_manager_.get());
        analyze_ = std::make_unique<Analyze>(sm_manager_.get());

        sm_manager_->create_db(TEST_DB);
        sm_manager_->open_db(TEST_DB);
        sql_helper_.init(sm_manager_.get(), lock_manager_.get(), txn_manager_.get(), ql_manager_.get(),
                         log_manager_.get(), planner_.get(), optimizer_.get(), portal_.get(), analyze_.get());
    }

    void TearDown() override {
        if (sm_manager_ != nullptr) {
            sm_manager_->close_db();
            sm_manager_->drop_db(TEST_DB);
        }
        analyze_.reset();
        portal_.reset();
        optimizer_.reset();
        planner_.reset();
        log_manager_.reset();
        ql_manager_.reset();
        txn_manager_.reset();
        lock_manager_.reset();
        sm_manager_.reset();
        ix_manager_.reset();
        rm_manager_.reset();
        bpm_.reset();
        disk_manager_.reset();

        if (!base_dir_.empty() && chdir(base_dir_.c_str()) < 0) {
            perror("chdir");
        }
        if (dir_disk_manager_->is_dir(TEST_DIR)) {
            dir_disk_manager_->destroy_dir(TEST_DIR);
        }
        dir_disk_manager_.reset();
    }

    std::string exec(const std::string &sql) { return sql_helper_.execute_sql(sql); }

    void expect_success(const std::string &res) {
        EXPECT_TRUE(res.empty() || res.find("Error: ") != 0) << res;
    }

    void expect_total_records(const std::string &res, int n) {
        std::string expected = "Total record(s): " + std::to_string(n);
        EXPECT_NE(res.find(expected), std::string::npos) << res;
    }

    void expect_contains(const std::string &res, const std::string &needle) {
        EXPECT_NE(res.find(needle), std::string::npos) << res;
    }

    void expect_not_contains(const std::string &res, const std::string &needle) {
        EXPECT_EQ(res.find(needle), std::string::npos) << res;
    }

    std::unique_ptr<DiskManager> dir_disk_manager_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<RmManager> rm_manager_;
    std::unique_ptr<IxManager> ix_manager_;
    std::unique_ptr<SmManager> sm_manager_;
    std::unique_ptr<LockManager> lock_manager_;
    std::unique_ptr<TransactionManager> txn_manager_;
    std::unique_ptr<QlManager> ql_manager_;
    std::unique_ptr<LogManager> log_manager_;
    std::unique_ptr<Planner> planner_;
    std::unique_ptr<Optimizer> optimizer_;
    std::unique_ptr<Portal> portal_;
    std::unique_ptr<Analyze> analyze_;
    SqlHelper sql_helper_;
    std::string base_dir_;
};

}  // namespace

TEST_F(TransactionOjTest, AbortWithoutIndexRestoresRows) {
    expect_success(exec("CREATE TABLE student (id INT, name CHAR(8), score FLOAT);"));
    expect_success(exec("INSERT INTO student VALUES (1, 'xiaohong', 90.0);"));

    expect_success(exec("BEGIN;"));
    expect_success(exec("INSERT INTO student VALUES (2, 'xiaoming', 99.0);"));
    expect_success(exec("DELETE FROM student WHERE id = 2;"));
    expect_success(exec("ABORT;"));

    std::string res = exec("SELECT * FROM student ORDER BY id;");
    expect_total_records(res, 1);
    expect_contains(res, "xiaohong");
    expect_not_contains(res, "xiaoming");
}

TEST_F(TransactionOjTest, AbortWithIndexRestoresIndexAndTable) {
    expect_success(exec("CREATE TABLE stock (s_id INT, qty INT, dist CHAR(8));"));
    for (int i = 1; i <= 80; ++i) {
        expect_success(exec("INSERT INTO stock VALUES (" + std::to_string(i) + ", " +
                            std::to_string(i * 10) + ", 'base');"));
    }
    expect_success(exec("CREATE INDEX stock(s_id);"));

    expect_success(exec("BEGIN;"));
    expect_success(exec("INSERT INTO stock VALUES (100, 1000, 'newone');"));
    expect_success(exec("UPDATE stock SET qty = 777 WHERE s_id = 10;"));
    expect_success(exec("DELETE FROM stock WHERE s_id = 20;"));
    expect_success(exec("ABORT;"));

    std::string res = exec("SELECT * FROM stock WHERE s_id = 100;");
    expect_total_records(res, 0);
    res = exec("SELECT * FROM stock WHERE s_id = 10;");
    expect_total_records(res, 1);
    expect_contains(res, "100");
    expect_not_contains(res, "777");
    res = exec("SELECT * FROM stock WHERE s_id = 20;");
    expect_total_records(res, 1);
    expect_contains(res, "200");
}

TEST_F(TransactionOjTest, AbortManyIndexedInsertsDoesNotBreakServer) {
    expect_success(exec("CREATE TABLE order_line (ol_id INT, qty INT, dist CHAR(8));"));
    expect_success(exec("CREATE INDEX order_line(ol_id);"));

    expect_success(exec("BEGIN;"));
    for (int i = 1; i <= 700; ++i) {
        expect_success(exec("INSERT INTO order_line VALUES (" + std::to_string(i) + ", " +
                            std::to_string(i % 10) + ", 'neword');"));
    }
    expect_success(exec("ABORT;"));

    std::string res = exec("SELECT * FROM order_line WHERE ol_id = 350;");
    expect_total_records(res, 0);
    res = exec("SELECT * FROM order_line;");
    expect_total_records(res, 0);
}

TEST_F(TransactionOjTest, CommitWithIndexKeepsChanges) {
    expect_success(exec("CREATE TABLE orders (o_id INT, carrier INT, entry DATETIME);"));
    expect_success(exec("CREATE INDEX orders(o_id);"));

    expect_success(exec("BEGIN;"));
    expect_success(exec("INSERT INTO orders VALUES (1, 0, '2024-01-02 03:04:05');"));
    expect_success(exec("UPDATE orders SET carrier = 8 WHERE o_id = 1;"));
    expect_success(exec("COMMIT;"));

    std::string res = exec("SELECT * FROM orders WHERE o_id = 1;");
    expect_total_records(res, 1);
    expect_contains(res, "8");
}
