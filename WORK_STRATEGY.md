# RMDB 性能优化赛题——两人并行分工工作策略

---

## 0. 文档目的与基本假设

- 当前仓库是**骨架**：8 类核心函数全是 `Todo:` 空壳（详见 `src/storage/`、`src/record/`、`src/index/`、`src/transaction/`、`src/execution/`、`src/system/`）。
- 性能优化之前必须先把正确性版本填完，**否则任何优化都是空中楼阁**。
- 两人**不能在同一文件长期并行改**——容易冲突、合并痛苦、互相等。下面按"互不重叠的模块切分 + 明确的接口契约"来分工。
- 排名的目标函数是 `load_time + first_phase_run_time + second_phase_run_time` 三个数之和。**每一秒都在拼排名**，所以**先跑起来、再迭代优化**是铁律。

---

## 1. 两人分工总览

| 阶段 | Person A（存储 + 记录 + 系统管理） | Person B（索引 + 事务 + 锁 + Parser/特性） |
|------|-----------------------------------|-------------------------------------------|
| **P0 正确性骨架（4~5 天）** | DiskManager / BufferPoolManager / RmFileHandle / RmScan / SmManager（open/close/create_table/drop_table/create_index/drop_index） | IxIndexHandle（lower_bound/insert/split/合并重分配）/ IxScan / LockManager / TransactionManager（begin/commit/abort）|
| **P1 执行器 + 新特性（4~5 天）** | SeqScan / IndexScan / Projection / NestedLoopJoin / Sort / Insert / Update / Delete 八个 Executor | Parser 扩展（LOAD 语法、UPDATE 表达式、`set output_file off`）、Analyze 扩展、LOAD 执行器、Update 表达式求值、`output_file_on` 全局开关 |
| **P2 联调与端到端（1~2 天）** | Portal 联通，harness 跑通整条 SQL 链路 | 跑 TPC-C 全量并出 load_time + 两个阶段 time |
| **P3 性能调优（按 ROI 排序，2~3 天）** | 缓冲池页级 latch / LRU→Clock / 火山模型批量化（仅 Scan）/ B+ 树二分 | 日志组提交 / 锁 NO-WAIT + 固定加锁顺序 / TPC-C 各事务特化 / bulk load + 索引排序构建 |
| **P4 压测 + 收尾（1 天）** | 反复跑 harness，定位瓶颈，回归测试 | 写最终 commit、调阈值、清理 TODO 注释 |

> **关键约束**：A、B 在同一阶段**改的文件列表不重叠**。Portal/Optimizer/rmdb.cpp 是公共面，**只能一人改完后另一人再改**。为此我们用第 2 节的 git 协作机制。

---

## 2. 协作机制

### 2.1 分支与集成

- 主分支 `main`：永远可编译、可启动、可完成正确性测试。
- A 在 `feat/storage-executor` 上工作，B 在 `feat/index-txn-feature` 上工作。
- **每 12 小时至少一次合 main**（rebase，不 merge），冲突当场解。
- 任何人改 `portal.h`、`optimizer/*`、`rmdb.cpp`、`analyze.cpp` 这些公共头/源时，**必须**在群里 @ 对方并指明改了什么。

### 2.2 接口契约（写代码前先定）

A 和 B 在 P0 启动时先在群里确认以下接口签名，**这是不可绕过的契约**——B 写索引时要按这套接口写，A 写存储层也要按这套接口写：

