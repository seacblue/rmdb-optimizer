/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#undef NDEBUG

#define private public

#include "record/rm.h"
#include "storage/buffer_pool_manager.h"

#undef private

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <thread>  // NOLINT
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"
#include "index/ix_manager.h"
#include "recovery/log_manager.h"
#include "replacer/lru_replacer.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"
#include "transaction/concurrency/lock_manager.h"
#include "transaction/transaction_manager.h"

const std::string TEST_DB_NAME = "BufferPoolManagerTest_db";  // 以数据库名作为根目录
const std::string TEST_FILE_NAME = "basic";                   // 测试文件的名字
const std::string TEST_FILE_NAME_CCUR = "concurrency";        // 测试文件的名字
const std::string TEST_FILE_NAME_BIG = "bigdata";             // 测试文件的名字
constexpr int MAX_FILES = 32;
constexpr int MAX_PAGES = 128;
constexpr size_t TEST_BUFFER_POOL_SIZE = MAX_FILES * MAX_PAGES;

// 创建BufferPoolManager
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(TEST_BUFFER_POOL_SIZE, disk_manager.get());

std::unordered_map<int, char *> mock;  // fd -> buffer

char *mock_get_page(int fd, int page_no) { return &mock[fd][page_no * PAGE_SIZE]; }

void check_disk(int fd, int page_no) {
    char buf[PAGE_SIZE];
    disk_manager->read_page(fd, page_no, buf, PAGE_SIZE);
    char *mock_buf = mock_get_page(fd, page_no);
    assert(memcmp(buf, mock_buf, PAGE_SIZE) == 0);
}

void check_disk_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
}

void check_cache(int fd, int page_no) {
    Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
    char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE];
    assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);
    buffer_pool_manager->unpin_page(PageId{fd, page_no}, false);
}

void check_cache_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_cache(fd, page_no);
        }
    }
}

void rand_buf(int size, char *buf) {
    for (int i = 0; i < size; i++) {
        int rand_ch = rand() & 0xff;
        buf[i] = rand_ch;
    }
}

int rand_fd() {
    assert(mock.size() == MAX_FILES);
    int fd_idx = rand() % MAX_FILES;
    auto it = mock.begin();
    for (int i = 0; i < fd_idx; i++) {
        it++;
    }
    return it->first;
}

struct rid_hash_t {
    size_t operator()(const Rid &rid) const { return (rid.page_no << 16) | rid.slot_no; }
};

struct rid_equal_t {
    bool operator()(const Rid &x, const Rid &y) const { return x.page_no == y.page_no && x.slot_no == y.slot_no; }
};

void check_equal(const RmFileHandle *file_handle,
                 const std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> &mock) {
    // Test all records
    for (auto &entry : mock) {
        Rid rid = entry.first;
        auto mock_buf = (char *)entry.second.c_str();
        auto rec = file_handle->get_record(rid, nullptr);
        assert(memcmp(mock_buf, rec->data, file_handle->file_hdr_.record_size) == 0);
    }
    // Randomly get record
    for (int i = 0; i < 10; i++) {
        Rid rid = {.page_no = 1 + rand() % (file_handle->file_hdr_.num_pages - 1),
                   .slot_no = rand() % file_handle->file_hdr_.num_records_per_page};
        bool mock_exist = mock.count(rid) > 0;
        bool rm_exist = file_handle->is_record(rid);
        assert(rm_exist == mock_exist);
    }
    // Test RM scan
    size_t num_records = 0;
    for (RmScan scan(file_handle); !scan.is_end(); scan.next()) {
        assert(mock.count(scan.rid()) > 0);
        auto rec = file_handle->get_record(scan.rid(), nullptr);
        assert(memcmp(rec->data, mock.at(scan.rid()).c_str(), file_handle->file_hdr_.record_size) == 0);
        num_records++;
    }
    assert(num_records == mock.size());
}

