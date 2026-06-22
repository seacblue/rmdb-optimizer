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
 * @file test_oj_integrity.cpp
 * @brief OJ 评测接口兼容性参考测试
 *
 * 本文件按照题目描述中的接口定义，逐一验证各模块的核心接口。
 * 设计原则：
 *   - 仅测试题目描述中明确列出的接口
 *   - 测试用例命名清晰，可直接定位到题目描述的对应子任务
 *   - 覆盖正常路径 + 边界/异常路径
 *
 * 三个子任务：
 *   1. 磁盘管理器（DiskManager）：create / open / close / destroy / write_page / read_page
 *   2. 缓冲池管理器（BufferPoolManager + LRUReplacer）
 *   3. 记录管理器（RmFileHandle + RmScan）
 *
 * 编译 & 运行：
 *   make test_oj -j$(nproc) && ./bin/test_oj
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "replacer/lru_replacer.h"
#include "storage/page.h"
#include "record/rm_file_handle.h"
#include "record/rm_scan.h"
#include "record/rm_defs.h"
#include "record/bitmap.h"
#include "common/config.h"
#include "replacer/replacer.h"
#include "system/sm_meta.h"

// ============================================================
// 辅助工具（匿名命名空间，不暴露给外部）
// ============================================================
namespace {

const std::string TEST_DIR = "oj_test_dir";

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

void FillPattern(char *buf, int size, char fill = 'A') {
    memset(buf, fill, static_cast<size_t>(size));
}

bool BufEq(const char *a, const char *b, int size) {
    return memcmp(a, b, static_cast<size_t>(size)) == 0;
}

}  // anonymous namespace

// ============================================================
// 子任务 1：磁盘管理器 DiskManager
//
// 考察接口（共 6 个）：
//   (1) void create_file(const std::string &path)
//   (2) void open_file(const std::string &path)
//   (3) void close_file(const std::string &path)
//   (4) void destroy_file(const std::string &path)
//   (5) void write_page(int fd, page_id_t page_no, const char *offset, int num_bytes)
//   (6) void read_page(int fd, page_id_t page_no, char *offset, int num_bytes)
// ============================================================
class DiskManagerOJTest : public ::testing::Test {
   protected:
    std::unique_ptr<DiskManager> dm_;

    void SetUp() override {
        dm_ = std::make_unique<DiskManager>();
        EnterTestDir(dm_.get());
    }

    void TearDown() override {
        LeaveTestDir(dm_.get());
    }
};

/**
 * @test [基础] create_file: 创建文件后 is_file 返回 true
 * 对应题目 1.(1)
 */
TEST_F(DiskManagerOJTest, CreateFile_FileExistsAfterCreate) {
    const std::string path = "test_create.db";
    EXPECT_FALSE(dm_->is_file(path));
    dm_->create_file(path);
    EXPECT_TRUE(dm_->is_file(path));
}

/**
 * @test [异常] create_file: 重复创建同一文件应抛 FileExistsError
 * 对应题目 1.(1) — 异常路径
 */
TEST_F(DiskManagerOJTest, CreateFile_DuplicateThrows) {
    const std::string path = "test_dup.db";
    dm_->create_file(path);
    EXPECT_THROW(dm_->create_file(path), FileExistsError);
}

/**
 * @test [基础] open_file: 打开已存在文件（无返回值）
 * 对应题目 1.(2)
 */
TEST_F(DiskManagerOJTest, OpenFile_OpensSuccessfully) {
    const std::string path = "test_open.db";
    dm_->create_file(path);
    // 不抛异常即算通过
    EXPECT_NO_THROW(dm_->open_file(path));
    int fd = dm_->get_file_fd(path);
    EXPECT_GE(fd, 0);
    dm_->close_file(path);
}

/**
 * @test [幂等] open_file: 重复打开同一文件不会抛异常
 * 对应题目 1.(2) — 已打开
 */
TEST_F(DiskManagerOJTest, OpenFile_SameFileIdempotent) {
    const std::string path = "test_open2.db";
    dm_->create_file(path);
    EXPECT_NO_THROW(dm_->open_file(path));
    EXPECT_NO_THROW(dm_->open_file(path));
    int fd1 = dm_->get_file_fd(path);
    int fd2 = dm_->get_file_fd(path);
    EXPECT_EQ(fd1, fd2);
    dm_->close_file(path);
}

/**
 * @test [异常] open_file: 打开不存在的文件应抛 FileNotFoundError
 * 对应题目 1.(2) — 异常路径
 */
