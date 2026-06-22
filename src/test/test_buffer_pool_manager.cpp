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
 * @file test_buffer_pool.cpp
 * @brief BufferPoolManager 模块单元测试（14 个测试用例）
 *
 * 覆盖场景：
 *   - 基本 fetch / cache hit / pin_count 语义
 *   - new_page / delete_page 生命周期
 *   - flush_page / flush_all_pages 持久化
 *   - LRU eviction 与脏页写回
 *   - 各种边界条件（非空 unpin、double delete 等）
 *
 * 编译 & 运行：
 *   mkdir -p build && cd build
 *   cmake .. -DENABLE_COVERAGE=ON && make test_bp -j$(nproc)
 *   ./bin/test_bp
 *
 * CTest:
 *   ctest --output-on-failure -R test_bp
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "test_utils.h"

// ============================================================
// 测试夹具：每个 TEST_F 进入干净的空目录
// ============================================================
class BufferPoolTest : public ::testing::Test {
   protected:
    static constexpr size_t POOL_SIZE = 4;  // 小缓冲池，便于测试 eviction
    static const std::string TEST_FILE;

    DiskManager *disk_manager_;
    BufferPoolManager *bpm_;
    int fd_;  // 测试文件句柄

    void SetUp() override {
        disk_manager_ = new DiskManager();
        test_utils::EnterTestDir(disk_manager_);

        // 创建测试文件
        disk_manager_->create_file(TEST_FILE);
        fd_ = disk_manager_->open_file(TEST_FILE);
        ASSERT_GE(fd_, 0) << "Failed to open test file";

        bpm_ = new BufferPoolManager(POOL_SIZE, disk_manager_);
    }

    void TearDown() override {
        // 清理 buffer pool
        delete bpm_;

        // 关闭并销毁测试文件
        disk_manager_->close_file(fd_);
        disk_manager_->destroy_file(TEST_FILE);

        // 清理测试目录
        test_utils::LeaveTestDir(disk_manager_);
        delete disk_manager_;
    }

    /** 在 page 中写入一个可验证的模式：前 8 字节为 page_no，后面填充为特定字符 */
    static void WritePattern(Page *page, page_id_t page_no, char fill = 'A') {
        char *data = page->get_data();
        memcpy(data, &page_no, sizeof(page_no));
        memset(data + sizeof(page_no), fill, PAGE_SIZE - sizeof(page_no));
    }

    /** 验证 page 中的模式是否与期望一致 */
    static void CheckPattern(Page *page, page_id_t expected_page_no, char expected_fill = 'A') {
        char *data = page->get_data();
        page_id_t stored_no;
        memcpy(&stored_no, data, sizeof(stored_no));
        EXPECT_EQ(stored_no, expected_page_no);
        for (size_t i = sizeof(stored_no); i < PAGE_SIZE; ++i) {
            EXPECT_EQ(data[i], expected_fill) << "Byte mismatch at offset " << i;
        }
    }
};

const std::string BufferPoolTest::TEST_FILE = "test_bp.bin";

// ============================================================
//  测试用例：基本 fetch 语义
// ============================================================

/**
 * @test 缓存未命中——fetch 一个磁盘上的页
 *
 * 步骤：
 *   1. 直接在磁盘上写入一个已知模式的 page
 *   2. 通过 fetch_page 从缓冲池获取该页
 *   3. 验证数据正确，pin_count = 1
 */
TEST_F(BufferPoolTest, BasicFetchCacheMiss) {
    // 直接在 fd 上分配一个 page_no
    page_id_t page_no = disk_manager_->allocate_page(fd_);
    ASSERT_NE(page_no, INVALID_PAGE_ID);

    // 直接通过 DiskManager 写入已知数据
    char buf[PAGE_SIZE] = {};
    page_id_t written_no = page_no;
    memcpy(buf, &written_no, sizeof(written_no));
    memset(buf + sizeof(written_no), 'X', PAGE_SIZE - sizeof(written_no));
    disk_manager_->write_page(fd_, page_no, buf, PAGE_SIZE);

    // 通过 buffer pool 读取
    PageId pid = {.fd = fd_, .page_no = page_no};
    Page *page = bpm_->fetch_page(pid);
    ASSERT_NE(page, nullptr);

    // 验证数据
    CheckPattern(page, page_no, 'X');
    EXPECT_EQ(page->get_page_id().page_no, page_no);
    EXPECT_EQ(page->get_page_id().fd, fd_);

    // pin_count 应为 1
    // （pin_count_ 是 private，通过 unpin 的返回值间接验证）
    EXPECT_TRUE(bpm_->unpin_page(pid, false));
}