// std::cout can call this, for example: std::cout << rid
std::ostream &operator<<(std::ostream &os, const Rid &rid) {
    return os << '(' << rid.page_no << ", " << rid.slot_no << ')';
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_BIG，记录其文件描述符fd */

class BigStorageTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->get_file_fd的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_BIG)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_BIG);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_BIG);
        assert(disk_manager_->is_file(TEST_FILE_NAME_BIG));
        // 打开测试文件
        disk_manager_->open_file(TEST_FILE_NAME_BIG);
        fd_ = disk_manager_->get_file_fd(TEST_FILE_NAME_BIG);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(TEST_FILE_NAME_BIG);
        // disk_manager_->destroy_file(TEST_FILE_NAME_BIG);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST(LRUReplacerTest, SampleTest) {
    LRUReplacer lru_replacer(7);

    // Scenario: unpin six elements, i.e. add them to the replacer.
    lru_replacer.unpin(1);
    lru_replacer.unpin(2);
    lru_replacer.unpin(3);
    lru_replacer.unpin(4);
    lru_replacer.unpin(5);
    lru_replacer.unpin(6);
    lru_replacer.unpin(1);
    EXPECT_EQ(6, lru_replacer.Size());

    // Scenario: get three victims from the lru.
    int value;
    lru_replacer.victim(&value);
    EXPECT_EQ(1, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(2, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(3, value);

    // Scenario: pin elements in the replacer.
    // Note that 3 has already been victimized, so pinning 3 should have no effect.
    lru_replacer.pin(3);
    lru_replacer.pin(4);
    EXPECT_EQ(2, lru_replacer.Size());

    // Scenario: unpin 4. We expect that the reference bit of 4 will be set to 1.
    lru_replacer.unpin(4);

    // Scenario: continue looking for victims. We expect these victims.
    lru_replacer.victim(&value);
    EXPECT_EQ(5, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(6, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(4, value);
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME，记录其文件描述符fd */
class BufferPoolManagerTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->get_file_fd的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME)) {
            disk_manager_->destroy_file(TEST_FILE_NAME);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME);
        assert(disk_manager_->is_file(TEST_FILE_NAME));
        // 打开测试文件
        disk_manager_->open_file(TEST_FILE_NAME);
        fd_ = disk_manager_->get_file_fd(TEST_FILE_NAME);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(TEST_FILE_NAME);
        // disk_manager_->destroy_file(TEST_FILE_NAME);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

// NOLINTNEXTLINE
TEST_F(BufferPoolManagerTest, SampleTest) {
    // create BufferPoolManager
    const size_t buffer_pool_size = 10;
    auto disk_manager = BufferPoolManagerTest::disk_manager_.get();
    auto bpm = std::make_unique<BufferPoolManager>(buffer_pool_size, disk_manager);
    // create tmp PageId
    int fd = BufferPoolManagerTest::fd_;
    PageId page_id_temp = {.fd = fd, .page_no = INVALID_PAGE_ID};
    auto *page0 = bpm->new_page(&page_id_temp);

    // Scenario: The buffer pool is empty. We should be able to create a new page.
    ASSERT_NE(nullptr, page0);
    EXPECT_EQ(0, page_id_temp.page_no);

    // Scenario: Once we have a page, we should be able to read and write content.
    snprintf(page0->get_data(), sizeof(page0->get_data()), "Hello");
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));

    // Scenario: We should be able to create new pages until we fill up the buffer pool.
    for (size_t i = 1; i < buffer_pool_size; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: Once the buffer pool is full, we should not be able to create any new pages.
    for (size_t i = buffer_pool_size; i < buffer_pool_size * 2; ++i) {
        EXPECT_EQ(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: After unpinning pages {0, 1, 2, 3, 4} and pinning another 4 new pages,
    // there would still be one cache frame left for reading page 0.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(true, bpm->unpin_page(PageId{fd, i}, true));
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: We should be able to fetch the data we wrote a while ago.
    page0 = bpm->fetch_page(PageId{fd, 0});
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));
    EXPECT_EQ(true, bpm->unpin_page(PageId{fd, 0}, true));
    // new_page again, and now all buffers are pinned. Page 0 would be failed to fetch.
    EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    EXPECT_EQ(nullptr, bpm->fetch_page(PageId{fd, 0}));

    bpm->flush_all_pages(fd);
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_CCUR，记录其文件描述符fd */

// Add by jiawen
class BufferPoolManagerConcurrencyTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->get_file_fd的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_CCUR)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_CCUR);
        assert(disk_manager_->is_file(TEST_FILE_NAME_CCUR));
        // 打开测试文件
        disk_manager_->open_file(TEST_FILE_NAME_CCUR);
        fd_ = disk_manager_->get_file_fd(TEST_FILE_NAME_CCUR);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(TEST_FILE_NAME_CCUR);
        // disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST_F(BufferPoolManagerConcurrencyTest, ConcurrencyTest) {
    const int num_threads = 5;
    const int num_runs = 50;

    // get fd
    int fd = BufferPoolManagerConcurrencyTest::fd_;

    for (int run = 0; run < num_runs; run++) {
        // create BufferPoolManager
        auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
        std::shared_ptr<BufferPoolManager> bpm{new BufferPoolManager(50, disk_manager)};

        std::vector<std::thread> threads;
        for (int tid = 0; tid < num_threads; tid++) {
            threads.push_back(std::thread([&bpm, fd]() {  // NOLINT
                PageId temp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
                std::vector<PageId> page_ids;
                for (int i = 0; i < 10; i++) {
                    auto new_page = bpm->new_page(&temp_page_id);
                    EXPECT_NE(nullptr, new_page);
                    ASSERT_NE(nullptr, new_page);
                    strcpy(new_page->get_data(), std::to_string(temp_page_id.page_no).c_str());  // NOLINT
                    page_ids.push_back(temp_page_id);
                }
                for (int i = 0; i < 10; i++) {
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[i], true));
                }
                for (int j = 0; j < 10; j++) {
                    auto page = bpm->fetch_page(page_ids[j]);
                    EXPECT_NE(nullptr, page);
                    ASSERT_NE(nullptr, page);
                    EXPECT_EQ(0, std::strcmp(std::to_string(page_ids[j].page_no).c_str(), (page->get_data())));
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[j], true));
                }
                for (int j = 0; j < 10; j++) {
                    EXPECT_EQ(1, bpm->delete_page(page_ids[j]));
                }
                bpm->flush_all_pages(fd);  // add this test by jiawen
            }));
        }  // end loop tid=[0,num_threads)

        for (int i = 0; i < num_threads; i++) {
            threads[i].join();
        }
    }  // end loop run=[0,num_runs)
}

