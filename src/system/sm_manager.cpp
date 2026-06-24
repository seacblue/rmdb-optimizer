/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <unordered_set>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

namespace {

std::string format_output_row(const std::vector<std::string> &columns) {
    std::string out = "|";
    for (const auto &column : columns) {
        out += " " + column + " |";
    }
    out += "\n";
    return out;
}

void append_context_output(Context *context, const std::string &text) {
    if (context == nullptr || context->data_send_ == nullptr || context->offset_ == nullptr) {
        return;
    }
    if (context->ellipsis_ == false &&
        *context->offset_ + static_cast<int>(text.size()) + RECORD_COUNT_LENGTH < BUFFER_LENGTH) {
        memcpy(context->data_send_ + *(context->offset_), text.c_str(), text.size());
        *(context->offset_) += static_cast<int>(text.size());
    } else {
        context->ellipsis_ = true;
    }
}

MemoryIndexComparator make_mem_index_comparator(const IndexMeta &index) {
    MemoryIndexComparator comp;
    for (const auto &col : index.cols) {
        comp.col_types.push_back(col.type);
        comp.col_lens.push_back(col.len);
    }
    return comp;
}

std::string build_key_from_record(const IndexMeta &index, const char *rec_data) {
    std::string key;
    key.reserve(index.col_tot_len);
    for (const auto &col : index.cols) {
        key.append(rec_data + col.offset, col.len);
    }
    return key;
}

}  // namespace

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
    // 读取数据库元数据
    std::ifstream ifs(DB_META_NAME);
    ifs >> db_;
    // 打开所有表的记录文件
    for (auto &entry : db_.tabs_) {
        auto &tab_name = entry.first;
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
    }
    // 打开所有表的索引文件
    for (auto &entry : db_.tabs_) {
        auto &tab_name = entry.first;
        auto &tab = entry.second;
        for (auto &index : tab.indexes) {
            std::string ix_name = ix_manager_->get_index_name(tab_name, index.cols);
            if (disk_manager_->is_file(ix_name)) {
                ihs_.emplace(ix_name, ix_manager_->open_index(tab_name, index.cols));
            }
            auto mem_index = std::make_shared<MemoryIndex>();
            mem_index->meta = index;
            mem_index->entries = std::map<std::string, Rid, MemoryIndexComparator>(make_mem_index_comparator(index));
            RmFileHandle *fh = fhs_.at(tab_name).get();
            RmScan scan(fh);
            while (!scan.is_end()) {
                Rid rid = scan.rid();
                auto rec = fh->get_record(rid, nullptr);
                mem_index->entries.emplace(build_key_from_record(index, rec->data), rid);
                scan.next();
            }
            mem_indexes_[ix_name] = mem_index;
        }
    }
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    // 刷新元数据
    flush_meta();
    // 关闭所有索引句柄
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();
    mem_indexes_.clear();
    // 关闭所有表的记录文件句柄
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();
    db_ = DbMeta();
    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

