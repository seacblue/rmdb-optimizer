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
 * @file test_execution.cpp
 * @brief 执行器模块（8 种 Executor）单元测试。
 *
 * 覆盖场景：
 *   - InsertExecutor ：插入单条记录并验证 rid
 *   - SeqScanExecutor：全表扫描 + 条件过滤
 *   - UpdateExecutor ：更新记录并验证字段变更
 *   - DeleteExecutor ：删除记录并验证扫描结果为空
 *   - ProjectionExecutor：投影部分列
 *   - SortExecutor   ：单列排序（升序 / 降序）
 *   - NestedLoopJoinExecutor：两个表的等值连接
 *   - IndexScanExecutor：通过索引（退化全表扫描）条件扫描
 *
 * 编译 & 运行：
 *   mkdir -p build && cd build
 *   cmake .. -DENABLE_COVERAGE=ON && make test_execution -j$(nproc)
 *   ./bin/test_execution
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "execution/execution_manager.h"
#include "execution/executor_abstract.h"
#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "execution/execution_sort.h"
#include "index/ix.h"
#include "record/rm.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "system/sm.h"
#include "system/sm_manager.h"
#include "test_utils.h"
#include "common/config.h"

// ============================================================
// 测试夹具
// ============================================================
class ExecutorTest : public ::testing::Test {
   protected:
    static const std::string TEST_DB;
    static const std::string TABLE1;
    static const std::string TABLE2;

    DiskManager *disk_manager_;
    BufferPoolManager *bpm_;
    RmManager *rm_manager_;
    IxManager *ix_manager_;
    SmManager *sm_manager_;
    Context *context_;

    void SetUp() override {
        disk_manager_ = new DiskManager();
        test_utils::EnterTestDir(disk_manager_);

        bpm_ = new BufferPoolManager(BUFFER_POOL_SIZE, disk_manager_);
        rm_manager_ = new RmManager(disk_manager_, bpm_);
        ix_manager_ = new IxManager(disk_manager_, bpm_);
        sm_manager_ = new SmManager(disk_manager_, bpm_, rm_manager_, ix_manager_);

        // 创建数据库
        sm_manager_->create_db(TEST_DB);
        sm_manager_->open_db(TEST_DB);

        // 创建表1: students(id INT, name CHAR(16), score FLOAT)
        // 手动构造 ColDef 列表，便于精确控制字段顺序
        {
            std::vector<ColDef> col_defs;
            col_defs.push_back({"id", TYPE_INT, 4});
            col_defs.push_back({"name", TYPE_STRING, 16});
            col_defs.push_back({"score", TYPE_FLOAT, 4});
            sm_manager_->create_table(TABLE1, col_defs, context_);
        }

        // 创建表2: courses(cid INT, title CHAR(16))
        {
            std::vector<ColDef> col_defs;
            col_defs.push_back({"cid", TYPE_INT, 4});
            col_defs.push_back({"title", TYPE_STRING, 16});
            sm_manager_->create_table(TABLE2, col_defs, context_);
        }

        context_ = new Context(nullptr, nullptr, nullptr);
    }

    void TearDown() override {
        delete context_;
        sm_manager_->close_db();
        delete sm_manager_;
        delete ix_manager_;
        delete rm_manager_;
        delete bpm_;

        test_utils::LeaveTestDir(disk_manager_);
        delete disk_manager_;
    }

    /** 获取表的 TabMeta */
    TabMeta get_tab(const std::string &name) {
        return sm_manager_->db_.get_table(name);
    }

    /** 获取表的文件句柄 */
    RmFileHandle *get_fh(const std::string &name) {
        return sm_manager_->fhs_.at(name).get();
    }

    /** 构造一条 int 值 (Value) */
    static Value make_int_val(int v) {
        Value val;
        val.set_int(v);
        return val;
    }

    /** 构造一条 float 值 (Value) */
    static Value make_float_val(float v) {
        Value val;
        val.set_float(v);
        return val;
    }

    /** 构造一条 string 值 (Value) */
    static Value make_str_val(const std::string &s) {
        Value val;
        val.set_str(s);
        return val;
    }

