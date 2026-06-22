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
 *   2. INSERT 合法 DATETIME 字符串值
 *   3. SELECT 正确读取并格式化 DATETIME 值
 *   4. 非法 DATETIME 字符串插入应抛出异常（InvalidDatetimeError）
 *   5. DATETIME 边界值（最大值/最小值）测试
 *   6. WHERE 条件中 DATETIME 比较
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

#include "execution/execution_manager.h"
#include "execution/executor_abstract.h"
#include "execution/executor_insert.h"
#include "execution/executor_seq_scan.h"
#include "index/ix.h"
#include "record/rm.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "system/sm.h"
#include "system/sm_manager.h"
#include "errors.h"

// ============================================================
// DATETIME 编码/解码工具（与 common.h 保持一致）
// ============================================================
namespace {

int64_t encode_datetime(const std::string &str) {
    // str must be "YYYY-MM-DD HH:MM:SS" (19 chars)
    int year  = std::stoi(str.substr(0, 4));
    int month = std::stoi(str.substr(5, 2));
    int day   = std::stoi(str.substr(8, 2));
    int hour  = std::stoi(str.substr(11, 2));
    int min   = std::stoi(str.substr(14, 2));
    int sec   = std::stoi(str.substr(17, 2));
    return (int64_t)year * 10000000000LL + (int64_t)month * 100000000LL +
           (int64_t)day * 1000000LL + (int64_t)hour * 10000LL +
           (int64_t)min * 100LL + (int64_t)sec;
}

std::string decode_datetime(int64_t val) {
    int sec   = val % 100; val /= 100;
    int min   = val % 100; val /= 100;
    int hour  = val % 100; val /= 100;
    int day   = val % 100; val /= 100;
    int month = val % 100; val /= 100;
    int year  = static_cast<int>(val);
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, min, sec);
    return std::string(buf);
}

}  // anonymous namespace

// ============================================================
// 辅助函数（代替 test_utils.h，避免跨目录依赖）
// ============================================================
namespace {

const std::string TEST_DIR = "oj_datetime_test_dir";

void EnterTestDir(DiskManager *dm) {
    if (!dm->is_dir(TEST_DIR)) {
        dm->create_dir(TEST_DIR);
    }
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
// 测试夹具
// ============================================================
class DateTimeTest : public ::testing::Test {
   protected:
    static const std::string TEST_DB;
    static const std::string TABLE;

    DiskManager *disk_manager_;
    BufferPoolManager *bpm_;
    RmManager *rm_manager_;
    IxManager *ix_manager_;
    SmManager *sm_manager_;

    void SetUp() override {
        disk_manager_ = new DiskManager();
        EnterTestDir(disk_manager_);

        bpm_ = new BufferPoolManager(BUFFER_POOL_SIZE, disk_manager_);
        rm_manager_ = new RmManager(disk_manager_, bpm_);
        ix_manager_ = new IxManager(disk_manager_, bpm_);
        sm_manager_ = new SmManager(disk_manager_, bpm_, rm_manager_, ix_manager_);

        sm_manager_->create_db(TEST_DB);
        sm_manager_->open_db(TEST_DB);
    }

    void TearDown() override {
        sm_manager_->close_db();
        sm_manager_->drop_db(TEST_DB);

        delete sm_manager_;
        delete ix_manager_;
        delete rm_manager_;
        delete bpm_;
        delete disk_manager_;

        LeaveTestDir(disk_manager_);
    }
};

const std::string DateTimeTest::TEST_DB = "datetime_oj_test_db";
const std::string DateTimeTest::TABLE = "test_datetime";

// ============================================================
// 用例 1: CREATE TABLE 支持 DATETIME
// ============================================================
TEST_F(DateTimeTest, CreateTableWithDateTime) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"dt", TYPE_DATETIME, sizeof(int64_t)});
    col_defs.push_back({"remark", TYPE_STRING, 64});
    ASSERT_NO_THROW(sm_manager_->create_table(TABLE, col_defs, nullptr));

    auto &tab = sm_manager_->db_.get_table(TABLE);
    ASSERT_EQ(tab.cols.size(), 2);
    EXPECT_EQ(tab.cols[0].type, TYPE_DATETIME);
    EXPECT_EQ(tab.cols[0].len, sizeof(int64_t));
    EXPECT_EQ(tab.cols[1].type, TYPE_STRING);
    EXPECT_EQ(tab.cols[1].len, 64);
}

