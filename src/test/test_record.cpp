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
 * @file test_record.cpp
 * @brief RmFileHandle + RmScan 模块单元测试（20+ 个测试用例）
 *
 * 覆盖场景：
 *   - RmManager 文件生命周期（create / open / close / destroy）
 *   - 记录 CRUD（insert auto-slot / insert at Rid / get / delete / update）
 *   - bitmap 状态管理（is_record、空闲 slot 重用）
 *   - 多页面自动扩展（跨页插入、scan 跨页遍历）
 *   - 空闲页链表管理（full→release→reuse）
 *   - RmScan 全表扫描 / 空表扫描 / 增量扫描
 *   - 边界条件（操作空 slot、double delete、边界 page_no 校验）
 *
 * 编译 & 运行：
 *   mkdir -p build && cd build
 *   cmake .. -DENABLE_COVERAGE=ON && make test_record -j$(nproc)
 *   ./bin/test_record
 *
 * CTest:
 *   ctest --output-on-failure -R test_record
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "record/rm.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "test_utils.h"

// ============================================================
// 测试夹具
// ============================================================
class RecordTest : public ::testing::Test {
   protected:
    static constexpr size_t POOL_SIZE = 64;      // 足够容纳多页
    static constexpr int TEST_RECORD_SIZE = 64;   // 每记录 64 字节
    static const std::string TEST_FILE;

    DiskManager *disk_manager_;
    BufferPoolManager *bpm_;
    RmManager *rm_manager_;

    void SetUp() override {
        disk_manager_ = new DiskManager();
        test_utils::EnterTestDir(disk_manager_);

        bpm_ = new BufferPoolManager(POOL_SIZE, disk_manager_);
        rm_manager_ = new RmManager(disk_manager_, bpm_);

        // 创建测试表文件
        rm_manager_->create_file(TEST_FILE, TEST_RECORD_SIZE);
    }

    void TearDown() override {
        // 关闭并销毁测试文件
        rm_manager_->destroy_file(TEST_FILE);

        delete rm_manager_;
        delete bpm_;

        test_utils::LeaveTestDir(disk_manager_);
        delete disk_manager_;
    }

    /** 用指定填充字节写入记录缓冲区 */
    static void FillRecord(char *buf, int size, char fill_byte) {
        memset(buf, fill_byte, size);
    }

    /** 用唯一序号写入记录缓冲区（前 4 字节为序号，后续填充固定模式） */
    static void FillRecordSeq(char *buf, int size, int seq) {
        memcpy(buf, &seq, sizeof(seq));
        for (int i = sizeof(seq); i < size; ++i) {
            buf[i] = static_cast<char>((seq + i) & 0xff);
        }
    }

    /** 验证记录缓冲区是否与序号匹配 */
    static void CheckRecordSeq(const char *buf, int size, int seq) {
        int stored_seq;
        memcpy(&stored_seq, buf, sizeof(stored_seq));
        EXPECT_EQ(stored_seq, seq);
        for (int i = sizeof(seq); i < size; ++i) {
            EXPECT_EQ(buf[i], static_cast<char>((seq + i) & 0xff));
        }
    }

    /** 计算给定 record_size 下每页能容纳的记录数 */
    static int ComputeRecordsPerPage(int record_size) {
        return (8 * (PAGE_SIZE - 1 - static_cast<int>(sizeof(RmFileHdr))) + 1) /
               (1 + record_size * 8);
    }
};

const std::string RecordTest::TEST_FILE = "test_record_table";

// ============================================================
// 1. RmManager 文件生命周期
// ============================================================

/**
 * @test 创建文件 → open 验证文件头 → close → reopen 验证持久性
 */
TEST_F(RecordTest, CreateFileAndOpen) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    const RmFileHdr &hdr = fh->get_file_hdr();
    EXPECT_EQ(hdr.record_size, TEST_RECORD_SIZE);
    EXPECT_EQ(hdr.num_pages, 1);               // 只有 header page
    EXPECT_EQ(hdr.first_free_page_no, -1);     // 初始无数据页
    EXPECT_EQ(hdr.bitmap_size, (ComputeRecordsPerPage(TEST_RECORD_SIZE) + 7) / 8);

    // close（会 flush header 到磁盘）
    rm_manager_->close_file(fh.get());

    // reopen 验证文件头持久化
    auto fh2 = rm_manager_->open_file(TEST_FILE);
    const RmFileHdr &hdr2 = fh2->get_file_hdr();
    EXPECT_EQ(hdr2.record_size, TEST_RECORD_SIZE);
    EXPECT_EQ(hdr2.num_pages, 1);
    rm_manager_->close_file(fh2.get());
}