```cpp
// PageId / Page 已存在，不动

// DiskManager 必须实现的最小集（A 负责）
void write_page(int fd, page_id_t page_no, const char *offset, int num_bytes);
void read_page (int fd, page_id_t page_no, char *offset, int num_bytes);
void create_file(const std::string &path);
void destroy_file(const std::string &path);
int  open_file  (const std::string &path);   // 已打开则返回原 fd
void close_file (int fd);
int  get_file_size(const std::string &file_name);
bool is_file    (const std::string &path);

// BufferPoolManager（A 负责，B 改 lru_replacer 时只动 .cpp，不动 .h）
Page* fetch_page (PageId page_id);
bool  unpin_page (PageId page_id, bool is_dirty);
Page* new_page   (PageId* page_id);
bool  flush_page (PageId page_id);
void  flush_all_pages(int fd);

// RmFileHandle（A 负责）
std::unique_ptr<RmRecord> get_record   (const Rid& rid, Context*) const;
Rid  insert_record (char* buf, Context*);            // 不指定位置
void insert_record (const Rid& rid, char* buf);      // 指定位置（load 用）
void delete_record (const Rid& rid, Context*);
void update_record (const Rid& rid, char* buf, Context*);

// IxIndexHandle（B 负责）
bool  get_value    (const char* key, std::vector<Rid>* result, Transaction*);
page_id_t insert_entry(const char* key, const Rid& value, Transaction*);
bool  delete_entry (const char* key, Transaction*);
Iid   lower_bound  (const char* key);
Iid   upper_bound  (const char* key);
Iid   leaf_begin   () const;
Iid   leaf_end     () const;

// LockManager（B 负责）
bool lock_shared_on_record / lock_exclusive_on_record / lock_IS_on_table / lock_IX_on_table
bool unlock(Transaction*, LockDataId);
// 内部 LockRequestQueue 已给，B 补全条件变量 + group_lock_mode 状态机

// TransactionManager（B 负责）
Transaction* begin(Transaction*, LogManager*);   // nullptr 表示新事务
void commit(Transaction*, LogManager*);
void abort (Transaction*, LogManager*);
```

任何一方要改这层签名，先在群里贴 diff，**对端确认后再合**。

### 2.3 公共文件改造窗口

- `portal.h`、`portal.cpp`（如有）：归 A。
- `optimizer/*`：归 B 主导，A 需要加 executor 类型时在群里申请。
- `rmdb.cpp`（socket loop）：归 B（要识别 `set output_file off`）。
- `analyze.cpp`/`analyze.h`：归 B（UpdateStmt、LoadStmt 分支）。
- `parser/*`（含 lex.l / yacc.y）：归 B。

> 之所以这样切，是因为 B 三个新特性（LOAD、UPDATE 表达式、output_file off）都集中在 parser/analyze/rmdb 入口；A 集中在底层实现。**A 改完存储 B 才能改执行器；B 改完 parser A 才能改 executor**——这是 P0/P1 的依赖关系，必须串行。

---

## 3. P0 正确性骨架（4~5 天）

### 3.1 共同起步（Day 1 上午）

1. 两人一起在 `main` 上搭好分支。
2. **统一代码风格**：跑一遍 `.clang-format`（仓库已带），每人 IDE 都开 format-on-save。**避免 AI 代码风格漂移**。
3. 把 `BUFFER_POOL_SIZE`（`src/common/config.h:35`）讨论决定：默认 65536=256MB，**如果机器 ≥ 16GB 内存，建议改成 262144=1GB**。性能阶段 1GB 收益明显，但需注意内存压力。这条**今天就决定**，不要到 P3 再改。
4. 一起实现 `bitmap.h` 已在；不需要动。

### 3.2 Person A 工作清单

| 任务 | 文件 | AI 使用要点 |
|------|------|-------------|
| 实现 DiskManager 6 个方法 | `src/storage/disk_manager.cpp` | **不要让 AI 一次性写全部**。先让 AI 给 `read_page/write_page` 模板（lseek+read/write + 错误检查），自己写 `create_file`（注意 `O_EXCL` 防止重创建）和 `open_file`（维护 `path2fd_/fd2path_`），最后让 AI 补错误路径分支 |
| 实现 BufferPoolManager | `src/storage/buffer_pool_manager.cpp` | AI 适合写 `find_victim_page` + `unpin_page` 这两个工具方法。`fetch_page/new_page` 自己写，**特别注意 dirty page 写回的边界**（要先 mark dirty 再写回，不能反过来） |
| 实现 LRUReplacer | `src/replacer/lru_replacer.cpp` | 让 AI 写第一版，**自己 review list 迭代器失效问题**（erase 之后 iterator 自增会跳过元素） |
| 实现 RmFileHandle | `src/record/rm_file_handle.cpp` | 9 个方法。AI 写 `fetch_page_handle/create_new_page_handle/create_page_handle` 模板（套路高度重复），自己写 `insert_record`（注意 page_hdr 更新顺序和 `first_free_page_no` 的双向链表维护）和 `release_page_handle`（删除后页面变空闲的回插逻辑）。`update_record` 简单到可以自己写 |
| 实现 RmScan | `src/record/rm_scan.cpp` | 简单：bitmap 顺序扫 + next_bit。**AI 一把过** |
| 补 SmManager | `src/system/sm_manager.cpp` | `open_db`（读 `db.meta` 解析 + 打开所有表文件 + 打开所有索引文件）、`close_db`（对称）、`create_index`（调 ix_manager + 更新 tab.indexes + 刷 meta）、`drop_table`（删文件 + 删 tab meta + 删索引）都是模板代码，**AI 一把过** |

