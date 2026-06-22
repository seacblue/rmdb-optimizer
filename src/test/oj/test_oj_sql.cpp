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
 * @file test_oj_sql.cpp
 * @brief 通过完整 SQL 流水线测试 BIGINT（Problem 3）和 DATETIME（Problem 4）
 *
 * 核心设计：
 *   通过完整的 SQL 流水线：
 *     yy_scan_string → yyparse → analyze → optimizer → portal → QlManager
 *   每条 SQL 语句作为一个独立事务自动提交。
 *
 * 编译 & 运行：
 *   mkdir -p build && cd build
 *   cmake .. && make test_oj_sql -j$(nproc) && ./bin/test_oj_sql
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "analyze/analyze.h"
#include "common/common.h"
#include "common/config.h"
#include "common/context.h"
#include "common/datetime_utils.h"
#include "execution/execution_manager.h"
#include "index/ix.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parser.h"
#include "parser/parser_defs.h"
#include "portal.h"
#include "record/rm.h"
#include "recovery/log_manager.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "system/sm.h"
#include "system/sm_manager.h"
#include "transaction/concurrency/lock_manager.h"
#include "transaction/transaction_manager.h"

// ============================================================
// SqlHelper — 封装完整 SQL 执行流水线
// ============================================================
namespace {

class SqlHelper {
   public:
    SqlHelper()
        : sm_manager_(nullptr),
          lock_manager_(nullptr),
          txn_manager_(nullptr),
          ql_manager_(nullptr),
          log_manager_(nullptr),
          planner_(nullptr),
          optimizer_(nullptr),
          portal_(nullptr),
          analyze_(nullptr) {
        pthread_mutex_init(&buffer_mutex_, nullptr);
    }

    ~SqlHelper() { pthread_mutex_destroy(&buffer_mutex_); }

    void init(SmManager *sm, LockManager *lm, TransactionManager *tm,
              QlManager *qm, LogManager *logm, Planner *p, Optimizer *opt,
              Portal *port, Analyze *an) {
        sm_manager_ = sm;
        lock_manager_ = lm;
        txn_manager_ = tm;
        ql_manager_ = qm;
        log_manager_ = logm;
        planner_ = p;
        optimizer_ = opt;
        portal_ = port;
        analyze_ = an;
        txn_id_ = INVALID_TXN_ID;
    }

    /** 执行单条 SQL，返回 data_send 缓冲区内容 */
    std::string execute_sql(const std::string &sql) {
        char data_send[BUFFER_LENGTH];
        memset(data_send, 0, BUFFER_LENGTH);
        int offset = 0;

        Context *context = new Context(lock_manager_, log_manager_, nullptr, data_send, &offset);
        set_transaction(context);

        bool finish_analyze = false;
        pthread_mutex_lock(&buffer_mutex_);

        YY_BUFFER_STATE buf = yy_scan_string(sql.c_str());
        if (yyparse() == 0) {
            if (ast::parse_tree != nullptr) {
                try {
                    std::shared_ptr<Query> query = analyze_->do_analyze(ast::parse_tree);
                    yy_delete_buffer(buf);
                    finish_analyze = true;
                    pthread_mutex_unlock(&buffer_mutex_);

                    std::shared_ptr<Plan> plan = optimizer_->plan_query(query, context);
                    std::shared_ptr<PortalStmt> portalStmt = portal_->start(plan, context);
                    portal_->run(portalStmt, ql_manager_, &txn_id_, context);
                    portal_->drop();
                } catch (TransactionAbortException &e) {
                    std::string str = "abort\n";
                    memcpy(data_send, str.c_str(), str.length());
                    data_send[str.length()] = '\0';
                    offset = static_cast<int>(str.length());
                    txn_manager_->abort(context->txn_, log_manager_);
                } catch (RMDBError &e) {
                    memcpy(data_send, e.what(), e.get_msg_len());
                    data_send[e.get_msg_len()] = '\n';
                    data_send[e.get_msg_len() + 1] = '\0';
                    offset = e.get_msg_len() + 1;
                }
            }
        }
        if (!finish_analyze) {
            yy_delete_buffer(buf);
            pthread_mutex_unlock(&buffer_mutex_);
        }

        // 自动提交非显式事务
        if (context->txn_->get_txn_mode() == false) {
            txn_manager_->commit(context->txn_, context->log_mgr_);
        }

        std::string result(data_send);
        delete context;
        return result;
    }

   private:
    void set_transaction(Context *context) {
        context->txn_ = txn_manager_->get_transaction(txn_id_);
        if (context->txn_ == nullptr ||
            context->txn_->get_state() == TransactionState::COMMITTED ||
            context->txn_->get_state() == TransactionState::ABORTED) {
            context->txn_ = txn_manager_->begin(nullptr, context->log_mgr_);
            txn_id_ = context->txn_->get_transaction_id();
            context->txn_->set_txn_mode(false);
        }
    }

