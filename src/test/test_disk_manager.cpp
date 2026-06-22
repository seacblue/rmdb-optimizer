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
 * @file test_disk_manager.cpp
 * @brief DiskManager 模块独立单元测试。
 *
 * 本文件是一个完全自包含的 Google Test 程序，
 * 不依赖其他模块测试文件，可独立编译运行。
 *
 * 编译 & 运行（在 build 目录下）：
 *   make test_dm -j$(nproc) && ./bin/test_dm
 *
 * 团队分工说明：
 * - DiskManager 由 Person A 负责实现
 * - 对应模块：src/storage/disk_manager.cpp
 * - 测试通过标准：全部 14 个测试用例 PASS
 *
 * !!! 注意事项：
 * - 测试会在当前目录创建 module_test_dir/ 临时目录
 * - 测试文件操作全部在此临时目录中进行
 * - 测试结束后自动清理临时目录
 * - 如果测试中途崩溃，手动 rm -rf module_test_dir 即可
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "storage/disk_manager.h"

// ============================================================
// 辅助函数（本文件私有，不依赖外部头文件）
// ============================================================

namespace {

const std::string TEST_DIR = "module_test_dir";

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

void RandBuf(int size, char *buf) {
    for (int i = 0; i < size; i++) {
        buf[i] = static_cast<char>(rand() & 0xff);
    }
}

}  // anonymous namespace

// ============================================================
// DiskManager 测试套件
// ============================================================

class DiskManagerTest : public ::testing::Test {
   protected:
    std::unique_ptr<DiskManager> dm_;

    void SetUp() override {
        srand(static_cast<unsigned>(time(nullptr)));
        dm_ = std::make_unique<DiskManager>();
        EnterTestDir(dm_.get());
    }

    void TearDown() override {
        LeaveTestDir(dm_.get());
    }
};

/**
 * @test 创建文件 → is_file 返回 true
 */
TEST_F(DiskManagerTest, CreateFile) {
    const std::string path = "test_create.db";
    EXPECT_FALSE(dm_->is_file(path));
    dm_->create_file(path);
    EXPECT_TRUE(dm_->is_file(path));
}

/**
 * @test 重复创建文件 → 抛 FileExistsError
 */
TEST_F(DiskManagerTest, CreateDuplicateFile) {
    const std::string path = "test_dup.db";
    dm_->create_file(path);
    EXPECT_THROW(dm_->create_file(path), FileExistsError);
}

/**
 * @test 打开文件 → 返回合法 fd
 *      重复打开同一文件 → 返回相同 fd（幂等）
 */
TEST_F(DiskManagerTest, OpenFile) {
    const std::string path = "test_open.db";
    dm_->create_file(path);

    int fd1 = dm_->open_file(path);
    EXPECT_GE(fd1, 0);

    int fd2 = dm_->open_file(path);
    EXPECT_EQ(fd1, fd2);
}

/**
 * @test 关闭文件 → 关闭后可以再次打开并操作
 */
TEST_F(DiskManagerTest, CloseFile) {
    const std::string path = "test_close.db";
    dm_->create_file(path);
    int fd = dm_->open_file(path);
    dm_->close_file(fd);

    int fd2 = dm_->open_file(path);
    EXPECT_GE(fd2, 0);
    dm_->close_file(fd2);
}

/**
 * @test 关闭未打开的文件 → 抛 FileNotOpenError
 */
TEST_F(DiskManagerTest, CloseNotOpenFile) {
    EXPECT_THROW(dm_->close_file(9999), FileNotOpenError);
}

/**
 * @test 写一个页面 → 读同一个页面 → 数据完全一致
 *  覆盖 read_page / write_page 基本路径
 */
TEST_F(DiskManagerTest, WriteReadPage) {
    const std::string path = "test_rw.db";
    dm_->create_file(path);
    int fd = dm_->open_file(path);

    char write_buf[PAGE_SIZE];
    char read_buf[PAGE_SIZE];
    RandBuf(PAGE_SIZE, write_buf);

    dm_->write_page(fd, 0, write_buf, PAGE_SIZE);
    dm_->read_page(fd, 0, read_buf, PAGE_SIZE);

    EXPECT_EQ(0, memcmp(write_buf, read_buf, PAGE_SIZE));

    dm_->close_file(fd);
}

