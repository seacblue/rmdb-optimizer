/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

/**
 * @file test_oj1.cpp
 * @brief OJ Problem 1 -- Storage Management (DiskManager + BufferPoolManager + LRUReplacer + RmFileHandle)
 *
 * Direct API calls (no client-server)
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "replacer/lru_replacer.h"
#include "storage/page.h"
#include "record/rm_file_handle.h"
#include "record/rm_scan.h"
#include "record/rm_defs.h"
#include "record/rm_manager.h"
#include "common/config.h"
#include "replacer/replacer.h"

namespace {

const std::string TEST_DIR = "oj1_test_dir";

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
        perror("chdir ..");
        FAIL() << "Cannot return from test directory";
    }
    if (dm->is_dir(TEST_DIR)) {
        dm->destroy_dir(TEST_DIR);
    }
}

// ============================================================
// Subtask 1: DiskManager basic interface tests
// ============================================================
class DiskOJ1Test : public ::testing::Test {
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

TEST_F(DiskOJ1Test, CreateAndOpenFile) {
    dm_->create_file("test.db");
    dm_->open_file("test.db");
    int fd = dm_->get_file_fd("test.db");
    EXPECT_GE(fd, 0);
    dm_->close_file("test.db");
    dm_->destroy_file("test.db");
}

TEST_F(DiskOJ1Test, WriteAndReadPage) {
    const char *filename = "rw_test.db";
    dm_->create_file(filename);
    dm_->open_file(filename);
    int fd = dm_->get_file_fd(filename);

    char write_buf[PAGE_SIZE];
    memset(write_buf, 'A', PAGE_SIZE);
    dm_->write_page(fd, 0, write_buf, PAGE_SIZE);

    char read_buf[PAGE_SIZE];
    memset(read_buf, 0, PAGE_SIZE);
    dm_->read_page(fd, 0, read_buf, PAGE_SIZE);
    EXPECT_EQ(memcmp(write_buf, read_buf, PAGE_SIZE), 0);

    dm_->close_file(filename);
    dm_->destroy_file(filename);
}

TEST_F(DiskOJ1Test, MultiPageWrite) {
    const char *filename = "multi_page.db";
    dm_->create_file(filename);
    dm_->open_file(filename);
    int fd = dm_->get_file_fd(filename);

    const int NUM_PAGES = 10;
    for (int i = 0; i < NUM_PAGES; i++) {
        char buf[PAGE_SIZE];
        memset(buf, 'A' + i, PAGE_SIZE);
        dm_->write_page(fd, i, buf, PAGE_SIZE);
    }

    for (int i = 0; i < NUM_PAGES; i++) {
        char buf[PAGE_SIZE];
        memset(buf, 0, PAGE_SIZE);
        dm_->read_page(fd, i, buf, PAGE_SIZE);
        char expected[PAGE_SIZE];
        memset(expected, 'A' + i, PAGE_SIZE);
        EXPECT_EQ(memcmp(buf, expected, PAGE_SIZE), 0);
    }

    dm_->close_file(filename);
    dm_->destroy_file(filename);
}

// ============================================================
// Subtask 2: LRUReplacer interface tests
// ============================================================
class LRUOJ1Test : public ::testing::Test {
   protected:
    std::unique_ptr<LRUReplacer> lru_;

    void SetUp() override {
        lru_ = std::make_unique<LRUReplacer>(10);
    }
};

TEST_F(LRUOJ1Test, VictimOrder) {
    lru_->unpin(1);
    lru_->unpin(2);
    lru_->unpin(3);

    frame_id_t fid;
    EXPECT_TRUE(lru_->victim(&fid));
    EXPECT_EQ(fid, 1);
    EXPECT_TRUE(lru_->victim(&fid));
    EXPECT_EQ(fid, 2);
    EXPECT_TRUE(lru_->victim(&fid));
    EXPECT_EQ(fid, 3);
    EXPECT_FALSE(lru_->victim(&fid));
}

TEST_F(LRUOJ1Test, PinAfterUnpin) {
    lru_->unpin(5);
    lru_->unpin(6);
    lru_->pin(5);

    frame_id_t fid;
    EXPECT_TRUE(lru_->victim(&fid));
    EXPECT_EQ(fid, 6);
}

TEST_F(LRUOJ1Test, SizeTracking) {
    EXPECT_EQ(lru_->Size(), 0);
    lru_->unpin(1);
    EXPECT_EQ(lru_->Size(), 1);
    lru_->unpin(2);
    EXPECT_EQ(lru_->Size(), 2);
    frame_id_t fid;
    lru_->victim(&fid);
    EXPECT_EQ(lru_->Size(), 1);
}

// ============================================================
// Subtask 3: BufferPoolManager basic interface tests
// ============================================================
class BPMOJ1Test : public ::testing::Test {
   protected:
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<BufferPoolManager> bpm_;
    int fd_;

    void SetUp() override {
        dm_ = std::make_unique<DiskManager>();
        EnterTestDir(dm_.get());
        dm_->create_file("bpm_test.db");
        dm_->open_file("bpm_test.db");
        fd_ = dm_->get_file_fd("bpm_test.db");
        ASSERT_GE(fd_, 0);
        bpm_ = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, dm_.get());
    }

    void TearDown() override {
        bpm_.reset();
        dm_->close_file("bpm_test.db");
        dm_->destroy_file("bpm_test.db");
        LeaveTestDir(dm_.get());
    }
};

TEST_F(BPMOJ1Test, NewPageAndDeletePage) {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&page_id);
    ASSERT_NE(page, nullptr);
    EXPECT_NE(page_id.page_no, INVALID_PAGE_ID);
    bpm_->unpin_page(page_id, false);
    EXPECT_TRUE(bpm_->delete_page(page_id));
}

TEST_F(BPMOJ1Test, FetchPage) {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&page_id);
    ASSERT_NE(page, nullptr);
    bpm_->unpin_page(page_id, false);

    Page *fetched = bpm_->fetch_page(page_id);
    ASSERT_NE(fetched, nullptr);
    EXPECT_EQ(fetched->get_page_id().page_no, page_id.page_no);
    bpm_->unpin_page(page_id, false);
}

TEST_F(BPMOJ1Test, FlushPage) {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&page_id);
    ASSERT_NE(page, nullptr);
    const char *data = "Hello, BufferPool!";
    memcpy(page->get_data(), data, strlen(data) + 1);
    bpm_->unpin_page(page_id, true);
    EXPECT_TRUE(bpm_->flush_page(page_id));
}

TEST_F(BPMOJ1Test, DeletePinnedPageFails) {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *page = bpm_->new_page(&page_id);
    ASSERT_NE(page, nullptr);
    EXPECT_FALSE(bpm_->delete_page(page_id));
    bpm_->unpin_page(page_id, false);
    EXPECT_TRUE(bpm_->delete_page(page_id));
}

// ============================================================
// Subtask 4: RmFileHandle interface tests (Record layer)
// ============================================================
class RecordOJ1Test : public ::testing::Test {
   protected:
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<RmManager> rm_manager_;

    void SetUp() override {
        dm_ = std::make_unique<DiskManager>();
        EnterTestDir(dm_.get());
        bpm_ = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, dm_.get());
        rm_manager_ = std::make_unique<RmManager>(dm_.get(), bpm_.get());
    }

    void TearDown() override {
        rm_manager_.reset();
        bpm_.reset();
        LeaveTestDir(dm_.get());
    }
};

TEST_F(RecordOJ1Test, CreateAndOpenFileHandle) {
    int record_size = 4 + 16 + 4;
    rm_manager_->create_file("students", record_size);
    auto fh = rm_manager_->open_file("students");
    ASSERT_NE(fh, nullptr);

    RmFileHdr hdr = fh->get_file_hdr();
    EXPECT_EQ(hdr.num_pages, 1);
    EXPECT_EQ(hdr.record_size, record_size);

    rm_manager_->close_file(fh.get());
    rm_manager_->destroy_file("students");
}

TEST_F(RecordOJ1Test, InsertAndGetRecord) {
    int record_size = 4 + 16;
    rm_manager_->create_file("students", record_size);
    auto fh = rm_manager_->open_file("students");
    ASSERT_NE(fh, nullptr);

    char rec_buf[32] = {0};
    int id_val = 42;
    char name_val[16] = "Alice";
    memcpy(rec_buf, &id_val, 4);
    memcpy(rec_buf + 4, name_val, 16);

    Rid rid = fh->insert_record(rec_buf, nullptr);
    EXPECT_GE(rid.page_no, 0);
    EXPECT_GE(rid.slot_no, 0);

    auto record = fh->get_record(rid, nullptr);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(memcmp(record->data, rec_buf, record_size), 0);

    rm_manager_->close_file(fh.get());
    rm_manager_->destroy_file("students");
}

TEST_F(RecordOJ1Test, DeleteRecord) {
    int record_size = 4;
    rm_manager_->create_file("t", record_size);
    auto fh = rm_manager_->open_file("t");
    ASSERT_NE(fh, nullptr);

    int val = 1;
    char buf[8] = {0};
    memcpy(buf, &val, 4);

    Rid rid = fh->insert_record(buf, nullptr);
    EXPECT_TRUE(fh->is_record(rid));

    fh->delete_record(rid, nullptr);
    EXPECT_FALSE(fh->is_record(rid));

    rm_manager_->close_file(fh.get());
    rm_manager_->destroy_file("t");
}

TEST_F(RecordOJ1Test, UpdateRecord) {
    int record_size = 8;
    rm_manager_->create_file("t2", record_size);
    auto fh = rm_manager_->open_file("t2");
    ASSERT_NE(fh, nullptr);

    int val1 = 100;
    char buf[16] = {0};
    memcpy(buf, &val1, 4);

    Rid rid = fh->insert_record(buf, nullptr);

    int val2 = 200;
    char update_buf[16] = {0};
    memcpy(update_buf, &val2, 4);
    fh->update_record(rid, update_buf, nullptr);

    auto record = fh->get_record(rid, nullptr);
    ASSERT_NE(record, nullptr);
    int read_val;
    memcpy(&read_val, record->data, 4);
    EXPECT_EQ(read_val, 200);

    rm_manager_->close_file(fh.get());
    rm_manager_->destroy_file("t2");
}

TEST_F(RecordOJ1Test, MultipleRecordsScan) {
    int record_size = 4;
    rm_manager_->create_file("scan_test", record_size);
    auto fh = rm_manager_->open_file("scan_test");
    ASSERT_NE(fh, nullptr);

    std::vector<Rid> rids;
    for (int i = 0; i < 5; i++) {
        char buf[8];
        memset(buf, 0, 8);
        memcpy(buf, &i, 4);
        Rid rid = fh->insert_record(buf, nullptr);
        rids.push_back(rid);
    }

    for (int i = 0; i < 5; i++) {
        auto rec = fh->get_record(rids[i], nullptr);
        ASSERT_NE(rec, nullptr);
        int val;
        memcpy(&val, rec->data, 4);
        EXPECT_EQ(val, i);
    }

    rm_manager_->close_file(fh.get());
    rm_manager_->destroy_file("scan_test");
}

}  // namespace