    SmManager *sm_manager_;
    LockManager *lock_manager_;
    TransactionManager *txn_manager_;
    QlManager *ql_manager_;
    LogManager *log_manager_;
    Planner *planner_;
    Optimizer *optimizer_;
    Portal *portal_;
    Analyze *analyze_;
    pthread_mutex_t buffer_mutex_;
    txn_id_t txn_id_ = INVALID_TXN_ID;
};

}  // anonymous namespace

// ============================================================
// 测试夹具
// ============================================================
class SqlTest : public ::testing::Test {
   protected:
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
        enter_test_dir();

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

        sm_manager_->create_db(TEST_DB);
        sm_manager_->open_db(TEST_DB);

        sql_helper_.init(sm_manager_, lock_manager_, txn_manager_, ql_manager_,
                         log_manager_, planner_, optimizer_, portal_, analyze_);
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

        leave_test_dir();
    }

    // ------ 断言工具 ------

    void expect_success(const std::string &result) {
        EXPECT_TRUE(result.empty() || result.find("Error:") == std::string::npos)
            << "Expected success but got: [" << result << "]";
    }

    void expect_error(const std::string &result) {
        EXPECT_TRUE(result.find("Error:") != std::string::npos)
            << "Expected error but got: [" << result << "]";
    }

    void expect_output_contains(const std::string &result, const std::string &substr) {
        EXPECT_NE(result.find(substr), std::string::npos)
            << "Expected output to contain \"" << substr << "\" but got:\n" << result;
    }

    void expect_output_not_contains(const std::string &result, const std::string &substr) {
        EXPECT_EQ(result.find(substr), std::string::npos)
            << "Expected output NOT to contain \"" << substr << "\" but got:\n" << result;
    }

   private:
    static constexpr const char *TEST_DIR = "oj_sql_test_dir";
    static constexpr const char *TEST_DB = "oj_sql_test_db";

    void enter_test_dir() {
        if (!disk_manager_->is_dir(TEST_DIR)) {
            disk_manager_->create_dir(TEST_DIR);
        }
        ASSERT_TRUE(disk_manager_->is_dir(TEST_DIR));
        if (chdir(TEST_DIR) < 0) {
            perror("chdir");
            FAIL() << "Cannot enter test directory: " << TEST_DIR;
        }
    }

    void leave_test_dir() {
        if (chdir("..") < 0) {
            perror("chdir");
        }
        if (disk_manager_->is_dir(TEST_DIR)) {
            disk_manager_->destroy_dir(TEST_DIR);
        }
    }
};

// ============================================================
// BIGINT 测试用例（Problem 3）
// ============================================================

TEST_F(SqlTest, CreateTableWithBigInt) {
    auto res = sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT, sid INT);");
    expect_success(res);
}

TEST_F(SqlTest, InsertAndSelectBigInt) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT, sid INT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (372036854775807, 233421);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (-922337203685477580, 124332);"));

    auto res = sql_helper_.execute_sql("SELECT * FROM test_bigint;");
    expect_output_contains(res, "372036854775807");
    expect_output_contains(res, "233421");
    expect_output_contains(res, "-922337203685477580");
    expect_output_contains(res, "124332");
    expect_output_contains(res, "Total record(s): 2");
}

TEST_F(SqlTest, IntAutoWidenToBigInt) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT, sid INT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (42, 100);"));

    auto res = sql_helper_.execute_sql("SELECT * FROM test_bigint;");
    expect_output_contains(res, "42");
    expect_output_contains(res, "Total record(s): 1");
}

TEST_F(SqlTest, BigIntNarrowToIntInRange) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE t_int (val INT);"));

    // In-range: should succeed
    expect_success(sql_helper_.execute_sql("INSERT INTO t_int VALUES (999);"));
    auto res1 = sql_helper_.execute_sql("SELECT * FROM t_int;");
    expect_output_contains(res1, "999");

    // Out-of-range: should fail
    auto res2 = sql_helper_.execute_sql("INSERT INTO t_int VALUES (999999999999);");
    expect_error(res2);
}

TEST_F(SqlTest, BigIntBoundaryValues) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (0);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (1);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (-1);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (9223372036854775807);"));   // LLONG_MAX
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (-9223372036854775808);"));  // LLONG_MIN

    auto res = sql_helper_.execute_sql("SELECT * FROM test_bigint;");
    expect_output_contains(res, "Total record(s): 5");
    expect_output_contains(res, "9223372036854775807");
    expect_output_contains(res, "-9223372036854775808");
}