// TODO: fix detected memory leaks found by Google Test
const std::string STORAGE_TEST_DB_NAME = "StorageTest_db";

TEST(StorageTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    // Create and enter test directory (like other tests do)
    if (!disk_manager->is_dir(STORAGE_TEST_DB_NAME)) {
        disk_manager->create_dir(STORAGE_TEST_DB_NAME);
    }
    ASSERT_TRUE(disk_manager->is_dir(STORAGE_TEST_DB_NAME));
    if (chdir(STORAGE_TEST_DB_NAME.c_str()) < 0) {
        throw UnixError();
    }

    /** Test disk_manager */
    std::vector<std::string> filenames(MAX_FILES);  // MAX_FILES=32
    std::unordered_map<int, std::string> fd2name;
    for (size_t i = 0; i < filenames.size(); i++) {
        auto &filename = filenames[i];
        filename = std::to_string(i) + ".txt";
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // open without create
        try {
            disk_manager->open_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }

        disk_manager->create_file(filename);
        assert(disk_manager->is_file(filename));
        try {
            disk_manager->create_file(filename);
            assert(false);
        } catch (const FileExistsError &e) {
        }

        // open file
        disk_manager->open_file(filename);
        int fd = disk_manager->get_file_fd(filename);
        char *tmp = new char[PAGE_SIZE * MAX_PAGES];  // TODO: fix error in detected memory leaks

        mock[fd] = tmp;
        fd2name[fd] = filename;

        disk_manager->set_fd2pageno(fd, 0);  // diskmanager在fd对应的文件中从0开始分配page_no
    }

    /** Test buffer_pool_manager*/
    int num_pages = 0;
    char init_buf[PAGE_SIZE];
    for (auto &fh : mock) {
        int fd = fh.first;
        for (page_id_t i = 0; i < MAX_PAGES; i++) {
            rand_buf(PAGE_SIZE, init_buf);  // 将init_buf填充PAGE_SIZE个字节的随机数据

            PageId tmp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
            Page *page = buffer_pool_manager->new_page(&tmp_page_id);
            int page_no = tmp_page_id.page_no;
            assert(page_no != INVALID_PAGE_ID);
            assert(page_no == i);

            memcpy(page->get_data(), init_buf, PAGE_SIZE);
            buffer_pool_manager->unpin_page(PageId{fd, page_no}, true);

            char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE]
            memcpy(mock_buf, init_buf, PAGE_SIZE);

            num_pages++;

            check_cache(fd, page_no);  // 调用了fetch_page, unpin_page
        }
    }
    check_cache_all();

    assert(num_pages == TEST_BUFFER_POOL_SIZE);

    /** Test flush_all_pages() */
    // Flush and test disk
    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    for (int r = 0; r < 10000; r++) {
        int fd = rand_fd();
        int page_no = rand() % MAX_PAGES;
        // fetch page
        Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
        char *mock_buf = mock_get_page(fd, page_no);
        assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);

        // modify
        rand_buf(PAGE_SIZE, init_buf);
        memcpy(page->get_data(), init_buf, PAGE_SIZE);
        memcpy(mock_buf, init_buf, PAGE_SIZE);

        buffer_pool_manager->unpin_page(page->get_page_id(), true);
        // BufferPool::mark_dirty(page);

        // flush
        if (rand() % 10 == 0) {
            buffer_pool_manager->flush_page(page->get_page_id());
            check_disk(fd, page_no);
        }
        // flush entire file
        if (rand() % 100 == 0) {
            buffer_pool_manager->flush_all_pages(fd);
        }
        // re-open file
        if (rand() % 100 == 0) {
            auto filename = fd2name[fd];
            disk_manager->close_file(filename);
            char *buf = mock[fd];
            fd2name.erase(fd);
            mock.erase(fd);
            disk_manager->open_file(filename);
            int new_fd = disk_manager->get_file_fd(filename);
            mock[new_fd] = buf;
            fd2name[new_fd] = filename;
        }
        // assert equal in cache
        check_cache(fd, page_no);
    }
    check_cache_all();

    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    // close and destroy files
    for (auto &entry : fd2name) {
        auto &filename = entry.second;
        disk_manager->close_file(filename);
        disk_manager->destroy_file(filename);
        try {
            disk_manager->destroy_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }
    }

    // Return to parent directory
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