// ============================================================
// 2. 记录插入 + 读取
// ============================================================

/**
 * @test insert_record（自动分配 slot）→ get_record 验证数据完整性
 */
TEST_F(RecordTest, InsertAndGetRecord) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    char buf[TEST_RECORD_SIZE];
    FillRecord(buf, TEST_RECORD_SIZE, 'X');

    Rid rid = fh->insert_record(buf, nullptr);
    EXPECT_EQ(rid.page_no, 1);      // 第一个数据页
    EXPECT_EQ(rid.slot_no, 0);      // 第一个 slot

    // 读取验证
    auto rec = fh->get_record(rid, nullptr);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->size, TEST_RECORD_SIZE);
    for (int i = 0; i < TEST_RECORD_SIZE; ++i) {
        EXPECT_EQ(rec->data[i], 'X');
    }

    rm_manager_->close_file(fh.get());
}

/**
 * @test insert_record 在指定 Rid 位置插入
 */
TEST_F(RecordTest, InsertAtSpecificRid) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    // 先用 auto-insert 创建一个 page
    char buf[TEST_RECORD_SIZE];
    FillRecord(buf, TEST_RECORD_SIZE, 'A');
    Rid auto_rid = fh->insert_record(buf, nullptr);

    // 用 insert_record(rid, buf) 在指定 slot 插入
    FillRecord(buf, TEST_RECORD_SIZE, 'B');
    Rid specific_rid = {auto_rid.page_no, auto_rid.slot_no + 1};
    fh->insert_record(specific_rid, buf);

    // 验证
    auto rec = fh->get_record(specific_rid, nullptr);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->size, TEST_RECORD_SIZE);
    EXPECT_EQ(rec->data[0], 'B');

    rm_manager_->close_file(fh.get());
}

// ============================================================
// 3. 记录删除
// ============================================================

/**
 * @test delete_record 后 get_record 返回 nullptr，is_record 返回 false
 */
TEST_F(RecordTest, DeleteRecord) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    char buf[TEST_RECORD_SIZE];
    FillRecord(buf, TEST_RECORD_SIZE, 'Y');
    Rid rid = fh->insert_record(buf, nullptr);

    // 删除前存在
    EXPECT_TRUE(fh->is_record(rid));

    fh->delete_record(rid, nullptr);

    // 删除后不存在
    EXPECT_FALSE(fh->is_record(rid));
    auto rec = fh->get_record(rid, nullptr);
    EXPECT_EQ(rec, nullptr);

    rm_manager_->close_file(fh.get());
}

/**
 * @test 删除不存在的记录
 *
 * - 重复删除已删除的 slot：不应崩溃
 * - 删除不存在的 page_no：fetch_page_handle 会抛出 PageNotExistError
 */
TEST_F(RecordTest, DeleteNonExistentRecord) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    // 先插入一条记录创建 page
    char buf[TEST_RECORD_SIZE];
    FillRecord(buf, TEST_RECORD_SIZE, 'Z');
    Rid rid = fh->insert_record(buf, nullptr);

    // 删除存在的记录
    fh->delete_record(rid, nullptr);
    // 重复删除同一 slot — bitmap 已被清除，delete 直接返回（不崩溃）
    EXPECT_NO_THROW(fh->delete_record(rid, nullptr));

    // 删除不存在的 page_no → fetch_page_handle 抛出异常
    EXPECT_THROW(fh->delete_record({999, 0}, nullptr), PageNotExistError);

    rm_manager_->close_file(fh.get());
}

/**
 * @test 删除后重新插入，slot 被重用
 */
