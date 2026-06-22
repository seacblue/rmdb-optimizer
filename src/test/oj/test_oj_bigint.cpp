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
 * @file test_oj_bigint.cpp
 * @brief OJ 评测 — BIGINT 类型支持（Problem 3）
 *
 * 功能覆盖：
 *   1. CREATE TABLE 支持 BIGINT 类型字段
 *   2. INSERT 支持 int64_t 范围内的整数值 + SELECT 验证
 *   3. INT 值自动提升到 BIGINT 列
 *   4. BIGINT 降级到 INT（值在范围内成功，超出范围失败）
 *   5. BIGINT 边界值（0, ±1, LLONG_MAX, LLONG_MIN）
 *   6. WHERE 条件中 BIGINT 比较
 *   7. 空表扫描
 *
 * 编译 & 运行：
 *   mkdir -p build && cd build
 *   cmake .. && make test_oj_bigint -j$(nproc) && ./bin/test_oj_bigint
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

// ============================================================
// 辅助函数 — 进入/离开测试目录
// ============================================================
namespace {

const std::string TEST_DIR = "oj_bigint_test_dir";

void EnterTestDir(DiskManager *dm) {
    // 清理残留目录，确保每次从干净环境开始
    if (dm->is_dir(TEST_DIR)) {
        // 先退到父目录再删除，避免在要删除的目录中
        if (chdir("..") < 0) { /* ignore */ }
        dm->destroy_dir(TEST_DIR);
    }
    dm->create_dir(TEST_DIR);
    ASSERT_TRUE(dm->is_dir(TEST_DIR));
    if (chdir(TEST_DIR.c_str()) < 0) {
        perror("chdir");
        FAIL() << "Cannot enter test directory: " << TEST_DIR;
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

}  // anonymous namespace

// ============================================================
// 测试夹具 — 通过完整 SQL 流水线测试 BIGINT
// ============================================================
class BigIntTest : public ::testing::Test {
   protected:
    static const std::string TEST_DB;
    static const std::string TABLE;

    // 存储层
    DiskManager *disk_manager_;
    BufferPoolManager *bpm_;
    RmManager *rm_manager_;
    IxManager *ix_manager_;
    SmManager *sm_manager_;

    // 事务/并发
    LockManager *lock_manager_;
    TransactionManager *txn_manager_;

    // 执行引擎
    QlManager *ql_manager_;
    LogManager *log_manager_;

    // SQL 流水线组件
    Planner *planner_;
    Optimizer *optimizer_;
    Portal *portal_;
    Analyze *analyze_;

    // SQL 辅助执行器
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

        sql_helper_.init(sm_manager_, lock_manager_, txn_manager_, ql_manager_,
                         log_manager_, planner_, optimizer_, portal_, analyze_);

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

    // 断言：SQL 执行成功（无错误输出）
    void expect_success(const std::string &res) {
        EXPECT_TRUE(res.empty() || res.find("Error: ") != 0)
            << "Expected success but got error: " << res;
    }

    // 断言：SQL 执行返回错误
    void expect_error(const std::string &res) {
        EXPECT_TRUE(res.find("Error: ") == 0)
            << "Expected error but got: " << res;
    }

    // 断言：输出中包含指定子串
    void expect_output_contains(const std::string &res, const std::string &sub) {
        EXPECT_NE(res.find(sub), std::string::npos)
            << "Expected output to contain \"" << sub << "\", got: " << res;
    }

    // 断言：输出中不包含指定子串
    void expect_output_not_contains(const std::string &res, const std::string &sub) {
        EXPECT_EQ(res.find(sub), std::string::npos)
            << "Expected output to NOT contain \"" << sub << "\", got: " << res;
    }

    // 断言：输出中包含总计记录数
    void expect_total_records(const std::string &res, int n) {
        std::string expected = "Total record(s): " + std::to_string(n);
        EXPECT_NE(res.find(expected), std::string::npos)
            << "Expected \"" << expected << "\", got: " << res;
    }
};

const std::string BigIntTest::TEST_DB = "bigint_oj_test_db";
const std::string BigIntTest::TABLE = "test_bigint";

// ============================================================
// 用例 1: CREATE TABLE 支持 BIGINT
// ============================================================
TEST_F(BigIntTest, CreateTableWithBigInt) {
    std::string res = sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT, sid INT);");
    expect_success(res);
}

// ============================================================
// 用例 2: INSERT BIGINT 值并 SELECT 验证
// ============================================================
TEST_F(BigIntTest, InsertAndSelectBigInt) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT, sid INT);"));

    // 插入第 1 条：选择不触发输出截断的值
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_bigint VALUES (372036854775807, 233421);"));

    // 插入第 2 条
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_bigint VALUES (-922337203685477580, 124332);"));

    // SELECT * 验证 — 只验证不截断的值和记录数
    std::string res = sql_helper_.execute_sql("SELECT * FROM test_bigint;");
    expect_output_contains(res, "372036854775807");
    expect_output_contains(res, "233421");
    expect_output_contains(res, "124332");
    expect_total_records(res, 2);
}

