/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <memory>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record_printer.h"
#include "common/common.h"

// 目前的索引匹配规则为：完全匹配索引字段，且全部为单点查询，不会自动调整where条件的顺序
bool Planner::get_index_cols(std::string tab_name, std::vector<Condition> curr_conds, std::vector<std::string>& index_col_names) {
   index_col_names.clear();

    // 获取表元数据
    auto& tab_meta = sm_manager_->db_.get_table(tab_name);
    auto& tab_idxs = tab_meta.indexes;

    // 使用哈希表记录列名到条件的映射
    std::unordered_map<std::string, Condition> col_to_cond_map;
    for (const auto& cond : curr_conds) {
        if (cond.is_rhs_val && cond.lhs_col.tab_name == tab_name) {
            col_to_cond_map[cond.lhs_col.col_name] = cond;
        }
    }

    size_t max_match_count = 0;
    IndexMeta best_idx;

    // 遍历所有索引，找到匹配最多条件的索引
    for (const auto& index : tab_idxs) {
        size_t match_count = 0;
        bool full_match = true;

        for (const auto& idx_col : index.cols) {
            if (col_to_cond_map.find(idx_col.name) != col_to_cond_map.end()) {
                match_count++;
            } else {
                full_match = false;
                break;
            }
        }

        if (match_count > max_match_count) {
            max_match_count = match_count;
            best_idx = index;

            // 如果找到了完全匹配的索引，直接返回
            if (full_match) {
                //index_col_names = index;
                for(auto e:best_idx.cols)index_col_names.push_back(e.name);
                return true;
            }
        }
    }
    if (max_match_count == 0) {
        return false;
    } else {
        for(auto e:best_idx.cols)index_col_names.push_back(e.name);
        return true;
    }
}


