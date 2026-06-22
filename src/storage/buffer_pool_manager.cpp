/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"

// ============================================================
//  辅助方法
// ============================================================

/**
 * @description: 从 free_list 或 replacer 中得到可淘汰帧页的 *frame_id
 *
 * 策略：
 *   1. 优先从 free_list_ 中取空闲帧（从未使用或已 delete 归还的帧）
 *   2. 若 free_list_ 为空，使用 replacer_（LRU策略）淘汰一个 unpinned 的帧
 *
 * @return {bool} true: 可替换帧查找成功 , false: 可替换帧查找失败
 * @param {frame_id_t*} frame_id 输出参数，成功找到的可替换帧 id
 */
bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    // 1.1 优先使用空闲帧
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    // 1.2 空闲帧耗尽 → 用 LRU 淘汰一个 unpinned 页面
    return replacer_->victim(frame_id);
}

/**
 * @description: 更新页面数据 —— 将 frame 中当前 page 替换为一个新 page。
 *
 * 流程：
 *   1. 若当前 page 是脏页，先写回磁盘
 *   2. 更新 page_table_：删除旧 page_id 的映射，建立新 page_id → frame_id 映射
 *   3. 重置 page 的 data 缓冲区，设置新的 page_id，清除脏标记
 *
 * @param {Page*}       page         当前占据该帧的 Page 对象
 * @param {PageId}      new_page_id  即将放入该帧的新 page_id
 * @param {frame_id_t}  new_frame_id 帧编号（与 page 所在帧一致）
 */
void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    // 1. 脏页写回磁盘
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }
    // 2. 更新 page_table_：移除旧映射，插入新映射
    page_table_.erase(page->id_);
    page_table_.emplace(new_page_id, new_frame_id);
    // 3. 重置 page 数据缓冲区，设置新的 page_id
    page->reset_memory();
    page->id_ = new_page_id;
}

// ============================================================
//  核心公开方法
// ============================================================

/**
 * @description: 从 buffer pool 获取需要的页。
 *
 * 缓存命中（page_table_ 中存在 page_id）：
 *   将对应帧 pin_count++，从 replacer 中移除（防止被淘汰），返回 Page*。
 *
 * 缓存未命中（page_table_ 中不存在）：
 *   1. 通过 find_victim_page 获得一个可用帧
 *   2. 若该帧原 page 为脏页，调用 update_page 写回磁盘并更新 page_table_
 *   3. 从磁盘读取目标页数据到该帧
 *   4. pin_count 置 1，返回 Page*
 *
 * @param {PageId} page_id 需要获取的页的 PageId
 * @return {Page*} 成功返回 Page*，失败（无可用帧）返回 nullptr
 */
Page* BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};

    // 1.1 缓存命中
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t frame_id = it->second;
        Page *page = &pages_[frame_id];
        page->pin_count_++;                 // 增加固定计数
        replacer_->pin(frame_id);           // 从 replacer 移除（不可淘汰）
        return page;
    }

    // 1.2 缓存未命中 → 找一个可用帧
    frame_id_t victim_frame_id;
    if (!find_victim_page(&victim_frame_id)) {
        return nullptr;                     // 缓冲池已满且无可淘汰页
    }

    Page *page = &pages_[victim_frame_id];

    // 2. 若 victim 帧之前被使用过（有有效 page_id），处理脏页写回并更新映射
    if (page->id_.page_no != INVALID_PAGE_ID) {
        update_page(page, page_id, victim_frame_id);
    } else {
        // 从未使用过的帧（来自 free_list_），直接建立映射
        page_table_.emplace(page_id, victim_frame_id);
        page->id_ = page_id;
    }

    // 3. 从磁盘读取目标页数据
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);

    // 4. 固定该页（pin_count=1 表示被使用中）
    page->pin_count_ = 1;

    return page;
}

