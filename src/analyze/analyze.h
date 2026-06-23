/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cassert>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "parser/parser.h"
#include "system/sm.h"
#include "common/common.h"
#include "common/type_cast.h"

class Query{
    public:
    std::shared_ptr<ast::TreeNode> parse;
    // TODO jointree
    // where条件
    std::vector<Condition> conds;
    // 投影列
    std::vector<TabCol> cols;
    // 表名
    std::vector<std::string> tables;
    // update 的set 值
    std::vector<SetClause> set_clauses;
    //insert 的values值
    std::vector<Value> values;
    // load 的文件路径
    std::string load_file;

    Query(){}

};

class Analyze
{
private:
    SmManager *sm_manager_;
public:
    Analyze(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Analyze(){}

    std::shared_ptr<Query> do_analyze(std::shared_ptr<ast::TreeNode> root);

private:
    TabCol check_column(const std::vector<ColMeta> &all_cols, TabCol target);
    void get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols);
    void get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds);
    void check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds);
    Value convert_sv_value(const std::shared_ptr<ast::Value> &sv_val);
    Value eval_constant_expr(const std::shared_ptr<ast::Expr> &expr);
    CompOp convert_sv_comp_op(ast::SvCompOp op);
    ColType resolve_expr_type(const std::shared_ptr<ast::Expr> &expr, const std::vector<ColMeta> &all_cols);
};

inline Value eval_set_expr(const std::shared_ptr<ast::Expr>& expr,
                           const char* record_buf,
                           const std::vector<ColMeta>& cols,
                           const std::unordered_map<std::string, size_t>& col_offset) {
    auto eval_col = [&](const std::shared_ptr<ast::Col>& col) -> Value {
        auto qualified_name = col->tab_name.empty() ? col->col_name : (col->tab_name + "." + col->col_name);
        auto it = col_offset.find(qualified_name);
        if (it == col_offset.end()) {
            it = col_offset.find(col->col_name);
        }
        if (it == col_offset.end()) {
            throw ColumnNotFoundError(qualified_name);
        }
        const auto &meta = cols[it->second];
        return TypeCaster::read_raw_value(record_buf + meta.offset, meta.type, meta.len);
    };

    std::function<Value(const std::shared_ptr<ast::Expr>&)> eval_impl =
        [&](const std::shared_ptr<ast::Expr>& node) -> Value {
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(node)) {
            return TypeCaster::parse_integer_literal(int_lit->val);
        }
        if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(node)) {
            return Value::make_float(float_lit->val);
        }
        if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(node)) {
            return Value::make_string(str_lit->val);
        }
        if (auto col = std::dynamic_pointer_cast<ast::Col>(node)) {
            return eval_col(col);
        }
        if (auto unary = std::dynamic_pointer_cast<ast::UnaryExpr>(node)) {
            Value rhs = eval_impl(unary->rhs);
            if (!TypeCaster::is_numeric(rhs.type)) {
                throw IncompatibleTypeError(coltype2str(rhs.type), "NUMERIC");
            }
            return TypeCaster::negate_value(rhs);
        }
        if (auto arith = std::dynamic_pointer_cast<ast::ArithExpr>(node)) {
            Value lhs = eval_impl(arith->lhs);
            Value rhs = eval_impl(arith->rhs);
            if (!TypeCaster::is_numeric(lhs.type) || !TypeCaster::is_numeric(rhs.type)) {
                throw IncompatibleTypeError(coltype2str(lhs.type), coltype2str(rhs.type));
            }
            switch (arith->op) {
                case ast::SV_OP_ADD:
                    return TypeCaster::apply_arithmetic(lhs, rhs, '+');
                case ast::SV_OP_SUB:
                    return TypeCaster::apply_arithmetic(lhs, rhs, '-');
                case ast::SV_OP_MUL:
                    return TypeCaster::apply_arithmetic(lhs, rhs, '*');
                case ast::SV_OP_DIV:
                    return TypeCaster::apply_arithmetic(lhs, rhs, '/');
                default:
                    throw InternalError("Unexpected arithmetic operator");
            }
        }
        throw InternalError("Unsupported update expression");
    };

    return eval_impl(expr);
}
