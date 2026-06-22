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
 *   2. INSERT 支持 int64_t 范围内的整数值
 *   3. SELECT 正确读取 BIGINT 值  
 *   4. 超出 BIGINT 范围的值插入应抛出异常
 *   5. BIGINT 与 INT 的隐式转换（INSERT 时类型兼容）
 *   6. WHERE 条件中 BIGINT 使用
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

// ============================================================
// 辅助函数（代替 test_utils.h，避免跨目录依赖）
// ============================================================
namespace {

const std::string TEST_DIR = "oj_bigint_test_dir";

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
class BigIntTest : public ::testing::Test {
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

const std::string BigIntTest::TEST_DB = "bigint_oj_test_db";
const std::string BigIntTest::TABLE = "test_bigint";

// ============================================================
// 用例 1: CREATE TABLE 支持 BIGINT
// ============================================================
TEST_F(BigIntTest, CreateTableWithBigInt) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"bid", TYPE_BIGINT, sizeof(int64_t)});
    col_defs.push_back({"sid", TYPE_INT, sizeof(int)});
    ASSERT_NO_THROW(sm_manager_->create_table(TABLE, col_defs, nullptr));

    auto &tab = sm_manager_->db_.get_table(TABLE);
    ASSERT_EQ(tab.cols.size(), 2);
    EXPECT_EQ(tab.cols[0].type, TYPE_BIGINT);
    EXPECT_EQ(tab.cols[0].len, sizeof(int64_t));
    EXPECT_EQ(tab.cols[1].type, TYPE_INT);
    EXPECT_EQ(tab.cols[1].len, sizeof(int));
}

// ============================================================
// 用例 2: INSERT BIGINT 值并 SELECT 验证
// ============================================================
TEST_F(BigIntTest, InsertAndSelectBigInt) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"bid", TYPE_BIGINT, sizeof(int64_t)});
    col_defs.push_back({"sid", TYPE_INT, sizeof(int)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);
    auto &tab = sm_manager_->db_.get_table(TABLE);

    // 插入第 1 条
    {
        Value v1, v2;
        v1.set_bigint(372036854775807LL);
        v2.set_int(233421);
        std::vector<Value> vals = {v1, v2};
        InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
        exec.Next();
    }

    // 插入第 2 条
    {
        Value v1, v2;
        v1.set_bigint(-922337203685477580LL);
        v2.set_int(124332);
        std::vector<Value> vals = {v1, v2};
        InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
        exec.Next();
    }

    // SELECT * 验证
    SeqScanExecutor scan(sm_manager_, TABLE, {}, nullptr);
    scan.beginTuple();

    // 第 1 条
    ASSERT_FALSE(scan.is_end());
    auto rec1 = scan.Next();
    ASSERT_NE(rec1, nullptr);
    EXPECT_EQ(*(int64_t *)(rec1->data + tab.cols[0].offset), 372036854775807LL);
    EXPECT_EQ(*(int *)(rec1->data + tab.cols[1].offset), 233421);
    scan.nextTuple();

    // 第 2 条
    ASSERT_FALSE(scan.is_end());
    auto rec2 = scan.Next();
    ASSERT_NE(rec2, nullptr);
    EXPECT_EQ(*(int64_t *)(rec2->data + tab.cols[0].offset), -922337203685477580LL);
    EXPECT_EQ(*(int *)(rec2->data + tab.cols[1].offset), 124332);
    scan.nextTuple();

    EXPECT_TRUE(scan.is_end());
}

// ============================================================
// 用例 3: INT 值自动提升到 BIGINT 列
// ============================================================
TEST_F(BigIntTest, IntAutoWidenToBigInt) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"bid", TYPE_BIGINT, sizeof(int64_t)});
    col_defs.push_back({"sid", TYPE_INT, sizeof(int)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);
    auto &tab = sm_manager_->db_.get_table(TABLE);

    // INSERT INT → BIGINT 列
    {
        Value v1, v2;
        v1.set_int(42);
        v2.set_int(100);
        std::vector<Value> vals = {v1, v2};
        EXPECT_NO_THROW({
            InsertExecutor exec(sm_manager_, TABLE, vals, nullptr);
            exec.Next();
        });
    }

    // 验证
    SeqScanExecutor scan(sm_manager_, TABLE, {}, nullptr);
    scan.beginTuple();
    ASSERT_FALSE(scan.is_end());
    auto rec = scan.Next();
    EXPECT_EQ(*(int64_t *)(rec->data + tab.cols[0].offset), 42LL);
}

