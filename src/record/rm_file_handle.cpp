/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"

// ============================================================
//  内部辅助：获取 / 创建页面句柄
// ============================================================

/**
 * @description: 获取指定页面的页面句柄。
 *
 * 从 buffer pool 中 fetch 指定 page_no 的页面，封装为 RmPageHandle 返回。
 * 注意：返回的 handle 关联的 Page 在 buffer pool 中处于 pinned 状态，
 *       调用方完成操作后应通过 buffer_pool_manager_ 显式 unpin。
 *
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 * @throw PageNotExistError 若 page_no 超出有效范围
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    // 校验 page_no 合法性：数据页从 RM_FIRST_RECORD_PAGE 开始
    if (page_no < RM_FIRST_RECORD_PAGE || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError("", page_no);
    }
    PageId pid = {.fd = fd_, .page_no = page_no};
    Page *page = buffer_pool_manager_->fetch_page(pid);
    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个全新的 page handle（在磁盘上分配一个新页）。
 *
 * 步骤：
 *   1. 通过 buffer_pool_manager_->new_page() 在磁盘上分配一个新页
 *   2. 初始化 RmPageHdr：next_free_page_no = -1, num_records = 0
 *   3. 初始化 bitmap：全部置 0（所有 slot 空闲）
 *   4. 更新 file_hdr_.num_pages++（跟踪总页数）
 *
 * @return {RmPageHandle} 新页面的句柄（page 处于 pinned 状态）
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    // 1. 通过 buffer pool 分配一个新页
    PageId pid = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&pid);

    // 2. 初始化页面元数据
    RmPageHandle ph(&file_hdr_, page);
    ph.page_hdr->next_free_page_no = RM_NO_PAGE;       // 新页初始为最后一个空闲页
    ph.page_hdr->num_records = 0;
    Bitmap::init(ph.bitmap, file_hdr_.bitmap_size);     // bitmap 全部置 0

    // 3. 将新页加入空闲页链表（新页必然有空闲空间）
    ph.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = pid.page_no;

    // 4. 更新文件头中的总页数
    file_hdr_.num_pages++;

    return ph;
}

/**
 * @brief 获取一个有空闲空间的页面句柄。
 *
 * 策略：
 *   1. 如果 file_hdr_.first_free_page_no != -1，说明有空闲页，直接 fetch
 *   2. 否则，通过 create_new_page_handle() 分配一个新页
 *
 * @return RmPageHandle 有空闲空间的页面句柄（page 处于 pinned 状态）
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no != RM_NO_PAGE) {
        // 有空闲页 → 直接获取第一个空闲页
        return fetch_page_handle(file_hdr_.first_free_page_no);
    }
    // 无空闲页 → 创建新页
    return create_new_page_handle();
}

/**
 * @description: 当一个页面从"没有空闲空间"变为"有空闲空间"时，
 *               将其插入空闲页链表头部。
 *
 * 步骤：
 *   1. 将 page_handle 的 next_free_page_no 指向当前 file_hdr 的 first_free_page_no
 *   2. 将 file_hdr 的 first_free_page_no 指向该页面
 */
void RmFileHandle::release_page_handle(RmPageHandle &page_handle) {
    // 该页的下一空闲页指针指向当前空闲页链表头部
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    // 将该页设为新的空闲页链表头部
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
}

// ============================================================
//  记录 CRUD 操作
// ============================================================

/**
 * @description: 获取当前表中记录号为 rid 的记录。
 *
 * 步骤：
 *   1. 获取 rid.page_no 对应的 page handle
 *   2. 通过 bitmap 检查该 slot 是否有记录（is_set），没有则返回 nullptr
 *   3. 从 slot 位置拷贝 record_size 字节构造 RmRecord
 *   4. unpin 页面
 *
 * @param {Rid&} rid 记录号（page_no + slot_no）
 * @param {Context*} context 上下文（本方法中暂未使用）
 * @return {unique_ptr<RmRecord>} rid 对应的记录对象指针，不存在则返回 nullptr
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid &rid, Context *context) const {
    // 1. 获取 page handle
    RmPageHandle ph = fetch_page_handle(rid.page_no);

    // 2. 检查记录是否存在
    if (!Bitmap::is_set(ph.bitmap, rid.slot_no)) {
        // slot 空闲，无记录 → unpin 后返回 nullptr
        buffer_pool_manager_->unpin_page({fd_, rid.page_no}, false);
        return nullptr;
    }

    // 3. 构造 RmRecord
    char *slot_data = ph.get_slot(rid.slot_no);
    auto rec = std::make_unique<RmRecord>(file_hdr_.record_size, slot_data);

    // 4. unpin 页面（未修改，不脏）
    buffer_pool_manager_->unpin_page({fd_, rid.page_no}, false);

    return rec;
}

/**
 * @description: 在当前表中插入一条记录，由系统自动分配位置（首个空闲 slot）。
 *
 * 步骤：
 *   1. 通过 create_page_handle() 获取有空闲空间的页面
 *   2. 通过 Bitmap::first_bit(false, ...) 找到第一个空闲 slot
 *   3. 将 buf 中 record_size 字节拷贝到 slot 位置
 *   4. 设置 bitmap 对应位
 *   5. page_hdr->num_records++
 *   6. 若页面已满（num_records == num_records_per_page），
 *      从空闲链表中移除该页（更新 file_hdr_.first_free_page_no）
 *   7. unpin 页面（标记脏）
 *
 * @param {char*} buf 要插入的记录数据
 * @param {Context*} context 上下文
 * @return {Rid} 插入记录的记录号
 */