**A 的 P0 自测标准**：
- `src/test/performance_test` 下随便挑两个 CSV，自己写个 mini-harness 调 `create_table → insert → select → update → delete` 全跑通。
- `unit_test.cpp` 里的 `RecordManagerTest` 能过。

### 3.3 Person B 工作清单

| 任务 | 文件 | AI 使用要点 |
|------|------|-------------|
| 实现 IxNodeHandle | `src/index/ix_index_handle.cpp` | `lower_bound/upper_bound`：让 AI 写，自己加 unit test 验证；`insert/insert_pairs/erase_pair/remove`：套路清楚，AI 一把过；`internal_lookup/leaf_lookup`：调用前面三个，AI 写 |
| 实现 IxIndexHandle B+ 树核心 | `src/index/ix_index_handle.cpp` | 这是**最难的模块**，建议**不要让 AI 一次写完所有 split/coalesce/redistribute**，而是一次只让 AI 写一个函数：先 `split`，自己 debug（最常错的是分裂后父节点 key 的选择——是 `new_node` 的第一个 key，不是 old_node 的最后一个 key）；再 `insert_into_parent`（递归逻辑）；再 `coalesce_or_redistribute`；再 `coalesce/redistribute`。**每个函数都加 unit test** |
| 实现 `find_leaf_page` | `src/index/ix_index_handle.cpp` | 自带 latching 时序很微妙，**自己写**，AI 写容易踩坑（没 unpin、忘 release） |
| 实现 `insert_entry/delete_entry` | `src/index/ix_index_handle.cpp` | 自己写，逻辑链长，AI 写易漏 |
| 实现 LockManager | `src/transaction/concurrency/lock_manager.cpp` | **让 AI 写最简版：互斥 latch_ + LockRequestQueue 状态机 + 6 个加锁接口**。内部用 `std::condition_variable::wait_for` 或直接 NO-WAIT（见 P3）。**先正确再优化** |
| 实现 TransactionManager | `src/transaction/transaction_manager.cpp` | `begin`：分配 txn_id（用 `next_txn_id_++`）、写 begin log、状态设 GROWING；`commit`：写 commit log、flush log、状态 COMMITTED、释放所有 lock_set_ 中的锁；`abort`：遍历 write_set_ 倒序回滚、写 abort log、状态 ABORTED、释放锁。**AI 写，自己补 `get_transaction` 里的 `thread_id` 检查逻辑（多线程下要放开）** |
| 实现 LogManager（最小版） | `src/recovery/log_manager.cpp` | 先实现顺序写：log buffer + 一次 `fsync` 即可。**P3 再做组提交** |

**B 的 P0 自测标准**：
- `IxIndexHandleTest`（自己写）能过：建索引 → 插入 1000 条 → 单点查 + 范围查 + 删除。
- 起两个线程同时 insert 不同 key，**不崩溃**（不一定正确，但要不死锁）。
- 跑 `begin → insert 一条 → commit`，再跑 `begin → update 一条 → abort`，**能 undo**。

### 3.4 P0 集成（Day 5）

A 把 8 个 executor 暂时写成空壳（`Next()` 直接返回 `nullptr`），让 Portal 链路能跑通；B 配合改 `update set col=val` 的执行路径不报语法错。**这一步只是让 A 的存储 + B 的索引能合到一起编译过**，不要求功能完整。

