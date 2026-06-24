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
#include "recovery/log_recovery.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "system/sm.h"
#include "system/sm_manager.h"
#include "test_oj_sql_helper.h"
#include "transaction/concurrency/lock_manager.h"
#include "transaction/transaction_manager.h"

namespace {

constexpr const char *TEST_DIR = "oj_recovery_test_dir";
constexpr const char *TEST_DB = "oj_recovery_test_db";

void enter_test_dir(DiskManager *disk_manager) {
    if (disk_manager->is_dir(TEST_DIR)) {
        disk_manager->destroy_dir(TEST_DIR);
    }
    disk_manager->create_dir(TEST_DIR);
    ASSERT_TRUE(disk_manager->is_dir(TEST_DIR));
    if (chdir(TEST_DIR) < 0) {
        perror("chdir");
        FAIL() << "Cannot enter test directory";
    }
}

class RunningDb {
   public:
    explicit RunningDb(bool open_existing) {
        disk_manager_ = std::make_unique<DiskManager>();
        bpm_ = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager_.get());
        rm_manager_ = std::make_unique<RmManager>(disk_manager_.get(), bpm_.get());
        ix_manager_ = std::make_unique<IxManager>(disk_manager_.get(), bpm_.get());
        sm_manager_ = std::make_unique<SmManager>(disk_manager_.get(), bpm_.get(), rm_manager_.get(), ix_manager_.get());
        lock_manager_ = std::make_unique<LockManager>();
        txn_manager_ = std::make_unique<TransactionManager>(lock_manager_.get(), sm_manager_.get());
        ql_manager_ = std::make_unique<QlManager>(sm_manager_.get(), txn_manager_.get());
        log_manager_ = std::make_unique<LogManager>(disk_manager_.get());
        recovery_ = std::make_unique<RecoveryManager>(disk_manager_.get(), bpm_.get(), sm_manager_.get());
        planner_ = std::make_unique<Planner>(sm_manager_.get());
        optimizer_ = std::make_unique<Optimizer>(sm_manager_.get(), planner_.get());
        portal_ = std::make_unique<Portal>(sm_manager_.get());
        analyze_ = std::make_unique<Analyze>(sm_manager_.get());

        if (!open_existing) {
            sm_manager_->create_db(TEST_DB);
        }
        sm_manager_->open_db(TEST_DB);
        bpm_->set_flush_log_callback([this](lsn_t page_lsn) {
            if (page_lsn != INVALID_LSN && log_manager_->get_persist_lsn() < page_lsn) {
                log_manager_->flush_log_to_disk();
            }
        });

        if (open_existing) {
            recovery_->set_log_manager(log_manager_.get());
            recovery_->analyze();
            recovery_->redo();
            recovery_->undo();
        }

        sql_helper_.init(sm_manager_.get(), lock_manager_.get(), txn_manager_.get(), ql_manager_.get(),
                         log_manager_.get(), planner_.get(), optimizer_.get(), portal_.get(), analyze_.get());
    }

    ~RunningDb() = default;

    std::string exec(const std::string &sql) { return sql_helper_.execute_sql(sql); }

    void flush_log() { log_manager_->flush_log_to_disk(); }

    void shutdown_cleanly() {
        if (!open_) {
            return;
        }
        log_manager_->flush_log_to_disk();
        sm_manager_->close_db();
        open_ = false;
    }

    void simulate_crash() {
        if (!open_) {
            return;
        }
        log_manager_->flush_log_to_disk();
        // Intentionally skip SmManager::close_db(); the following reset drops
        // in-memory buffers just like a process crash would.
        analyze_.reset();
        portal_.reset();
        optimizer_.reset();
        planner_.reset();
        recovery_.reset();
        log_manager_.reset();
        ql_manager_.reset();
        txn_manager_.reset();
        lock_manager_.reset();
        sm_manager_.reset();
        ix_manager_.reset();
        rm_manager_.reset();
        bpm_.reset();
        disk_manager_.reset();
        open_ = false;
    }

    void drop_db() {
        if (!open_) {
            return;
        }
        sm_manager_->drop_db(TEST_DB);
        open_ = false;
    }