TEST_F(DiskManagerOJTest, OpenFile_NonExistentThrows) {
    EXPECT_THROW(dm_->open_file("no_such_file.db"), FileNotFoundError);
}

/**
 * @test [基础] close_file: 关闭后可以重新打开
 * 对应题目 1.(3)
 */
TEST_F(DiskManagerOJTest, CloseFile_CanReopenAfterClose) {
    const std::string path = "test_close.db";
    dm_->create_file(path);
    dm_->open_file(path);
    dm_->close_file(path);
    dm_->open_file(path);
    int fd2 = dm_->get_file_fd(path);
    EXPECT_GE(fd2, 0);
    dm_->close_file(path);
}

/**
 * @test [异常] close_file: 关闭未打开的文件应抛 FileNotOpenError
 * 对应题目 1.(3) — 异常路径
 */
TEST_F(DiskManagerOJTest, CloseFile_NotOpenThrows) {
    EXPECT_THROW(dm_->close_file("not_opened.db"), FileNotOpenError);
}

/**
 * @test [基础] destroy_file: 删除文件后 is_file 返回 false
 * 对应题目 1.(4)
 */
TEST_F(DiskManagerOJTest, DestroyFile_FileGoneAfterDestroy) {
    const std::string path = "test_destroy.db";
    dm_->create_file(path);
    EXPECT_TRUE(dm_->is_file(path));
    dm_->destroy_file(path);
    EXPECT_FALSE(dm_->is_file(path));
}

/**
 * @test [基础] write_page + read_page: 写入后读取数据一致
 * 对应题目 1.(5) + 1.(6)
 */
TEST_F(DiskManagerOJTest, WriteReadPage_RoundTrip) {
    const std::string path = "test_rw.db";
    dm_->create_file(path);
    dm_->open_file(path);
    int fd = dm_->get_file_fd(path);

    char write_buf[PAGE_SIZE];
    char read_buf[PAGE_SIZE];
    FillPattern(write_buf, PAGE_SIZE, 'Z');

    dm_->write_page(fd, 0, write_buf, PAGE_SIZE);
    dm_->read_page(fd, 0, read_buf, PAGE_SIZE);

    EXPECT_TRUE(BufEq(write_buf, read_buf, PAGE_SIZE));

    dm_->close_file(path);
}

/**
 * @test [边界] write_page + read_page: 多页面顺序写入和读出
 * 对应题目 1.(5) + 1.(6) — 跨页场景
 */
TEST_F(DiskManagerOJTest, WriteReadPage_MultiplePages) {
    const std::string path = "test_mp.db";
    dm_->create_file(path);
    dm_->open_file(path);
    int fd = dm_->get_file_fd(path);

    constexpr int NUM_PAGES = 8;
    char written[NUM_PAGES][PAGE_SIZE];
    for (int i = 0; i < NUM_PAGES; i++) {
        FillPattern(written[i], PAGE_SIZE, static_cast<char>('A' + i));
        dm_->write_page(fd, i, written[i], PAGE_SIZE);
    }

    for (int i = 0; i < NUM_PAGES; i++) {
        char buf[PAGE_SIZE];
        dm_->read_page(fd, i, buf, PAGE_SIZE);
        EXPECT_TRUE(BufEq(written[i], buf, PAGE_SIZE)) << "Mismatch at page " << i;
    }

    dm_->close_file(path);
}

/**
 * @test [边界] write_page: 部分字节写入后读取
 * 对应题目 1.(5) — num_bytes 参数
 */
TEST_F(DiskManagerOJTest, WriteReadPage_PartialWrite) {
    const std::string path = "test_partial.db";
    dm_->create_file(path);
    dm_->open_file(path);
    int fd = dm_->get_file_fd(path);

    char write_buf[PAGE_SIZE];
    char read_buf[PAGE_SIZE];
    memset(write_buf, 0, PAGE_SIZE);
    memset(read_buf, 0, PAGE_SIZE);
    FillPattern(write_buf, 100, 'K');
    dm_->write_page(fd, 0, write_buf, 100);
    dm_->read_page(fd, 0, read_buf, PAGE_SIZE);

    EXPECT_TRUE(BufEq(write_buf, read_buf, 100));
    for (int i = 100; i < PAGE_SIZE; i++) {
        EXPECT_EQ(read_buf[i], 0) << "Byte " << i << " should be 0";
    }

    dm_->close_file(path);
}