    /** 构造条件：列 OP 列（同一张表的列-列比较） */
    static Condition make_cond_col(const std::string &tab1, const std::string &col1,
                                    CompOp op, const std::string &tab2, const std::string &col2) {
        Condition c;
        c.lhs_col = {tab1, col1};
        c.is_rhs_val = false;
        c.rhs_col = {tab2, col2};
        c.op = op;
        return c;
    }

    /** 构造 TabCol */
    static TabCol make_tabcol(const std::string &tab, const std::string &col) {
        return {tab, col};
    }

    /** 构造条件：列 OP 值 */
    static Condition make_cond_val(const std::string &tab, const std::string &col,
                                   CompOp op, const Value &val) {
        Condition c;
        c.lhs_col = {tab, col};
        c.is_rhs_val = true;
        c.rhs_val = val;
        c.op = op;
        return c;
    }
};

const std::string ExecutorTest::TEST_DB = "exec_test_db";
const std::string ExecutorTest::TABLE1 = "students";
const std::string ExecutorTest::TABLE2 = "courses";

// ============================================================
// 测试用例
// ============================================================

// ---------- 1. InsertExecutor ----------
TEST_F(ExecutorTest, InsertAndScan) {
    auto tab = get_tab(TABLE1);
    auto fh = get_fh(TABLE1);

    // 插入一条学生记录 (id=1, name='Alice', score=95.5)
    std::vector<Value> vals = {make_int_val(1), make_str_val("Alice"), make_float_val(95.5f)};
    auto insert_exec = std::make_unique<InsertExecutor>(sm_manager_, TABLE1, vals, context_);

    insert_exec->beginTuple();
    EXPECT_FALSE(insert_exec->is_end());
    auto rec = insert_exec->Next();
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->size, fh->get_file_hdr().record_size);

    // 验证插入的记录可以通过 RmFileHandle 读到
    auto read_rec = fh->get_record(insert_exec->rid(), context_);
    EXPECT_EQ(*(int *)(read_rec->data + tab.cols[0].offset), 1);
    EXPECT_FLOAT_EQ(*(float *)(read_rec->data + tab.cols[2].offset), 95.5f);
}

// ---------- 2. SeqScanExecutor ----------
TEST_F(ExecutorTest, SeqScanWithFilter) {
    auto tab = get_tab(TABLE1);
    // 插入多条学生记录
    auto insert1 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(1), make_str_val("Alice"), make_float_val(95.0f)}, context_);
    insert1->Next();
    auto insert2 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(2), make_str_val("Bob"), make_float_val(80.0f)}, context_);
    insert2->Next();
    auto insert3 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(3), make_str_val("Charlie"), make_float_val(70.0f)}, context_);
    insert3->Next();

    // 全表扫描（无条件）
    {
        std::vector<Condition> empty_conds;
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
        int count = 0;
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) {
            auto r = seq->Next();
            ASSERT_NE(r, nullptr);
            count++;
        }
        EXPECT_EQ(count, 3);
    }

    // 条件扫描：score >= 80.0
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "score", OP_GE, make_float_val(80.0f)));
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, conds, context_);
        int count = 0;
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) {
            auto r = seq->Next();
            ASSERT_NE(r, nullptr);
            count++;
        }
        EXPECT_EQ(count, 2);  // Alice(95) and Bob(80)
    }

    // 条件扫描：id = 1
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "id", OP_EQ, make_int_val(1)));
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, conds, context_);
        seq->beginTuple();
        EXPECT_FALSE(seq->is_end());
        auto r = seq->Next();
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(*(int *)(r->data + tab.cols[0].offset), 1);
    }
}

// ---------- 3. UpdateExecutor ----------
TEST_F(ExecutorTest, UpdateRecord) {
    auto tab = get_tab(TABLE1);
    auto fh = get_fh(TABLE1);

    // 插入一条记录
    auto insert = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(1), make_str_val("Alice"), make_float_val(90.0f)}, context_);
    insert->Next();
    Rid rid = insert->rid();

    // 通过 SeqScan 找到该记录的 rid
    std::vector<Rid> rids;
    {
        std::vector<Condition> empty_conds;
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) {
            rids.push_back(seq->rid());
        }
    }
    ASSERT_EQ(rids.size(), 1);

    // 构造 SET 子句：score = 100.0
    SetClause set_clause;
    set_clause.lhs = make_tabcol(TABLE1, "score");
    set_clause.rhs = make_float_val(100.0f);

    auto update = std::make_unique<UpdateExecutor>(sm_manager_, TABLE1,
        std::vector<SetClause>{set_clause},
        std::vector<Condition>{}, rids, context_);
    update->Next();

    // 验证更新结果
    auto verify_rec = fh->get_record(rids[0], context_);
    EXPECT_FLOAT_EQ(*(float *)(verify_rec->data + tab.cols[2].offset), 100.0f);
}

