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
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    rid_ = Rid{RM_FIRST_RECORD_PAGE, -1};
    next();
}

RmScan::~RmScan() {
    release_cache();
}

void RmScan::release_cache() const {
    if (cached_page_ != nullptr) {
        file_handle_->buffer_pool_manager_->unpin_page(cached_page_->get_page_id(), false);
        cached_page_ = nullptr;
        cached_page_no_ = -1;
    }
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 * 使用页面缓存避免同一页面重复 fetch_page/unpin_page
 */
void RmScan::next() {
    if (is_end()) {
        return;
    }

    int page_no = rid_.page_no;
    int slot_no = rid_.slot_no;
    while (page_no < file_handle_->file_hdr_.num_pages) {
        // 使用缓存的页面句柄，避免重复 fetch
        if (cached_page_no_ != page_no || cached_page_ == nullptr) {
            release_cache();
            RmPageHandle tmp = file_handle_->fetch_page_handle(page_no);
            cached_page_ = tmp.page;
            cached_page_no_ = page_no;
        }
        RmPageHandle page_handle(&file_handle_->file_hdr_, cached_page_);
        int next_slot = Bitmap::next_bit(true, page_handle.bitmap,
                                         file_handle_->file_hdr_.num_records_per_page, slot_no);
        if (next_slot < file_handle_->file_hdr_.num_records_per_page) {
            rid_ = Rid{page_no, next_slot};
            return;
        }
        // move to next page — release current cache
        release_cache();
        page_no++;
        slot_no = -1;
    }
    rid_ = Rid{file_handle_->file_hdr_.num_pages, -1};
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    return rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}
