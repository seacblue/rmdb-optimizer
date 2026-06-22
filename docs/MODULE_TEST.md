# RMDB 模块测试指引 & CI 规范

> **本文档面向团队协作场景**，说明如何对 RMDB 底层模块进行**独立单元测试**，
> 以及项目 CI 的验收规则。
>
> 编写者：Person A 已完成 LRUReplacer 和 DiskManager 的实现及测试，
> 以下内容供团队成员参考，确保各模块可独立验证并满足 CI 门槛。

---

## 1. 核心理念

### 为什么需要模块级测试？

| 原因 | 说明 |
|------|------|
| **独立验证** | 各模块（LRUReplacer / DiskManager / BPM / B+树 / LockManager ...）可以独立于上层逻辑进行验证 |
| **快速定位** | 集成测试失败时，能立刻知道是哪个模块出错 |
| **团队协作** | A 和 B 在不同分支开发，各自跑通自己的测试再合 main，避免互相阻塞 |
| **CI 强制** | GitHub Actions 会检查**每个模块是否有关联测试**，以及**行覆盖率是否达标**，不满足直接拒绝 PR |

### 测试架构

```
src/test/
├── CMakeLists.txt              ← 注册所有测试目标 + CTest
├── test_utils.h                ← 共享工具（目录管理、随机缓冲区）
├── test_lru_replacer.cpp       ← LRUReplacer 单元测试（Person A）
├── test_disk_manager.cpp       ← DiskManager 单元测试（Person A）
├── test_buffer_pool.cpp        ← (Person A 待添加)
├── test_rm_file_handle.cpp     ← (Person A 待添加)
├── test_ix_index_handle.cpp    ← (Person B 待添加)
├── test_lock_manager.cpp       ← (Person B 待添加)
└── test_transaction.cpp        ← (Person B 待添加)
```

> **重要**：不再使用单个 `module_test.cpp` 大文件。每个模块独立文件、独立编译目标。

### CI 强制规则

CI 会做以下三项检查，**任何一项不通过 → PR 被拒绝**：

| # | 检查项 | 规则 |
|---|--------|------|
| 1 | **模块测试完备性** | 每个 `add_library(xxx)` 必须有关联的 `test_xxx` 目标 |
| 2 | **全部测试通过** | `ctest --output-on-failure` 零失败 |
| 3 | **行覆盖率 ≥ 阈值** | 每个模块 ≥ 80% 行覆盖率（可在 `scripts/ci_check.sh` 中调整） |

---

## 2. 如何运行模块测试

### 本地运行（WSL / Linux）

```bash
# 1. 构建（若已 cmake 过则直接 make）
cd /home/seako/rmdb/build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make test_lru -j$(nproc)
make test_dm -j$(nproc)

# 2. 运行单个测试
./bin/test_lru
./bin/test_dm

# 3. 过滤特定用例
./bin/test_lru --gtest_filter=*Victim*
./bin/test_dm --gtest_filter=DiskManagerTest.CreateFile

# 4. 批量运行全部模块测试
make run_all_tests
# 或
ctest --output-on-failure
```

### 覆盖率本地预览

```bash
cd /home/seako/rmdb/build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
make -j$(nproc)
ctest --output-on-failure
bash ../scripts/ci_check.sh
# 打开覆盖率 HTML 报告
# 如果 WSL 有浏览器：  xdg-open coverage_report/html/index.html
# 否则复制到 Windows： cp -r coverage_report/html /mnt/c/Users/YourName/Desktop/
```

---

## 3. 如何添加新模块测试

### 四步法（以 BufferPoolManager 为例）

#### Step 1：创建测试文件

创建 `src/test/test_buffer_pool.cpp`：

```cpp
#include <gtest/gtest.h>
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"

class BufferPoolManagerTest : public ::testing::Test {
   protected:
    void SetUp() override { /* ... */ }
    void TearDown() override { /* ... */ }
};

TEST_F(BufferPoolManagerTest, NewPage) {
    // ...
}
```

#### Step 2：在 `src/test/CMakeLists.txt` 中注册