Rid RmFileHandle::insert_record(char *buf, Context *context) {
    // 1. 获取有空闲空间的页面
    RmPageHandle ph = create_page_handle();
    int page_no = ph.page->get_page_id().page_no;

    // 2. 找到第一个空闲 slot
    int slot_no = Bitmap::first_bit(false, ph.bitmap, file_hdr_.num_records_per_page);
    // first_bit 在 bitmap 全满时会返回 num_records_per_page，但此时不应该发生
    // （因为 create_page_handle 保证返回的页面有空闲空间）

    // 3. 将记录数据拷贝到 slot
    char *slot = ph.get_slot(slot_no);
    memcpy(slot, buf, file_hdr_.record_size);

    // 4. 更新 bitmap
    Bitmap::set(ph.bitmap, slot_no);

    // 5. 更新页面元数据
    ph.page_hdr->num_records++;

    // 6. 如果页面已满，从空闲链表中移除
    bool page_full = (ph.page_hdr->num_records >= file_hdr_.num_records_per_page);
    if (page_full) {
        // 该页不再是空闲页 → 将 first_free_page_no 指向 next_free_page_no
        file_hdr_.first_free_page_no = ph.page_hdr->next_free_page_no;
        ph.page_hdr->next_free_page_no = RM_NO_PAGE;
    }

    // 7. unpin（标记脏页）
    buffer_pool_manager_->unpin_page({fd_, page_no}, true);

    return {page_no, slot_no};
}

/**
 * @description: 在当前表中的**指定位置**插入一条记录。
 *
 * 此版本用于需要精确控制插入位置的场景（如恢复、索引维护）。
 * 步骤：
 *   1. 获取 rid.page_no 对应的 page handle
 *   2. 将 buf 拷贝到 rid.slot_no 位置
 *   3. 设置 bitmap
 *   4. num_records++
 *   5. unpin（标记脏）
 *
 * @param {Rid&} rid 要插入的位置
 * @param {char*} buf 记录数据
 */
void RmFileHandle::insert_record(const Rid &rid, char *buf) {
    // 1. 获取 page handle
    RmPageHandle ph = fetch_page_handle(rid.page_no);

    // 2. 将数据拷贝到指定 slot
    char *slot = ph.get_slot(rid.slot_no);
    memcpy(slot, buf, file_hdr_.record_size);

    // 3. 更新 bitmap
    Bitmap::set(ph.bitmap, rid.slot_no);

    // 4. 更新页面元数据
    ph.page_hdr->num_records++;

    // 5. unpin（标记脏）
    buffer_pool_manager_->unpin_page({fd_, rid.page_no}, true);
}

/**
 * @description: 删除记录文件中记录号为 rid 的记录。
 *
 * 步骤：
 *   1. 获取 rid.page_no 对应的 page handle
 *   2. 检查 bitmap 对应位是否有效（有记录才删除）
 *   3. 清除 bitmap 对应位
 *   4. num_records--
 *   5. 如果页面之前是满的（num_records 之前 == num_records_per_page），
 *      现在变为未满 → 调用 release_page_handle() 将其插入空闲链表
 *   6. unpin（标记脏）
 *
 * @param {Rid&} rid 要删除的记录号
 * @param {Context*} context 上下文
 */
void RmFileHandle::delete_record(const Rid &rid, Context *context) {
    // 1. 获取 page handle
    RmPageHandle ph = fetch_page_handle(rid.page_no);

    // 2. 检查记录是否存在；若不存在则 unpin 后直接返回
    if (!Bitmap::is_set(ph.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page({fd_, rid.page_no}, false);
        return;
    }

    // 3. 判断删除前页面是否已满（用于决定是否需要 release）
    bool was_full = (ph.page_hdr->num_records >= file_hdr_.num_records_per_page);

    // 4. 清除 bitmap
    Bitmap::reset(ph.bitmap, rid.slot_no);

    // 5. 更新页面元数据
    ph.page_hdr->num_records--;

    // 6. 如果页面从"满"变为"未满"，加入空闲链表
    if (was_full && ph.page_hdr->num_records < file_hdr_.num_records_per_page) {
        release_page_handle(ph);
    }

    // 7. unpin（标记脏）
    buffer_pool_manager_->unpin_page({fd_, rid.page_no}, true);
}

/**
 * @description: 更新记录文件中记录号为 rid 的记录。
 *
 * 步骤：
 *   1. 获取 rid.page_no 对应的 page handle
 *   2. 检查 bitmap 对应位（有记录才更新）
 *   3. 将 buf 中 record_size 字节拷贝到 slot 位置
 *   4. unpin（标记脏）
 *
 * @param {Rid&} rid 要更新的记录号
 * @param {char*} buf 新记录数据
 * @param {Context*} context 上下文
 */
void RmFileHandle::update_record(const Rid &rid, char *buf, Context *context) {
    // 1. 获取 page handle
    RmPageHandle ph = fetch_page_handle(rid.page_no);

    // 2. 检查记录是否存在；若不存在则 unpin 后直接返回
    if (!Bitmap::is_set(ph.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page({fd_, rid.page_no}, false);
        return;
    }

    // 3. 拷贝新数据到 slot
    char *slot = ph.get_slot(rid.slot_no);
    memcpy(slot, buf, file_hdr_.record_size);

    // 4. unpin（标记脏）
    buffer_pool_manager_->unpin_page({fd_, rid.page_no}, true);
}
