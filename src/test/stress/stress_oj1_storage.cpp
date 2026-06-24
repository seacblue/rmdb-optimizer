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
 * @file stress_oj1_storage.cpp
 * @brief 存储模块压力测试 — DiskManager / BufferPoolManager / LRUReplacer
 *
 * 测试大量文件操作、海量页面读写、缓冲区并发压力。
 * 每次测试报告执行时间。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "replacer/lru_replacer.h"
#include "storage/page.h"
#include "common/config.h"

namespace {

const std::string STRESS_DIR = "stress_storage_dir";

class StressStorageTest : public ::testing::Test {
   protected:
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<BufferPoolManager> bpm_;

    void SetUp() override {
        dm_ = std::make_unique<DiskManager>();
        if (!dm_->is_dir(STRESS_DIR)) {
            dm_->create_dir(STRESS_DIR);
        }
        ASSERT_TRUE(dm_->is_dir(STRESS_DIR));
        ASSERT_EQ(chdir(STRESS_DIR.c_str()), 0);
        bpm_ = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, dm_.get());
    }

    void TearDown() override {
        bpm_.reset();
        ASSERT_EQ(chdir(".."), 0);
        if (dm_->is_dir(STRESS_DIR)) {
            dm_->destroy_dir(STRESS_DIR);
        }
        dm_.reset();
    }
};

TEST_F(StressStorageTest, DISK_1000FilesWriteRead) {
    const int NUM_FILES = 200;
    const int PAGES_PER_FILE = 50;
    std::vector<std::string> filenames;
    std::vector<int> fds;

    auto start = std::chrono::steady_clock::now();

    // 创建并打开 200 个文件
    for (int i = 0; i < NUM_FILES; i++) {
        std::string fname = "stress_file_" + std::to_string(i) + ".db";
        dm_->create_file(fname);
        dm_->open_file(fname);
        filenames.push_back(fname);
        fds.push_back(dm_->get_file_fd(fname));
    }

    // 每个文件写 50 页
    for (int f = 0; f < NUM_FILES; f++) {
        for (int p = 0; p < PAGES_PER_FILE; p++) {
            char buf[PAGE_SIZE];
            memset(buf, 'A' + (f % 26), PAGE_SIZE);
            dm_->write_page(fds[f], p, buf, PAGE_SIZE);
        }
    }

    // 验证所有数据
    for (int f = 0; f < NUM_FILES; f++) {
        for (int p = 0; p < PAGES_PER_FILE; p++) {
            char read_buf[PAGE_SIZE];
            char expect_buf[PAGE_SIZE];
            memset(read_buf, 0, PAGE_SIZE);
            memset(expect_buf, 'A' + (f % 26), PAGE_SIZE);
            dm_->read_page(fds[f], p, read_buf, PAGE_SIZE);
            EXPECT_EQ(memcmp(read_buf, expect_buf, PAGE_SIZE), 0);
        }
    }

    // 关闭并清理
    for (int i = 0; i < NUM_FILES; i++) {
        dm_->close_file(filenames[i]);
        dm_->destroy_file(filenames[i]);
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] DISK_1000FilesWriteRead: " << NUM_FILES << " files x "
              << PAGES_PER_FILE << " pages = " << (NUM_FILES * PAGES_PER_FILE)
              << " pages in " << ms << " ms (" << (ms / (NUM_FILES * PAGES_PER_FILE)) << " ms/page)" << std::endl;
}

TEST_F(StressStorageTest, BPM_AllocateAndPinUnpin) {
    const int NUM_PAGES = 5000;
    std::vector<PageId> pids;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < NUM_PAGES; i++) {
        PageId pid{0, i};
        Page *page = bpm_->new_page(&pid);
        ASSERT_NE(page, nullptr);
        pids.push_back(pid);
        // Pin count is 1 after new_page
    }

    // Fetch and unpin all
    for (auto &pid : pids) {
        Page *page = bpm_->fetch_page(pid);
        ASSERT_NE(page, nullptr);
        bpm_->unpin_page(pid, false);
    }

    // Unpin the initial pin from new_page
    for (auto &pid : pids) {
        bpm_->unpin_page(pid, false);
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] BPM_AllocateAndPinUnpin: " << NUM_PAGES << " pages in "
              << ms << " ms (" << (ms * 1000 / NUM_PAGES) << " us/page)" << std::endl;
}

TEST_F(StressStorageTest, BPM_ConcurrentPageAccess) {
    const int NUM_THREADS = 8;
    const int PAGES_PER_THREAD = 200;
    std::vector<PageId> pids;

    // 预分配页面
    for (int i = 0; i < NUM_THREADS * PAGES_PER_THREAD; i++) {
        PageId pid{0, i};
        Page *page = bpm_->new_page(&pid);
        ASSERT_NE(page, nullptr);
        pids.push_back(pid);
        bpm_->unpin_page(pid, false);
    }

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([this, t, PAGES_PER_THREAD]() {
            int base = t * PAGES_PER_THREAD;
            for (int i = 0; i < PAGES_PER_THREAD; i++) {
                PageId pid{0, base + i};
                Page *page = bpm_->fetch_page(pid);
                if (page) {
                    bpm_->unpin_page(pid, false);
                }
            }
        });
    }

    for (auto &th : threads) {
        th.join();
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    int total_ops = NUM_THREADS * PAGES_PER_THREAD;
    std::cout << "\n  [STRESS] BPM_ConcurrentPageAccess: " << NUM_THREADS << " threads x "
              << PAGES_PER_THREAD << " pages = " << total_ops << " ops in "
              << ms << " ms (" << (ms * 1000 / total_ops) << " us/op)" << std::endl;
}

TEST_F(StressStorageTest, LRU_10000VictimCalls) {
    const int FRAMES = 1000;
    auto lru = std::make_unique<LRUReplacer>(FRAMES);

    auto start = std::chrono::steady_clock::now();

    // Unpin 10000 times
    for (int i = 0; i < 10000; i++) {
        lru->unpin(i % FRAMES);
    }

    // Victim 10000 times
    for (int i = 0; i < 10000; i++) {
        frame_id_t fid;
        lru->victim(&fid);
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\n  [STRESS] LRU_10000VictimCalls: 20000 ops in "
              << ms << " ms (" << (ms * 1000 / 20000) << " us/op)" << std::endl;
}
}
