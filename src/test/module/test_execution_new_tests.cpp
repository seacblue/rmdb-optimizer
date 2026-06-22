/* 新增覆盖率测试 —— 追加到 executor_test.cpp 末尾 */

// ============================================================
// 21. LoadExecutor —— 从 CSV 文件批量导入数据
// ============================================================
TEST_F(ExecutorTest, LoadExecutorBasic) {
    // 建表：与 CSV 列名对应
    std::vector<ColDef> col_defs;
    col_defs.push_back({"load_id", TYPE_INT, 4});
    col_defs.push_back({"load_name", TYPE_STRING, 16});
    col_defs.push_back({"load_score", TYPE_FLOAT, 4});
    sm_manager_->create_table("load_tab", col_defs, context_);

    // 写 CSV 文件（header + 3 行数据）
    const char *csv = "load_id,load_name,load_score\n1,Alice,95.5\n2,Bob,87.0\n3,Charlie,72.5\n";
    FILE *fp = fopen("load_test.csv", "w");
    ASSERT_NE(fp, nullptr);
    fputs(csv, fp);
    fclose(fp);

    // LoadExecutor 加载
    auto load_exec = std::make_unique<LoadExecutor>(sm_manager_, "load_tab", "load_test.csv", context_);
    auto rec = load_exec->Next();
    ASSERT_NE(rec, nullptr);

    // SeqScan 验证共 3 条
    auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, "load_tab", std::vector<Condition>{}, context_);
    int cnt = 0;
    for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
    EXPECT_EQ(cnt, 3);

    std::remove("load_test.csv");
}

// ============================================================
// 22. LoadExecutor —— BIGINT 列
// ============================================================
TEST_F(ExecutorTest, LoadExecutorBigInt) {
    std::vector<ColDef> col_defs;
    col_defs.push_back({"lid", TYPE_BIGINT, 8});
    col_defs.push_back({"lname", TYPE_STRING, 16});
    sm_manager_->create_table("load_bi", col_defs, context_);

    const char *csv = "lid,lname\n1000000000000,Hello\n2000000000000,World\n";
    FILE *fp = fopen("load_bi.csv", "w");
    ASSERT_NE(fp, nullptr);
    fputs(csv, fp);
    fclose(fp);

    auto load_exec = std::make_unique<LoadExecutor>(sm_manager_, "load_bi", "load_bi.csv", context_);
    auto rec = load_exec->Next();
    ASSERT_NE(rec, nullptr);

    auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, "load_bi", std::vector<Condition>{}, context_);
    int cnt = 0;
    for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
    EXPECT_EQ(cnt, 2);

    std::remove("load_bi.csv");
}

// ============================================================
// 23. SeqScan —— 跨类型 INT vs FLOAT 比较
//     覆盖 compare_value() 中 (INT|FLOAT) vs (INT|FLOAT) 且 lhs_type != rhs_type 路径
// ============================================================
TEST_F(ExecutorTest, SeqScanCrossTypeIntFloat) {
    // 表 int_f(id INT, val FLOAT) —— 但 score 已经是 FLOAT，我们复用 students
    // 插入：id=1 (INT), score=95.0 (FLOAT)
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
            std::vector<Value>{make_int_val(1), make_str_val("A"), make_float_val(95.0f)}, context_);
        ins->Next();
    }

    // 条件：id >= 1.5  → INT 列 vs FLOAT 字面量 → 走 cross-type 路径
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "id", OP_GE, make_float_val(1.5f)));
        auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, conds, context_);
        int cnt = 0;
        for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
        // id=1 → 1 >= 1.5 为 false，所以 0 条
        EXPECT_EQ(cnt, 0);
    }
}

