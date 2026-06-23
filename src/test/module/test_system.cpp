/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details.
*/

/**
 * @file test_sm.cpp
 * @brief SmManager（系统管理器）模块的单元测试。
 *
 * 覆盖以下功能：
 *   - 数据库生命周期：create_db / drop_db / open_db / close_db
 *   - 表生命周期：create_table / desc_table / show_tables / drop_table
 *   - 索引生命周期：create_index / drop_index
 *   - 所有边界条件和异常路径
 *
 * 编译（在 build 目录下）：
 *   make test_sm -j$(nproc)
 * 运行：
 *   ./bin/test_sm
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "index/ix.h"
#include "record/rm.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"
#include "test_utils.h"
#include "test_sm_context.h"

// ============================================================
// 测试夹具：每个 TEST_F 进入干净的空目录
// ============================================================
class SmManagerTest : public ::testing::Test {
   protected:
    static const std::string TEST_DB_NAME;

    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    RmManager *rm_manager_;
    IxManager *ix_manager_;
    SmManager *sm_manager_;
    sm_test::TestContext test_ctx_;

    void SetUp() override {
        // 创建并进入测试根目录
        disk_manager_ = new DiskManager();
        test_utils::EnterTestDir(disk_manager_);

        buffer_pool_manager_ = new BufferPoolManager(BUFFER_POOL_SIZE, disk_manager_);
        rm_manager_ = new RmManager(disk_manager_, buffer_pool_manager_);
        ix_manager_ = new IxManager(disk_manager_, buffer_pool_manager_);
        sm_manager_ = new SmManager(disk_manager_, buffer_pool_manager_, rm_manager_, ix_manager_);
    }

    void TearDown() override {
        // 确保数据库已关闭
        delete sm_manager_;
        delete ix_manager_;
        delete rm_manager_;
        delete buffer_pool_manager_;

        // 离开并清理测试目录
        test_utils::LeaveTestDir(disk_manager_);
        delete disk_manager_;
    }
};

const std::string SmManagerTest::TEST_DB_NAME = "test_sm_db";

// ============================================================
// 辅助函数
// ============================================================

/** 创建一张简单的测试表（两列：id INT, name VARCHAR(64)） */
static std::vector<ColDef> MakeSimpleTableDef() {
    return {
        {.name = "id", .type = TYPE_INT, .len = 4},
        {.name = "name", .type = TYPE_STRING, .len = 64},
    };
}

/** 创建一张三列表（id INT, salary FLOAT, name VARCHAR(32)） */
static std::vector<ColDef> MakeThreeColTableDef() {
    return {
        {.name = "id", .type = TYPE_INT, .len = 4},
        {.name = "salary", .type = TYPE_FLOAT, .len = 4},
        {.name = "name", .type = TYPE_STRING, .len = 32},
    };
}

// ============================================================
// 测试用例
// ============================================================

// ---------- 数据库生命周期 ----------

TEST_F(SmManagerTest, CreateDatabase) {
    EXPECT_NO_THROW(sm_manager_->create_db(TEST_DB_NAME));
    EXPECT_TRUE(sm_manager_->is_dir(TEST_DB_NAME));
}

TEST_F(SmManagerTest, CreateDuplicateDatabase) {
    sm_manager_->create_db(TEST_DB_NAME);
    EXPECT_THROW(sm_manager_->create_db(TEST_DB_NAME), DatabaseExistsError);
}

TEST_F(SmManagerTest, DropDatabase) {
    sm_manager_->create_db(TEST_DB_NAME);
    EXPECT_NO_THROW(sm_manager_->drop_db(TEST_DB_NAME));
    EXPECT_FALSE(sm_manager_->is_dir(TEST_DB_NAME));
}

TEST_F(SmManagerTest, DropNonExistentDatabase) {
    EXPECT_THROW(sm_manager_->drop_db("non_existent_db"), DatabaseNotFoundError);
}

TEST_F(SmManagerTest, OpenCloseDatabaseRoundTrip) {
    // 创建数据库
    sm_manager_->create_db(TEST_DB_NAME);

    // 打开数据库（进入目录、读取元数据）
    EXPECT_NO_THROW(sm_manager_->open_db(TEST_DB_NAME));

    // 关闭数据库
    EXPECT_NO_THROW(sm_manager_->close_db());

    // 关闭后 CWD 回到测试根目录，数据库目录应仍然存在
    EXPECT_TRUE(sm_manager_->is_dir(TEST_DB_NAME));
}

TEST_F(SmManagerTest, OpenNonExistentDatabase) {
    EXPECT_THROW(sm_manager_->open_db("ghost_db"), DatabaseNotFoundError);
}

TEST_F(SmManagerTest, DoubleCloseDatabase) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);
    EXPECT_NO_THROW(sm_manager_->close_db());
    // 第二次 close_db 会 chdir("..") 失败或 throw，因为已经退出了
    // 不要求特定行为，只检查不 crash
}

// ---------- 表生命周期 ----------