TEST(RecordManagerTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    // 创建RmManager类的对象rm_manager
    auto disk_manager = std::make_unique<DiskManager>();
    auto buffer_pool_manager = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
    auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());

    std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> mock;

    std::string filename = "abc.txt";

    int record_size = 4 + rand() % 256;  // 元组大小随便设置，只要不超过RM_MAX_RECORD_SIZE
    // test files
    {
        // 删除残留的同名文件
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // 将file header写入到磁盘中的filename文件
        rm_manager->create_file(filename, record_size);
        // 将磁盘中的filename文件读出到内存中的file handle的file header
        std::unique_ptr<RmFileHandle> file_handle = rm_manager->open_file(filename);
        // 检查filename文件在内存中的file header的参数
        assert(file_handle->file_hdr_.record_size == record_size);
        assert(file_handle->file_hdr_.first_free_page_no == RM_NO_PAGE);
        assert(file_handle->file_hdr_.num_pages == 1);

        int max_bytes = file_handle->file_hdr_.record_size * file_handle->file_hdr_.num_records_per_page +
                        file_handle->file_hdr_.bitmap_size + (int)sizeof(RmPageHdr);
        assert(max_bytes <= PAGE_SIZE);
        int rand_val = rand();
        file_handle->file_hdr_.num_pages = rand_val;
        rm_manager->close_file(file_handle.get());

        // reopen file
        file_handle = rm_manager->open_file(filename);
        assert(file_handle->file_hdr_.num_pages == rand_val);
        rm_manager->close_file(file_handle.get());
        rm_manager->destroy_file(filename);
    }
    // test pages
    rm_manager->create_file(filename, record_size);
    auto file_handle = rm_manager->open_file(filename);

    char write_buf[PAGE_SIZE];
    size_t add_cnt = 0;
    size_t upd_cnt = 0;
    size_t del_cnt = 0;
    for (int round = 0; round < 1000; round++) {
        double insert_prob = 1. - mock.size() / 250.;
        double dice = rand() * 1. / RAND_MAX;
        if (mock.empty() || dice < insert_prob) {
            rand_buf(file_handle->file_hdr_.record_size, write_buf);
            Rid rid = file_handle->insert_record(write_buf, nullptr);
            mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
            add_cnt++;
            //            std::cout << "insert " << rid << '\n'; // operator<<(cout,rid)
        } else {
            // update or erase random rid
            int rid_idx = rand() % mock.size();
            auto it = mock.begin();
            for (int i = 0; i < rid_idx; i++) {
                it++;
            }
            auto rid = it->first;
            if (rand() % 2 == 0) {
                // update
                rand_buf(file_handle->file_hdr_.record_size, write_buf);
                file_handle->update_record(rid, write_buf, nullptr);
                mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
                upd_cnt++;
                //                std::cout << "update " << rid << '\n';
            } else {
                // erase
                file_handle->delete_record(rid, nullptr);
                mock.erase(rid);
                del_cnt++;
                //                std::cout << "delete " << rid << '\n';
            }
        }
        // Randomly re-open file
        if (round % 50 == 0) {
            rm_manager->close_file(file_handle.get());
            file_handle = rm_manager->open_file(filename);
        }
        check_equal(file_handle.get(), mock);
    }
    assert(mock.size() == add_cnt - del_cnt);
    std::cout << "insert " << add_cnt << '\n' << "delete " << del_cnt << '\n' << "update " << upd_cnt << '\n';
    // clean up
    rm_manager->close_file(file_handle.get());
    rm_manager->destroy_file(filename);
}