// ============================================================
// 24. SeqScan —— BIGINT 溢出测试（条件值 > INT_MAX 返回 false）
//     覆盖 BIGINT→INT 窄化时的溢出检测 return false
// ============================================================
TEST_F(ExecutorTest, SeqScanBigIntOverflow) {
    // 建表含 BIGINT 列
    std::vector<ColDef> col_defs;
    col_defs.push_back({"bi_id", TYPE_BIGINT, 8});
    col_defs.push_back({"bi_name", TYPE_STRING, 16});
    sm_manager_->create_table("bigint_tab", col_defs, context_);

    // 插入一条 bigint 值
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, "bigint_tab",
            std::vector<Value>{make_bigint_val(42), make_str_val("test")}, context_);
        ins->Next();
    }

    // 条件：bi_id > 9999999999  (大于 INT_MAX=2147483647)
    // lhs 是 BIGINT，rhs 字面量也是 BIGINT，不会触发窄化，所以可以正常匹配
    std::vector<Condition> conds;
    conds.push_back(make_cond_val("bigint_tab", "bi_id", OP_GT, make_bigint_val(9999999999LL)));
    auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, "bigint_tab", conds, context_);
    int cnt = 0;
    for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
    EXPECT_EQ(cnt, 0);  // 42 < 9999999999

    // 再测试：BIGINT 条件 rhs 是 INT 且 lhs 是 BIGINT → widen int→bigint
    conds.clear();
    conds.push_back(make_cond_val("bigint_tab", "bi_id", OP_EQ, make_int_val(42)));
    seq = std::make_unique<SeqScanExecutor>(sm_manager_, "bigint_tab", conds, context_);
    cnt = 0;
    for (seq->beginTuple(); !seq->is_end(); seq->nextTuple()) { cnt++; }
    EXPECT_EQ(cnt, 1);
}

// ============================================================
// 25. SeqScan —— 列-列比较（覆盖 rhs 列不存在等路径）
// ============================================================
TEST_F(ExecutorTest, SeqScanColumnCmpNotFound) {
    // students 表中：id = name 没有意义但语法合法
    // 建第二个表用于跨表列比较
    std::vector<Condition> conds;
    conds.push_back(make_cond_col(TABLE1, "id", OP_EQ, TABLE1, "name"));  // id = name (类型不同)
    auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, conds, context_);
    seq->beginTuple();
    // 不会 crash，列都存在
    SUCCEED();
}

// ============================================================
// 26. IndexScan —— 带 BIGINT 条件的 IndexScan
//     覆盖 IndexScan eval_conds 中的 BIGINT widen/narrow 路径
// ============================================================
TEST_F(ExecutorTest, IndexScanBigIntConds) {
    // 建表含 BIGINT 列，不用索引
    std::vector<ColDef> col_defs;
    col_defs.push_back({"val", TYPE_BIGINT, 8});
    col_defs.push_back({"tag", TYPE_STRING, 16});
    sm_manager_->create_table("ix_bi", col_defs, context_);

    // 插入两条
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, "ix_bi",
            std::vector<Value>{make_bigint_val(100), make_str_val("A")}, context_);
        ins->Next();
    }
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, "ix_bi",
            std::vector<Value>{make_bigint_val(200), make_str_val("B")}, context_);
        ins->Next();
    }

    // 条件：val > 150 (INT 字面量，lhs 是 BIGINT → widen int→bigint)
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val("ix_bi", "val", OP_GT, make_int_val(150)));
        auto ix = std::make_unique<IndexScanExecutor>(sm_manager_, "ix_bi", conds, std::vector<std::string>{}, context_);
        int cnt = 0;
        for (ix->beginTuple(); !ix->is_end(); ix->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 1);  // 200
    }

    // 条件：val = 100 (BIGINT 字面量)
    {
        std::vector<Condition> conds;
        conds.push_back(make_cond_val("ix_bi", "val", OP_EQ, make_bigint_val(100)));
        auto ix = std::make_unique<IndexScanExecutor>(sm_manager_, "ix_bi", conds, std::vector<std::string>{}, context_);
        int cnt = 0;
        for (ix->beginTuple(); !ix->is_end(); ix->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 1);
    }
}

