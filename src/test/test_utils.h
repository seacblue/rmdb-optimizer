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
 * @file test_utils.h
 * @brief 模块测试的共享工具函数。
 *
 * 各模块测试文件（test_lru_replacer.cpp、test_disk_manager.cpp 等）
 * 可以 #include 此头文件复用目录管理和随机缓冲区生成函数。
 *
 * 设计原则：每个测试文件仍然是独立可编译的（自带 main），
 * 此头文件只是减少代码重复，不产生链接依赖。
 */

#pragma once

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "storage/disk_manager.h"

namespace test_utils {

/** 测试用目录，所有磁盘文件操作均在此目录下进行 */
static const std::string TEST_DIR = "module_test_dir";

/**
 * @brief 创建测试目录 TEST_DIR 并 cd 进入。
 *        如果目录已存在则不重复创建。
 */
inline void EnterTestDir(DiskManager *dm) {
    if (!dm->is_dir(TEST_DIR)) {
        dm->create_dir(TEST_DIR);
    }
    ASSERT_TRUE(dm->is_dir(TEST_DIR));
    if (chdir(TEST_DIR.c_str()) < 0) {
        perror("chdir");
        FAIL() << "Cannot enter test directory: " << TEST_DIR;
    }
}

/**
 * @brief 返回上层目录并销毁测试目录 TEST_DIR。
 *        不 assert，允许清理时失败（不影响测试结果判断）。
 */
inline void LeaveTestDir(DiskManager *dm) {
    if (chdir("..") < 0) {
        perror("chdir");
    }
    if (dm->is_dir(TEST_DIR)) {
        dm->destroy_dir(TEST_DIR);
    }
}

/**
 * @brief 用随机数据填充缓冲区。
 * @param size 缓冲区大小（字节）
 * @param buf  输出缓冲区
 */
inline void RandBuf(int size, char *buf) {
    for (int i = 0; i < size; i++) {
        buf[i] = static_cast<char>(rand() & 0xff);
    }
}

}  // namespace test_utils