// ============================================================
// 用例 4: BIGINT 降级到 INT（值必须在范围内）
// ============================================================
TEST_F(BigIntTest, BigIntNarrowToIntInRange) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"val", TYPE_INT, sizeof(int)});
    sm_manager_->create_table("t_int", col_defs, nullptr);

    // 可降级
    {
        Value v;
        v.set_bigint(999);
        std::vector<Value> vals = {v};
        EXPECT_NO_THROW({
            InsertExecutor exec(sm_manager_, "t_int", vals, nullptr);
            exec.Next();
        });
    }

    // 不可降级（超出 INT 范围）
    {
        Value v;
        v.set_bigint(999999999999LL);
        std::vector<Value> vals = {v};
        EXPECT_THROW({
            InsertExecutor exec(sm_manager_, "t_int", vals, nullptr);
            exec.Next();
        }, IncompatibleTypeError);
    }
}

// ============================================================
// 用例 5: BIGINT 边界值测试
// ============================================================
TEST_F(BigIntTest, BigIntBoundaryValues) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"bid", TYPE_BIGINT, sizeof(int64_t)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);
    auto &tab = sm_manager_->db_.get_table(TABLE);

    int64_t test_vals[] = {0LL, 1LL, -1LL, LLONG_MAX, LLONG_MIN};
    for (auto val : test_vals) {
        Value v;
        v.set_bigint(val);
        InsertExecutor exec(sm_manager_, TABLE, {v}, nullptr);
        exec.Next();
    }

    SeqScanExecutor scan(sm_manager_, TABLE, {}, nullptr);
    scan.beginTuple();
    for (auto expected : test_vals) {
        ASSERT_FALSE(scan.is_end());
        auto rec = scan.Next();
        EXPECT_EQ(*(int64_t *)(rec->data + tab.cols[0].offset), expected);
        scan.nextTuple();
    }
    EXPECT_TRUE(scan.is_end());
}

// ============================================================
// 用例 6: WHERE 条件 BIGINT 比较
// ============================================================
TEST_F(BigIntTest, BigIntWhereClause) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"bid", TYPE_BIGINT, sizeof(int64_t)});
    col_defs.push_back({"sid", TYPE_INT, sizeof(int)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);
    auto &tab = sm_manager_->db_.get_table(TABLE);

    // 插入数据
    int64_t bids[] = {1000LL, 2000LL, 3000LL};
    int sids[] = {10, 20, 30};
    for (int i = 0; i < 3; i++) {
        Value v1, v2;
        v1.set_bigint(bids[i]);
        v2.set_int(sids[i]);
        InsertExecutor exec(sm_manager_, TABLE, {v1, v2}, nullptr);
        exec.Next();
    }

    // WHERE bid > 1500 （BIGINT 列与 INT 值比较）
    Condition cond;
    cond.lhs_col.tab_name = TABLE;
    cond.lhs_col.col_name = "bid";
    cond.op = OP_GT;
    cond.is_rhs_val = true;
    cond.rhs_val.set_int(1500);

    SeqScanExecutor scan(sm_manager_, TABLE, {cond}, nullptr);
    scan.beginTuple();

    // 期望: 2000, 3000
    ASSERT_FALSE(scan.is_end());
    auto rec1 = scan.Next();
    EXPECT_EQ(*(int64_t *)(rec1->data + tab.cols[0].offset), 2000LL);
    scan.nextTuple();

    ASSERT_FALSE(scan.is_end());
    auto rec2 = scan.Next();
    EXPECT_EQ(*(int64_t *)(rec2->data + tab.cols[0].offset), 3000LL);
    scan.nextTuple();

    EXPECT_TRUE(scan.is_end());
}

// ============================================================
// 用例 7: 空表扫描
// ============================================================
TEST_F(BigIntTest, EmptyTable) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"bid", TYPE_BIGINT, sizeof(int64_t)});
    sm_manager_->create_table(TABLE, col_defs, nullptr);

    SeqScanExecutor scan(sm_manager_, TABLE, {}, nullptr);
    scan.beginTuple();
    EXPECT_TRUE(scan.is_end());
}