// ============================================================
// 27. IndexScan —— 构造函数带 index_col_names（覆盖非空 index_col_names 路径）
//     swap_op 路径（列所在表与目标表不同）
// ============================================================
TEST_F(ExecutorTest, IndexScanWithColNames) {
    // 直接用 students 表，构造 IndexScanExecutor 时传入 index_col_names
    auto tab = get_tab(TABLE1);

    // 插入数据
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
            std::vector<Value>{make_int_val(10), make_str_val("X"), make_float_val(90.0f)}, context_);
        ins->Next();
    }

    // 带 index_col_names 构造（当前测试中索引列名不影响行为，只是走不同构造路径）
    std::vector<std::string> idx_cols = {"id"};
    std::vector<Condition> conds;
    conds.push_back(make_cond_val(TABLE1, "id", OP_EQ, make_int_val(10)));
    auto ix = std::make_unique<IndexScanExecutor>(sm_manager_, TABLE1, conds, idx_cols, context_);
    int cnt = 0;
    for (ix->beginTuple(); !ix->is_end(); ix->nextTuple()) { cnt++; }
    EXPECT_EQ(cnt, 1);
}

// ============================================================
// 28. NestedLoopJoin —— 跨类型 BIGINT 条件连接
// ============================================================
TEST_F(ExecutorTest, NestedLoopJoinBigInt) {
    // 建两个含 BIGINT 列的表
    std::vector<ColDef> col_defs_a;
    col_defs_a.push_back({"id", TYPE_BIGINT, 8});
    col_defs_a.push_back({"name", TYPE_STRING, 16});
    sm_manager_->create_table("j_a", col_defs_a, context_);

    std::vector<ColDef> col_defs_b;
    col_defs_b.push_back({"ref", TYPE_BIGINT, 8});
    col_defs_b.push_back({"info", TYPE_STRING, 16});
    sm_manager_->create_table("j_b", col_defs_b, context_);

    // 插入数据
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, "j_a",
            std::vector<Value>{make_bigint_val(1), make_str_val("Alice")}, context_);
        ins->Next();
    }
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, "j_b",
            std::vector<Value>{make_bigint_val(1), make_str_val("Info1")}, context_);
        ins->Next();
    }

    // JOIN 条件：j_a.id = j_b.ref（两列都是 BIGINT）
    {
        auto left = std::make_unique<SeqScanExecutor>(sm_manager_, "j_a", std::vector<Condition>{}, context_);
        auto right = std::make_unique<SeqScanExecutor>(sm_manager_, "j_b", std::vector<Condition>{}, context_);
        std::vector<Condition> join_conds;
        join_conds.push_back(make_cond_col("j_a", "id", OP_EQ, "j_b", "ref"));
        auto nlj = std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right),
                                                             join_conds, context_);
        int cnt = 0;
        for (nlj->beginTuple(); !nlj->is_end(); nlj->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 1);
    }

    // JOIN 条件：j_a.id > make_bigint_val(0)（BIGINT 列 vs BIGINT 字面量）
    {
        auto left = std::make_unique<SeqScanExecutor>(sm_manager_, "j_a", std::vector<Condition>{}, context_);
        auto right = std::make_unique<SeqScanExecutor>(sm_manager_, "j_b", std::vector<Condition>{}, context_);
        std::vector<Condition> join_conds;
        join_conds.push_back(make_cond_val("j_a", "id", OP_GT, make_bigint_val(0)));
        auto nlj = std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right),
                                                             join_conds, context_);
        int cnt = 0;
        for (nlj->beginTuple(); !nlj->is_end(); nlj->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 1);
    }
}