TEST_F(SqlTest, BigIntWhereClause) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT, sid INT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (1000, 10);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (2000, 20);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (3000, 30);"));

    auto res = sql_helper_.execute_sql("SELECT * FROM test_bigint WHERE bid > 1500;");
    expect_output_contains(res, "2000");
    expect_output_contains(res, "3000");
    expect_output_not_contains(res, "1000");
    expect_output_contains(res, "Total record(s): 2");
}

TEST_F(SqlTest, EmptyTableBigInt) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT);"));
    auto res = sql_helper_.execute_sql("SELECT * FROM test_bigint;");
    expect_output_contains(res, "Total record(s): 0");
}

// ============================================================
// DATETIME 测试用例（Problem 4）
// ============================================================

TEST_F(SqlTest, CreateTableWithDateTime) {
    auto res = sql_helper_.execute_sql("CREATE TABLE test_datetime (dt DATETIME, val INT);");
    expect_success(res);
}

TEST_F(SqlTest, InsertAndSelectDateTime) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_datetime (dt DATETIME, val INT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-05-18 09:12:19', 100);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('1999-12-31 23:59:59', 200);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2000-01-01 00:00:00', 300);"));

    auto res = sql_helper_.execute_sql("SELECT * FROM test_datetime;");
    expect_output_contains(res, "2023-05-18 09:12:19");
    expect_output_contains(res, "1999-12-31 23:59:59");
    expect_output_contains(res, "2000-01-01 00:00:00");
    expect_output_contains(res, "Total record(s): 3");
}

TEST_F(SqlTest, InsertInvalidDateTime) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_datetime (dt DATETIME, val INT);"));

    // Invalid month 13
    auto r1 = sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('1999-13-07 12:30:00', 36);");
    expect_error(r1);

    // Invalid day 30 in Feb (non-leap)
    auto r2 = sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-02-30 10:00:00', 1);");
    expect_error(r2);

    // Invalid day 29 in Feb (non-leap)
    auto r3 = sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-02-29 10:00:00', 2);");
    expect_error(r3);

    // Valid leap year Feb 29
    auto r4 = sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2024-02-29 10:00:00', 3);");
    expect_success(r4);

    // Invalid hour 25
    auto r5 = sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-05-18 25:00:00', 4);");
    expect_error(r5);

    // Invalid minute 60
    auto r6 = sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-05-18 10:60:00', 5);");
    expect_error(r6);

    // Invalid second 99
    auto r7 = sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-05-18 10:30:99', 6);");
    expect_error(r7);

    // Wrong separator
    auto r8 = sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023/05/18 10:30:00', 7);");
    expect_error(r8);

    // Wrong format (too short)
    auto r9 = sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-05-18', 8);");
    expect_error(r9);
}

TEST_F(SqlTest, DateTimeBoundaryValues) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_datetime (dt DATETIME);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('1000-01-01 00:00:00');"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('9999-12-31 23:59:59');"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2024-02-29 12:00:00');"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2000-01-01 00:00:00');"));

    auto res = sql_helper_.execute_sql("SELECT * FROM test_datetime;");
    expect_output_contains(res, "1000-01-01 00:00:00");
    expect_output_contains(res, "9999-12-31 23:59:59");
    expect_output_contains(res, "2024-02-29 12:00:00");
    expect_output_contains(res, "2000-01-01 00:00:00");
    expect_output_contains(res, "Total record(s): 4");
}

TEST_F(SqlTest, DateTimeWhereClause) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_datetime (dt DATETIME, val INT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-01-01 00:00:00', 10);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-06-15 12:00:00', 20);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-12-31 23:59:59', 30);"));

    auto res = sql_helper_.execute_sql("SELECT * FROM test_datetime WHERE dt < '2023-06-15 12:00:00';");

    expect_output_contains(res, "2023-01-01 00:00:00");
    expect_output_contains(res, "10");
    expect_output_not_contains(res, "2023-06-15 12:00:00");
    expect_output_not_contains(res, "2023-12-31 23:59:59");
    expect_output_contains(res, "Total record(s): 1");
}

TEST_F(SqlTest, DateTimeWhereEq) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_datetime (dt DATETIME, val INT);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-01-01 00:00:00', 10);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-06-15 12:00:00', 20);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_datetime VALUES ('2023-12-31 23:59:59', 30);"));

    auto res = sql_helper_.execute_sql("SELECT * FROM test_datetime WHERE dt = '2023-06-15 12:00:00';");

    expect_output_contains(res, "2023-06-15 12:00:00");
    expect_output_contains(res, "20");
    expect_output_not_contains(res, "2023-01-01");
    expect_output_not_contains(res, "2023-12-31");
    expect_output_contains(res, "Total record(s): 1");
}

TEST_F(SqlTest, EmptyTableDateTime) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_datetime (dt DATETIME);"));
    auto res = sql_helper_.execute_sql("SELECT * FROM test_datetime;");
    expect_output_contains(res, "Total record(s): 0");
}