```cmake
add_executable(test_bpm test_buffer_pool.cpp)
target_link_libraries(test_bpm storage lru_replacer ${TEST_LIBS})
target_include_directories(test_bpm PRIVATE ${TEST_INCLUDES})
add_test(NAME test_bpm COMMAND test_bpm
    WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
```

> 文件中已有模板（被注释掉了），取消注释并修改链接库即可。

#### Step 3：在 `scripts/ci_check.sh` 中添加该模块

```bash
MODULES=(
    "lru_replacer:test_lru:src/replacer:80"
    "storage:test_dm:src/storage:80"
    "buffer_pool:test_bpm:src/storage:80"   # ← 追加这一行
    # ...
)
```

格式：`库名:测试目标名:源文件目录:最低覆盖率(%)`

#### Step 4：编译 & 验证

```bash
cd build && cmake .. && make test_bpm -j$(nproc) && ./bin/test_bpm
```

本地跑通后提交 PR，CI 会自动运行并通过三项检查。

---

## 4. 已完成的模块状态

### ✅ LRUReplacer（已完成）

| 测试用例 | 验证内容 | 状态 |
|----------|---------|------|
| `BasicVictimOrder` | unpin 1,2,3 → victim 依次得到 1,2,3 | ✅ |
| `EmptyReplacer` | 空 replacer victim 返回 false | ✅ |
| `PinRemovesFrame` | pin 后 Size 减少，被 pin 的 frame 不会被淘汰 | ✅ |
| `PinNonExistent` | pin 不存在 frame 不报错 | ✅ |
| `DuplicateUnpinIsIdempotent` | 重复 unpin 同一 frame 不会重复加入 | ✅ |
| `FullLifecycle` | unpin→pin→unpin 完整生命周期 | ✅ |
| `ComprehensiveScenario` | 综合随机场景 | ✅ |
| `HighVolume` | 10000 个 frame 批量操作 | ✅ |
| `ConcurrentStress` | 4 线程并发，不死锁即通过 | ✅ |

**文件**: `src/test/test_lru_replacer.cpp` · **编译**: `make test_lru -j$(nproc)` · **运行**: `./bin/test_lru`

### ✅ DiskManager（已完成）

| 测试用例 | 验证内容 | 状态 |
|----------|---------|------|
| `CreateFile` | 创建文件后 is_file 返回 true | ✅ |
| `CreateDuplicateFile` | 重复创建抛 FileExistsError | ✅ |
| `OpenFile` | 打开文件返回合法 fd，重复打开返回相同 fd | ✅ |
| `CloseFile` | 关闭后可以再次打开 | ✅ |
| `CloseNotOpenFile` | 关闭未打开文件抛 FileNotOpenError | ✅ |
| `WriteReadPage` | 写入一页再读出，数据完全一致 | ✅ |
| `MultiplePages` | 16 页批量读写 | ✅ |
| `AllocatePage` | 页号自增分配 | ✅ |
| `FileSize` | 写页后文件大小 = PAGE_SIZE | ✅ |
| `DestroyFile` | 删除文件后 is_file 返回 false | ✅ |
| `DestroyOpenFile` | 删除未关闭文件抛 FileNotClosedError | ✅ |
| `DestroyNonExistent` | 删除不存在文件抛 FileNotFoundError | ✅ |
| `FileNameFdMapping` | get_file_name / get_file_fd 双向映射 | ✅ |

**文件**: `src/test/test_disk_manager.cpp` · **编译**: `make test_dm -j$(nproc)` · **运行**: `./bin/test_dm`

---

## 5. CI 基础设施

### 工作流文件

`.github/workflows/ci.yml` — 每次 push/PR 时自动运行：

```
检出代码 → 安装依赖(flex/bison/lcov) → cmake配置(含覆盖率) →
编译全部目标 → ctest运行测试 → ci_check.sh检查模块+覆盖率 →
上传HTML覆盖率报告
```

### 核心检查脚本

`scripts/ci_check.sh` — 三阶段检查：