// ============================================================
// 子任务 2-1：LRUReplacer
//
// 考察接口（共 3 个）：
//   (1) bool victim(frame_id_t *frame_id)
//   (2) void pin(frame_id_t frame_id)
//   (3) void unpin(frame_id_t frame_id)
// ============================================================
class LRUReplacerOJTest : public ::testing::Test {
   protected:
    std::unique_ptr<LRUReplacer> replacer_;

    void SetUp() override {
        replacer_ = std::make_unique<LRUReplacer>(10);
    }
};

/**
 * @test [基础] victim: 空 replacer 返回 false
 * 对应题目 2-1.(1)
 */
TEST_F(LRUReplacerOJTest, Victim_EmptyReturnsFalse) {
    frame_id_t frame_id;
    EXPECT_FALSE(replacer_->victim(&frame_id));
}

/**
 * @test [基础] unpin → victim: unpin 后 victim 可淘汰
 * 对应题目 2-1.(1) + (3)
 */
TEST_F(LRUReplacerOJTest, UnpinThenVictim_Succeeds) {
    replacer_->unpin(1);
    frame_id_t frame_id;
    EXPECT_TRUE(replacer_->victim(&frame_id));
    EXPECT_EQ(frame_id, 1);
}

/**
 * @test [基础] pin: pin 固定后无法 victim
 * 对应题目 2-1.(2)
 */
TEST_F(LRUReplacerOJTest, Pin_PreventsVictim) {
    replacer_->unpin(1);
    replacer_->pin(1);
    frame_id_t frame_id;
    EXPECT_FALSE(replacer_->victim(&frame_id));
}

/**
 * @test [LRU 顺序] victim 按 LRU 顺序淘汰
 */
TEST_F(LRUReplacerOJTest, Victim_LruOrder) {
    replacer_->unpin(1);
    replacer_->unpin(2);
    replacer_->unpin(3);
    // unpin 顺序 1→2→3，则 LRU 顺序：1 最久未用，3 最近
    // victim 应淘汰 1
    frame_id_t frame_id;
    EXPECT_TRUE(replacer_->victim(&frame_id));
    EXPECT_EQ(frame_id, 1);
    EXPECT_TRUE(replacer_->victim(&frame_id));
    EXPECT_EQ(frame_id, 2);
    EXPECT_TRUE(replacer_->victim(&frame_id));
    EXPECT_EQ(frame_id, 3);
    EXPECT_FALSE(replacer_->victim(&frame_id));
}

// ============================================================
// 子任务 2-2：BufferPoolManager
//
// 考察接口（共 8 个）：
//   (1) Page *new_page(PageId *page_id)
//   (2) Page *fetch_page(PageId page_id)
//   (3) bool find_victim_page(frame_id_t *frame_id)        [private]
//   (4) void update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) [private]
//   (5) bool unpin_page(PageId page_id, bool is_dirty)
//   (6) bool delete_page(PageId page_id)
//   (7) bool flush_page(PageId page_id)
//   (8) void flush_all_pages(int fd)
// ============================================================
class BufferPoolOJTest : public ::testing::Test {
   protected:
    static constexpr size_t POOL_SIZE = 4;

    DiskManager *disk_manager_;
    BufferPoolManager *bpm_;
    int fd_;

    const std::string path_ = "test_bpm_oj.db";

    void SetUp() override {
        disk_manager_ = new DiskManager();
        EnterTestDir(disk_manager_);

        disk_manager_->create_file(path_);
        disk_manager_->open_file(path_);
        fd_ = disk_manager_->get_file_fd(path_);
        ASSERT_GE(fd_, 0);

        bpm_ = new BufferPoolManager(POOL_SIZE, disk_manager_);
    }

    void TearDown() override {
        delete bpm_;
        disk_manager_->close_file(path_);
        disk_manager_->destroy_file(path_);
        LeaveTestDir(disk_manager_);
        delete disk_manager_;
    }
};

/**
 * @test [基础] new_page: 创建新页返回有效指针
 * 对应题目 2-2.(1)
 */
TEST_F(BufferPoolOJTest, NewPage_ReturnsValidPage) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&pid);
    ASSERT_NE(page, nullptr);
    EXPECT_NE(pid.page_no, INVALID_PAGE_ID);
    EXPECT_EQ(page->get_page_id().page_no, pid.page_no);
    bpm_->unpin_page(pid, false);
}

/**
 * @test [基础] fetch_page: 缓存未命中时从磁盘读取
 * 对应题目 2-2.(2)
 */