TEST_F(RecordTest, DeleteAndReinsertReusesSlot) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    char buf[TEST_RECORD_SIZE];
    FillRecord(buf, TEST_RECORD_SIZE, 'A');
    Rid rid1 = fh->insert_record(buf, nullptr);
    EXPECT_EQ(rid1.slot_no, 0);

    FillRecord(buf, TEST_RECORD_SIZE, 'B');
    Rid rid2 = fh->insert_record(buf, nullptr);
    EXPECT_EQ(rid2.slot_no, 1);

    // 删除 slot 0
    fh->delete_record(rid1, nullptr);

    // 重新插入 → 应重用 slot 0
    FillRecord(buf, TEST_RECORD_SIZE, 'C');
    Rid rid3 = fh->insert_record(buf, nullptr);
    EXPECT_EQ(rid3.slot_no, 0);    // 重用 freed slot

    // slot 1 仍存在
    EXPECT_TRUE(fh->is_record(rid2));

    rm_manager_->close_file(fh.get());
}

// ============================================================
// 4. 记录更新
// ============================================================

/**
 * @test update_record 验证数据更新
 */
TEST_F(RecordTest, UpdateRecord) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    char buf[TEST_RECORD_SIZE];
    FillRecord(buf, TEST_RECORD_SIZE, 'O');
    Rid rid = fh->insert_record(buf, nullptr);

    // 更新
    FillRecord(buf, TEST_RECORD_SIZE, 'N');
    fh->update_record(rid, buf, nullptr);

    // 验证
    auto rec = fh->get_record(rid, nullptr);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->data[0], 'N');

    rm_manager_->close_file(fh.get());
}

/**
 * @test 更新不存在的记录
 *
 * - 先插入一条记录创建 page 1
 * - 更新不存在的 slot_no（超出 bitmap 范围）：应不崩溃（内部通过 bitmap 检测到无记录后直接返回）
 * - 更新不存在的 page_no：fetch_page_handle 抛出 PageNotExistError
 */
TEST_F(RecordTest, UpdateNonExistentRecord) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    // 先插入一条记录，确保 page 1 存在
    char insert_buf[TEST_RECORD_SIZE];
    FillRecord(insert_buf, TEST_RECORD_SIZE, 'A');
    fh->insert_record(insert_buf, nullptr);

    // 更新存在的 page 上不存在的 slot — bitmap 检测后直接返回
    char buf[TEST_RECORD_SIZE] = {0};
    EXPECT_NO_THROW(fh->update_record({1, 999}, buf, nullptr));

    // 更新不存在的 page → fetch_page_handle 抛出异常
    EXPECT_THROW(fh->update_record({999, 0}, buf, nullptr), PageNotExistError);

    rm_manager_->close_file(fh.get());
}

/**
 * @test 读取空 slot 返回 nullptr
 */
TEST_F(RecordTest, GetNonExistentRecord) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    // 先插入一条以创建 page
    char buf[TEST_RECORD_SIZE] = {0};
    fh->insert_record(buf, nullptr);

    // 读取不存在的 slot
    auto rec = fh->get_record({1, 999}, nullptr);
    EXPECT_EQ(rec, nullptr);

    rm_manager_->close_file(fh.get());
}

// ============================================================
// 5. 多页面自动扩展
// ============================================================

/**
 * @test 插入超过一页容量的记录，验证多页自动扩展
 */
TEST_F(RecordTest, MultiplePagesAutoExtend) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    int rpp = fh->get_file_hdr().num_records_per_page;
    int total = rpp + 5;   // 跨第二页

    std::vector<Rid> rids;
    char buf[TEST_RECORD_SIZE];
    for (int i = 0; i < total; ++i) {
        FillRecordSeq(buf, TEST_RECORD_SIZE, i);
        Rid rid = fh->insert_record(buf, nullptr);
        rids.push_back(rid);
    }

    // 文件头应该指示有至少 2 页数据
    EXPECT_GE(fh->get_file_hdr().num_pages, 2);

    // 第一页的最后一个记录 slot 应该是 rpp - 1
    EXPECT_EQ(rids[rpp - 1].slot_no, rpp - 1);
    // 第二页的第一个记录 slot 应该是 0
    EXPECT_EQ(rids[rpp].page_no, 2);
    EXPECT_EQ(rids[rpp].slot_no, 0);

    // 验证所有记录数据完整性
    for (int i = 0; i < total; ++i) {
        auto rec = fh->get_record(rids[i], nullptr);
        ASSERT_NE(rec, nullptr) << "Record " << i << " missing";
        CheckRecordSeq(rec->data, TEST_RECORD_SIZE, i);
    }

    rm_manager_->close_file(fh.get());
}