const std::string TEST_TXN_DB_NAME = "TxnModuleTest_db";
const std::string TEST_TXN_FILE_NAME = "txn.tbl";

class TxnModuleTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<RmManager> rm_manager_;
    std::unique_ptr<IxManager> ix_manager_;
    std::unique_ptr<SmManager> sm_manager_;
    std::unique_ptr<LogManager> log_manager_;
    std::unique_ptr<LockManager> lock_manager_;
    std::unique_ptr<TransactionManager> txn_manager_;
    RmFileHandle *file_handle_ = nullptr;

    void SetUp() override {
        ::testing::Test::SetUp();
        disk_manager_ = std::make_unique<DiskManager>();
        buffer_pool_manager_ = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager_.get());
        rm_manager_ = std::make_unique<RmManager>(disk_manager_.get(), buffer_pool_manager_.get());
        ix_manager_ = std::make_unique<IxManager>(disk_manager_.get(), buffer_pool_manager_.get());
        sm_manager_ = std::make_unique<SmManager>(
            disk_manager_.get(), buffer_pool_manager_.get(), rm_manager_.get(), ix_manager_.get());
        log_manager_ = std::make_unique<LogManager>(disk_manager_.get());
        lock_manager_ = std::make_unique<LockManager>();
        txn_manager_ = std::make_unique<TransactionManager>(lock_manager_.get(), sm_manager_.get());

        if (!disk_manager_->is_dir(TEST_TXN_DB_NAME)) {
            disk_manager_->create_dir(TEST_TXN_DB_NAME);
        }
        ASSERT_EQ(0, chdir(TEST_TXN_DB_NAME.c_str()));

        if (disk_manager_->is_file(TEST_TXN_FILE_NAME)) {
            disk_manager_->destroy_file(TEST_TXN_FILE_NAME);
        }
        if (disk_manager_->is_file(LOG_FILE_NAME)) {
            disk_manager_->destroy_file(LOG_FILE_NAME);
        }

        rm_manager_->create_file(TEST_TXN_FILE_NAME, sizeof(int));
        sm_manager_->fhs_[TEST_TXN_FILE_NAME] = rm_manager_->open_file(TEST_TXN_FILE_NAME);
        file_handle_ = sm_manager_->fhs_[TEST_TXN_FILE_NAME].get();
        disk_manager_->create_file(LOG_FILE_NAME);
    }

    void TearDown() override {
        if (disk_manager_->GetLogFd() != -1) {
            disk_manager_->close_file(LOG_FILE_NAME);
            disk_manager_->SetLogFd(-1);
        }
        if (file_handle_ != nullptr) {
            rm_manager_->close_file(file_handle_);
        }
        sm_manager_->fhs_.clear();
        if (disk_manager_->is_file(TEST_TXN_FILE_NAME)) {
            disk_manager_->destroy_file(TEST_TXN_FILE_NAME);
        }
        if (disk_manager_->is_file(LOG_FILE_NAME)) {
            disk_manager_->destroy_file(LOG_FILE_NAME);
        }
        ASSERT_EQ(0, chdir(".."));
    }
};