/**
 * @description: 取消固定 pin_count > 0 的缓冲页。
 *
 * 流程：
 *   1. 查找 page_table_，若不存在返回 false
 *   2. 若 pin_count_ <= 0 返回 false（未固定的页不能再次 unpin）
 *   3. pin_count_ 自减 1
 *   4. 若自减后 pin_count_ == 0，调用 replacer_->unpin 使其可被淘汰
 *   5. 根据 is_dirty 参数设置脏页标记
 *
 * @param {PageId} page_id  目标 page_id
 * @param {bool}   is_dirty 若需标记为脏则 true
 * @return {bool}  成功（pin_count 从 >0 减到 >=0）返回 true，否则 false
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};

    // 1. 查找页表
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;                       // 页面不在缓冲池中
    }

    // 2. 检查 pin_count
    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];
    if (page->pin_count_ <= 0) {
        return false;                       // pin_count 已为 0，不可再减
    }

    // 3. pin_count 自减
    page->pin_count_--;

    // 4. 若 pin_count 降到 0，允许该帧被淘汰
    if (page->pin_count_ == 0) {
        replacer_->unpin(frame_id);
    }

    // 5. 更新脏页标记（只有 is_dirty=true 时才置脏，false 时不清除已有脏标记）
    if (is_dirty) {
        page->is_dirty_ = true;
    }

    return true;
}

/**
 * @description: 将目标页写回磁盘，**不**考虑 pin_count。
 *
 * 流程：
 *   1. 查找 page_table_，若不存在返回 false
 *   2. 无论是否脏页，都将整个 PAGE_SIZE 写回磁盘
 *   3. 清除脏页标记
 *
 * @param {PageId} page_id 目标页，不能为 INVALID_PAGE_ID
 * @return {bool}  成功返回 true，页表中不存在则返回 false
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock lock{latch_};

    // 1. 查找页表
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;                       // 页面不在缓冲池中
    }

    // 2. 无论是否脏都写回磁盘
    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);

    // 3. 清除脏页标记
    page->is_dirty_ = false;

    return true;
}

/**
 * @description: 创建一个新的 page —— 在磁盘上分配一个新页面并载入缓冲池。
 *
 * 流程：
 *   1. 获得一个可用帧，若失败返回 nullptr
 *   2. 若该帧原 page 为脏页，写回磁盘并从 page_table_ 中移除旧映射
 *   3. 调用 disk_manager_->allocate_page(fd) 分配新 page_no
 *   4. 在 page_table_ 中建立新映射
 *   5. 重置 page 数据缓冲区，设置 page_id，pin_count=1
 *
 * @param {PageId*} page_id
 *       输入：page_id->fd 指明在哪个文件中分配；
 *       输出：分配成功后填充 page_id->page_no
 * @return {Page*} 成功返回 Page*，失败返回 nullptr
 */
Page* BufferPoolManager::new_page(PageId* page_id) {
    std::scoped_lock lock{latch_};

    // 1. 获得一个可用帧
    frame_id_t new_frame_id;
    if (!find_victim_page(&new_frame_id)) {
        return nullptr;
    }

    Page *page = &pages_[new_frame_id];

    // 2. 若该帧原页面有效且脏，写回磁盘并从 page_table_ 移除
    if (page->id_.page_no != INVALID_PAGE_ID) {
        if (page->is_dirty_) {
            disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
            page->is_dirty_ = false;
        }
        page_table_.erase(page->id_);       // 移除旧映射
    }

    // 3. 在磁盘上分配新 page_no
    int fd = page_id->fd;                   // 调用方指定文件
    page_id_t new_page_no = disk_manager_->allocate_page(fd);
    *page_id = {fd, new_page_no};           // 输出新 page_id

    // 4. 建立新映射
    page_table_.emplace(*page_id, new_frame_id);

    // 5. 初始化 page 元数据
    page->reset_memory();
    page->id_ = *page_id;
    page->pin_count_ = 1;                   // 固定（调用方负责 unpin）
    page->is_dirty_ = false;

    return page;
}

/**
 * @description: 从 buffer pool 删除目标页。
 *
 * 流程：
 *   1. 若 page_table_ 中不存在，直接返回 true（页面已不在缓冲池）
 *   2. 若 pin_count > 0，返回 false（页面正被使用，不能删除）
 *   3. 若为脏页，写回磁盘
 *   4. 从 page_table_ 中移除映射
 *   5. 重置 page 元数据（id, is_dirty, pin_count, data）
 *   6. 将帧归还 free_list_ 并从 replacer 中移除
 *
 * @param {PageId} page_id 目标页
 * @return {bool} 不存在或删除成功返回 true；存在但 pin_count > 0 返回 false
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock lock{latch_};

    // 1. 查找页表，不存在则视为已删除
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true;
    }

    // 2. 检查 pin_count
    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];
    if (page->pin_count_ > 0) {
        return false;                       // 正在被使用，不能删除
    }

    // 3. 脏页写回磁盘
    if (page->is_dirty_) {
        disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    }

    // 4. 从 page_table_ 移除映射
    page_table_.erase(it);

    // 5. 重置 page 元数据
    page->id_ = {INVALID_PAGE_ID, INVALID_PAGE_ID};
    page->is_dirty_ = false;
    page->pin_count_ = 0;
    page->reset_memory();

    // 6. 将帧归还 free_list_，并从 replacer 中移除（若存在）
    free_list_.push_back(frame_id);
    replacer_->pin(frame_id);               // pin 操作会将其从 replacer 中移除

    return true;
}

/**
 * @description: 将 buffer pool 中属于指定文件 fd 的所有脏页写回磁盘。
 *
 * 注意：此方法遍历整个 pages_ 数组，不会修改 pin_count_，
 *       仅将 dirty 页面刷新到磁盘并清除脏标记。
 *
 * @param {int} fd 文件句柄
 */
void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};

    for (size_t i = 0; i < pool_size_; ++i) {
        Page *page = &pages_[i];
        // 只处理属于该 fd 且 page_no 有效的页面
        if (page->id_.fd == fd && page->id_.page_no != INVALID_PAGE_ID) {
            if (page->is_dirty_) {
                disk_manager_->write_page(fd, page->id_.page_no, page->data_, PAGE_SIZE);
                page->is_dirty_ = false;
            }
        }
    }
}