// ---------- 4. DeleteExecutor ----------
TEST_F(ExecutorTest, DeleteRecord) {
    // 插入一条记录
    auto insert = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(1), make_str_val("ToDelete"), make_float_val(50.0f)}, context_);
    insert->Next();

    // 扫描获取 rid
    std::vector<Rid> rids;
    {
        std::vector<Condition> empty_conds;
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) {
            rids.push_back(seq->rid());
        }
    }
    ASSERT_EQ(rids.size(), 1);

    // 删除
    auto del = std::make_unique<DeleteExecutor>(sm_manager_, TABLE1,
        std::vector<Condition>{}, rids, context_);
    del->Next();

    // 验证：扫描结果应为空
    {
        std::vector<Condition> empty_conds;
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
        seq->beginTuple();
        EXPECT_TRUE(seq->is_end());
    }
}

// ---------- 5. ProjectionExecutor ----------
TEST_F(ExecutorTest, Projection) {
    auto tab = get_tab(TABLE1);

    // 插入一条记录
    auto insert = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(42), make_str_val("Proj"), make_float_val(88.0f)}, context_);
    insert->Next();

    // 创建 SeqScan -> Projection
    std::vector<Condition> empty_conds;
    auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);

    // 投影 id 和 name 两列
    std::vector<TabCol> sel_cols = {make_tabcol(TABLE1, "id"), make_tabcol(TABLE1, "name")};
    auto proj = std::make_unique<ProjectionExecutor>(std::move(seq), sel_cols);

    proj->beginTuple();
    EXPECT_FALSE(proj->is_end());
    auto rec = proj->Next();
    ASSERT_NE(rec, nullptr);

    // 投影后只有两列
    EXPECT_EQ(proj->cols().size(), 2);
    EXPECT_EQ(proj->cols()[0].name, "id");
    EXPECT_EQ(proj->cols()[1].name, "name");
    EXPECT_EQ(*(int *)(rec->data + proj->cols()[0].offset), 42);
}

// ---------- 6. SortExecutor ----------
TEST_F(ExecutorTest, Sort) {
    auto tab = get_tab(TABLE1);

    // 插入多条记录（分数无序）
    auto i1 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(1), make_str_val("A"), make_float_val(90.0f)}, context_);
    i1->Next();
    auto i2 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(2), make_str_val("B"), make_float_val(60.0f)}, context_);
    i2->Next();
    auto i3 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(3), make_str_val("C"), make_float_val(80.0f)}, context_);
    i3->Next();

    // SeqScan -> Sort (按 score 升序)
    std::vector<Condition> empty_conds;
    auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
    auto sort = std::make_unique<SortExecutor>(std::move(seq), make_tabcol(TABLE1, "score"), false);

    std::vector<float> scores;
    for (sort->beginTuple(); !sort->is_end(); sort->nextTuple()) {
        auto rec = sort->Next();
        ASSERT_NE(rec, nullptr);
        scores.push_back(*(float *)(rec->data + tab.cols[2].offset));
    }
    ASSERT_EQ(scores.size(), 3);
    EXPECT_FLOAT_EQ(scores[0], 60.0f);
    EXPECT_FLOAT_EQ(scores[1], 80.0f);
    EXPECT_FLOAT_EQ(scores[2], 90.0f);

    // 降序
    auto seq2 = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
    auto sort_desc = std::make_unique<SortExecutor>(std::move(seq2), make_tabcol(TABLE1, "score"), true);

    scores.clear();
    for (sort_desc->beginTuple(); !sort_desc->is_end(); sort_desc->nextTuple()) {
        auto rec = sort_desc->Next();
        ASSERT_NE(rec, nullptr);
        scores.push_back(*(float *)(rec->data + tab.cols[2].offset));
    }
    EXPECT_FLOAT_EQ(scores[0], 90.0f);
    EXPECT_FLOAT_EQ(scores[1], 80.0f);
    EXPECT_FLOAT_EQ(scores[2], 60.0f);
}