TEST_F(TxnModuleTest, LogManagerFlushRoundTrip) {
    BeginLogRecord begin_log(7);
    EXPECT_EQ(0, log_manager_->add_log_to_buffer(&begin_log));

    int old_value_int = 11;
    int new_value_int = 19;
    RmRecord old_value(sizeof(int), reinterpret_cast<char *>(&old_value_int));
    RmRecord new_value(sizeof(int), reinterpret_cast<char *>(&new_value_int));
    Rid rid{3, 4};

    InsertLogRecord insert_log(7, old_value, rid, TEST_TXN_FILE_NAME);
    DeleteLogRecord delete_log(7, old_value, rid, TEST_TXN_FILE_NAME);
    UpdateLogRecord update_log(7, old_value, new_value, rid, TEST_TXN_FILE_NAME);
    CommitLogRecord commit_log(7);

    EXPECT_EQ(1, log_manager_->add_log_to_buffer(&insert_log));
    EXPECT_EQ(2, log_manager_->add_log_to_buffer(&delete_log));
    EXPECT_EQ(3, log_manager_->add_log_to_buffer(&update_log));
    EXPECT_EQ(4, log_manager_->add_log_to_buffer(&commit_log));
    log_manager_->flush_log_to_disk();

    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    ASSERT_GT(file_size, 0);

    std::vector<char> raw(file_size);
    ASSERT_EQ(file_size, disk_manager_->read_log(raw.data(), file_size, 0));

    BeginLogRecord begin_log_copy;
    begin_log_copy.deserialize(raw.data());
    EXPECT_EQ(LogType::begin, begin_log_copy.log_type_);
    EXPECT_EQ(7, begin_log_copy.log_tid_);

    InsertLogRecord insert_log_copy;
    insert_log_copy.deserialize(raw.data() + begin_log_copy.log_tot_len_);
    EXPECT_EQ(rid.page_no, insert_log_copy.rid_.page_no);
    EXPECT_EQ(rid.slot_no, insert_log_copy.rid_.slot_no);
    EXPECT_EQ(0, std::memcmp(old_value.data, insert_log_copy.insert_value_.data, sizeof(int)));
    EXPECT_EQ(TEST_TXN_FILE_NAME,
              std::string(insert_log_copy.table_name_, insert_log_copy.table_name_size_));

    DeleteLogRecord delete_log_copy;
    delete_log_copy.deserialize(raw.data() + begin_log_copy.log_tot_len_ + insert_log_copy.log_tot_len_);
    EXPECT_EQ(rid.page_no, delete_log_copy.rid_.page_no);
    EXPECT_EQ(0, std::memcmp(old_value.data, delete_log_copy.delete_value_.data, sizeof(int)));

    UpdateLogRecord update_log_copy;
    update_log_copy.deserialize(raw.data() + begin_log_copy.log_tot_len_ + insert_log_copy.log_tot_len_ +
                                delete_log_copy.log_tot_len_);
    EXPECT_EQ(rid.slot_no, update_log_copy.rid_.slot_no);
    EXPECT_EQ(0, std::memcmp(old_value.data, update_log_copy.old_value_.data, sizeof(int)));
    EXPECT_EQ(0, std::memcmp(new_value.data, update_log_copy.new_value_.data, sizeof(int)));

    CommitLogRecord commit_log_copy;
    commit_log_copy.deserialize(raw.data() + begin_log_copy.log_tot_len_ + insert_log_copy.log_tot_len_ +
                                delete_log_copy.log_tot_len_ + update_log_copy.log_tot_len_);
    EXPECT_EQ(LogType::commit, commit_log_copy.log_type_);
    EXPECT_EQ(7, commit_log_copy.log_tid_);
}

TEST_F(TxnModuleTest, LockManagerCompatibilityAndShrinkPhase) {
    Transaction txn1(1);
    Transaction txn2(2);
    Transaction txn3(3);
    Rid rid{1, 0};

    EXPECT_TRUE(lock_manager_->lock_shared_on_table(&txn1, 100));
    EXPECT_TRUE(lock_manager_->lock_shared_on_table(&txn2, 100));

    try {
        lock_manager_->lock_exclusive_on_table(&txn1, 100);
        FAIL();
    } catch (const TransactionAbortException &e) {
        EXPECT_EQ(AbortReason::UPGRADE_CONFLICT, e.GetAbortReason());
    }

    EXPECT_TRUE(lock_manager_->unlock(&txn1, LockDataId(100, LockDataType::TABLE)));
    EXPECT_EQ(TransactionState::SHRINKING, txn1.get_state());

    try {
        lock_manager_->lock_shared_on_record(&txn1, rid, 100);
        FAIL();
    } catch (const TransactionAbortException &e) {
        EXPECT_EQ(AbortReason::LOCK_ON_SHIRINKING, e.GetAbortReason());
    }

    EXPECT_TRUE(lock_manager_->lock_shared_on_record(&txn3, rid, 100));
    EXPECT_EQ(2U, txn3.get_lock_set()->size());
    EXPECT_EQ(1U, txn3.get_lock_set()->count(LockDataId(100, LockDataType::TABLE)));
    EXPECT_EQ(1U, txn3.get_lock_set()->count(LockDataId(100, rid, LockDataType::RECORD)));
}