| 阶段 | 检查内容 | 失败后果 |
|------|---------|---------|
| 0 | 前置条件（构建目录、lcov、二进制） | 跳过后续检查 |
| 1 | 模块测试完备性 — 每个库都有对应测试？ | ❌ CI 拒绝 |
| 2 | 全部测试通过 — ctest 零失败 | ❌ CI 拒绝 |
| 3 | 各模块行覆盖率 ≥ 阈值（默认 80%） | ❌ CI 拒绝 |

### 覆盖率阈值配置

编辑 `scripts/ci_check.sh` 中的 `MODULES` 数组，每行最后一列即最低行覆盖率：

```bash
"lru_replacer:test_lru:src/replacer:80"   # 80%
"storage:test_dm:src/storage:75"           # 75%（可根据模块复杂程度调整）
```

### 覆盖率报告

CI 通过后，可以在 GitHub Actions 页面下载 `coverage-report` 构件（artifact），
用浏览器打开 `index.html` 即可看到每个文件的逐行覆盖情况。

---

## 6. 测试编写原则

| 原则 | 说明 |
|------|------|
| **单一断言** | 每个 `TEST` 只验证一个行为，失败时立刻知道问题所在 |
| **独立环境** | `SetUp`/`TearDown` 创建独立环境，互不影响 |
| **边界覆盖** | 不仅测试正常路径，还要测试空、满、重复、不存在等边界 |
| **不依赖其他模块** | 测试 LRUReplacer 时不需要 DiskManager 实现正确 |
| **覆盖率导向** | 写测试时打开覆盖率预览，确保新代码被覆盖 |

---

## 7. CI 本地模拟

在提交 PR 前，建议在本地完整跑一遍 CI 流程：

```bash
# 确保在项目根目录
cd /home/seako/rmdb

# 1. 全量构建
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
make -j$(nproc)

# 2. 运行所有模块测试
ctest --output-on-failure

# 3. 运行 CI 检查脚本
bash ../scripts/ci_check.sh

# 如果全部通过，说明本地 CI 已 PASS，可以安全提交 PR
```

---

## 8. 常见问题

### Q: CI 报错 "模块 xxx → 缺少测试文件"

说明你添加了新模块但没有写测试。

**解决方案**：按照第 3 节的四步法添加测试。

### Q: CI 报错 "行覆盖率 45% < 80%"

说明测试用例覆盖的代码路径不够。

**解决方案**：
1. 查看覆盖率报告 HTML，找到未覆盖的代码行
2. 增加对应场景的测试用例
3. 重新提交

### Q: 编译报错 `undefined reference to ...`

检查 `src/test/CMakeLists.txt` 中 `target_link_libraries` 是否包含所需库。

### Q: 本地没有 lcov

```bash
sudo apt-get install lcov
```

lcov 仅在 CI 和本地预览覆盖率时需要，如果只是跑测试则不需要。

---

## 附录：文件结构速查

| 文件 | 用途 |
|------|------|
| `.github/workflows/ci.yml` | GitHub Actions 工作流定义 |
| `scripts/ci_check.sh` | CI 核心检查（模块完备性 + 覆盖率阈值） |
| `cmake/EnableCoverage.cmake` | gcov 编译标志模块 |
| `src/test/CMakeLists.txt` | 所有模块测试的 CMake 注册 |
| `src/test/test_utils.h` | 共享测试工具函数 |
| `src/test/test_lru_replacer.cpp` | LRUReplacer 测试（Person A） |
| `src/test/test_disk_manager.cpp` | DiskManager 测试（Person A） |
  - lru_replacer → LRUReplacer
  - record       → RmFileHandle, RmScan（如有需要）
  - gtest_main   → Google Test 框架
```

### Q: 测试文件残留

`DiskManagerTest` 的 `SetUp/TearDown` 会自动创建和清理 `module_test_dir` 目录。
如果测试中途崩溃导致残留，手动删除即可：

```bash
rm -rf module_test_dir
```

### Q: 如何在 WSL 下运行

项目本身需要在 Linux/WSL 环境下编译运行（因为使用了 Linux 系统调用如 `lseek`、`open`、`unlink`）。
确保已在 WSL 中安装 GCC、CMake、flex、bison、readline。