// ============================================================
// 6. 空闲页链表管理
// ============================================================

/**
 * @test 填满一页 → 删除一条 → 新插入应进入该页（而非新建页）
 */
TEST_F(RecordTest, FreePageReuseAfterFull) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    int rpp = fh->get_file_hdr().num_records_per_page;

    // 填满第一页
    std::vector<Rid> rids;
    char buf[TEST_RECORD_SIZE];
    for (int i = 0; i < rpp; ++i) {
        FillRecordSeq(buf, TEST_RECORD_SIZE, i);
        rids.push_back(fh->insert_record(buf, nullptr));
    }

    // 此时 first_free_page_no 应为 -1（无空闲页）
    EXPECT_EQ(fh->get_file_hdr().first_free_page_no, -1);

    // 再插入一条 → 应创建新页（page 2）
    Rid extra = fh->insert_record(buf, nullptr);
    EXPECT_EQ(extra.page_no, 2);

    // 删除 page 1 中的一条
    fh->delete_record(rids[0], nullptr);

    // 此时 first_free_page_no 应指向 page 1（它有了空闲空间）
    EXPECT_EQ(fh->get_file_hdr().first_free_page_no, 1);

    // 新插入应进入 page 1（重用 freed slot）
    FillRecordSeq(buf, TEST_RECORD_SIZE, 999);
    Rid reused = fh->insert_record(buf, nullptr);
    EXPECT_EQ(reused.page_no, 1);
    EXPECT_EQ(reused.slot_no, 0);    // 重用 slot 0

    // 验证数据
    auto rec = fh->get_record(reused, nullptr);
    ASSERT_NE(rec, nullptr);
    CheckRecordSeq(rec->data, TEST_RECORD_SIZE, 999);

    // page 2 的记录仍存在
    EXPECT_TRUE(fh->is_record(extra));

    rm_manager_->close_file(fh.get());
}

// ============================================================
// 7. RmScan 扫描测试
// ============================================================

/**
 * @test 空表扫描 — is_end() 立即为 true
 */
TEST_F(RecordTest, ScanEmptyTable) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    RmScan scan(fh.get());
    EXPECT_TRUE(scan.is_end());

    rm_manager_->close_file(fh.get());
}

/**
 * @test 全表扫描 — 验证每个记录的 rid 和内容
 */
TEST_F(RecordTest, ScanFullTable) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    int rpp = fh->get_file_hdr().num_records_per_page;
    int total = rpp + 3;   // 两页

    // 插入记录
    char buf[TEST_RECORD_SIZE];
    for (int i = 0; i < total; ++i) {
        FillRecordSeq(buf, TEST_RECORD_SIZE, i);
        fh->insert_record(buf, nullptr);
    }

    // 全表扫描
    RmScan scan(fh.get());
    int count = 0;
    while (!scan.is_end()) {
        Rid rid = scan.rid();
        EXPECT_EQ(rid.slot_no, count % rpp);  // slot 在每个页内递增
        if (count < rpp) {
            EXPECT_EQ(rid.page_no, 1);
        } else {
            EXPECT_EQ(rid.page_no, 2);
        }

        // 验证数据
        auto rec = fh->get_record(rid, nullptr);
        ASSERT_NE(rec, nullptr);
        CheckRecordSeq(rec->data, TEST_RECORD_SIZE, count);

        ++count;
        scan.next();
    }
    EXPECT_EQ(count, total);

    rm_manager_->close_file(fh.get());
}

/**
 * @test 部分删除后扫描 — scan 跳过已删除的记录
 */
TEST_F(RecordTest, ScanAfterPartialDelete) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    // 插入 5 条
    std::vector<Rid> rids;
    char buf[TEST_RECORD_SIZE];
    for (int i = 0; i < 5; ++i) {
        FillRecordSeq(buf, TEST_RECORD_SIZE, i);
        rids.push_back(fh->insert_record(buf, nullptr));
    }

    // 删除 slot 1 和 slot 3
    fh->delete_record(rids[1], nullptr);
    fh->delete_record(rids[3], nullptr);

    // 扫描 — 应只看到 0, 2, 4
    RmScan scan(fh.get());
    int expected[] = {0, 2, 4};
    int idx = 0;
    while (!scan.is_end()) {
        Rid rid = scan.rid();
        EXPECT_EQ(rid.slot_no, expected[idx]);
        auto rec = fh->get_record(rid, nullptr);
        ASSERT_NE(rec, nullptr);
        CheckRecordSeq(rec->data, TEST_RECORD_SIZE, expected[idx]);
        ++idx;
        scan.next();
    }
    EXPECT_EQ(idx, 3);

    rm_manager_->close_file(fh.get());
}