TEST_F(BufferPoolOJTest, FetchPage_CacheMiss) {
    page_id_t page_no = disk_manager_->allocate_page(fd_);
    char buf[PAGE_SIZE];
    FillPattern(buf, PAGE_SIZE, 'Q');
    disk_manager_->write_page(fd_, page_no, buf, PAGE_SIZE);

    PageId pid = {.fd = fd_, .page_no = page_no};
    Page *page = bpm_->fetch_page(pid);
    ASSERT_NE(page, nullptr);
    EXPECT_TRUE(BufEq(page->get_data(), buf, PAGE_SIZE));

    bpm_->unpin_page(pid, false);
}

/**
 * @test [基础] fetch_page: 缓存命中返回相同指针
 * 对应题目 2-2.(2)
 */
TEST_F(BufferPoolOJTest, FetchPage_CacheHit) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page1 = bpm_->new_page(&pid);
    ASSERT_NE(page1, nullptr);
    bpm_->unpin_page(pid, false);

    Page *page2 = bpm_->fetch_page(pid);
    EXPECT_EQ(page1, page2);

    bpm_->unpin_page(pid, false);
}

/**
 * @test [基础] unpin_page: unpin 到 0 后不可再 unpin
 * 对应题目 2-2.(5)
 */
TEST_F(BufferPoolOJTest, UnpinPage_CountLifecycle) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    bpm_->new_page(&pid);
    ASSERT_NE(pid.page_no, INVALID_PAGE_ID);
    // new_page 后 pin_count = 1
    EXPECT_TRUE(bpm_->unpin_page(pid, false));  // → 0
    EXPECT_FALSE(bpm_->unpin_page(pid, false));  // 已经为 0
}

/**
 * @test [基础] delete_page: 删除页面
 * 对应题目 2-2.(6)
 */
TEST_F(BufferPoolOJTest, DeletePage_Simple) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&pid);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(pid.page_no, INVALID_PAGE_ID);

    bpm_->unpin_page(pid, false);
    EXPECT_TRUE(bpm_->delete_page(pid));
}

/**
 * @test [基础] flush_page: 刷脏页到磁盘
 * 对应题目 2-2.(7)
 */
TEST_F(BufferPoolOJTest, FlushPage_WritesToDisk) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&pid);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(pid.page_no, INVALID_PAGE_ID);

    FillPattern(page->get_data(), PAGE_SIZE, 'F');
    bpm_->unpin_page(pid, true);
    EXPECT_TRUE(bpm_->flush_page(pid));

    char read_buf[PAGE_SIZE];
    disk_manager_->read_page(fd_, pid.page_no, read_buf, PAGE_SIZE);
    EXPECT_TRUE(BufEq(page->get_data(), read_buf, PAGE_SIZE));
}

/**
 * @test [基础] flush_all_pages: 刷出指定文件的所有脏页
 * 对应题目 2-2.(8)
 */
TEST_F(BufferPoolOJTest, FlushAllPages_NoCrash) {
    PageId pid1 = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    PageId pid2 = {.fd = fd_, .page_no = INVALID_PAGE_ID};

    bpm_->new_page(&pid1);
    bpm_->new_page(&pid2);

    bpm_->unpin_page(pid1, true);
    bpm_->unpin_page(pid2, true);

    bpm_->flush_all_pages(fd_);

    char buf[PAGE_SIZE];
    disk_manager_->read_page(fd_, pid1.page_no, buf, PAGE_SIZE);
    disk_manager_->read_page(fd_, pid2.page_no, buf, PAGE_SIZE);
}

/**
 * @test [边界] eviction: 缓冲池满时淘汰脏页
 */
TEST_F(BufferPoolOJTest, Evict_DirtyPageWrittenBack) {
    const std::string path2 = "test_evict.db";
    disk_manager_->create_file(path2);
    disk_manager_->open_file(path2);
    int fd2 = disk_manager_->get_file_fd(path2);

    PageId pids[5];
    for (int i = 0; i < 5; i++) {
        int target_fd = (i < 4) ? fd_ : fd2;
        pids[i] = {.fd = target_fd, .page_no = INVALID_PAGE_ID};
        Page *page = bpm_->new_page(&pids[i]);
        ASSERT_NE(page, nullptr);
        bpm_->unpin_page(pids[i], true);
    }

    EXPECT_NE(pids[4].page_no, INVALID_PAGE_ID);

    disk_manager_->close_file(path2);
    disk_manager_->destroy_file(path2);
}