TEST_F(TxnModuleTest, TransactionCommitPersistsInsert) {
    Transaction *txn = txn_manager_->begin(nullptr, log_manager_.get());
    Context context(lock_manager_.get(), log_manager_.get(), txn);
    int value = 42;

    Rid rid = file_handle_->insert_record(reinterpret_cast<char *>(&value), &context);
    txn_manager_->commit(txn, log_manager_.get());

    EXPECT_EQ(TransactionState::COMMITTED, txn->get_state());
    EXPECT_TRUE(file_handle_->is_record(rid));
    auto record = file_handle_->get_record(rid, nullptr);
    EXPECT_EQ(value, *reinterpret_cast<int *>(record->data));
    EXPECT_TRUE(txn->get_write_set()->empty());
    EXPECT_EQ(0U, TransactionManager::txn_map.count(txn->get_transaction_id()));
    delete txn;
}

TEST_F(TxnModuleTest, TransactionAbortUndoesUpdateAndDelete) {
    int original_value = 7;
    int deleted_value = 9;
    Rid update_rid = file_handle_->insert_record(reinterpret_cast<char *>(&original_value), nullptr);
    Rid delete_rid = file_handle_->insert_record(reinterpret_cast<char *>(&deleted_value), nullptr);

    Transaction *txn = txn_manager_->begin(nullptr, log_manager_.get());
    Context context(lock_manager_.get(), log_manager_.get(), txn);
    int updated_value = 21;

    file_handle_->update_record(update_rid, reinterpret_cast<char *>(&updated_value), &context);
    file_handle_->delete_record(delete_rid, &context);
    txn_manager_->abort(txn, log_manager_.get());

    auto update_record = file_handle_->get_record(update_rid, nullptr);
    EXPECT_EQ(original_value, *reinterpret_cast<int *>(update_record->data));
    EXPECT_TRUE(file_handle_->is_record(delete_rid));
    auto restored_record = file_handle_->get_record(delete_rid, nullptr);
    EXPECT_EQ(deleted_value, *reinterpret_cast<int *>(restored_record->data));
    EXPECT_TRUE(txn->get_write_set()->empty());
    EXPECT_EQ(TransactionState::ABORTED, txn->get_state());
    delete txn;
}

TEST(IndexNodeTest, LeafAndInternalOperations) {
    IxFileHdr file_hdr(IX_NO_PAGE, 0, IX_INIT_ROOT_PAGE, 1, sizeof(int), 4, (4 + 1) * sizeof(int), 0, 0);
    file_hdr.col_types_.push_back(TYPE_INT);
    file_hdr.col_lens_.push_back(sizeof(int));

    Page leaf_page;
    auto *leaf_hdr = reinterpret_cast<IxPageHdr *>(leaf_page.get_data());
    leaf_hdr->parent = IX_NO_PAGE;
    leaf_hdr->num_key = 0;
    leaf_hdr->is_leaf = true;
    leaf_hdr->prev_leaf = IX_NO_PAGE;
    leaf_hdr->next_leaf = IX_NO_PAGE;
    IxNodeHandle leaf(&file_hdr, &leaf_page);

    int key10 = 10;
    int key20 = 20;
    int key30 = 30;
    leaf.insert(reinterpret_cast<char *>(&key20), Rid{2, 0});
    leaf.insert(reinterpret_cast<char *>(&key10), Rid{1, 0});
    leaf.insert(reinterpret_cast<char *>(&key30), Rid{3, 0});

    EXPECT_EQ(0, leaf.lower_bound(reinterpret_cast<char *>(&key10)));
    EXPECT_EQ(1, leaf.lower_bound(reinterpret_cast<char *>(&key20)));
    EXPECT_EQ(2, leaf.upper_bound(reinterpret_cast<char *>(&key20)));

    Rid *rid = nullptr;
    ASSERT_TRUE(leaf.leaf_lookup(reinterpret_cast<char *>(&key20), &rid));
    EXPECT_EQ(2, rid->page_no);

    leaf.remove(reinterpret_cast<char *>(&key20));
    EXPECT_EQ(2, leaf.get_size());
    EXPECT_EQ(1, leaf.upper_bound(reinterpret_cast<char *>(&key10)));

    Page internal_page;
    auto *internal_hdr = reinterpret_cast<IxPageHdr *>(internal_page.get_data());
    internal_hdr->parent = IX_NO_PAGE;
    internal_hdr->num_key = 0;
    internal_hdr->is_leaf = false;
    internal_hdr->prev_leaf = IX_NO_PAGE;
    internal_hdr->next_leaf = IX_NO_PAGE;
    IxNodeHandle internal(&file_hdr, &internal_page);
    internal.insert_pair(0, reinterpret_cast<char *>(&key10), Rid{100, 0});
    internal.insert_pair(1, reinterpret_cast<char *>(&key20), Rid{200, 0});
    internal.insert_pair(2, reinterpret_cast<char *>(&key30), Rid{300, 0});

    int probe15 = 15;
    int probe25 = 25;
    EXPECT_EQ(100, internal.internal_lookup(reinterpret_cast<char *>(&probe15)));
    EXPECT_EQ(200, internal.internal_lookup(reinterpret_cast<char *>(&probe25)));
}