---

## 4. P1 执行器 + 三个新特性（4~5 天）

### 4.1 Person A：八个 Executor

执行器里**所有方法都是空的**。这一节是**AI 一次性做透的最好场景**——套路高度雷同：构造时建上下文，`beginTuple/nextTuple/Next` 三件套。

**AI 一把过的清单**（每个 ≤ 200 行，AI 一次一个）：

1. `SeqScanExecutor`：构造时建 `RmScan(fh_)`，`beginTuple` 触发 `scan_->next()` 直到 `is_end()` 或 `fed_conds_` 命中，`nextTuple` 同理，`Next()` 返回 `fh_->get_record(rid_)`。**注意**：scan 出来的 rid 是 `RmScan::rid_`，要 `scan_` 里有状态机。
2. `IndexScanExecutor`：调 `ix_manager_->open_index` 拿 ih，构造 `lower/upper` 边界走 `IxScan`，loop 同 SeqScan。
3. `ProjectionExecutor`：直接转发 `prev_->Next()`，按 `sel_idxs_` 重新拼 record。**简单**。
4. `NestedLoopJoinExecutor`：双层循环 + `fed_conds_` 命中后 Next 返回拼接的 record。**注意**：右表每次要从 `beginTuple` 重置。**AI 一次过**。
5. `SortExecutor`：在内存里 `std::sort`，全量物化（外部排序 P3 再做）。**AI 一次过**。
6. `InsertExecutor`：已在 `executor_insert.h`，主体写好了，**自己加 flush 逻辑**（commit 时要把脏页刷盘）。
7. `UpdateExecutor`：按 `set_clauses_` 修改 record buffer。**P1 这里要支持表达式求值**——所以**先写一个 stub**（只支持 `col=val`），等 B 的表达式语法合进来后再升级。
8. `DeleteExecutor`：从 record buffer 拿 rid，调 `fh_->delete_record`，**注意要从索引中删**（ix_manager）。

**A 的 P1 自测标准**：
- 跑 5 条手写 SQL：`create table → insert × N → select → update → delete`，结果符合预期。
- `update set col=col+1`（即便还没支持表达式，至少 `set col=5` 要通）。

### 4.2 Person B：Parser 扩展 + LOAD + 输出开关

#### 4.2.1 UPDATE 表达式（最优先）

这是 TPC-C 5 个事务的**核心需求**：`set balance = balance - amount` 之类的语句占 80% 写事务。

**实施步骤**：

1. **AST 改造**（`src/parser/ast.h`）：
   - `SetClause` 改为 `col_name + shared_ptr<Expr> rhs`（不再用 `Value`）。
   - 新增 `BinaryExpr`（继承 `Expr`）：`shared_ptr<Expr> lhs, Op op, shared_ptr<Expr> rhs`，Op 为 `+ - * /`。
   - `IntLit/FloatLit/Col` 保持不变但全部继承 `Expr`。

2. **词法/语法**（`src/parser/lex.l` `src/parser/yacc.y`）：
   - lex.l 加 `+ - * /` 四个 single_op（已经有 `single_op` 模式，加进去即可）。
   - yacc.y 改 `setClause` 规则支持 `colName '=' expr`；新增 `expr` 规则 `expr '+' expr | expr '-' expr | ...`，处理运算符优先级（左结合、+ - 低、* / 高）。
   - yacc.y 改 `valueList` / `condition` 中 `expr` 的位置（保持兼容）。

3. **分析**（`src/analyze/analyze.cpp`）：
   - 新增 `convert_sv_expr` 函数：递归构造 `BinaryExpr`。
   - UpdateStmt 分支：遍历 `set_clauses`，对每个 `set_clause` 调用 `convert_sv_expr` 写到 `query->set_clauses[i].rhs`。
   - 增加 `eval_expr(expr, record, cols)` 工具：求值一个表达式，返回 `Value`。