// ---------- 7. NestedLoopJoinExecutor ----------
TEST_F(ExecutorTest, NestedLoopJoin) {
    // 向 students 插入两条记录
    auto i1 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(1), make_str_val("Alice"), make_float_val(90.0f)}, context_);
    i1->Next();
    auto i2 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(2), make_str_val("Bob"), make_float_val(80.0f)}, context_);
    i2->Next();

    // 向 courses 插入两条记录
    auto c1 = std::make_unique<InsertExecutor>(sm_manager_, TABLE2,
        std::vector<Value>{make_int_val(1), make_str_val("Math")}, context_);
    c1->Next();
    auto c2 = std::make_unique<InsertExecutor>(sm_manager_, TABLE2,
        std::vector<Value>{make_int_val(2), make_str_val("CS")}, context_);
    c2->Next();

    // 左表=students, 右表=courses, 无条件 → 笛卡尔积 (2×2=4)
    std::vector<Condition> empty_conds;
    auto left = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
    auto right = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE2, empty_conds, context_);

    auto join = std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right),
                                                         empty_conds);
    int count = 0;
    for (join->beginTuple(); !join->is_end(); join->nextTuple()) {
        auto rec = join->Next();
        ASSERT_NE(rec, nullptr);
        count++;
    }
    EXPECT_EQ(count, 4);

    // 等值连接：students.id = courses.cid → 2 条匹配
    auto left2 = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
    auto right2 = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE2, empty_conds, context_);

    std::vector<Condition> join_conds;
    Condition jc;
    jc.lhs_col = make_tabcol(TABLE1, "id");
    jc.rhs_col = make_tabcol(TABLE2, "cid");
    jc.is_rhs_val = false;
    jc.op = OP_EQ;
    join_conds.push_back(jc);

    auto join2 = std::make_unique<NestedLoopJoinExecutor>(std::move(left2), std::move(right2),
                                                          join_conds);
    count = 0;
    for (join2->beginTuple(); !join2->is_end(); join2->nextTuple()) {
        auto rec = join2->Next();
        ASSERT_NE(rec, nullptr);
        count++;
    }
    EXPECT_EQ(count, 2);
}

// ---------- 8. IndexScanExecutor ----------
TEST_F(ExecutorTest, IndexScanFallback) {
    auto tab = get_tab(TABLE1);

    // 插入数据
    auto i1 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(1), make_str_val("X"), make_float_val(70.0f)}, context_);
    i1->Next();
    auto i2 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(2), make_str_val("Y"), make_float_val(90.0f)}, context_);
    i2->Next();
    auto i3 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(3), make_str_val("Z"), make_float_val(85.0f)}, context_);
    i3->Next();

    // 使用 IndexScanExecutor（退化全表扫描），带条件 score >= 85.0
    std::vector<Condition> conds;
    conds.push_back(make_cond_val(TABLE1, "score", OP_GE, make_float_val(85.0f)));

    auto ix_scan = std::make_unique<IndexScanExecutor>(sm_manager_, TABLE1, conds,
                                                       std::vector<std::string>{}, context_);
    int count = 0;
    for (ix_scan->beginTuple(); !ix_scan->is_end(); ix_scan->nextTuple()) {
        auto rec = ix_scan->Next();
        ASSERT_NE(rec, nullptr);
        count++;
    }
    EXPECT_EQ(count, 2);  // Y(90) and Z(85)
}

// ---------- 9. InsertWithIndex ----------
TEST_F(ExecutorTest, InsertWithIndex) {
    // 在 students(id) 上创建索引
    sm_manager_->create_index(TABLE1, {"id"}, context_);
    auto tab = get_tab(TABLE1);

    // 插入记录 → 应当命中 insert.h 的索引插入循环
    auto insert = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(10), make_str_val("IdxIns"), make_float_val(99.0f)}, context_);
    insert->Next();

    // 验证记录确实插入了
    auto fh = get_fh(TABLE1);
    auto rec = fh->get_record(insert->rid(), context_);
    EXPECT_EQ(*(int *)(rec->data + tab.cols[0].offset), 10);
}

