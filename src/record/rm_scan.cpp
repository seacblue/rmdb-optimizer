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

RmScan::RmScan(const RmFileHandle *file_handle, bool reverse) : file_handle_(file_handle), reverse_(reverse) {
    const RmFileHdr &hdr = file_handle_->file_hdr_;
    if (reverse_) {
        rid_.page_no = hdr.num_pages - 1;
        rid_.slot_no = hdr.num_records_per_page;
    } else {
        rid_.page_no = RM_FIRST_RECORD_PAGE;
        rid_.slot_no = -1;
    }
    next();
}

void RmScan::next() {
    if (rid_.page_no == RM_NO_PAGE) return;

    const RmFileHdr &hdr = file_handle_->file_hdr_;

    if (reverse_) {
        rid_.slot_no--;

        while (rid_.page_no >= RM_FIRST_RECORD_PAGE) {
            RmPageHandle ph = file_handle_->fetch_page_handle(rid_.page_no);

            int prev_slot = Bitmap::prev_bit(true, ph.bitmap, rid_.slot_no + 1);

            if (prev_slot >= 0) {
                rid_.slot_no = prev_slot;
                file_handle_->buffer_pool_manager_->unpin_page({file_handle_->fd_, rid_.page_no}, false);
                return;
            }

            file_handle_->buffer_pool_manager_->unpin_page({file_handle_->fd_, rid_.page_no}, false);
            rid_.page_no--;
            rid_.slot_no = hdr.num_records_per_page;
        }

        rid_.page_no = RM_NO_PAGE;
        rid_.slot_no = -1;
        return;
    }

    // === 正向扫描（原有逻辑） ===
    if (rid_.page_no < RM_FIRST_RECORD_PAGE) {
        rid_.page_no = RM_FIRST_RECORD_PAGE;
        rid_.slot_no = -1;
    }

    rid_.slot_no++;

    while (rid_.page_no < hdr.num_pages) {
        RmPageHandle ph = file_handle_->fetch_page_handle(rid_.page_no);

        int next_slot = Bitmap::next_bit(true, ph.bitmap, hdr.num_records_per_page, rid_.slot_no - 1);

        if (next_slot < hdr.num_records_per_page) {
            rid_.slot_no = next_slot;
            file_handle_->buffer_pool_manager_->unpin_page({file_handle_->fd_, rid_.page_no}, false);
            return;
        }

        file_handle_->buffer_pool_manager_->unpin_page({file_handle_->fd_, rid_.page_no}, false);
        rid_.page_no++;
        rid_.slot_no = -1;
    }

    rid_.page_no = RM_NO_PAGE;
    rid_.slot_no = -1;
}

bool RmScan::is_end() const {
    return rid_.page_no == RM_NO_PAGE || rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

Rid RmScan::rid() const {
    return rid_;
}