void SmManager::show_index(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    if (tab.indexes.empty()) {
        return;
    }

    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    for (const auto &index : tab.indexes) {
        std::string cols = "(";
        for (size_t i = 0; i < index.cols.size(); ++i) {
            if (i > 0) {
                cols += ",";
            }
            cols += index.cols[i].name;
        }
        cols += ")";
        std::string line = format_output_row({tab.name, "unique", cols});
        append_context_output(context, line);
        outfile << line;
    }
    outfile.close();
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    (void)tab;
    auto saved_indexes = db_.tabs_[tab_name].indexes;
    for (auto &index : saved_indexes) {
        drop_index(tab_name, index.cols, context);
    }
    // 先正确关闭表的记录文件句柄（刷盘并关闭fd），再从 map 中移除
    auto it = fhs_.find(tab_name);
    if (it != fhs_.end()) {
        rm_manager_->close_file(it->second.get());
        fhs_.erase(it);
    }
    // 删除记录文件
    rm_manager_->destroy_file(tab_name);
    // 从元数据中移除表
    db_.tabs_.erase(tab_name);
    // 刷新元数据
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    std::vector<ColMeta> index_cols;
    for (auto &col_name : col_names) {
        auto col_it = tab.get_col(col_name);
        index_cols.push_back(*col_it);
    }

    int index_col_tot_len = 0;
    for (auto &col : index_cols) {
        index_col_tot_len += col.len;
    }

    ix_manager_->create_index(tab_name, index_cols);

    std::string ix_name = ix_manager_->get_index_name(tab_name, index_cols);
    auto ih = ix_manager_->open_index(tab_name, index_cols);

    RmFileHandle *fh = fhs_.at(tab_name).get();
    RmScan scan(fh);
    std::unordered_set<std::string> seen_keys;
    std::vector<std::pair<std::string, Rid>> entries_to_build;
    while (!scan.is_end()) {
        Rid rid = {.page_no = scan.rid().page_no, .slot_no = scan.rid().slot_no};
        auto rec = fh->get_record(rid, context);
        auto key_buf = std::make_unique<char[]>(index_col_tot_len);
        int offset = 0;
        for (auto &col : index_cols) {
            memcpy(key_buf.get() + offset, rec->data + col.offset, col.len);
            offset += col.len;
        }
        std::string key_str(key_buf.get(), key_buf.get() + index_col_tot_len);
        if (seen_keys.find(key_str) != seen_keys.end()) {
            ix_manager_->close_index(ih.get());
            ix_manager_->destroy_index(tab_name, index_cols);
            throw UniqueConstraintError(ix_name);
        }
        seen_keys.insert(key_str);
        ih->insert_entry(key_buf.get(), rid, nullptr);
        entries_to_build.emplace_back(key_str, rid);
        scan.next();
    }

    for (auto &col : tab.cols) {
        for (auto &col_name : col_names) {
            if (col.name == col_name) {
                col.index = true;
            }
        }
    }

    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.col_tot_len = index_col_tot_len;
    index_meta.col_num = static_cast<int>(col_names.size());
    for (auto &col : index_cols) {
        index_meta.cols.push_back(col);
    }
    tab.indexes.push_back(index_meta);

    auto mem_index = std::make_shared<MemoryIndex>();
    mem_index->meta = index_meta;
    mem_index->entries = std::map<std::string, Rid, MemoryIndexComparator>(make_mem_index_comparator(index_meta));
    for (auto &entry : entries_to_build) {
        mem_index->entries.emplace(entry.first, entry.second);
    }

    ihs_.emplace(ix_name, std::move(ih));
    mem_indexes_[ix_name] = mem_index;
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    auto index_it = tab.get_index_meta(col_names);
    std::vector<ColMeta> index_cols = index_it->cols;
    drop_index(tab_name, index_cols, context);
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    (void)context;

    std::vector<std::string> col_names;
    for (auto &col : cols) {
        col_names.push_back(col.name);
    }

    tab.get_index_meta(col_names);

    std::string ix_name = ix_manager_->get_index_name(tab_name, cols);
    auto ih_it = ihs_.find(ix_name);
    if (ih_it != ihs_.end()) {
        ix_manager_->close_index(ih_it->second.get());
        ihs_.erase(ih_it);
    }
    mem_indexes_.erase(ix_name);

    ix_manager_->destroy_index(tab_name, cols);

    auto &indexes = tab.indexes;
    for (auto it = indexes.begin(); it != indexes.end(); ++it) {
        if (it->col_num == static_cast<int>(cols.size())) {
            bool match = true;
            for (size_t i = 0; i < cols.size(); ++i) {
                if (it->cols[i].name != cols[i].name) {
                    match = false;
                    break;
                }
            }
            if (match) {
                indexes.erase(it);
                break;
            }
        }
    }

    for (auto &col : tab.cols) {
        bool still_indexed = false;
        for (const auto &index : tab.indexes) {
            for (const auto &idx_col : index.cols) {
                if (idx_col.name == col.name) {
                    still_indexed = true;
                    break;
                }
            }
            if (still_indexed) {
                break;
            }
        }
        col.index = still_indexed;
    }

    flush_meta();
}

void SmManager::rebuild_indexes(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    if (tab.indexes.empty()) {
        return;
    }

    RmFileHandle *fh = fhs_.at(tab_name).get();
    auto saved_indexes = tab.indexes;
    for (const auto &index : saved_indexes) {
        std::string ix_name = ix_manager_->get_index_name(tab_name, index.cols);
        auto ih_it = ihs_.find(ix_name);
        if (ih_it != ihs_.end()) {
            ix_manager_->close_index(ih_it->second.get());
            ihs_.erase(ih_it);
        }
        mem_indexes_.erase(ix_name);
        if (disk_manager_->is_file(ix_name)) {
            ix_manager_->destroy_index(tab_name, index.cols);
        }

        ix_manager_->create_index(tab_name, index.cols);
        auto ih = ix_manager_->open_index(tab_name, index.cols);
        auto mem_index = std::make_shared<MemoryIndex>();
        mem_index->meta = index;
        mem_index->entries = std::map<std::string, Rid, MemoryIndexComparator>(make_mem_index_comparator(index));

        RmScan scan(fh);
        std::unordered_set<std::string> seen_keys;
        while (!scan.is_end()) {
            Rid rid = {.page_no = scan.rid().page_no, .slot_no = scan.rid().slot_no};
            auto rec = fh->get_record(rid, context);
            auto key_buf = std::make_unique<char[]>(index.col_tot_len);
            int offset = 0;
            for (const auto &col : index.cols) {
                memcpy(key_buf.get() + offset, rec->data + col.offset, col.len);
                offset += col.len;
            }
            std::string key_str(key_buf.get(), key_buf.get() + index.col_tot_len);
            if (seen_keys.find(key_str) != seen_keys.end()) {
                ix_manager_->close_index(ih.get());
                ix_manager_->destroy_index(tab_name, index.cols);
                throw UniqueConstraintError(ix_name);
            }
            seen_keys.insert(key_str);
            ih->insert_entry(key_buf.get(), rid, nullptr);
            mem_index->entries.emplace(key_str, rid);
            scan.next();
        }
        ihs_[ix_name] = std::move(ih);
        mem_indexes_[ix_name] = mem_index;
    }
}

std::shared_ptr<MemoryIndex> SmManager::get_memory_index(const std::string &tab_name, const std::vector<std::string> &col_names) {
    std::string ix_name = ix_manager_->get_index_name(tab_name, col_names);
    auto it = mem_indexes_.find(ix_name);
    if (it == mem_indexes_.end()) {
        throw IndexNotFoundError(tab_name, col_names);
    }
    return it->second;
}
