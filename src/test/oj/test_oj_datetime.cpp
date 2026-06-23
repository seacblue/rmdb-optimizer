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
 * @file test_oj_datetime.cpp
 * @brief OJ 评测 — DATETIME 类型支持（Problem 4）
 *
 * 功能覆盖：
 *   1. CREATE TABLE 支持 DATETIME 类型字段
 *   2. INSERT 合法 DATETIME 字符串值 + SELECT 验证格式
 *   3. 非法 DATETIME 字符串插入应返回错误
 *   4. DATETIME 边界值（最大值/最小值）测试
 *   5. WHERE 条件中 DATETIME 比较
 *   6. WHERE 条件 DATETIME 相等比较
 *   7. 空表扫描
 *
 * 编译 & 运行：
 *   mkdir -p build && cd build
 *   cmake .. && make test_oj_datetime -j$(nproc) && ./bin/test_oj_datetime
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

const std::string TEST_DIR = "oj_datetime_test_dir";

void EnterTestDir(DiskManager *dm) {
    // 清理残留目录，确保每次从干净环境开始
    if (dm->is_dir(TEST_DIR)) {
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
// 测试夹具 — 通过完整 SQL 流水线测试 DATETIME
// ============================================================
class DateTimeTest : public ::testing::Test {
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

const std::string DateTimeTest::TEST_DB = "datetime_oj_test_db";
const std::string DateTimeTest::TABLE = "test_datetime";

// ============================================================
// 用例 1: CREATE TABLE 支持 DATETIME
// ============================================================
TEST_F(DateTimeTest, CreateTableWithDateTime) {
    std::string res = sql_helper_.execute_sql(
        "CREATE TABLE test_datetime (dt DATETIME, remark CHAR(64));");
    expect_success(res);
}

// ============================================================
// 用例 2: INSERT 合法 DATETIME 值并 SELECT 验证
// ============================================================
TEST_F(DateTimeTest, InsertAndSelectDateTime) {
    expect_success(sql_helper_.execute_sql(
        "CREATE TABLE test_datetime (dt DATETIME, val INT);"));

    // 插入第 1 条
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-05-18 09:12:19', 100);"));

    // 插入第 2 条
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('1999-12-31 23:59:59', 200);"));

    // 插入第 3 条
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2000-01-01 00:00:00', 300);"));

    // SELECT * 验证输出格式 — 注意 DATETIME 值可能在列宽内被截断
    std::string res = sql_helper_.execute_sql("SELECT * FROM test_datetime;");
    // 检查可见的前缀部分
    EXPECT_NE(res.find("2023-05-18"), std::string::npos)
        << "Expected to find '2023-05-18' in output";
    EXPECT_NE(res.find("1999-12-31"), std::string::npos)
        << "Expected to find '1999-12-31' in output";
    EXPECT_NE(res.find("2000-01-01"), std::string::npos)
        << "Expected to find '2000-01-01' in output";
    expect_total_records(res, 3);
}

// ============================================================
// 用例 3: 非法 DATETIME 值插入应返回错误
// ============================================================
TEST_F(DateTimeTest, InsertInvalidDateTime) {
    expect_success(sql_helper_.execute_sql(
        "CREATE TABLE test_datetime (dt DATETIME, val FLOAT);"));

    // 无效月份 13
    expect_error(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('1999-13-07 12:30:00', 36.0);"));

    // 无效日期 2 月 30 日
    expect_error(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-02-30 10:00:00', 1.0);"));

    // 无效日期 2 月 29 日（非闰年）
    expect_error(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-02-29 10:00:00', 2.0);"));

    // 闰年 2 月 29 日（有效）
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2024-02-29 10:00:00', 3.0);"));

    // 无效小时 25
    expect_error(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-05-18 25:00:00', 4.0);"));

    // 无效分钟 70
    expect_error(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-05-18 12:70:00', 5.0);"));

    // 无效秒 99
    expect_error(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-05-18 12:30:99', 6.0);"));

    // 格式错误：缺少空格（用 T 代替）
    expect_error(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-05-18T12:30:00', 7.0);"));

    // 年份过小
    expect_error(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('0999-01-01 00:00:00', 8.0);"));

    // 月份 0
    expect_error(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-00-01 00:00:00', 9.0);"));
}

// ============================================================
// 用例 4: DATETIME 边界值测试
// ============================================================
TEST_F(DateTimeTest, DateTimeBoundaryValues) {
    expect_success(sql_helper_.execute_sql(
        "CREATE TABLE test_datetime (dt DATETIME);"));

    // 最小值
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('1000-01-01 00:00:00');"));
    // 最大值
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('9999-12-31 23:59:59');"));
    // 闰年
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2024-02-29 12:00:00');"));
    // 千禧年
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2000-01-01 00:00:00');"));
    // 普通值
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-06-15 08:30:45');"));

    // SELECT * 验证 — 检查可见前缀
    std::string res = sql_helper_.execute_sql("SELECT * FROM test_datetime;");
    EXPECT_NE(res.find("1000-01-01"), std::string::npos);
    EXPECT_NE(res.find("9999-12-31"), std::string::npos);
    EXPECT_NE(res.find("2024-02-29"), std::string::npos);
    EXPECT_NE(res.find("2000-01-01"), std::string::npos);
    EXPECT_NE(res.find("2023-06-15"), std::string::npos);
    expect_total_records(res, 5);
}

// ============================================================
// 用例 5: WHERE 条件 DATETIME 比较（实际过滤）
// ============================================================
TEST_F(DateTimeTest, DateTimeWhereClause) {
    expect_success(sql_helper_.execute_sql(
        "CREATE TABLE test_datetime (dt DATETIME, val INT);"));

    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-01-01 00:00:00', 10);"));
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-06-15 12:00:00', 20);"));
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-12-31 23:59:59', 30);"));

    // 验证数据写入正确
    std::string res = sql_helper_.execute_sql("SELECT * FROM test_datetime;");
    expect_total_records(res, 3);

    // WHERE dt > '2023-06-01 00:00:00' → 2 条 (June 15, Dec 31)
    res = sql_helper_.execute_sql(
        "SELECT * FROM test_datetime WHERE dt > '2023-06-01 00:00:00';");
    expect_total_records(res, 2);

    // WHERE dt < '2023-06-01 00:00:00' → 1 条 (Jan 1)
    res = sql_helper_.execute_sql(
        "SELECT * FROM test_datetime WHERE dt < '2023-06-01 00:00:00';");
    expect_total_records(res, 1);

    // WHERE val > 10 → 2 条 (20, 30)
    res = sql_helper_.execute_sql(
        "SELECT * FROM test_datetime WHERE val > 10;");
    expect_total_records(res, 2);
}

// ============================================================
// 用例 6: WHERE 条件 DATETIME 相等比较
// ============================================================
TEST_F(DateTimeTest, DateTimeWhereEq) {
    expect_success(sql_helper_.execute_sql(
        "CREATE TABLE test_datetime (dt DATETIME, val INT);"));

    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-01-01 00:00:00', 10);"));
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-06-15 12:00:00', 20);"));
    expect_success(sql_helper_.execute_sql(
        "INSERT INTO test_datetime VALUES ('2023-12-31 23:59:59', 30);"));

    // 验证数据写入正确
    std::string res = sql_helper_.execute_sql("SELECT * FROM test_datetime;");
    expect_total_records(res, 3);

    // WHERE dt = '2023-06-15 12:00:00' → 1 条
    res = sql_helper_.execute_sql(
        "SELECT * FROM test_datetime WHERE dt = '2023-06-15 12:00:00';");
    expect_total_records(res, 1);

    // WHERE dt = '2024-01-01 00:00:00' → 0 条（不存在的值）
    res = sql_helper_.execute_sql(
        "SELECT * FROM test_datetime WHERE dt = '2024-01-01 00:00:00';");
    expect_total_records(res, 0);

    // WHERE val = 10 → 1 条
    res = sql_helper_.execute_sql(
        "SELECT * FROM test_datetime WHERE val = 10;");
    expect_total_records(res, 1);
}

// ============================================================
// 用例 7: 空表扫描（DATETIME 列）
// ============================================================
TEST_F(DateTimeTest, EmptyTableWithDateTime) {
    expect_success(sql_helper_.execute_sql(
        "CREATE TABLE test_datetime (dt DATETIME);"));

    std::string res = sql_helper_.execute_sql("SELECT * FROM test_datetime;");
    expect_total_records(res, 0);
}