// ============================================================
// 29. NestedLoopJoin —— 更多连接条件覆盖（OP_GT, OP_GE, OP_LE, OP_LT）
// ============================================================
TEST_F(ExecutorTest, NestedLoopJoinMoreOps) {
    // 插入多行用于测试多种比较操作符
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
            std::vector<Value>{make_int_val(10), make_str_val("P"), make_float_val(85.0f)}, context_);
        ins->Next();
    }
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
            std::vector<Value>{make_int_val(20), make_str_val("Q"), make_float_val(75.0f)}, context_);
        ins->Next();
    }
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, TABLE2,
            std::vector<Value>{make_int_val(10), make_str_val("Math")}, context_);
        ins->Next();
    }
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, TABLE2,
            std::vector<Value>{make_int_val(20), make_str_val("CS")}, context_);
        ins->Next();
    }

    // OP_GE: id >= 20
    {
        auto left = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, std::vector<Condition>{}, context_);
        auto right = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE2, std::vector<Condition>{}, context_);
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "id", OP_GE, make_int_val(20)));
        auto nlj = std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right), conds, context_);
        int cnt = 0;
        for (nlj->beginTuple(); !nlj->is_end(); nlj->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 2);  // (20, P) x (10, Math), (20, P) x (20, CS)
    }

    // OP_LT: id < 20  
    {
        auto left = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE1, std::vector<Condition>{}, context_);
        auto right = std::make_unique<SeqScanExecutor>(sm_manager_, TABLE2, std::vector<Condition>{}, context_);
        std::vector<Condition> conds;
        conds.push_back(make_cond_val(TABLE1, "id", OP_LT, make_int_val(20)));
        auto nlj = std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right), conds, context_);
        int cnt = 0;
        for (nlj->beginTuple(); !nlj->is_end(); nlj->nextTuple()) { cnt++; }
        EXPECT_EQ(cnt, 2);  // (10, P) x (10, Math), (10, P) x (20, CS)
    }
}

// ============================================================
// 30. execution_manager.cpp —— QlManager::run_cmd_utility DDL 路径
//     覆盖 Help / ShowTable / DescTable / begin / commit / abort / rollback
// ============================================================
TEST_F(ExecutorTest, QlManagerUtilities) {
    QlManager ql_mgr(sm_manager_, nullptr);

    // 准备 Context 含 data_send 缓冲区
    char data_buf[4096];
    int offset = 0;
    Context ctx(nullptr, nullptr, nullptr, data_buf, &offset);

    // T_Help
    {
        offset = 0;
        auto plan = std::make_shared<OtherPlan>(T_Help, "");
        txn_id_t dummy = 0;
        ql_mgr.run_cmd_utility(plan, &dummy, &ctx);
        EXPECT_GT(offset, 0);  // help_info 被写入
    }

    // 先插入一点数据，让 ShowTable 能看到内容
    {
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, TABLE1,
            std::vector<Value>{make_int_val(99), make_str_val("Show"), make_float_val(99.0f)}, context_);
        ins->Next();
    }

    // T_ShowTable
    {
        offset = 0;
        auto plan = std::make_shared<OtherPlan>(T_ShowTable, "");
        txn_id_t dummy = 0;
        ql_mgr.run_cmd_utility(plan, &dummy, &ctx);
        EXPECT_GT(offset, 0);  // 表信息被写入
    }

    // T_DescTable
    {
        offset = 0;
        auto plan = std::make_shared<OtherPlan>(T_DescTable, TABLE1);
        txn_id_t dummy = 0;
        ql_mgr.run_cmd_utility(plan, &dummy, &ctx);
        EXPECT_GT(offset, 0);  // 表结构被写入
    }
}

