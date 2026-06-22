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
 * @file test_lru_replacer.cpp
 * @brief LRUReplacer 模块独立单元测试。
 *
 * 本文件是一个完全自包含的 Google Test 程序，
 * 不依赖其他模块测试文件，可独立编译运行。
 *
 * 编译 & 运行（在 build 目录下）：
 *   make test_lru -j$(nproc) && ./bin/test_lru
 *
 * 团队分工说明：
 * - LRUReplacer 由 Person A 负责实现
 * - 对应模块：src/replacer/lru_replacer.cpp
 * - 测试通过标准：全部 9 个测试用例 PASS
 */

#include <gtest/gtest.h>

#include <memory>
#include <thread>  // NOLINT
#include <vector>

#include "replacer/lru_replacer.h"

// ============================================================
// LRUReplacer 测试套件
// ============================================================

class LRUReplacerTest : public ::testing::Test {
   protected:
    void SetUp() override {
        srand(static_cast<unsigned>(time(nullptr)));
    }
};

/**
 * @test 基本 LRU 淘汰顺序
 *  unpin 1,2,3 → victim 应依次得到 1,2,3
 *  验证：最近 unpin 的在链表前端，淘汰从后端（最久未用）取
 */
TEST_F(LRUReplacerTest, BasicVictimOrder) {
    LRUReplacer replacer(10);

    replacer.unpin(1);
    replacer.unpin(2);
    replacer.unpin(3);
    ASSERT_EQ(3, replacer.Size());

    frame_id_t f;
    EXPECT_TRUE(replacer.victim(&f));
    EXPECT_EQ(1, f);
    EXPECT_TRUE(replacer.victim(&f));
    EXPECT_EQ(2, f);
    EXPECT_TRUE(replacer.victim(&f));
    EXPECT_EQ(3, f);
    EXPECT_EQ(0, replacer.Size());
}

/**
 * @test 空 replacer → victim 返回 false，不修改入参
 */
TEST_F(LRUReplacerTest, EmptyReplacer) {
    LRUReplacer replacer(10);
    frame_id_t f = 999;
    EXPECT_FALSE(replacer.victim(&f));
    EXPECT_EQ(999, f);
}

/**
 * @test pin 从 replacer 中移除 frame，此后不再被淘汰
 */
TEST_F(LRUReplacerTest, PinRemovesFrame) {
    LRUReplacer replacer(10);
    replacer.unpin(5);
    replacer.unpin(7);
    ASSERT_EQ(2, replacer.Size());

    replacer.pin(5);
    EXPECT_EQ(1, replacer.Size());

    frame_id_t f;
    replacer.victim(&f);
    EXPECT_EQ(7, f);
}

/**
 * @test pin 一个不在 replacer 中的 frame → 无操作，不崩溃
 */
TEST_F(LRUReplacerTest, PinNonExistent) {
    LRUReplacer replacer(10);
    replacer.unpin(1);
    replacer.pin(999);
    EXPECT_EQ(1, replacer.Size());
}

/**
 * @test 重复 unpin 同一 frame → 幂等，不会重复加入
 */
TEST_F(LRUReplacerTest, DuplicateUnpinIsIdempotent) {
    LRUReplacer replacer(10);
    replacer.unpin(1);
    replacer.unpin(1);
    EXPECT_EQ(1, replacer.Size());

    frame_id_t f;
    replacer.victim(&f);
    EXPECT_EQ(1, f);
    EXPECT_EQ(0, replacer.Size());
}

/**
 * @test unpin → pin → unpin 完整生命周期
 */
TEST_F(LRUReplacerTest, FullLifecycle) {
    LRUReplacer replacer(10);

    replacer.unpin(1);
    replacer.unpin(2);
    replacer.pin(1);
    replacer.unpin(1);
    ASSERT_EQ(2, replacer.Size());

    frame_id_t f;
    replacer.victim(&f);
    EXPECT_EQ(2, f);
    replacer.victim(&f);
    EXPECT_EQ(1, f);
}

/**
 * @test 综合场景：unpin → victim → pin → unpin → victim
 *  来自原 unit_test.cpp 的 SampleTest，确保向后兼容
 */
TEST_F(LRUReplacerTest, ComprehensiveScenario) {
    LRUReplacer replacer(7);

    replacer.unpin(1);
    replacer.unpin(2);
    replacer.unpin(3);
    replacer.unpin(4);
    replacer.unpin(5);
    replacer.unpin(6);
    replacer.unpin(1);
    EXPECT_EQ(6, replacer.Size());

    frame_id_t value;
    replacer.victim(&value);
    EXPECT_EQ(1, value);
    replacer.victim(&value);
    EXPECT_EQ(2, value);
    replacer.victim(&value);
    EXPECT_EQ(3, value);

    replacer.pin(3);
    replacer.pin(4);
    EXPECT_EQ(2, replacer.Size());

    replacer.unpin(4);

    replacer.victim(&value);
    EXPECT_EQ(5, value);
    replacer.victim(&value);
    EXPECT_EQ(6, value);
    replacer.victim(&value);
    EXPECT_EQ(4, value);
}

/**
 * @test 大批量操作：10000 个 frame unpin 后全部淘汰
 *  验证 LRU 顺序正确且无性能问题
 */
TEST_F(LRUReplacerTest, HighVolume) {
    constexpr int N = 10000;
    LRUReplacer replacer(N);

    for (int i = 0; i < N; i++) {
        replacer.unpin(i);
    }
    EXPECT_EQ(N, replacer.Size());

    for (int i = 0; i < N; i++) {
        frame_id_t f;
        EXPECT_TRUE(replacer.victim(&f));
        EXPECT_EQ(i, f);
    }
    EXPECT_EQ(0, replacer.Size());
}

/**
 * @test 多线程并发压力测试
 *  4 线程各执行 500 次 unpin + 250 次 victim
 *  目标：不死锁、不崩溃（不验证严格正确性）
 */
TEST_F(LRUReplacerTest, ConcurrentStress) {
    LRUReplacer replacer(1000);
    constexpr int NUM_THREADS = 4;
    constexpr int OPS_PER_THREAD = 500;

    auto worker = [&replacer](int tid) {
        int base = tid * OPS_PER_THREAD;
        for (int i = 0; i < OPS_PER_THREAD; i++) {
            replacer.unpin(base + i);
        }
        for (int i = 0; i < OPS_PER_THREAD / 2; i++) {
            frame_id_t f;
            replacer.victim(&f);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(worker, i);
    }
    for (auto &t : threads) {
        t.join();
    }
    SUCCEED();
}

// ============================================================
// 主函数
// ============================================================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