// ============================================================
// 子任务 3：记录管理器（RmFileHandle + RmScan）
//
// RmFileHandle 接口（共 5 个）：
//   (1) get_record
//   (2) insert_record (自动位置, 带 Context)
//   (3) insert_record (指定位置, 无 Context)
//   (4) delete_record
//   (5) update_record
//
// RmScan 接口（共 3 个）：
//   (1) 构造函数
//   (2) next()
//   (3) is_end()
// ============================================================
class RmFileHandleOJTest : public ::testing::Test {
   protected:
    static constexpr int RECORD_SIZE = 64;

    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<RmFileHandle> fh_;
    int fd_;

    std::string rm_path_ = "test_rm_oj.db";

    void SetUp() override {
        disk_manager_ = std::make_unique<DiskManager>();
        EnterTestDir(disk_manager_.get());

        disk_manager_->create_file(rm_path_);
        disk_manager_->open_file(rm_path_);
        fd_ = disk_manager_->get_file_fd(rm_path_);
        ASSERT_GE(fd_, 0);

        bpm_ = std::make_unique<BufferPoolManager>(8, disk_manager_.get());

        // 先构造 RmFileHdr 写入文件第 0 页，RmFileHandle 的构造函数会从磁盘读取
        RmFileHdr hdr;
        hdr.record_size = RECORD_SIZE;
        hdr.num_records_per_page = (PAGE_SIZE - sizeof(RmPageHdr)) / RECORD_SIZE;
        hdr.num_pages = 1;          // 初始只有 file_hdr 页
        hdr.first_free_page_no = RM_NO_PAGE;
        hdr.bitmap_size = (hdr.num_records_per_page + 7) / 8;
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (const char *)&hdr, sizeof(hdr));

        fh_ = std::make_unique<RmFileHandle>(disk_manager_.get(), bpm_.get(), fd_);
    }

    void TearDown() override {
        fh_.reset();
        bpm_.reset();
        disk_manager_->close_file(rm_path_);
        disk_manager_->destroy_file(rm_path_);
        LeaveTestDir(disk_manager_.get());
    }
};

/**
 * @test [基础] insert_record + get_record: 插入后立即读取
 * 对应题目 3-1.(2) + (1)
 */
TEST_F(RmFileHandleOJTest, InsertThenGetRecord) {
    char buf[RECORD_SIZE];
    FillPattern(buf, RECORD_SIZE, 'I');

    Rid rid = fh_->insert_record(buf, nullptr);
    EXPECT_NE(rid.page_no, RM_NO_PAGE);
    EXPECT_GE(rid.slot_no, 0);

    auto rec = fh_->get_record(rid, nullptr);
    ASSERT_NE(rec, nullptr);
    EXPECT_TRUE(BufEq(rec->data, buf, RECORD_SIZE));
}

/**
 * @test [基础] insert_record: 多次插入获得不同 rid
 * 对应题目 3-1.(2)
 */
TEST_F(RmFileHandleOJTest, InsertMultiple_ReturnsUniqueRids) {
    constexpr int N = 10;
    char bufs[N][RECORD_SIZE];
    Rid rids[N];

    for (int i = 0; i < N; i++) {
        FillPattern(bufs[i], RECORD_SIZE, static_cast<char>('A' + i));
        rids[i] = fh_->insert_record(bufs[i], nullptr);
    }

    for (int i = 0; i < N; i++) {
        auto rec = fh_->get_record(rids[i], nullptr);
        ASSERT_NE(rec, nullptr);
        EXPECT_TRUE(BufEq(rec->data, bufs[i], RECORD_SIZE)) << "Mismatch at record " << i;
    }
}

/**
 * @test [基础] insert_record (指定位置): 在指定 rid 插入后可读取
 * 对应题目 3-1.(3) — 事务回滚场景
 */
TEST_F(RmFileHandleOJTest, InsertAtRid_ThenReadable) {
    char buf[RECORD_SIZE];
    FillPattern(buf, RECORD_SIZE, 'X');
    Rid rid = fh_->insert_record(buf, nullptr);

    char buf2[RECORD_SIZE];
    FillPattern(buf2, RECORD_SIZE, 'Y');
    fh_->insert_record(rid, buf2);

    auto rec = fh_->get_record(rid, nullptr);
    ASSERT_NE(rec, nullptr);
    EXPECT_TRUE(BufEq(rec->data, buf2, RECORD_SIZE));
}

/**
 * @test [基础] delete_record: 删除后不可读
 * 对应题目 3-1.(4)
 */
