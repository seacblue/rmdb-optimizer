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
 * @file test_sm_context.h
 * @brief 为 SmManager 测试提供有效的 Context 对象。
 *
 * RecordPrinter 在传入 nullptr context 时会解引用空指针导致 segfault，
 * 因此所有需要打印输出的测试必须使用此辅助函数创建有效的 Context。
 */

#pragma once

#include <cstring>
#include <memory>

#include "common/context.h"

namespace sm_test {

/** RecordPrinter 的写入缓冲区大小 */
static constexpr int kContextBufSize = 4096;

/**
 * @brief 测试上下文助手 — 包含 Context 及 data_send 缓冲区。
 *
 * RecordPrinter 会将格式化结果写入 context->data_send_，
 * 因此必须分配有效缓冲区。此结构体保证缓冲区与 Context 同生命周期。
 */
struct TestContext {
    std::unique_ptr<char[]> buf;
    std::unique_ptr<Context> ctx;
    int offset;

    TestContext()
        : buf(std::make_unique<char[]>(kContextBufSize)),
          offset(0) {
        memset(buf.get(), 0, kContextBufSize);
        ctx = std::make_unique<Context>(nullptr, nullptr, nullptr, buf.get(), &offset);
    }
};

}  // namespace sm_test