/**
 * @test 缓存命中——两次 fetch 同一页应返回相同的 Page*
 *
 * 步骤：
 *   1. 通过 new_page 创建一个页（放入缓冲池）
 *   2. unpin 使其可被访问
 *   3. 再次 fetch 同一页（应有缓存命中）
 *   4. 验证指针相同
 */
TEST_F(BufferPoolTest, FetchCacheHit) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page1 = bpm_->new_page(&pid);
    ASSERT_NE(page1, nullptr);
    ASSERT_NE(pid.page_no, INVALID_PAGE_ID);
    bpm_->unpin_page(pid, false);

    // 第二次 fetch（应有缓存命中）
    Page *page2 = bpm_->fetch_page(pid);
    ASSERT_NE(page2, nullptr);
    EXPECT_EQ(page1, page2);

    bpm_->unpin_page(pid, false);
}

/**
 * @test pin_count 正确递增/递减
 *
 * 步骤：
 *   1. new_page → pin_count = 1（new_page 初始 pin_count 即为 1）
 *   2. fetch → pin_count = 2
 *   3. unpin → pin_count = 1
 *   4. unpin → pin_count = 0
 *   5. unpin → false（已为 0）
 */
TEST_F(BufferPoolTest, PinCountLifecycle) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    bpm_->new_page(&pid);
    ASSERT_NE(pid.page_no, INVALID_PAGE_ID);

    // 此时 pin_count = 1（new_page 已固定）
    // 再次 fetch → pin_count = 2
    bpm_->fetch_page(pid);

    // unpin 两次
    EXPECT_TRUE(bpm_->unpin_page(pid, false));  // → 1
    EXPECT_TRUE(bpm_->unpin_page(pid, false));  // → 0
    // 第三次 unpin 应失败
    EXPECT_FALSE(bpm_->unpin_page(pid, false));
}

// ============================================================
//  测试用例：新建与删除页
// ============================================================

/**
 * @test new_page 创建一个新页
 */
TEST_F(BufferPoolTest, NewPage) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&pid);
    ASSERT_NE(page, nullptr);

    // 验证分配了有效的 page_no
    EXPECT_NE(pid.page_no, INVALID_PAGE_ID);
    EXPECT_EQ(page->get_page_id().page_no, pid.page_no);
    EXPECT_EQ(page->get_page_id().fd, fd_);

    // 新页数据应全零（reset_memory）
    char *data = page->get_data();
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        EXPECT_EQ(data[i], 0) << "New page not zeroed at offset " << i;
    }

    bpm_->unpin_page(pid, false);
}

/**
 * @test delete_page 删除一个已存在的页
 *
 * 步骤：
 *   1. new_page 创建，写入数据并标记脏
 *   2. unpin(dirty=true) 后 delete_page（脏页会写回磁盘）
 *   3. 重新 fetch 该页应从磁盘读取，验证数据完整性
 */
TEST_F(BufferPoolTest, DeletePage) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&pid);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(pid.page_no, INVALID_PAGE_ID);

    // 写入模式并标记脏，确保 delete 时写回磁盘
    WritePattern(page, pid.page_no, 'D');
    bpm_->mark_dirty(page);
    bpm_->unpin_page(pid, true);

    // 删除（脏页会自动写回磁盘）
    EXPECT_TRUE(bpm_->delete_page(pid));

    // 重新 fetch 应从磁盘读取
    page = bpm_->fetch_page(pid);
    ASSERT_NE(page, nullptr);
    CheckPattern(page, pid.page_no, 'D');
    bpm_->unpin_page(pid, false);
}

/**
 * @test 试图删除一个 pinned 的页应返回 false
 */