/**
 * @test 增量扫描：插入一批 → 扫描部分 → 再插入 → 再次扫描
 */
TEST_F(RecordTest, ScanIncremental) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    char buf[TEST_RECORD_SIZE];

    // 第一批：3 条
    for (int i = 0; i < 3; ++i) {
        FillRecordSeq(buf, TEST_RECORD_SIZE, i);
        fh->insert_record(buf, nullptr);
    }

    // 第一次扫描：应看到 0, 1, 2
    {
        RmScan scan(fh.get());
        int count = 0;
        while (!scan.is_end()) {
            auto rec = fh->get_record(scan.rid(), nullptr);
            ASSERT_NE(rec, nullptr);
            CheckRecordSeq(rec->data, TEST_RECORD_SIZE, count);
            ++count;
            scan.next();
        }
        EXPECT_EQ(count, 3);
    }

    // 第二批：再插 2 条
    for (int i = 3; i < 5; ++i) {
        FillRecordSeq(buf, TEST_RECORD_SIZE, i);
        fh->insert_record(buf, nullptr);
    }

    // 第二次扫描：应看到 0 ~ 4
    {
        RmScan scan(fh.get());
        int count = 0;
        while (!scan.is_end()) {
            auto rec = fh->get_record(scan.rid(), nullptr);
            ASSERT_NE(rec, nullptr);
            CheckRecordSeq(rec->data, TEST_RECORD_SIZE, count);
            ++count;
            scan.next();
        }
        EXPECT_EQ(count, 5);
    }

    rm_manager_->close_file(fh.get());
}

// ============================================================
// 8. 边界条件
// ============================================================

/**
 * @test 获取不存在的 page_no 应抛出异常
 */
TEST_F(RecordTest, FetchInvalidPageThrows) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    // page_no 0 是 header page，不应被 fetch
    EXPECT_THROW(fh->fetch_page_handle(0), PageNotExistError);
    // page_no 超出范围
    EXPECT_THROW(fh->fetch_page_handle(999), PageNotExistError);

    rm_manager_->close_file(fh.get());
}

/**
 * @test 文件创建时传空字符串 — 底层 disk_manager 会报 UnixError
 */
TEST_F(RecordTest, CreateFileEmptyName) {
    EXPECT_THROW(rm_manager_->create_file("", 64), UnixError);
}

/**
 * @test 记录大小为 0 或超过最大值时抛出
 */
TEST_F(RecordTest, InvalidRecordSize) {
    EXPECT_THROW(rm_manager_->create_file("bad_size_0", 0), InvalidRecordSizeError);
    EXPECT_THROW(rm_manager_->create_file("bad_size_big", 9999), InvalidRecordSizeError);
}

/**
 * @test 打开不存在的文件 — 底层 disk_manager 会报 UnixError
 */
TEST_F(RecordTest, OpenNonExistentFile) {
    EXPECT_THROW(rm_manager_->open_file("no_such_file"), UnixError);
}

/**
 * @test 大量记录交错插入删除，验证数据正确性
 */
TEST_F(RecordTest, InterleavedInsertDelete) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    char buf[TEST_RECORD_SIZE];
    std::vector<Rid> rids;

    // 插入 10 条
    for (int i = 0; i < 10; ++i) {
        FillRecordSeq(buf, TEST_RECORD_SIZE, i);
        rids.push_back(fh->insert_record(buf, nullptr));
    }

    // 删除偶数号
    for (int i = 0; i < 10; i += 2) {
        fh->delete_record(rids[i], nullptr);
    }

    // 再插入 5 条新记录（应重用被删的 slot）
    for (int i = 10; i < 15; ++i) {
        FillRecordSeq(buf, TEST_RECORD_SIZE, i);
        Rid rid = fh->insert_record(buf, nullptr);
        // slot 应该是某个已经释放的位置（0, 2, 4, 6, 8 之一）
        EXPECT_LT(rid.slot_no, 10);
        rids.push_back(rid);
    }

    // 全表扫描，验证所有存活记录的数量 = 5(奇数存留) + 5(新插入) = 10
    RmScan scan(fh.get());
    int count = 0;
    while (!scan.is_end()) {
        Rid rid = scan.rid();
        auto rec = fh->get_record(rid, nullptr);
        ASSERT_NE(rec, nullptr);
        // 验证数据完整性（记录本身不应被其他操作破坏）
        int stored_seq;
        memcpy(&stored_seq, rec->data, sizeof(stored_seq));
        EXPECT_GE(stored_seq, 1);    // 不是 0, 2, 4, 6, 8（被删了）
        ++count;
        scan.next();
    }
    EXPECT_EQ(count, 10);

    rm_manager_->close_file(fh.get());
}