// ============================================================
// 用例 2: INSERT 合法 DATETIME 值并 SELECT 验证
// ============================================================
TEST_F(DateTimeTest, InsertAndSelectDateTime) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"dt", TYPE_DATETIME, sizeof(int64_t)});
    col_defs.push_back({"val", TYPE_INT, sizeof(int)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);
    auto &tab = sm_manager_->db_.get_table(TABLE);

    // 插入第 1 条：使用普通日期时间
    {
        Value v1, v2;
        v1.set_str("2023-05-18 09:12:19");
        v2.set_int(100);
        std::vector<Value> vals = {v1, v2};
        InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
        exec.Next();
    }

    // 插入第 2 条：使用边界附近的日期
    {
        Value v1, v2;
        v1.set_str("1999-12-31 23:59:59");
        v2.set_int(200);
        std::vector<Value> vals = {v1, v2};
        InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
        exec.Next();
    }

    // 插入第 3 条：凌晨时间
    {
        Value v1, v2;
        v1.set_str("2000-01-01 00:00:00");
        v2.set_int(300);
        std::vector<Value> vals = {v1, v2};
        InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
        exec.Next();
    }

    // SELECT * 验证
    SeqScanExecutor scan(sm_manager_, TABLE, {}, nullptr);
    scan.beginTuple();

    // 第 1 条: 2023-05-18 09:12:19
    ASSERT_FALSE(scan.is_end());
    auto rec1 = scan.Next();
    EXPECT_EQ(*(int64_t *)(rec1->data + tab.cols[0].offset),
              encode_datetime("2023-05-18 09:12:19"));
    EXPECT_EQ(*(int *)(rec1->data + tab.cols[1].offset), 100);
    EXPECT_EQ(decode_datetime(*(int64_t *)(rec1->data + tab.cols[0].offset)),
              "2023-05-18 09:12:19");
    scan.nextTuple();

    // 第 2 条: 1999-12-31 23:59:59
    ASSERT_FALSE(scan.is_end());
    auto rec2 = scan.Next();
    EXPECT_EQ(*(int64_t *)(rec2->data + tab.cols[0].offset),
              encode_datetime("1999-12-31 23:59:59"));
    EXPECT_EQ(*(int *)(rec2->data + tab.cols[1].offset), 200);
    scan.nextTuple();

    // 第 3 条: 2000-01-01 00:00:00
    ASSERT_FALSE(scan.is_end());
    auto rec3 = scan.Next();
    EXPECT_EQ(*(int64_t *)(rec3->data + tab.cols[0].offset),
              encode_datetime("2000-01-01 00:00:00"));
    EXPECT_EQ(*(int *)(rec3->data + tab.cols[1].offset), 300);
    scan.nextTuple();

    EXPECT_TRUE(scan.is_end());
}