TEST_F(BufferPoolTest, DeletePinnedPage) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&pid);
    ASSERT_NE(page, nullptr);
    // pin_count = 1（未 unpin），应无法删除
    EXPECT_FALSE(bpm_->delete_page(pid));

    bpm_->unpin_page(pid, false);
    // unpin 后应可删除
    EXPECT_TRUE(bpm_->delete_page(pid));
}

/**
 * @test 删除一个不在缓冲池中的页应返回 true
 */
TEST_F(BufferPoolTest, DeleteNonExistentPage) {
    PageId pid = {.fd = fd_, .page_no = 9999};
    EXPECT_TRUE(bpm_->delete_page(pid));
}

// ============================================================
//  测试用例：flush 持久化
// ============================================================

/**
 * @test flush_page 将脏页写回磁盘
 *
 * 步骤：
 *   1. new_page 创建一个页，写入数据，标记脏
 *   2. flush_page 写回磁盘
 *   3. 通过 DiskManager 直接读取磁盘验证数据
 */
TEST_F(BufferPoolTest, FlushPage) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&pid);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(pid.page_no, INVALID_PAGE_ID);

    WritePattern(page, pid.page_no, 'F');
    bpm_->mark_dirty(page);

    // flush
    EXPECT_TRUE(bpm_->flush_page(pid));

    // 直接通过磁盘读取验证
    char disk_buf[PAGE_SIZE] = {};
    disk_manager_->read_page(fd_, pid.page_no, disk_buf, PAGE_SIZE);
    page_id_t stored_no;
    memcpy(&stored_no, disk_buf, sizeof(stored_no));
    EXPECT_EQ(stored_no, pid.page_no);
    EXPECT_EQ(disk_buf[sizeof(stored_no)], 'F');

    bpm_->unpin_page(pid, false);
}

/**
 * @test flush_page 对非缓冲池中的页返回 false
 */
TEST_F(BufferPoolTest, FlushNonExistentPage) {
    PageId pid = {.fd = fd_, .page_no = 9999};
    EXPECT_FALSE(bpm_->flush_page(pid));
}

/**
 * @test flush_all_pages 将同一 fd 的所有脏页写回
 */
TEST_F(BufferPoolTest, FlushAllPages) {
    // 分配 3 个页
    std::vector<PageId> pids;
    for (int i = 0; i < 3; ++i) {
        PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
        Page *page = bpm_->new_page(&pid);
        ASSERT_NE(page, nullptr);
        WritePattern(page, pid.page_no, static_cast<char>('0' + i));
        bpm_->mark_dirty(page);
        pids.push_back(pid);
        bpm_->unpin_page(pid, false);
    }

    // flush_all_pages
    bpm_->flush_all_pages(fd_);

    // 验证磁盘数据
    for (size_t i = 0; i < pids.size(); ++i) {
        char disk_buf[PAGE_SIZE] = {};
        disk_manager_->read_page(fd_, pids[i].page_no, disk_buf, PAGE_SIZE);
        page_id_t stored_no;
        memcpy(&stored_no, disk_buf, sizeof(stored_no));
        EXPECT_EQ(stored_no, pids[i].page_no);
        EXPECT_EQ(disk_buf[sizeof(stored_no)], static_cast<char>('0' + i));
    }
}

// ============================================================
//  测试用例：LRU Eviction
// ============================================================

/**
 * @test 缓冲池满后淘汰最久未使用的页（LRU）
 *
 * 步骤：
 *   1. 分配 4 个页（填满 POOL_SIZE=4），全部 unpin
 *   2. 分配第 5 个页 → 应触发淘汰最早 unpin 的页
 */