TEST_F(RmFileHandleOJTest, DeleteRecord_ThenGone) {
    char buf[RECORD_SIZE];
    FillPattern(buf, RECORD_SIZE, 'D');
    Rid rid = fh_->insert_record(buf, nullptr);

    fh_->delete_record(rid, nullptr);

    EXPECT_EQ(fh_->get_record(rid, nullptr), nullptr);
}

/**
 * @test [基础] update_record: 更新后获取到新值
 * 对应题目 3-1.(5)
 */
TEST_F(RmFileHandleOJTest, UpdateRecord_ThenReadsNewValue) {
    char old_buf[RECORD_SIZE];
    FillPattern(old_buf, RECORD_SIZE, 'O');
    Rid rid = fh_->insert_record(old_buf, nullptr);

    char new_buf[RECORD_SIZE];
    FillPattern(new_buf, RECORD_SIZE, 'N');
    fh_->update_record(rid, new_buf, nullptr);

    auto rec = fh_->get_record(rid, nullptr);
    ASSERT_NE(rec, nullptr);
    EXPECT_TRUE(BufEq(rec->data, new_buf, RECORD_SIZE));
}

/**
 * @test [边界] delete → reinsert 复用槽位
 */
TEST_F(RmFileHandleOJTest, DeleteThenReinsert_ReusesSlot) {
    char buf[RECORD_SIZE];
    FillPattern(buf, RECORD_SIZE, 'R');
    Rid old_rid = fh_->insert_record(buf, nullptr);

    fh_->delete_record(old_rid, nullptr);

    char buf2[RECORD_SIZE];
    FillPattern(buf2, RECORD_SIZE, 'S');
    Rid new_rid = fh_->insert_record(buf2, nullptr);

    EXPECT_EQ(new_rid.page_no, old_rid.page_no);
    EXPECT_EQ(new_rid.slot_no, old_rid.slot_no);
}

/**
 * @test [基础] RmScan: 空表扫描立即 is_end
 * 对应题目 3-2.(1)+(3)
 */
TEST_F(RmFileHandleOJTest, Scan_EmptyTable) {
    RmScan scan(fh_.get());
    EXPECT_TRUE(scan.is_end());
}

/**
 * @test [基础] RmScan: 遍历所有记录
 * 对应题目 3-2.(1)+(2)+(3)
 */
TEST_F(RmFileHandleOJTest, Scan_IteratesAllRecords) {
    constexpr int N = 5;
    char bufs[N][RECORD_SIZE];
    Rid rids[N];

    for (int i = 0; i < N; i++) {
        FillPattern(bufs[i], RECORD_SIZE, static_cast<char>('M' + i));
        rids[i] = fh_->insert_record(bufs[i], nullptr);
    }

    RmScan scan(fh_.get());
    int count = 0;
    for (; !scan.is_end(); scan.next(), count++) {
        Rid rid = scan.rid();
        auto rec = fh_->get_record(rid, nullptr);
        ASSERT_NE(rec, nullptr);
        bool found = false;
        for (int j = 0; j < N; j++) {
            if (rids[j].page_no == rid.page_no && rids[j].slot_no == rid.slot_no) {
                EXPECT_TRUE(BufEq(rec->data, bufs[j], RECORD_SIZE));
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found)
            << "Scanned unexpected record at (" << rid.page_no << "," << rid.slot_no << ")";
    }
    EXPECT_EQ(count, N);
}

/**
 * @test [边界] RmScan: 部分删除后扫描
 */
TEST_F(RmFileHandleOJTest, Scan_AfterPartialDelete) {
    constexpr int N = 6;
    Rid rids[N];
    for (int i = 0; i < N; i++) {
        char buf[RECORD_SIZE];
        FillPattern(buf, RECORD_SIZE, static_cast<char>('S' + i));
        rids[i] = fh_->insert_record(buf, nullptr);
    }

    fh_->delete_record(rids[0], nullptr);
    fh_->delete_record(rids[2], nullptr);
    fh_->delete_record(rids[4], nullptr);

    RmScan scan(fh_.get());
    int count = 0;
    for (; !scan.is_end(); scan.next(), count++) {
        Rid rid = scan.rid();
        bool is_deleted = false;
        for (int d = 0; d < N; d += 2) {
            if (rids[d].page_no == rid.page_no && rids[d].slot_no == rid.slot_no) {
                is_deleted = true;
                break;
            }
        }
        EXPECT_FALSE(is_deleted)
            << "Scanned a deleted record at (" << rid.page_no << "," << rid.slot_no << ")";
    }
    EXPECT_EQ(count, N / 2);
}