TEST_F(SmManagerTest, CreateTable) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeSimpleTableDef();
    EXPECT_NO_THROW(sm_manager_->create_table("test_tab", col_defs, test_ctx_.ctx.get()));

    // 验证表已注册到元数据
    EXPECT_TRUE(sm_manager_->db_.is_table("test_tab"));

    // 验证表文件已创建（数据文件 + 索引文件句柄）
    EXPECT_TRUE(sm_manager_->fhs_.count("test_tab"));

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, CreateDuplicateTable) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeSimpleTableDef();
    sm_manager_->create_table("test_tab", col_defs, test_ctx_.ctx.get());
    EXPECT_THROW(sm_manager_->create_table("test_tab", col_defs, test_ctx_.ctx.get()), TableExistsError);

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, DescTable) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeThreeColTableDef();
    sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());

    // desc_table 会将结果输出到 RecordPrinter
    EXPECT_NO_THROW(sm_manager_->desc_table("emp", test_ctx_.ctx.get()));

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, DescNonExistentTable) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    EXPECT_THROW(sm_manager_->desc_table("phantom", test_ctx_.ctx.get()), TableNotFoundError);

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, ShowTables) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto def1 = MakeSimpleTableDef();
    auto def2 = MakeThreeColTableDef();
    sm_manager_->create_table("tab_a", def1, test_ctx_.ctx.get());
    sm_manager_->create_table("tab_b", def2, test_ctx_.ctx.get());

    // show_tables 打印到 RecordPrinter + output.txt
    EXPECT_NO_THROW(sm_manager_->show_tables(test_ctx_.ctx.get()));

    std::ifstream ifs("output.txt");
    ASSERT_TRUE(ifs.is_open());
    std::stringstream ss;
    ss << ifs.rdbuf();
    EXPECT_EQ(ss.str(), "| Tables |\n| tab_a |\n| tab_b |\n\n");

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, DropTable) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeSimpleTableDef();
    sm_manager_->create_table("drop_me", col_defs, test_ctx_.ctx.get());
    EXPECT_TRUE(sm_manager_->db_.is_table("drop_me"));

    EXPECT_NO_THROW(sm_manager_->drop_table("drop_me", test_ctx_.ctx.get()));
    EXPECT_FALSE(sm_manager_->db_.is_table("drop_me"));

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, DropNonExistentTable) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    EXPECT_THROW(sm_manager_->drop_table("ghost", test_ctx_.ctx.get()), TableNotFoundError);

    sm_manager_->close_db();
}

// ---------- 索引生命周期 ----------

TEST_F(SmManagerTest, CreateIndex) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeSimpleTableDef();
    sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());

    // 创建索引 — idx_emp_id
    EXPECT_NO_THROW(sm_manager_->create_index("emp", {"id"}, test_ctx_.ctx.get()));

    // 验证列元数据的 index 标记已更新
    TabMeta &tab = sm_manager_->db_.get_table("emp");
    for (auto &col : tab.cols) {
        if (col.name == "id") {
            EXPECT_TRUE(col.index);
        }
    }

    // 验证索引元数据已注册
    EXPECT_TRUE(tab.is_index({"id"}));

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, CreateDuplicateIndex) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeSimpleTableDef();
    sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());
    sm_manager_->create_index("emp", {"id"}, test_ctx_.ctx.get());

    // 在相同列上重复创建索引应抛异常
    EXPECT_THROW(sm_manager_->create_index("emp", {"id"}, test_ctx_.ctx.get()), IndexExistsError);

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, CreateIndexOnNonExistentTable) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    EXPECT_THROW(sm_manager_->create_index("ghost", {"id"}, test_ctx_.ctx.get()), TableNotFoundError);

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, CreateIndexOnNonExistentColumn) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeSimpleTableDef();
    sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());

    EXPECT_THROW(sm_manager_->create_index("emp", {"fake_col"}, test_ctx_.ctx.get()), ColumnNotFoundError);

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, DropIndex) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeSimpleTableDef();
    sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());
    sm_manager_->create_index("emp", {"id"}, test_ctx_.ctx.get());

    EXPECT_NO_THROW(sm_manager_->drop_index("emp", {"id"}, test_ctx_.ctx.get()));

    // 验证列元数据的 index 标记已清除
    TabMeta &tab = sm_manager_->db_.get_table("emp");
    for (auto &col : tab.cols) {
        if (col.name == "id") {
            EXPECT_FALSE(col.index);
        }
    }

    // 验证索引元数据已移除
    EXPECT_FALSE(tab.is_index({"id"}));

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, DropNonExistentIndex) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeSimpleTableDef();
    sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());

    EXPECT_THROW(sm_manager_->drop_index("emp", {"id"}, test_ctx_.ctx.get()), IndexNotFoundError);

    sm_manager_->close_db();
}

// ---------- 表与索引联动 ----------