4. **执行**（A 在 P1 已经写好 UpdateExecutor 的 stub）：
   - B 给 A 提供一个函数签名：
     ```cpp
     Value eval_set_expr(const std::shared_ptr<ast::Expr>& expr,
                         const char* record_buf,
                         const std::vector<ColMeta>& cols,
                         const std::unordered_map<std::string, size_t>& col_offset);
     ```
   - A 在 UpdateExecutor 调它，**不关心语法树**。这样 B 改 expr 不会影响 A。

5. **测试**：
   - `update t set x = x + 1.5` —— 边界 1。
   - `update t set x = y * 2 + z` —— 多运算符。
   - `update t set x = -y`（一元负号如有需要也加）。

> **AI 使用提示**：语法和分析可以**让 AI 一次写完**（因为结构化），自己 review 优先级和结合性。表达式求值是**纯计算**，AI 一次过最稳。

#### 4.2.2 LOAD 命令

**Parser 改造**：
- lex.l 加 `LOAD`、`OUTPUT`（如果用 `set output_file off` 模式）。
- yacc.y 加 `LOAD file_name INTO tbName` 规则。新增 `LoadStmt` AST 节点：`tab_name, file_path`。
- 处理相对路径：题面说 `../../src/test/performance_test/table_data/...`，需要**拼接客户端 cwd**。

**执行**（`src/execution/executor_load.h` 新文件）：
- 解析 CSV：按行 split，**不通用解析器，按表 schema 决定每列类型**。具体策略：
  - 读 CSV header，列名顺序与表定义一致。
  - 对每行：按表 schema 依次解析字段（int/float/string），构造 record buffer。
  - 调 `RmFileHandle::insert_record(rid, buf)`（**指定位置版本**，避免再扫空闲页）。
  - **关闭日志**（把 `enable_logging = false`，load 完再开 + checkpoint）。
  - 索引在 load 完所有记录后**排序构建**：把 record 按主键排序后逐个 `ix->insert_entry`。

**关键路径**：
```cpp
// pseudocode
void load(tab_name, file_path) {
    open csv;
    disable_logging();
    fh = sm->fhs_[tab_name].get();
    for each row in csv {
        parse fields by tab.cols;
        // 不走 insert_record(无 rid)，直接写到 fh 当前最后页或新页
        // 自己实现 fast_insert：维护 last_page_no / last_slot_no
    }
    // 索引排序构建
    for each index on tab {
        sort all records by index key;
        build B+ 树 bottom-up（避免分裂）
    }
    enable_logging();
    log_manager->flush_log_to_disk();  // checkpoint
}
```

> **AI 使用提示**：CSV 解析 + 排序构建是模板化代码，AI 一次过。自己 review 路径处理（`../../` 相对客户端 cwd 而非 build 目录）和错误回滚（失败时 `delete_page` 已分配页）。

#### 4.2.3 `set output_file off`

**Parser 改造**：
- 题目说**没有分号**——所以这条命令不进 yacc 语法。
- **在 rmdb.cpp 的客户端循环里**单独识别：读到的 buffer 字符串完全匹配 `"set output_file off"` 或 `"set output_file on"`，不调 `yyparse`。
- 加一个全局 `std::atomic<bool> g_output_file_on = true;`。
- 所有写 `output.txt` 的地方（`execution_manager.cpp` `show_tables` / `select_from` / `desc_table` / 异常处理路径）都加 `if (g_output_file_on)` 守卫。

> **AI 使用提示**：纯字符串处理，AI 一次过。

### 4.3 P1 集成（Day 9~10）

1. B 合到 main，A rebase 拉 B 的改动，开始 `update set col=expr` 的 executor 接入。
2. 跑 TPC-C 5 个事务的**单线程版本**——确保结果正确。
3. 再起 8 线程并发跑 30 秒——确保不死锁（不一定要快）。

---

## 5. P2 联调与端到端（1~2 天）

两个人一起做：

1. **写一个端到端 harness**（不放进 src/，单独 `test_local/`）：脚本式跑
   ```
   1. ./bin/rmdb db   # 启动服务器
   2. load 9 个 CSV
   3. 单线程跑 5 个事务各 100 次
   4. 输出 load_time, tpmC
   ```