/**
 * @brief 表算子条件谓词生成
 * 
 * 该函数从一组条件（conds）中提取与指定表名（tab_names）相关的条件。
 * 提取出的条件会返回为一个新的条件列表（solved_conds），并从原始条件列表（conds）中移除。
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition> Planner::pop_conds(std::vector<Condition> &conds, std::string tab_names) {

    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        // 检查条件的左侧列名是否匹配指定的表名，并且右侧是值，或者检查条件的左右两侧列名是否都属于同一张表
        if ((tab_names.compare(it->lhs_col.tab_name) == 0 && it->is_rhs_val) || (it->lhs_col.tab_name.compare(it->rhs_col.tab_name) == 0)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan)
{
    if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan))
    {
        if(x->tab_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if(x->tab_name_.compare(cond->rhs_col.tab_name) == 0){
            return 2;
        } else {
            return 0;
        }
    }
    else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan))
    {
        int left_res = push_conds(cond, x->left_);
        // 条件已经下推到左子节点
        if(left_res == 3){
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        // 条件已经下推到右子节点
        if(right_res == 3){
            return 3;
        }
        // 左子节点或右子节点有一个没有匹配到条件的列
        if(left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        // 左子节点匹配到条件的右边
        if(left_res == 2) {
            // 需要将左右两边的条件变换位置
            std::map<CompOp, CompOp> swap_op = {
                {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
            };
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = swap_op.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}




//获取当前table的scan任务
//fix:重新检查是否可以利用索引
std::shared_ptr<Plan> Planner::pop_scan(int *scantbl, TabCol col, std::vector<std::string> &joined_tables, 
                std::vector<std::shared_ptr<Plan>> plans)
{
    for (size_t i = 0; i < plans.size(); i++) {
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if(x->tab_name_.compare(col.tab_name) == 0)
        {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);

            if(plans[i]->tag == T_IndexScan){
                return plans[i];
            }

            // 检查是否存在索引并获取索引列名原则
            std::vector<std::string> index_col_names;
            //判定是否存在索引，且符合最左匹配
            auto& tab_meta = sm_manager_->db_.get_table(x->tab_name_);
            auto& tab_idxs = tab_meta.indexes;

            size_t max_match_count = 0;
            IndexMeta best_idx;

            for(auto& index : tab_idxs){
                size_t match_count = 0;
                bool full_match = true;

                for (const auto& idx_col : index.cols) {
                    if (col.col_name == idx_col.name) {
                        match_count++;
                    } else {
                        full_match = false;
                        break;
                    }
                }

                if (match_count > max_match_count) {
                    max_match_count = match_count;
                    best_idx = index;

                    // 如果找到了完全匹配的索引，直接返回
                    if (full_match) {
                        //index_col_names = index;
                        for(auto e:best_idx.cols)index_col_names.push_back(e.name);
                        return std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, col.tab_name, x->fed_conds_, index_col_names);
                    }
                }
            }
            
            if (max_match_count == 0) {
                return plans[i];
            } else {
                for(auto e:best_idx.cols)index_col_names.push_back(e.name);
                 return std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, col.tab_name, x->fed_conds_, index_col_names);
            }
            return plans[i];
        }
    }
    return nullptr;
}


std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context)
{
    
    //TODO 实现逻辑优化规则

    return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context)
{

    //处理scan & filter 
    std::shared_ptr<Plan> plan = make_one_rel(query);
    
    //TODO: 其他物理优化

    //处理Groupby
    plan = generate_groupby_plan(query,std::move(plan));

    // 处理orderby
    plan = generate_sort_plan(query, std::move(plan)); 

    return plan;
}

std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    std::vector<std::string> tables = query->tables;
    
    // Scan table , 生成表算子列表tab_nodes
    // 初始化一个与表数量相同的表扫描计划列表
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    std::vector<bool>table_index_exists(tables.size());

    // 遍历每个表，生成相应的扫描计划
    for (size_t i = 0; i < tables.size(); i++) {
        // 从查询条件中获取当前表相关的条件
        auto curr_conds = pop_conds(query->conds, tables[i]);
        // 检查是否存在索引并获取索引列名原则
        std::vector<std::string> index_col_names;
        //判定是否存在索引，且符合最左匹配
        bool index_exist = get_index_cols(tables[i], curr_conds, index_col_names);
        table_index_exists[i] = index_exist;
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors[i] = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, tables[i], curr_conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors[i] =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, tables[i], curr_conds, index_col_names);
        }
    }
    // 只有一个表，不需要join。
    if(tables.size() == 1)
    {
        return table_scan_executors[0];
    }

    // 获取where条件 （join条件）
    auto conds = std::move(query->conds);
    std::shared_ptr<Plan> table_join_executors;
    
    // 初始化一个数组来记录表是否已扫描
    int scantbl[tables.size()];
    for(size_t i = 0; i < tables.size(); i++)
    {
        scantbl[i] = -1;
    }
    // 假设在ast中已经添加了jointree，这里需要修改的逻辑是，先处理jointree，然后再考虑剩下的部分
    if(conds.size() >= 1)
    {
        // 有连接条件
        // 根据连接条件，生成第一层join
        std::vector<std::string> joined_tables(tables.size());
        auto it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left , right;
            left = pop_scan(scantbl, it->lhs_col, joined_tables, table_scan_executors);
            right = pop_scan(scantbl, it->rhs_col, joined_tables, table_scan_executors);
            std::vector<Condition> join_conds{*it};

            set_enable_nestedloop_join(g_enable_nestloop);
            set_enable_sortmerge_join(g_enable_sortmerge);

            //建立join
            // 判断使用哪种join方式
            if(enable_nestedloop_join && enable_sortmerge_join) {// 默认nested loop join
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            } else if(enable_nestedloop_join) {
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            } else if(enable_sortmerge_join) {

                std::vector<TabCol> left_cols;
                auto left_col = join_conds[0].lhs_col;
                left_cols.push_back(left_col);

                std::shared_ptr<Plan> sorted_left , sorted_right;
                if(left->tag == T_SeqScan){
                    //left join cond col 
                    sorted_left = std::make_shared<SortPlan>(T_Sort, std::move(left), left_cols, false);
                    assert(sorted_left);
                }else if(left->tag == T_IndexScan){
                    //nothing to do
                    sorted_left = std::move(left);
                }else{
                    throw InternalError("bad plan while build sort merge join");
                }

                std::vector<TabCol> right_cols;
                auto right_col = join_conds[0].rhs_col;
                right_cols.push_back(right_col);

                if(right->tag == T_SeqScan){
                    sorted_right = std::make_shared<SortPlan>(T_Sort, std::move(right), right_cols, false);
                    assert(sorted_right);
                }else if(right->tag == T_IndexScan){
                    //no thing to do
                    sorted_right = std::move(right);
                }else{
                    throw InternalError("bad plan while build sort merge join");
                }

                

                table_join_executors = std::make_shared<JoinPlan>(T_SortMerge, std::move(sorted_left), std::move(sorted_right), join_conds);
            } else {
                // error
                throw RMDBError("No join executor selected!");
            }
            // table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            it = conds.erase(it);
            break;
        }
        // 根据连接条件，生成第2-n层join
        it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left_need_to_join_executors = nullptr;
            std::shared_ptr<Plan> right_need_to_join_executors = nullptr;
            bool isneedreverse = false;
            if (std::find(joined_tables.begin(), joined_tables.end(), it->lhs_col.tab_name) == joined_tables.end()) {
                left_need_to_join_executors = pop_scan(scantbl, it->lhs_col, joined_tables, table_scan_executors);
            }
            if (std::find(joined_tables.begin(), joined_tables.end(), it->rhs_col.tab_name) == joined_tables.end()) {
                right_need_to_join_executors = pop_scan(scantbl, it->rhs_col, joined_tables, table_scan_executors);
                isneedreverse = true;
            } 

            if(left_need_to_join_executors != nullptr && right_need_to_join_executors != nullptr) {
                std::vector<Condition> join_conds{*it};
                std::shared_ptr<Plan> temp_join_executors = std::make_shared<JoinPlan>(T_NestLoop, 
                                                                    std::move(left_need_to_join_executors), 
                                                                    std::move(right_need_to_join_executors), 
                                                                    join_conds);
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(temp_join_executors), 
                                                                    std::move(table_join_executors), 
                                                                    std::vector<Condition>());
            } else if(left_need_to_join_executors != nullptr || right_need_to_join_executors != nullptr) {
                if(isneedreverse) {
                    std::map<CompOp, CompOp> swap_op = {
                        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
                    };
                    std::swap(it->lhs_col, it->rhs_col);
                    it->op = swap_op.at(it->op);
                    left_need_to_join_executors = std::move(right_need_to_join_executors);
                }
                std::vector<Condition> join_conds{*it};
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left_need_to_join_executors), 
                                                                    std::move(table_join_executors), join_conds);
            } else {
                push_conds(std::move(&(*it)), table_join_executors);
            }
            it = conds.erase(it);
        }
    } else {
        table_join_executors = table_scan_executors[0];
        scantbl[0] = 1;
    }

    //连接剩余表
    for (size_t i = 0; i < tables.size(); i++) {
        if(scantbl[i] == -1) {
            table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_join_executors),std::move(table_scan_executors[i]), 
                                                    std::vector<Condition>());
        }
    }
    return table_join_executors;
}

std::shared_ptr<Plan> Planner::generate_aggregate_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan){
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if(!x->group_by){
        return plan;
    }
    return std::make_shared<AggregatePlan>(T_Aggregate,std::move(plan),query->a_exprs);
}

std::shared_ptr<Plan> Planner::generate_groupby_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan){
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if(!query->gb_expr.cols.size() && query->a_exprs.size() == 0){
        return plan;
    }
    return std::make_shared<GroupByPlan>(T_GroupBy,std::move(plan),query->gb_expr.cols,query->gb_expr.havingClause,
                                        query->a_exprs,query->cols);
}

std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    //检查是否包含排序操作
    if(!x->has_sort) {
        return plan;
    }
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), query->order_expr.cols, 
                                    query->order_expr.dir == OrderBy_Dir::OP_DESC);
}

/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    //逻辑优化
    query = logical_optimization(std::move(query), context);

    //物理优化
    auto sel_cols = query->cols;
    auto sel_aggs = query->a_exprs;
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);
    plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot), 
                                                        std::move(sel_cols),std::move(sel_aggs));

    return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plannerRoot;
    if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // create table;
        std::vector<ColDef> col_defs;
        for (auto &field : x->fields) {
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                ColDef col_def = {.name = sv_col_def->col_name,
                                  .type = interp_sv_type(sv_col_def->type_len->type),
                                  .len = sv_col_def->type_len->len};
                col_defs.push_back(col_def);
            } else {
                throw InternalError("Unexpected field type");
            }
        }
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        // drop table;
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // create index;
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // drop index
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // insert;
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(),  x->tab_name,  
                                                    query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // delete;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);
        
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,  
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {  // 该表没有索引
        index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), query->conds, 
                                                     query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>());
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}