/**
 * @test 多页面（16 页）写入再按页读出，逐页验证
 *  覆盖文件扩展、多个 page_no 的偏移计算
 */
TEST_F(DiskManagerTest, MultiplePages) {
    const std::string path = "test_mp.db";
    dm_->create_file(path);
    int fd = dm_->open_file(path);

    constexpr int NUM_PAGES = 16;
    char pages[NUM_PAGES][PAGE_SIZE];

    for (int i = 0; i < NUM_PAGES; i++) {
        memset(pages[i], static_cast<char>(i), PAGE_SIZE);
        dm_->write_page(fd, i, pages[i], PAGE_SIZE);
    }

    for (int i = 0; i < NUM_PAGES; i++) {
        char buf[PAGE_SIZE];
        dm_->read_page(fd, i, buf, PAGE_SIZE);
        EXPECT_EQ(0, memcmp(pages[i], buf, PAGE_SIZE));
    }

    dm_->close_file(fd);
}

/**
 * @test 分配页号 → 自增分配，每次返回 +1
 */
TEST_F(DiskManagerTest, AllocatePage) {
    const std::string path = "test_alloc.db";
    dm_->create_file(path);
    int fd = dm_->open_file(path);

    EXPECT_EQ(0, dm_->allocate_page(fd));
    EXPECT_EQ(1, dm_->allocate_page(fd));
    EXPECT_EQ(2, dm_->allocate_page(fd));

    dm_->close_file(fd);
}

/**
 * @test get_file_size
 *  空文件 = 0 字节，写入一页后 = PAGE_SIZE
 */
TEST_F(DiskManagerTest, FileSize) {
    const std::string path = "test_size.db";
    dm_->create_file(path);
    int fd = dm_->open_file(path);

    EXPECT_EQ(0, dm_->get_file_size(path));

    char buf[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    dm_->write_page(fd, 0, buf, PAGE_SIZE);

    EXPECT_EQ(PAGE_SIZE, dm_->get_file_size(path));

    dm_->close_file(fd);
}

/**
 * @test 删除文件 → is_file 返回 false
 */
TEST_F(DiskManagerTest, DestroyFile) {
    const std::string path = "test_destroy.db";
    dm_->create_file(path);
    int fd = dm_->open_file(path);
    dm_->close_file(fd);
    dm_->destroy_file(path);
    EXPECT_FALSE(dm_->is_file(path));
}

/**
 * @test 删除未关闭的文件 → 抛 FileNotClosedError
 *  防止数据丢失的安全检查
 */
TEST_F(DiskManagerTest, DestroyOpenFile) {
    const std::string path = "test_destroy_open.db";
    dm_->create_file(path);
    dm_->open_file(path);
    EXPECT_THROW(dm_->destroy_file(path), FileNotClosedError);
}

/**
 * @test 删除不存在的文件 → 抛 FileNotFoundError
 */
TEST_F(DiskManagerTest, DestroyNonExistent) {
    EXPECT_THROW(dm_->destroy_file("nonexistent.db"), FileNotFoundError);
}

/**
 * @test get_file_name / get_file_fd 双向映射
 */
TEST_F(DiskManagerTest, FileNameFdMapping) {
    const std::string path = "test_map.db";
    dm_->create_file(path);
    int fd = dm_->open_file(path);

    EXPECT_EQ(path, dm_->get_file_name(fd));
    EXPECT_EQ(fd, dm_->get_file_fd(path));

    dm_->close_file(fd);
}

// ============================================================
// 主函数
// ============================================================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    printf("===== DiskManager 模块测试 =====\n");
    printf("测试目录: %s\n", TEST_DIR.c_str());
    printf("PAGE_SIZE = %d\n", PAGE_SIZE);
    printf("===============================\n");
    return RUN_ALL_TESTS();
}