const std::string TEST_INDEX_DB_NAME = "IndexModuleTest_db";
const std::string TEST_INDEX_BASE_NAME = "orders";

class IndexModuleTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<IxManager> ix_manager_;
    std::unique_ptr<IxIndexHandle> ih_;
    std::vector<ColMeta> index_cols_;

    void SetUp() override {
        ::testing::Test::SetUp();
        disk_manager_ = std::make_unique<DiskManager>();
        buffer_pool_manager_ = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager_.get());
        ix_manager_ = std::make_unique<IxManager>(disk_manager_.get(), buffer_pool_manager_.get());
        index_cols_.push_back({"orders", "id", TYPE_INT, sizeof(int), 0, true});

        if (!disk_manager_->is_dir(TEST_INDEX_DB_NAME)) {
            disk_manager_->create_dir(TEST_INDEX_DB_NAME);
        }
        ASSERT_EQ(0, chdir(TEST_INDEX_DB_NAME.c_str()));

        if (ix_manager_->exists(TEST_INDEX_BASE_NAME, index_cols_)) {
            ix_manager_->destroy_index(TEST_INDEX_BASE_NAME, index_cols_);
        }
        ix_manager_->create_index(TEST_INDEX_BASE_NAME, index_cols_);
        ih_ = ix_manager_->open_index(TEST_INDEX_BASE_NAME, index_cols_);
    }

    void TearDown() override {
        if (ih_ != nullptr) {
            ix_manager_->close_index(ih_.get());
            ih_.reset();
        }
        if (ix_manager_->exists(TEST_INDEX_BASE_NAME, index_cols_)) {
            ix_manager_->destroy_index(TEST_INDEX_BASE_NAME, index_cols_);
        }
        ASSERT_EQ(0, chdir(".."));
    }
};

TEST_F(IndexModuleTest, InsertLookupScanAndDelete) {
    std::vector<int> keys(200);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(123);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int key : keys) {
        Rid rid{key, key + 1};
        ih_->insert_entry(reinterpret_cast<char *>(&key), rid, nullptr);
    }

    for (int key = 0; key < 200; ++key) {
        std::vector<Rid> result;
        ASSERT_TRUE(ih_->get_value(reinterpret_cast<char *>(&key), &result, nullptr));
        ASSERT_EQ(1U, result.size());
        EXPECT_EQ(key, result[0].page_no);
        EXPECT_EQ(key + 1, result[0].slot_no);
    }

    int lower_key = 40;
    int upper_key = 60;
    IxScan scan(ih_.get(), ih_->lower_bound(reinterpret_cast<char *>(&lower_key)),
                ih_->upper_bound(reinterpret_cast<char *>(&upper_key)), buffer_pool_manager_.get());
    int expected = lower_key;
    for (; !scan.is_end(); scan.next()) {
        Rid rid = scan.rid();
        EXPECT_EQ(expected, rid.page_no);
        EXPECT_EQ(expected + 1, rid.slot_no);
        expected++;
    }
    EXPECT_EQ(upper_key + 1, expected);

    for (int key = 0; key < 200; key += 2) {
        EXPECT_TRUE(ih_->delete_entry(reinterpret_cast<char *>(&key), nullptr));
    }

    for (int key = 0; key < 200; ++key) {
        std::vector<Rid> result;
        bool found = ih_->get_value(reinterpret_cast<char *>(&key), &result, nullptr);
        if (key % 2 == 0) {
            EXPECT_FALSE(found);
        } else {
            ASSERT_TRUE(found);
            EXPECT_EQ(key, result[0].page_no);
        }
    }
}