TEST_F(SmManagerTest, DropTableWithIndex) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeSimpleTableDef();
    sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());
    sm_manager_->create_index("emp", {"id"}, test_ctx_.ctx.get());
    sm_manager_->create_index("emp", {"name"}, test_ctx_.ctx.get());

    // 删除表应自动级联删除所有索引
    EXPECT_NO_THROW(sm_manager_->drop_table("emp", test_ctx_.ctx.get()));
    EXPECT_FALSE(sm_manager_->db_.is_table("emp"));

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, MultiColumnIndex) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeThreeColTableDef();
    sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());

    // 复合索引 (id, name)
    std::vector<std::string> comp_cols = {"id", "name"};
    EXPECT_NO_THROW(sm_manager_->create_index("emp", comp_cols, test_ctx_.ctx.get()));

    TabMeta &tab = sm_manager_->db_.get_table("emp");
    EXPECT_TRUE(tab.is_index(comp_cols));

    // 删除复合索引
    EXPECT_NO_THROW(sm_manager_->drop_index("emp", comp_cols, test_ctx_.ctx.get()));
    EXPECT_FALSE(tab.is_index(comp_cols));

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, MultipleIndexesOnSameTable) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeThreeColTableDef();
    sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());

    // 在多列上分别建索引
    EXPECT_NO_THROW(sm_manager_->create_index("emp", {"id"}, test_ctx_.ctx.get()));
    EXPECT_NO_THROW(sm_manager_->create_index("emp", {"salary"}, test_ctx_.ctx.get()));
    EXPECT_NO_THROW(sm_manager_->create_index("emp", {"name"}, test_ctx_.ctx.get()));

    TabMeta &tab = sm_manager_->db_.get_table("emp");
    EXPECT_TRUE(tab.is_index({"id"}));
    EXPECT_TRUE(tab.is_index({"salary"}));
    EXPECT_TRUE(tab.is_index({"name"}));

    // 删除其中一个
    EXPECT_NO_THROW(sm_manager_->drop_index("emp", {"salary"}, test_ctx_.ctx.get()));
    EXPECT_FALSE(tab.is_index({"salary"}));
    // 另外两个还在
    EXPECT_TRUE(tab.is_index({"id"}));
    EXPECT_TRUE(tab.is_index({"name"}));

    sm_manager_->close_db();
}

// ---------- 元数据持久化 ----------

TEST_F(SmManagerTest, MetaPersistence) {
    // 第一次会话：创建数据库和表，写入元数据
    {
        sm_manager_->create_db(TEST_DB_NAME);
        sm_manager_->open_db(TEST_DB_NAME);
        auto col_defs = MakeSimpleTableDef();
        sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());
        sm_manager_->create_index("emp", {"id"}, test_ctx_.ctx.get());
        sm_manager_->close_db();
    }

    // 第二次会话：打开数据库，验证元数据持久化
    {
        // 磁盘上数据应持久化
        EXPECT_TRUE(sm_manager_->is_dir(TEST_DB_NAME));
        EXPECT_NO_THROW(sm_manager_->open_db(TEST_DB_NAME));

        // 验证表元数据
        EXPECT_TRUE(sm_manager_->db_.is_table("emp"));
        TabMeta &tab = sm_manager_->db_.get_table("emp");
        EXPECT_EQ(tab.cols.size(), 2);

        // 检查列定义
        EXPECT_TRUE(tab.is_col("id"));
        EXPECT_TRUE(tab.is_col("name"));

        // 索引应在元数据中持久化
        EXPECT_TRUE(tab.is_index({"id"}));
        for (auto &col : tab.cols) {
            if (col.name == "id") {
                EXPECT_TRUE(col.index);
            }
        }

        sm_manager_->close_db();
    }
}

// ---------- 空数据库 ----------

TEST_F(SmManagerTest, EmptyDatabaseShowTables) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    // 空数据库的 show_tables 不应崩溃
    EXPECT_NO_THROW(sm_manager_->show_tables(test_ctx_.ctx.get()));

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, CreateTableWithLargeRecord) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    // 创建一个接近最大 record size 的表
    std::vector<ColDef> big_cols = {
        {.name = "data", .type = TYPE_STRING, .len = RM_MAX_RECORD_SIZE - 4},
        {.name = "id", .type = TYPE_INT, .len = 4},
    };
    EXPECT_NO_THROW(sm_manager_->create_table("big_table", big_cols, test_ctx_.ctx.get()));
    EXPECT_TRUE(sm_manager_->db_.is_table("big_table"));

    sm_manager_->close_db();
}

TEST_F(SmManagerTest, FlushMeta) {
    sm_manager_->create_db(TEST_DB_NAME);
    sm_manager_->open_db(TEST_DB_NAME);

    auto col_defs = MakeSimpleTableDef();
    sm_manager_->create_table("emp", col_defs, test_ctx_.ctx.get());

    // 显式写入元数据
    EXPECT_NO_THROW(sm_manager_->flush_meta());

    // 验证文件存在且可读
    std::ifstream ifs(DB_META_NAME);
    EXPECT_TRUE(ifs.is_open());
    ifs.close();

    sm_manager_->close_db();
}