// ============================================================
// 用例 3: 非法 DATETIME 值插入应抛出异常
// ============================================================
TEST_F(DateTimeTest, InsertInvalidDateTime) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"dt", TYPE_DATETIME, sizeof(int64_t)});
    col_defs.push_back({"val", TYPE_FLOAT, sizeof(float)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);

    // 无效月份 13
    {
        Value v1, v2;
        v1.set_str("1999-13-07 12:30:00");
        v2.set_float(36.0f);
        std::vector<Value> vals = {v1, v2};
        EXPECT_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        }, InvalidDatetimeError);
    }

    // 无效日期 2 月 30 日
    {
        Value v1, v2;
        v1.set_str("2023-02-30 10:00:00");
        v2.set_float(1.0f);
        std::vector<Value> vals = {v1, v2};
        EXPECT_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        }, InvalidDatetimeError);
    }

    // 无效日期 2 月 29 日（非闰年）
    {
        Value v1, v2;
        v1.set_str("2023-02-29 10:00:00");
        v2.set_float(2.0f);
        std::vector<Value> vals = {v1, v2};
        EXPECT_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        }, InvalidDatetimeError);
    }

    // 闰年 2 月 29 日（有效）
    {
        Value v1, v2;
        v1.set_str("2024-02-29 10:00:00");
        v2.set_float(3.0f);
        std::vector<Value> vals = {v1, v2};
        EXPECT_NO_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        });
    }

    // 无效小时 25
    {
        Value v1, v2;
        v1.set_str("2023-05-18 25:00:00");
        v2.set_float(4.0f);
        std::vector<Value> vals = {v1, v2};
        EXPECT_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        }, InvalidDatetimeError);
    }

    // 无效分钟 70
    {
        Value v1, v2;
        v1.set_str("2023-05-18 12:70:00");
        v2.set_float(5.0f);
        std::vector<Value> vals = {v1, v2};
        EXPECT_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        }, InvalidDatetimeError);
    }

    // 无效秒 99
    {
        Value v1, v2;
        v1.set_str("2023-05-18 12:30:99");
        v2.set_float(6.0f);
        std::vector<Value> vals = {v1, v2};
        EXPECT_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        }, InvalidDatetimeError);
    }

    // 格式错误：缺少空格
    {
        Value v1, v2;
        v1.set_str("2023-05-18T12:30:00");
        v2.set_float(7.0f);
        std::vector<Value> vals = {v1, v2};
        EXPECT_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        }, InvalidDatetimeError);
    }

    // 年份过小
    {
        Value v1, v2;
        v1.set_str("0999-01-01 00:00:00");
        v2.set_float(8.0f);
        std::vector<Value> vals = {v1, v2};
        EXPECT_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        }, InvalidDatetimeError);
    }

    // 月份 0
    {
        Value v1, v2;
        v1.set_str("2023-00-01 00:00:00");
        v2.set_float(9.0f);
        std::vector<Value> vals = {v1, v2};
        EXPECT_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        }, InvalidDatetimeError);
    }
}

// ============================================================
// 用例 4: DATETIME 边界值测试
// ============================================================
TEST_F(DateTimeTest, DateTimeBoundaryValues) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"dt", TYPE_DATETIME, sizeof(int64_t)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);
    auto &tab = sm_manager_->db_.get_table(TABLE);

    // 测试值：最小、最大、特殊日期
    std::vector<std::string> test_strs = {
        "1000-01-01 00:00:00",   // 最小值
        "9999-12-31 23:59:59",   // 最大值
        "2024-02-29 12:00:00",   // 闰年
        "2000-01-01 00:00:00",   // 千禧年
        "2023-06-15 08:30:45",   // 普通值
    };

    for (auto &s : test_strs) {
        Value v;
        v.set_str(s);
        InsertExecutor exec(sm_manager_, TABLE, {v}, nullptr);
        exec.Next();
    }

    SeqScanExecutor scan(sm_manager_, TABLE, {}, nullptr);
    scan.beginTuple();

    for (auto &expected_str : test_strs) {
        ASSERT_FALSE(scan.is_end());
        auto rec = scan.Next();
        int64_t encoded = *(int64_t *)(rec->data + tab.cols[0].offset);
        EXPECT_EQ(encoded, encode_datetime(expected_str));
        EXPECT_EQ(decode_datetime(encoded), expected_str);
        scan.nextTuple();
    }
    EXPECT_TRUE(scan.is_end());
}