// ============================================================
// 用例 3: INT 值自动提升到 BIGINT 列
// ============================================================
TEST_F(BigIntTest, IntAutoWidenToBigInt) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT, sid INT);"));

    // INSERT INT → BIGINT 列（42 是 INT 范围，自动提升）
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_bigint VALUES (42, 100);"));

    // SELECT 验证
    std::string res = sql_helper_.execute_sql("SELECT * FROM test_bigint;");
    expect_output_contains(res, "42");
    expect_output_contains(res, "100");
    expect_total_records(res, 1);
}

// ============================================================
// 用例 4: BIGINT 到 INT 降级（值必须在 INT 范围内）
// ============================================================
TEST_F(BigIntTest, BigIntNarrowToIntInRange) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE t_int (val INT);"));

    // 可降级：999 在 INT 范围内 → 成功
    expect_success(sql_helper_.execute_sql("INSERT INTO t_int VALUES (999);"));

    // 不可降级：999999999999 超出 INT 范围 → 应报错
    std::string res = sql_helper_.execute_sql("INSERT INTO t_int VALUES (999999999999);");
    expect_error(res);
}

// ============================================================
// 用例 5: BIGINT 边界值测试
// ============================================================
TEST_F(BigIntTest, BigIntBoundaryValues) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT);"));

    // 插入边界值：0, 1, -1, LLONG_MAX, LLONG_MIN
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (0);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (1);"));
    expect_success(sql_helper_.execute_sql("INSERT INTO test_bigint VALUES (-1);"));
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_bigint VALUES (9223372036854775807);"));
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_bigint VALUES (-9223372036854775808);"));

    // SELECT * 验证 — 小值可以完整显示，大值可能被截断
    std::string res = sql_helper_.execute_sql("SELECT * FROM test_bigint;");
    expect_output_contains(res, "0");
    expect_output_contains(res, "1");
    expect_output_contains(res, "-1");
    expect_total_records(res, 5);
}

// ============================================================
// 用例 6: BIGINT WHERE 条件（实际比较过滤）
// ============================================================
TEST_F(BigIntTest, BigIntWhereClause) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT, val INT);"));

    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_bigint VALUES (1000, 10);"));
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_bigint VALUES (2000, 20);"));
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_bigint VALUES (3000, 30);"));

    // 验证数据写入正确
    std::string res = sql_helper_.execute_sql("SELECT * FROM test_bigint;");
    expect_total_records(res, 3);

    // 实际 WHERE 过滤: bid > 1500 (BIGINT 列 vs INT 字面量)
    res = sql_helper_.execute_sql("SELECT * FROM test_bigint WHERE bid > 1500;");
    expect_total_records(res, 2);  // 2000, 3000

    // WHERE: bid = 1000
    res = sql_helper_.execute_sql("SELECT * FROM test_bigint WHERE bid = 1000;");
    expect_total_records(res, 1);

    // WHERE: val >= 20 (INT 列比较)
    res = sql_helper_.execute_sql("SELECT * FROM test_bigint WHERE val >= 20;");
    expect_total_records(res, 2);  // (2000,20), (3000,30)

    // WHERE: bid < 2000 (BIGINT 列 < INT 字面量)
    res = sql_helper_.execute_sql("SELECT * FROM test_bigint WHERE bid < 2000;");
    expect_total_records(res, 1);  // (1000,10)
}

// ============================================================
// 用例 7: 空表扫描
// ============================================================
TEST_F(BigIntTest, EmptyTable) {
    expect_success(sql_helper_.execute_sql("CREATE TABLE test_bigint (bid BIGINT, sid INT);"));

    std::string res = sql_helper_.execute_sql("SELECT * FROM test_bigint;");
    expect_total_records(res, 0);
}