/**
 * @test 跨多页扫描 + 删除其中一些再扫描
 */
TEST_F(RecordTest, MultiPageScanWithDeletes) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    int rpp = fh->get_file_hdr().num_records_per_page;
    int total = rpp * 2 + 5;   // 三页

    char buf[TEST_RECORD_SIZE];
    std::vector<Rid> rids;
    for (int i = 0; i < total; ++i) {
        FillRecordSeq(buf, TEST_RECORD_SIZE, i);
        rids.push_back(fh->insert_record(buf, nullptr));
    }

    // 删除第一页的所有记录
    for (int i = 0; i < rpp; ++i) {
        fh->delete_record(rids[i], nullptr);
    }

    // 扫描 — 应从第二页（page_no=2）开始
    RmScan scan(fh.get());
    int count = 0;
    while (!scan.is_end()) {
        Rid rid = scan.rid();
        EXPECT_NE(rid.page_no, 1);   // 不应包含第一页的记录
        auto rec = fh->get_record(rid, nullptr);
        ASSERT_NE(rec, nullptr);
        ++count;
        scan.next();
    }
    EXPECT_EQ(count, total - rpp);

    rm_manager_->close_file(fh.get());
}

// ============================================================
// 9. RmManager 多次 open/close/destroy 生命周期
// ============================================================

/**
 * @test 创建 → close → reopen → 再次插入 → 扫描，验证持久性
 */
TEST_F(RecordTest, PersistAcrossOpenClose) {
    constexpr int N = 20;

    // 第一轮：插入数据
    {
        auto fh = rm_manager_->open_file(TEST_FILE);
        char buf[TEST_RECORD_SIZE];
        for (int i = 0; i < N; ++i) {
            FillRecordSeq(buf, TEST_RECORD_SIZE, i);
            fh->insert_record(buf, nullptr);
        }
        rm_manager_->close_file(fh.get());
    }

    // 第二轮：重新打开，扫描验证数据完整
    {
        auto fh = rm_manager_->open_file(TEST_FILE);
        EXPECT_EQ(fh->get_file_hdr().num_pages, 1 + (N + fh->get_file_hdr().num_records_per_page - 1) / fh->get_file_hdr().num_records_per_page);

        RmScan scan(fh.get());
        int count = 0;
        while (!scan.is_end()) {
            auto rec = fh->get_record(scan.rid(), nullptr);
            ASSERT_NE(rec, nullptr);
            CheckRecordSeq(rec->data, TEST_RECORD_SIZE, count);
            ++count;
            scan.next();
        }
        EXPECT_EQ(count, N);
        rm_manager_->close_file(fh.get());
    }
}

// ============================================================
// 10. RmScan 跨多页时 rid 序列正确性
// ============================================================

/**
 * @test 扫描时 rid() 返回的 Rid 应能从文件句柄正确读取记录
 */
TEST_F(RecordTest, ScanRidIsValid) {
    auto fh = rm_manager_->open_file(TEST_FILE);
    ASSERT_NE(fh, nullptr);

    char buf[TEST_RECORD_SIZE];
    FillRecordSeq(buf, TEST_RECORD_SIZE, 42);
    fh->insert_record(buf, nullptr);

    RmScan scan(fh.get());
    ASSERT_FALSE(scan.is_end());

    Rid rid = scan.rid();
    auto rec = fh->get_record(rid, nullptr);
    ASSERT_NE(rec, nullptr);
    CheckRecordSeq(rec->data, TEST_RECORD_SIZE, 42);

    rm_manager_->close_file(fh.get());
}