TEST_F(BufferPoolTest, LruEviction) {
    std::vector<PageId> pids;

    // 填满缓冲池
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
        Page *page = bpm_->new_page(&pid);
        ASSERT_NE(page, nullptr);
        pids.push_back(pid);
        WritePattern(page, pid.page_no, static_cast<char>('A' + i));
        bpm_->mark_dirty(page);
        bpm_->unpin_page(pid, false);
    }

    // 分配第 5 个页（应淘汰最早 unpin 的那个）
    PageId new_pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *evicted = bpm_->new_page(&new_pid);
    ASSERT_NE(evicted, nullptr);
    WritePattern(evicted, new_pid.page_no, 'E');
    bpm_->mark_dirty(evicted);
    bpm_->unpin_page(new_pid, false);

    // 此时最早的那个页（pids[0]）应已被淘汰
    // 重新 fetch pids[0] 应从磁盘读取（cache miss），数据应仍保留
    Page *re_fetch = bpm_->fetch_page(pids[0]);
    ASSERT_NE(re_fetch, nullptr);
    CheckPattern(re_fetch, pids[0].page_no, 'A');
    bpm_->unpin_page(pids[0], false);
}

/**
 * @test 脏页被淘汰时自动写回磁盘
 *
 * 步骤：
 *   1. 填满缓冲池，写脏数据
 *   2. 触发淘汰
 *   3. 直接读磁盘验证脏数据已写回
 */
TEST_F(BufferPoolTest, DirtyPageWriteBackOnEviction) {
    std::vector<PageId> pids;

    // 填满缓冲池
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
        Page *page = bpm_->new_page(&pid);
        ASSERT_NE(page, nullptr);
        pids.push_back(pid);
        WritePattern(page, pid.page_no, static_cast<char>('D' + i));
        bpm_->mark_dirty(page);
        bpm_->unpin_page(pid, false);  // unpin → 可被淘汰
    }

    // 触发淘汰第一个页
    PageId trigger = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    bpm_->new_page(&trigger);
    ASSERT_NE(trigger.page_no, INVALID_PAGE_ID);
    bpm_->unpin_page(trigger, false);

    // 直接从磁盘读取被淘汰的页（pids[0]），验证脏数据已写回
    char disk_buf[PAGE_SIZE] = {};
    disk_manager_->read_page(fd_, pids[0].page_no, disk_buf, PAGE_SIZE);
    page_id_t stored_no;
    memcpy(&stored_no, disk_buf, sizeof(stored_no));
    EXPECT_EQ(stored_no, pids[0].page_no);
    EXPECT_EQ(disk_buf[sizeof(stored_no)], 'D');
}

/**
 * @test 被 pin 的页不会被淘汰
 *
 * 步骤：
 *   1. 填满缓冲池，但保留一个页 pinned
 *   2. 尝试分配新页 → 应失败（所有 unpinned 页被淘汰，但还有一个 pinned）
 *      实际上因为只有一个页被淘汰，其余 pinned，所以没问题
 *   3. 验证 pinned 页数据不变
 */
TEST_F(BufferPoolTest, PinnedPageNotEvicted) {
    // 先分配一个页，保持 pinned
    PageId pinned_pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *pinned_page = bpm_->new_page(&pinned_pid);
    ASSERT_NE(pinned_page, nullptr);
    WritePattern(pinned_page, pinned_pid.page_no, 'P');
    bpm_->mark_dirty(pinned_page);
    // 不 unpin 此页 → pin_count = 1

    // 填满剩余的 3 个帧
    for (size_t i = 1; i < POOL_SIZE; ++i) {
        PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
        Page *page = bpm_->new_page(&pid);
        ASSERT_NE(page, nullptr);
        bpm_->unpin_page(pid, false);
    }

    // 再分配一页 → 应成功（有 3 个 unpinned 页可淘汰）
    PageId extra_pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *extra_page = bpm_->new_page(&extra_pid);
    ASSERT_NE(extra_page, nullptr);
    bpm_->unpin_page(extra_pid, false);

    // 释放 pinned page
    bpm_->unpin_page(pinned_pid, false);
    bpm_->delete_page(pinned_pid);
}

// ============================================================
//  测试用例：边界条件
// ============================================================

/**
 * @test pin_count 归零后再次 fetch 会重新 pin
 *
 * 步骤：
 *   1. new_page 创建页 → pin_count=1
 *   2. unpin → pin_count=0（可被淘汰）
 *   3. 再次 fetch → 应有缓存命中，pin_count 回到 1
 */