2. **正确性回归**：
   - 9 张表 row 数符合预期。
   - NewOrder 后 o_id 自增符合。
   - Payment 后 c_balance 减少符合。
   - Delivery 后 c_balance 增加符合。
3. **多线程正确性**：8 线程跑 1000 笔事务，**所有 c_balance 总额守恒**（Payment 减的 = Delivery 加的 = 客户余额变动总和）。

---

## 6. P3 性能调优（按 ROI 排序，2~3 天）

**调优原则**：每改一项就**单独跑 5 次 harness** 取中位数，**确认不退步再合**。任何性能改动必须可回滚（用 `git revert` 或保留旧函数加 `_v2` 后缀）。

### 6.1 必做（影响 1 个数量级）

#### ① 日志组提交（**最大头**）

**位置**：`src/recovery/log_manager.cpp`

**改法**：
- 把 `flush_log_to_disk` 改成异步：commit 时 `pthread_cond_signal` 唤醒 flush 线程，flush 线程批量把 buffer 写到 `64KB` 或 `10ms` 触发一次 `fdatasync`。
- log buffer 锁用 spinlock 或 `std::mutex`，写满时 commit 阻塞。
- `fsync` → `fdatasync`（不需要写 metadata）。

**预期收益**：tpmC 翻 5~10 倍。

> **AI 使用提示**：组提交的代码量不大（~150 行），AI 一次过。自己 review 边界（buffer 满时阻塞、shutdown 时强制 flush）。

#### ② B+ 树二分查找

**位置**：`src/index/ix_index_handle.h:18` 改 `binary_search = true`。
**改法**：`lower_bound/upper_bound` 用 `std::lower_bound` + `ix_compare`。
**预期收益**：单点查询 1.5~2 倍。

#### ③ 缓冲池页级 latch

**位置**：`src/storage/buffer_pool_manager.cpp`
**改法**：
- `page_table_` 改 `std::array<std::mutex, 128> table_latches_;`，hash 选槽。
- `page.pin_count_` 改 `std::atomic<int>`。
- `replacer_` 自带 latch，不动。
- `free_list_` 改 lock-free 队列（`moodycamel::ConcurrentQueue`）或简单 mutex 队列。

**预期收益**：并发吞吐 2~4 倍。

> **AI 使用提示**：让 AI 写 hash-to-slot + atomic 改造，自己 review 死锁可能性（永远先 table_latch 再 page.pin_count）。

#### ④ `set output_file off` 路径**零开销**

**位置**：`src/execution/execution_manager.cpp:131` 起
**改法**：把 `if (g_output_file_on) { outfile.open(...); ... }` 守卫**内联到所有 fopen 处**，包括异常路径。**TPC-C 测试会发 `set output_file off`**，所以 `outfile` 对象都不构造。

**预期收益**：性能测试全量收益 10%+。

### 6.2 高 ROI

#### ⑤ LRU → Clock 或 2Q

**位置**：`src/replacer/lru_replacer.cpp`（不动 .h 接口）
**改法**：每个 frame 一个 `std::atomic<bool> ref_bit`，victim 时扫一遍找 ref=0；遇到 ref=1 清 0 继续。
**预期收益**：TPC-C 命中率从 ~85% 到 ~95%。

> **AI 使用提示**：Clock 算法 30 行代码，AI 一次过。

#### ⑥ 锁 NO-WAIT + 固定加锁顺序

**位置**：`src/transaction/concurrency/lock_manager.cpp`
**改法**：
- 加锁时若被占用**直接抛 `TransactionAbortException(DEADLOCK_PREVENTION)`**。
- NewOrder 内调用顺序固定：`warehouse → district → customer → items → stock → orders → order_line → new_orders → warehouse(末尾再更新)`。
- Payment 内：`warehouse → district → customer (last) → history`。
- Delivery 内：`district → new_orders → orders → order_line → stock → customer`。

**预期收益**：abort 率从 5% 降到 < 0.5%，重做代价几乎为 0。

#### ⑦ Bulk load + 索引排序构建

**位置**：B 写的 executor_load
**改法**：见 4.2.2 描述。
**预期收益**：load_time 减半。

### 6.3 中 ROI