   private:
    bool open_ = true;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<RmManager> rm_manager_;
    std::unique_ptr<IxManager> ix_manager_;
    std::unique_ptr<SmManager> sm_manager_;
    std::unique_ptr<LockManager> lock_manager_;
    std::unique_ptr<TransactionManager> txn_manager_;
    std::unique_ptr<QlManager> ql_manager_;
    std::unique_ptr<LogManager> log_manager_;
    std::unique_ptr<RecoveryManager> recovery_;
    std::unique_ptr<Planner> planner_;
    std::unique_ptr<Optimizer> optimizer_;
    std::unique_ptr<Portal> portal_;
    std::unique_ptr<Analyze> analyze_;
    SqlHelper sql_helper_;
};

class RecoveryOjTest : public ::testing::Test {
   protected:
    void SetUp() override {
        dir_disk_manager_ = std::make_unique<DiskManager>();
        char cwd[4096];
        ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
        base_dir_ = cwd;
        enter_test_dir(dir_disk_manager_.get());
    }

    void TearDown() override {
        db_.reset();
        if (!base_dir_.empty() && chdir(base_dir_.c_str()) < 0) {
            perror("chdir");
        }
        if (dir_disk_manager_->is_dir(TEST_DIR)) {
            dir_disk_manager_->destroy_dir(TEST_DIR);
        }
        dir_disk_manager_.reset();
    }

    void create_fresh_db() { db_ = std::make_unique<RunningDb>(false); }

    void restart_with_recovery() { db_ = std::make_unique<RunningDb>(true); }

    void clean_shutdown() {
        db_->shutdown_cleanly();
        db_.reset();
    }

    void crash_restart() {
        db_->simulate_crash();
        db_.reset();
        if (chdir("..") < 0) {
            perror("chdir");
            FAIL() << "Cannot leave crashed database directory";
        }
        restart_with_recovery();
    }

    std::string exec(const std::string &sql) { return db_->exec(sql); }

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

    static std::string trim_cell(const std::string &cell) {
        const auto begin = cell.find_first_not_of(' ');
        if (begin == std::string::npos) {
            return "";
        }
        const auto end = cell.find_last_not_of(' ');
        return cell.substr(begin, end - begin + 1);
    }

    static std::vector<std::string> parse_table_row(const std::string &line) {
        std::vector<std::string> row;
        size_t start = line.find('|');
        while (start != std::string::npos) {
            size_t end = line.find('|', start + 1);
            if (end == std::string::npos) {
                break;
            }
            row.push_back(trim_cell(line.substr(start + 1, end - start - 1)));
            start = end;
        }
        if (!row.empty() && row.back().empty()) {
            row.pop_back();
        }
        return row;
    }

    static bool has_row(const std::string &res, const std::vector<std::string> &expected) {
        size_t line_begin = 0;
        while (line_begin <= res.size()) {
            size_t line_end = res.find('\n', line_begin);
            if (line_end == std::string::npos) {
                line_end = res.size();
            }
            std::vector<std::string> row = parse_table_row(res.substr(line_begin, line_end - line_begin));
            if (row == expected) {
                return true;
            }
            if (line_end == res.size()) {
                break;
            }
            line_begin = line_end + 1;
        }
        return false;
    }

    void expect_row(const std::string &res, const std::vector<std::string> &expected) {
        EXPECT_TRUE(has_row(res, expected)) << res;
    }

    void expect_no_row(const std::string &res, const std::vector<std::string> &expected) {
        EXPECT_FALSE(has_row(res, expected)) << res;
    }

    std::unique_ptr<RunningDb> db_;
    std::unique_ptr<DiskManager> dir_disk_manager_;
    std::string base_dir_;
};

}  // namespace

TEST_F(RecoveryOjTest, CommittedInsertSurvivesAndUncommittedInsertIsUndone) {
    create_fresh_db();
    expect_success(exec("CREATE TABLE t1 (id INT, num INT);"));
    clean_shutdown();

    restart_with_recovery();
    expect_success(exec("BEGIN;"));
    expect_success(exec("INSERT INTO t1 VALUES (1, 1);"));
    expect_success(exec("COMMIT;"));
    expect_success(exec("BEGIN;"));
    expect_success(exec("INSERT INTO t1 VALUES (2, 2);"));

    crash_restart();

    std::string res = exec("SELECT * FROM t1 ORDER BY id;");
    expect_total_records(res, 1);
    expect_row(res, {"1", "1"});
    expect_no_row(res, {"2", "2"});
}