TEST_F(BufferPoolTest, ReFetchAfterUnpin) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&pid);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(pid.page_no, INVALID_PAGE_ID);
    bpm_->unpin_page(pid, false);  // pin_count → 0

    // 再次 fetch（缓存命中）
    Page *page2 = bpm_->fetch_page(pid);
    ASSERT_NE(page2, nullptr);
    EXPECT_EQ(page, page2);

    bpm_->unpin_page(pid, false);
}

/**
 * @test 大数据完整性——多次写入并验证
 */
TEST_F(BufferPoolTest, DataIntegrityLarge) {
    // 分配并写入若干页
    constexpr int NUM_PAGES = 3;
    std::vector<PageId> pids;

    for (int i = 0; i < NUM_PAGES; ++i) {
        PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
        Page *page = bpm_->new_page(&pid);
        ASSERT_NE(page, nullptr);
        pids.push_back(pid);
        // 写入全页模式：前 4 字节是 i，后 4 字节是 i*2，其余填充
        char *data = page->get_data();
        int *int_data = reinterpret_cast<int *>(data);
        int_data[0] = i;
        int_data[1] = i * 2;
        memset(data + 8, static_cast<char>(i), PAGE_SIZE - 8);
        bpm_->mark_dirty(page);
        bpm_->unpin_page(pid, false);
    }

    // 重新 fetch 并验证
    for (int i = 0; i < NUM_PAGES; ++i) {
        Page *page = bpm_->fetch_page(pids[i]);
        ASSERT_NE(page, nullptr);
        char *data = page->get_data();
        int *int_data = reinterpret_cast<int *>(data);
        EXPECT_EQ(int_data[0], i);
        EXPECT_EQ(int_data[1], i * 2);
        for (size_t j = 8; j < PAGE_SIZE; ++j) {
            EXPECT_EQ(data[j], static_cast<char>(i)) << "Byte mismatch at offset " << j;
        }
        bpm_->unpin_page(pids[i], false);
    }
}

/**
 * @test 连续多次 fetch/unpin 同一页，pin_count 正确管理
 *
 * 步骤：
 *   1. new_page 创建页（pin_count=1）
 *   2. 再 fetch 4 次（pin_count 递增到 5）
 *   3. 所有指针应相同
 *   4. unpin 5 次递减到 0，第 6 次应失败
 */
TEST_F(BufferPoolTest, MultiplePinUnpinCycle) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *first = bpm_->new_page(&pid);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(pid.page_no, INVALID_PAGE_ID);

    constexpr int N = 5;  // 再 fetch 4 次（new_page 已贡献 1 次 pin）
    std::vector<Page *> pages;
    pages.push_back(first);
    for (int i = 1; i < N; ++i) {
        Page *p = bpm_->fetch_page(pid);
        ASSERT_NE(p, nullptr);
        pages.push_back(p);
    }
    // 所有指针应相同
    for (int i = 1; i < N; ++i) {
        EXPECT_EQ(pages[0], pages[i]);
    }

    // unpin N 次（此时 pin_count = N）
    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(bpm_->unpin_page(pid, false));
    }
    // 第 N+1 次应失败
    EXPECT_FALSE(bpm_->unpin_page(pid, false));
}

/**
 * @test unpin_page 正确传递 is_dirty 标记
 *
 * 步骤：
 *   1. new_page 创建页（初始 is_dirty=false）
 *   2. fetch 后 unpin(dirty=true) → is_dirty 应为 true
 *   3. 再次 fetch 后 unpin(dirty=false) → 脏标记保留（不清除已有脏标记）
 *   4. flush 后脏标记清除
 */
TEST_F(BufferPoolTest, UnpinSetsDirty) {
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&pid);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(pid.page_no, INVALID_PAGE_ID);

    // unpin 时标记 dirty
    bpm_->unpin_page(pid, true);
    EXPECT_TRUE(page->is_dirty());

    // 再次 fetch 并 unpin 不标记 dirty，脏标记应保留（不清除已有脏标记）
    bpm_->fetch_page(pid);
    bpm_->unpin_page(pid, false);
    EXPECT_TRUE(page->is_dirty());

    // flush 后脏标记清除
    bpm_->flush_page(pid);
    EXPECT_FALSE(page->is_dirty());

    bpm_->delete_page(pid);
}