#### ⑧ TPC-C 各事务特化

**位置**：`src/optimizer/planner.cpp`（B 主导）
**改法**：
- OrderStatus 的 `ORDER BY o_id DESC LIMIT 1` 改用 `IxIndexHandle::upper_bound` 反向扫。
- StockLevel 的 `WHERE s_i_id IN (...)` 改用批量 `get_value` 一次性拿多 rid。
- Delivery 单事务内多 district 串行处理，复用同一 warehouse 锁。

**预期收益**：OrderStatus 和 StockLevel 提速 30%。

#### ⑨ 火山模型批量化（仅 SeqScan）

**位置**：`src/execution/executor_seq_scan.h`
**改法**：一次拉一批 64 条放到 `std::vector<std::unique_ptr<RmRecord>>`，上层一次取一条但内部避免重复虚函数。
**预期收益**：OrderStatus 范围读提速 20%。

> **AI 使用提示**：只改 Scan，其他算子保持火山模型。**改动控制在小范围**。

### 6.4 锦上添花

- **缓冲池加大到 1GB**（如果 P0 用了 256MB）—— 改 `BUFFER_POOL_SIZE` 一行。
- **`fsync` → `fdatasync`**（log_manager）—— 每次节省 metadata 写。
- **`O_DIRECT` 模式磁盘读**（`disk_manager.cpp`）—— 绕过 page cache，**在 SSD 上可能更快也可能更慢**，先 benchmark 再决定。
- **多端口 / epoll**（`rmdb.cpp`）—— 当前一个客户端一个线程，并发 8 时线程切换成本可接受。**除非机器 ≥ 32 核**，否则不要碰。

---

## 7. AI 协作方法（关键）

> 默认两人都会用 AI。这节写**怎么用 AI 才能不掉质量**。

### 7.1 一次性做透的场景（推荐）

| 场景 | 提示词要点 |
|------|-----------|
| B+ 树 split | 给 AI 看 `IxNodeHandle::insert_pairs`，让它写 `split`，但**强调**：分裂后父节点要插入的 key 是 `new_node` 的**第一个** key，不是 old_node 的最后一个。AI 经常错这里 |
| LockManager 6 个加锁接口 | 给 AI 看 LockDataId 和 LockRequestQueue 定义，让它一次写完，自己 review 死锁 |
| 8 个 Executor | 每个 ≤ 200 行，**每个文件单独问 AI**，不要一次问 8 个（容易让 AI 偷懒） |
| CSV 解析 | 给 AI 表 schema，让它写通用解析器。但**注意 string 字段不需要 unescape 引号**（TPC-C 数据无） |
| 日志组提交 | 给 AI 看 log_buffer 的现状，让它改异步 flush 线程。**强调 shutdown 时强制 flush** |

### 7.2 必须自己写的场景

- **IxIndexHandle 的 find_leaf_page**（并发语义微妙）
- **事务 commit/abort**（涉及 log、lock_set、write_set 的顺序，错了回滚不全）
- **Portal 入口**（要正确处理 throw 的所有异常）
- **B+ 树 insert_entry**（逻辑链长，AI 易漏 unpin）

### 7.3 避免"AI 痕迹"的几条铁律

1. **变量命名一致性**：用仓库已有的命名（`tab_name_`、`fh_`、`rid_`）。AI 喜欢用 `table_name`、`file_handle` 这种——**自己改成一致**。
2. **不要让 AI 加注释**：仓库的注释都是简短的"做什么"，AI 喜欢加"为什么"的长注释。**删掉 AI 加的多余注释**。
3. **缩进与格式**：`.clang-format` 跑一遍。
4. **异常处理风格**：用仓库已有的 `throw XXXError()` 而非 AI 偏好的 `std::runtime_error`。
5. **不要 AI 重构已有函数**：只让 AI 写新函数，**不要让它改你已经写对的函数**——AI 改完经常引入 bug。
6. **每个 AI 输出必须编译 + 跑相关单测**才能合。

### 7.4 Prompt 模板

> 给 AI 的标准 prompt（两人共用，存在 `docs/prompts.md`）：