// ---------- 10. UpdateWithIndex ----------
TEST_F(ExecutorTest, UpdateWithIndex) {
    sm_manager_->create_index(TABLE1, {"id"}, context_);

    // 插入一条记录
    auto insert = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(20), make_str_val("IdxUpd"), make_float_val(85.0f)}, context_);
    insert->Next();

    // 扫描获取 rid
    std::vector<Rid> rids;
    {
        std::vector<Condition> empty_conds;
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) {
            rids.push_back(seq->rid());
        }
    }
    ASSERT_EQ(rids.size(), 1);

    // 更新 → 应当命中 update.h 的旧索引删除 + 新索引插入两条循环
    SetClause sc;
    sc.lhs = make_tabcol(TABLE1, "score");
    sc.rhs = make_float_val(100.0f);
    auto update = std::make_unique<UpdateExecutor>(sm_manager_, TABLE1,
        std::vector<SetClause>{sc}, std::vector<Condition>{}, rids, context_);
    update->Next();

    // 验证更新结果
    auto fh = get_fh(TABLE1);
    auto rec = fh->get_record(rids[0], context_);
    EXPECT_FLOAT_EQ(*(float *)(rec->data + get_tab(TABLE1).cols[2].offset), 100.0f);
}

// ---------- 11. DeleteWithIndex ----------
TEST_F(ExecutorTest, DeleteWithIndex) {
    sm_manager_->create_index(TABLE1, {"id"}, context_);

    // 插入一条记录
    auto insert = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(30), make_str_val("IdxDel"), make_float_val(60.0f)}, context_);
    insert->Next();

    // 扫描获取 rid
    std::vector<Rid> rids;
    {
        std::vector<Condition> empty_conds;
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) {
            rids.push_back(seq->rid());
        }
    }
    ASSERT_EQ(rids.size(), 1);

    // 删除 → 应当命中 delete.h 的索引删除循环
    auto del = std::make_unique<DeleteExecutor>(sm_manager_, TABLE1,
        std::vector<Condition>{}, rids, context_);
    del->Next();

    // 验证：扫描结果应为空
    {
        std::vector<Condition> empty_conds;
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
        seq->beginTuple();
        EXPECT_TRUE(seq->is_end());
    }
}

// ---------- 12. SeqScanMultiOps (OP_LT, OP_GT) ----------
TEST_F(ExecutorTest, SeqScanMultiOps) {
    // 插入数据：id=1..3, score=95,80,70
    auto i1 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(1), make_str_val("A"), make_float_val(95.0f)}, context_);
    i1->Next();
    auto i2 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(2), make_str_val("B"), make_float_val(80.0f)}, context_);
    i2->Next();
    auto i3 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(3), make_str_val("C"), make_float_val(70.0f)}, context_);
    i3->Next();

    // OP_LT: score < 80 → 1 条 (C=70)
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "score", OP_LT, make_float_val(80.0f)));
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, conds, context_);
        int cnt = 0;
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 1);
    }

    // OP_GT: id > 1 → 2 条 (id=2,3)
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "id", OP_GT, make_int_val(1)));
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, conds, context_);
        int cnt = 0;
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 2);
    }

    // OP_NE: name != 'A' → 2 条 (B, C)
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "name", OP_NE, make_str_val("A")));
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, conds, context_);
        int cnt = 0;
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 2);
    }
}

// ---------- 13. SortByInt ----------
TEST_F(ExecutorTest, SortByInt) {
    // 插入无序 id 记录
    auto i1 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(3), make_str_val("C"), make_float_val(70.0f)}, context_);
    i1->Next();
    auto i2 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(1), make_str_val("A"), make_float_val(90.0f)}, context_);
    i2->Next();
    auto i3 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(2), make_str_val("B"), make_float_val(80.0f)}, context_);
    i3->Next();

    // 按 id（INT 类型）升序排序
    std::vector<Condition> empty_conds;
    auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
    auto sort = std::make_unique<SortExecutor>(std::move(seq), make_tabcol(TABLE1, "id"), false);

    std::vector<int> ids;
    for (sort->beginTuple(); !sort->is_end(); sort->nextTuple()) {
        auto rec = sort->Next();
        ids.push_back(*(int *)(rec->data + get_tab(TABLE1).cols[0].offset));
    }
    ASSERT_EQ(ids.size(), 3);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 2);
    EXPECT_EQ(ids[2], 3);
}