// ============================================================
// 用例 5: WHERE 条件 DATETIME 比较
// ============================================================
TEST_F(DateTimeTest, DateTimeWhereClause) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"dt", TYPE_DATETIME, sizeof(int64_t)});
    col_defs.push_back({"val", TYPE_INT, sizeof(int)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);
    auto &tab = sm_manager_->db_.get_table(TABLE);

    // 插入多条数据
    struct Row {
        std::string dt_str;
        int val;
    };
    Row rows[] = {
        {"2023-01-01 00:00:00", 10},
        {"2023-06-15 12:00:00", 20},
        {"2023-12-31 23:59:59", 30},
    };

    for (auto &r : rows) {
        Value v1, v2;
        v1.set_str(r.dt_str);
        v2.set_int(r.val);
        InsertExecutor exec(sm_manager_, TABLE, {v1, v2}, nullptr);
        exec.Next();
    }

    // WHERE dt < '2023-06-15 12:00:00'
    // 注意：由于测试绕过分析器，需要直接传入编码后的 int64_t 值
    Condition cond;
    cond.lhs_col.tab_name = TABLE;
    cond.lhs_col.col_name = "dt";
    cond.op = OP_LT;
    cond.is_rhs_val = true;
    cond.rhs_val.set_datetime(encode_datetime("2023-06-15 12:00:00"));

    SeqScanExecutor scan(sm_manager_, TABLE, {cond}, nullptr);
    scan.beginTuple();

    // 期望: 2023-01-01 00:00:00 (val=10)
    ASSERT_FALSE(scan.is_end());
    auto rec1 = scan.Next();
    EXPECT_EQ(*(int64_t *)(rec1->data + tab.cols[0].offset),
              encode_datetime("2023-01-01 00:00:00"));
    EXPECT_EQ(*(int *)(rec1->data + tab.cols[1].offset), 10);
    scan.nextTuple();

    EXPECT_TRUE(scan.is_end());
}

// ============================================================
// 用例 6: WHERE 条件 DATETIME 相等比较
// ============================================================
TEST_F(DateTimeTest, DateTimeWhereEq) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"dt", TYPE_DATETIME, sizeof(int64_t)});
    col_defs.push_back({"val", TYPE_INT, sizeof(int)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);
    auto &tab = sm_manager_->db_.get_table(TABLE);

    struct Row {
        std::string dt_str;
        int val;
    };
    Row rows[] = {
        {"2023-01-01 00:00:00", 10},
        {"2023-06-15 12:00:00", 20},
        {"2023-12-31 23:59:59", 30},
    };

    for (auto &r : rows) {
        Value v1, v2;
        v1.set_str(r.dt_str);
        v2.set_int(r.val);
        InsertExecutor exec(sm_manager_, TABLE, {v1, v2}, nullptr);
        exec.Next();
    }

    // WHERE dt = '2023-06-15 12:00:00'
    Condition cond;
    cond.lhs_col.tab_name = TABLE;
    cond.lhs_col.col_name = "dt";
    cond.op = OP_EQ;
    cond.is_rhs_val = true;
    cond.rhs_val.set_datetime(encode_datetime("2023-06-15 12:00:00"));

    SeqScanExecutor scan(sm_manager_, TABLE, {cond}, nullptr);
    scan.beginTuple();

    ASSERT_FALSE(scan.is_end());
    auto rec = scan.Next();
    EXPECT_EQ(*(int64_t *)(rec->data + tab.cols[0].offset),
              encode_datetime("2023-06-15 12:00:00"));
    EXPECT_EQ(*(int *)(rec->data + tab.cols[1].offset), 20);
    scan.nextTuple();

    EXPECT_TRUE(scan.is_end());
}

// ============================================================
// 用例 7: 空表扫描（DATETIME 列）
// ============================================================
TEST_F(DateTimeTest, EmptyTableWithDateTime) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"dt", TYPE_DATETIME, sizeof(int64_t)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);

    SeqScanExecutor scan(sm_manager_, TABLE, {}, nullptr);
    scan.beginTuple();
    EXPECT_TRUE(scan.is_end());
}