```
你正在给 RMDB 数据库（人大 TPC-C 比赛项目）补 [模块名] 的实现。
约束：
1. 严格遵循现有代码风格：snake_case 成员加 _，tab_name_/fh_/rid_ 等命名一致
2. 只在 [文件] 中修改，不要碰其他文件
3. 写完后必须满足 [接口签名]
4. 异常用 [错误类型]
5. 不要加注释，除非解释 why
6. 不要用 C++20 特性，保持 C++17
7. 不要重构已有代码
输入：[粘贴相关 .h 和现有 stub]
输出：[完整 .cpp]
```

---

## 8. 性能调优流程

每周（其实每两天）走一遍：

```
1. 跑 5 次 harness，取中位数
2. perf record -g 找热点
3. 选 1~2 个优化点（不贪多）
4. 改 → 单测 → 端到端 → 性能对比
5. 提升 ≥ 10% 才合，否则 revert
6. 写 changelog（一人记）
```

`perf top` 输出样例（假设）：
```
  35%  disk_manager::write_page    ← 日志热点，组提交能压到 5%
  18%  buffer_pool_manager::fetch_page   ← 锁竞争
  12%  rm_file_handle::insert_record     ← bitmap 操作，inline 优化
   8%  ix_index_handle::insert_entry     ← B+ 树分裂
   7%  lru_replacer::victim
  ...
```

按这个就能定调优顺序。

---

## 9. 风险与回退

| 风险 | 触发条件 | 回退方案 |
|------|---------|----------|
| AI 写的 B+ 树有 bug 但 unit test 漏了 | 端到端跑挂 | `git revert` 最近一个 commit |
| 日志组提交丢数据 | 突然断电 | 临时关组提交走原 sync 路径；后续用 `fdatasync` + 检验和 |
| 缓冲池页级 latch 引入死锁 | 压测时 hang | 加超时 + watchdog 线程；不行就回退全局 latch |
| 锁 NO-WAIT 大量 abort | tpmC 不升反降 | 改 Wound-Wait（按时间戳） |
| 调优后某一阶段时间变长 | 性能回归 | 单阶段独立测，确认只动了一个变量 |

**强制**：每次大改前 `git tag save-point-xxx`；改后跑通就留着，跑不通就 `git reset --hard save-point-xxx`。

---

## 10. 交付物清单

代码交付时一并提交：

1. `CHANGELOG.md`：每个 commit 写"性能提升 X%，对应 tpmC +Y"。
2. `PERF.md`：最终的 load_time / phase1 time / phase2 time / tpmC 数字。
3. `ARCH.md`：架构说明（人写的，不准 AI 写）。
4. 所有 unit test 通过。
5. 9 张表的 `create table` 语句（harness 用）。

---

## 11. 时间线（紧凑版）

| 时间 | 节点 |
|------|------|
| Day 1 | 分支、风格、BUFFER_POOL_SIZE 决定 |
| Day 2~5 | P0 正确性骨架（A 存储+执行器 stub，B 索引+锁+事务） |
| Day 6~10 | P1 八个 Executor + 三个新特性 |
| Day 11~12 | P2 联调、正确性回归、harness 跑通 |
| Day 13~15 | P3 性能调优（按 ROI 顺序） |
| Day 16 | 压测、写文档、清理 |
| Day 17 | 留 buffer 给临场修 bug |

> 17 天是中等水平节奏。如果想稳一点，把 Day 1~2 拉长到 3 天做 P0 review（两人互相读对方的代码）。

---

## 12. 最后的几条

1. **正确性第一**：性能优化全部建立在"跑得对"之上。任何优化前先有 unit test。
2. **每一次 commit 跑一次 harness**：哪怕只改了 5 行。
3. **AI 是加速器不是替代品**：让 AI 写模板代码，自己 review 边界。
4. **两人每天 15 分钟 sync**：说说今天改了哪些接口、明天要做什么、卡在哪里。
5. **保持冷静**：TPC-C 调优的边际收益是递减的。**前 3 个优化吃掉 80% 收益**，后面都是 1% 1% 抠。不值得为 1% 重写整个执行器。

祝两人协作顺利，排名靠前。