TEST_F(RecoveryOjTest, UncommittedUpdateAndDeleteAreRolledBack) {
    create_fresh_db();
    expect_success(exec("CREATE TABLE account (id INT, balance INT);"));
    expect_success(exec("INSERT INTO account VALUES (1, 100);"));
    expect_success(exec("INSERT INTO account VALUES (2, 200);"));
    clean_shutdown();

    restart_with_recovery();
    expect_success(exec("BEGIN;"));
    expect_success(exec("UPDATE account SET balance = 999 WHERE id = 1;"));
    expect_success(exec("DELETE FROM account WHERE id = 2;"));

    crash_restart();

    std::string res = exec("SELECT * FROM account ORDER BY id;");
    expect_total_records(res, 2);
    expect_row(res, {"1", "100"});
    expect_row(res, {"2", "200"});
    expect_not_contains(res, "999");
}

TEST_F(RecoveryOjTest, CommittedUpdateAndDeleteAreRedoneAfterCrash) {
    create_fresh_db();
    expect_success(exec("CREATE TABLE account (id INT, balance INT);"));
    expect_success(exec("INSERT INTO account VALUES (1, 100);"));
    expect_success(exec("INSERT INTO account VALUES (2, 200);"));
    clean_shutdown();

    restart_with_recovery();
    expect_success(exec("BEGIN;"));
    expect_success(exec("UPDATE account SET balance = 150 WHERE id = 1;"));
    expect_success(exec("DELETE FROM account WHERE id = 2;"));
    expect_success(exec("COMMIT;"));

    crash_restart();

    std::string res = exec("SELECT * FROM account ORDER BY id;");
    expect_total_records(res, 1);
    expect_row(res, {"1", "150"});
    expect_no_row(res, {"2", "200"});
}

TEST_F(RecoveryOjTest, RecoveryRebuildsIndexesForPostCrashQueries) {
    create_fresh_db();
    expect_success(exec("CREATE TABLE item (id INT, tag CHAR(8));"));
    expect_success(exec("INSERT INTO item VALUES (1, 'keep');"));
    expect_success(exec("INSERT INTO item VALUES (2, 'old');"));
    expect_success(exec("CREATE INDEX item(id);"));
    clean_shutdown();

    restart_with_recovery();
    expect_success(exec("BEGIN;"));
    expect_success(exec("INSERT INTO item VALUES (3, 'live');"));
    expect_success(exec("UPDATE item SET tag = 'done' WHERE id = 2;"));
    expect_success(exec("COMMIT;"));
    expect_success(exec("BEGIN;"));
    expect_success(exec("INSERT INTO item VALUES (4, 'lost');"));

    crash_restart();

    std::string res = exec("SELECT * FROM item WHERE id = 3;");
    expect_total_records(res, 1);
    expect_row(res, {"3", "live"});
    res = exec("SELECT * FROM item WHERE id = 4;");
    expect_total_records(res, 0);
    res = exec("SELECT * FROM item WHERE id = 2;");
    expect_total_records(res, 1);
    expect_row(res, {"2", "done"});
}

TEST_F(RecoveryOjTest, JoinAndOrderBySeeRecoveredConsistentState) {
    create_fresh_db();
    expect_success(exec("CREATE TABLE t1 (id INT, num INT);"));
    expect_success(exec("CREATE TABLE t2 (id INT, label CHAR(8));"));
    expect_success(exec("INSERT INTO t1 VALUES (1, 30);"));
    expect_success(exec("INSERT INTO t2 VALUES (1, 'base');"));
    clean_shutdown();

    restart_with_recovery();
    expect_success(exec("BEGIN;"));
    expect_success(exec("INSERT INTO t1 VALUES (2, 10);"));
    expect_success(exec("INSERT INTO t2 VALUES (2, 'commit');"));
    expect_success(exec("COMMIT;"));
    expect_success(exec("BEGIN;"));
    expect_success(exec("INSERT INTO t1 VALUES (3, 20);"));
    expect_success(exec("INSERT INTO t2 VALUES (3, 'abort');"));

    crash_restart();

    std::string res = exec("SELECT * FROM t1, t2 WHERE t1.id = t2.id ORDER BY num;");
    expect_total_records(res, 2);
    expect_row(res, {"2", "10", "2", "commit"});
    expect_row(res, {"1", "30", "1", "base"});
    expect_not_contains(res, "abort");
    expect_no_row(res, {"3", "20", "3", "abort"});
}