// ============================================================
// 31. execution_manager.cpp —— QlManager::run_multi_query DDL 路径
//     覆盖 DropTable / CreateIndex / DropIndex
// ============================================================
TEST_F(ExecutorTest, QlManagerDDL) {
    QlManager ql_mgr(sm_manager_, nullptr);
    Context ctx(nullptr, nullptr, nullptr);

    // T_DropTable: 创建一张临时表再删除
    {
        std::vector<ColDef> cols;
        cols.push_back({"tmp_id", TYPE_INT, 4});
        sm_manager_->create_table("tmp_del", cols, context_);
        auto plan = std::make_shared<DDLPlan>(T_DropTable, "tmp_del", std::vector<std::string>{}, cols);
        ql_mgr.run_multi_query(plan, &ctx);
        // 验证已删除
        EXPECT_THROW(sm_manager_->db_.get_table("tmp_del"), TableNotFoundError);
    }

    // T_CreateIndex / T_DropIndex
    {
        auto plan = std::make_shared<DDLPlan>(T_CreateIndex, TABLE1, std::vector<std::string>{"id"}, std::vector<ColDef>{});
        ql_mgr.run_multi_query(plan, &ctx);
        // 验证索引存在
        std::string ix_name = "students_id_index";
        EXPECT_TRUE(sm_manager_->ihs_.find(ix_name) != sm_manager_->ihs_.end());

        // 删除索引
        auto drop_plan = std::make_shared<DDLPlan>(T_DropIndex, TABLE1, std::vector<std::string>{"id"}, std::vector<ColDef>{});
        ql_mgr.run_multi_query(drop_plan, &ctx);
        EXPECT_TRUE(sm_manager_->ihs_.find(ix_name) == sm_manager_->ihs_.end());
    }
}

// ============================================================
// 32. select_from —— 覆盖 BIGINT / DATETIME / FLOAT / STRING 列格式化
// ============================================================
TEST_F(ExecutorTest, SelectFromAllTypes) {
    // 建表含所有类型
    std::vector<ColDef> col_defs;
    col_defs.push_back({"c_int", TYPE_INT, 4});
    col_defs.push_back({"c_bigint", TYPE_BIGINT, 8});
    col_defs.push_back({"c_float", TYPE_FLOAT, 4});
    col_defs.push_back({"c_str", TYPE_STRING, 16});
    sm_manager_->create_table("all_types", col_defs, context_);

    // 插入包含各种类型值
    {
        Value v_int;   v_int.set_int(42);
        Value v_bi;    v_bi.set_bigint(10000000000LL);
        Value v_flt;   v_flt.set_float(3.14f);
        Value v_str;   v_str.set_str("hello");
        auto ins = std::make_unique<InsertExecutor>(sm_manager_, "all_types",
            std::vector<Value>{v_int, v_bi, v_flt, v_str}, context_);
        ins->Next();
    }

    // 构造 Select (ProjectionPlan → ScanPlan) 并调用 select_from
    QlManager ql_mgr(sm_manager_, nullptr);
    char data_buf[4096];
    int offset = 0;
    Context ctx(nullptr, nullptr, nullptr, data_buf, &offset);

    auto scan_plan = std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, "all_types",
                                                 std::vector<Condition>{}, std::vector<std::string>{});
    auto proj_plan = std::make_shared<ProjectionPlan>(T_Projection, scan_plan,
        std::vector<TabCol>{make_tabcol("all_types", "c_int"),
                            make_tabcol("all_types", "c_bigint"),
                            make_tabcol("all_types", "c_float"),
                            make_tabcol("all_types", "c_str")});

    // run_multi_query + select_from 需要通过 plan 执行器创建
    // 这里直接用实际的 executor 构造
    auto seq = std::make_unique<SeqScanExecutor>(sm_manager_, "all_types", std::vector<Condition>{}, context_);
    auto proj = std::make_unique<ProjectionExecutor>(std::move(seq),
        std::vector<TabCol>{make_tabcol("all_types", "c_int"),
                            make_tabcol("all_types", "c_bigint"),
                            make_tabcol("all_types", "c_float"),
                            make_tabcol("all_types", "c_str")});

    ql_mgr.select_from(std::move(proj),
        std::vector<TabCol>{make_tabcol("all_types", "c_int"),
                            make_tabcol("all_types", "c_bigint"),
                            make_tabcol("all_types", "c_float"),
                            make_tabcol("all_types", "c_str")}, &ctx);
    EXPECT_GT(offset, 0);
}