// ---------- 14. NestedLoopJoinMultiOps (OP_LT, OP_GE) ----------
TEST_F(ExecutorTest, NestedLoopJoinMultiOps) {
    // 向 students 插入 2 条: id=1,2
    auto i1 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(1), make_str_val("A"), make_float_val(90.0f)}, context_);
    i1->Next();
    auto i2 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(2), make_str_val("B"), make_float_val(80.0f)}, context_);
    i2->Next();

    // 向 courses 插入 2 条: cid=1,2
    auto c1 = std::make_unique<InsertExecutor>(sm_manager_, TABLE2,
        std::vector<Value>{make_int_val(1), make_str_val("Math")}, context_);
    c1->Next();
    auto c2 = std::make_unique<InsertExecutor>(sm_manager_, TABLE2,
        std::vector<Value>{make_int_val(2), make_str_val("CS")}, context_);
    c2->Next();

    std::vector<Condition> empty_conds;

    // OP_LT: students.id < courses.cid → (1<1)=F, (1<2)=T, (2<1)=F, (2<2)=F → 1 match
    {
        auto left = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
        auto right = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE2, empty_conds, context_);
        Condition jc;
        jc.lhs_col = make_tabcol(TABLE1, "id");
        jc.rhs_col = make_tabcol(TABLE2, "cid");
        jc.is_rhs_val = false;
        jc.op = OP_LT;
        auto join = std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right),
                                                             std::vector<Condition>{jc});
        int cnt = 0;
        for (join->beginTuple(); !join->is_end(); join->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 1);
    }

    // OP_GE: students.id >= courses.cid → (1>=1)=T, (1>=2)=F, (2>=1)=T, (2>=2)=T → 3 matches
    {
        auto left = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
        auto right = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE2, empty_conds, context_);
        Condition jc;
        jc.lhs_col = make_tabcol(TABLE1, "id");
        jc.rhs_col = make_tabcol(TABLE2, "cid");
        jc.is_rhs_val = false;
        jc.op = OP_GE;
        auto join = std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right),
                                                             std::vector<Condition>{jc});
        int cnt = 0;
        for (join->beginTuple(); !join->is_end(); join->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 3);
    }
}

// ---------- 15. ColumnNotFound Throw ----------
TEST_F(ExecutorTest, ColumnNotFound) {
    std::vector<Condition> empty_conds;
    auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, empty_conds, context_);
    // 调用 get_col_offset 并传入不存在的列 → 应抛出 ColumnNotFoundError
    EXPECT_THROW(seq->get_col_offset({"students", "nonexistent_col"}), ColumnNotFoundError);
}

// ---------- 16. SeqScan OP_LE and OP_GE (补全 switch 分支) ----------
TEST_F(ExecutorTest, SeqScanOP_LE_GE) {
    // 插入数据: id=1..3, score=95,80,70
    auto i1 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(1), make_str_val("A"), make_float_val(95.0f)}, context_);
    i1->Next();
    auto i2 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(2), make_str_val("B"), make_float_val(80.0f)}, context_);
    i2->Next();
    auto i3 = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
        std::vector<Value>{make_int_val(3), make_str_val("C"), make_float_val(70.0f)}, context_);
    i3->Next();

    // OP_LE: score <= 80.0 → 2 条 (id=2 score=80, id=3 score=70)
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "score", OP_LE, make_float_val(80.0f)));
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, conds, context_);
        int cnt = 0;
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 2);
    }

    // OP_LE: id <= 1 → 1 条
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "id", OP_LE, make_int_val(1)));
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, conds, context_);
        int cnt = 0;
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 1);
    }

    // OP_GE: id >= 2 → 2 条
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "id", OP_GE, make_int_val(2)));
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, conds, context_);
        int cnt = 0;
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 2);
    }
}

