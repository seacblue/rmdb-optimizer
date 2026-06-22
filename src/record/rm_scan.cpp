/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化 file_handle 和 rid，将 rid 定位到第一个存放了记录的位置。
 *
 * 设计：
 *   rid_ 初始化为 {RM_FIRST_RECORD_PAGE, -1}，
 *   然后立即调用 next() 前进到第一条有效记录。
 *   若表中无记录，则 rid_.page_no 被设为 RM_NO_PAGE，is_end() 返回 true。
 *
 * @param file_handle 要扫描的文件句柄
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    rid_.page_no = RM_FIRST_RECORD_PAGE;
    rid_.slot_no = -1;
    next();     // 前进到第一条实际存在的记录
}

/**
 * @brief 将 rid_ 推进到下一条有效记录。
 *
 * 算法：
 *   1. rid_.slot_no++（在当前页中尝试推进一个 slot）
 *   2. 在当前页的 bitmap 中从 rid_.slot_no 开始找下一个 set bit
 *   3. 如果找到 → 更新 rid_.slot_no，unpin 当前页，返回
 *   4. 如果没找到 → unpin 当前页，移到下一页（page_no++），slot_no = 0，重复步骤2
 *   5. 如果 page_no 超出范围 → 设置 rid_ = {RM_NO_PAGE, -1}，表示扫描结束
 *
 * @note 每次访问 page 后立即 unpin，避免缓冲区 pin 泄漏。
 */
void RmScan::next() {
    if (rid_.page_no == RM_NO_PAGE) return;     // 已经到末尾
    if (rid_.page_no < RM_FIRST_RECORD_PAGE) {
        rid_.page_no = RM_FIRST_RECORD_PAGE;
        rid_.slot_no = -1;
    }

    const RmFileHdr &hdr = file_handle_->file_hdr_;
    rid_.slot_no++;                             // 从当前 slot 的下一个开始找

    while (rid_.page_no < hdr.num_pages) {
        // 获取当前页的 handle（临时对象，作用域结束后自动析构）
        RmPageHandle ph = file_handle_->fetch_page_handle(rid_.page_no);

        // 在当前页中找下一个 set bit（即有记录的 slot）
        int next_slot = Bitmap::next_bit(true, ph.bitmap, hdr.num_records_per_page, rid_.slot_no - 1);

        if (next_slot < hdr.num_records_per_page) {
            // 找到了 → 更新 slot_no，unpin 当前页，返回
            rid_.slot_no = next_slot;
            file_handle_->buffer_pool_manager_->unpin_page({file_handle_->fd_, rid_.page_no}, false);
            return;
        }

        // 当前页没有更多记录 → unpin 当前页，移向下一页
        file_handle_->buffer_pool_manager_->unpin_page({file_handle_->fd_, rid_.page_no}, false);
        rid_.page_no++;
        rid_.slot_no = -1;
    }

    // 所有页都已扫描完毕
    rid_.page_no = RM_NO_PAGE;
    rid_.slot_no = -1;
}

/**
 * @brief 判断是否已经扫描完文件中的所有记录。
 *
 * @return true  已无更多记录
 * @return false 还有未扫描的记录
 */
bool RmScan::is_end() const {
    return rid_.page_no == RM_NO_PAGE || rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

/**
 * @brief 返回当前扫描位置对应的记录 ID（Rid）。
 */
Rid RmScan::rid() const {
    return rid_;
}