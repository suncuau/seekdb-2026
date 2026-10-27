/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ob_resolver.h"
#include "lib/utility/ob_smart_call.h"
#include "lib/stat/ob_diagnostic_info_guard.h"
#include "sql/resolver/cmd/ob_alter_system_resolver.h"
#include "sql/resolver/dml/ob_insert_resolver.h"
#include "sql/resolver/dml/ob_update_resolver.h"
#include "sql/resolver/dml/ob_delete_resolver.h"
#include "sql/resolver/tcl/ob_start_trans_resolver.h"
#include "sql/resolver/tcl/ob_end_trans_resolver.h"
#include "sql/resolver/prepare/ob_prepare_resolver.h"
#include "sql/resolver/prepare/ob_execute_resolver.h"
#include "sql/resolver/prepare/ob_deallocate_resolver.h"
#include "sql/resolver/ddl/ob_create_table_resolver.h"
#include "sql/resolver/ddl/ob_rename_table_resolver.h"
#include "sql/resolver/ddl/ob_truncate_table_resolver.h"
#include "sql/resolver/ddl/ob_fork_table_resolver.h"
#include "sql/resolver/ddl/ob_fork_database_resolver.h"
#include "sql/resolver/ddl/ob_create_table_like_resolver.h"
#include "sql/resolver/ddl/ob_alter_table_resolver.h"
#include "sql/resolver/ddl/ob_drop_table_resolver.h"
#include "sql/resolver/ddl/ob_create_index_resolver.h"
#include "sql/resolver/ddl/ob_drop_index_resolver.h"
#include "sql/resolver/ddl/ob_create_database_resolver.h"
#include "sql/resolver/ddl/ob_alter_database_resolver.h"
#include "sql/resolver/ddl/ob_use_database_resolver.h"
#include "sql/resolver/ddl/ob_drop_database_resolver.h"
#include "sql/resolver/ddl/ob_create_view_resolver.h"
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ddl/ob_create_outline_resolver.h"
#include "sql/resolver/ddl/ob_alter_outline_resolver.h"
#include "sql/resolver/ddl/ob_drop_outline_resolver.h"
#include "sql/resolver/ddl/ob_drop_routine_resolver.h"
#include "sql/resolver/ddl/ob_trigger_resolver.h"
#include "sql/resolver/ddl/ob_optimize_resolver.h"
#include "ddl/ob_drop_routine_resolver.h"
#include "ddl/ob_alter_routine_resolver.h"
#include "sql/resolver/ddl/ob_create_package_resolver.h"
#include "sql/resolver/ddl/ob_drop_package_resolver.h"
#include "sql/resolver/ddl/ob_recyclebin_restore_resolver.h"
#include "sql/resolver/ddl/ob_purge_resolver.h"
#include "sql/resolver/ddl/ob_analyze_stmt_resolver.h"
#include "sql/resolver/ddl/ob_purge_resolver.h"
#include "sql/resolver/ddl/ob_set_comment_resolver.h"
#include "sql/resolver/ddl/ob_lock_table_resolver.h"
#include "sql/resolver/dml/ob_insert_resolver.h"
#include "sql/resolver/dml/ob_update_resolver.h"
#include "sql/resolver/dml/ob_delete_resolver.h"
#include "sql/resolver/dcl/ob_create_user_resolver.h"
#include "sql/resolver/dcl/ob_drop_user_resolver.h"
#include "sql/resolver/dcl/ob_rename_user_resolver.h"
#include "sql/resolver/dcl/ob_set_password_resolver.h"
#include "sql/resolver/dcl/ob_lock_user_resolver.h"
#include "sql/resolver/dcl/ob_grant_resolver.h"
#include "sql/resolver/dcl/ob_revoke_resolver.h"
#include "sql/resolver/dcl/ob_create_role_resolver.h"
#include "sql/resolver/dcl/ob_drop_role_resolver.h"
#include "sql/resolver/dcl/ob_alter_user_role_resolver.h"
#include "sql/resolver/tcl/ob_start_trans_resolver.h"
#include "sql/resolver/tcl/ob_end_trans_resolver.h"
#include "tcl/ob_savepoint_resolver.h"
#include "sql/resolver/cmd/ob_variable_set_resolver.h"
#include "sql/resolver/cmd/ob_show_resolver.h"
#include "sql/resolver/cmd/ob_diff_table_resolver.h"
#include "sql/resolver/cmd/ob_merge_table_resolver.h"
#include "sql/resolver/cmd/ob_kill_resolver.h"
#include "sql/resolver/cmd/ob_set_names_resolver.h"
#include "sql/resolver/cmd/ob_set_transaction_resolver.h"
#include "sql/resolver/cmd/ob_empty_query_resolver.h"
#include "sql/resolver/cmd/ob_anonymous_block_resolver.h"
#include "sql/resolver/cmd/ob_call_procedure_resolver.h"
#include "sql/resolver/cmd/ob_load_data_resolver.h"
#include "sql/resolver/prepare/ob_execute_resolver.h"
#include "sql/resolver/prepare/ob_deallocate_resolver.h"
#include "sql/resolver/ddl/ob_purge_resolver.h"
#include "sql/resolver/ddl/ob_set_comment_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_wrap_enum_set.h"
#include "sql/resolver/cmd/ob_get_diagnostics_resolver.h"
#include "sql/resolver/dcl/ob_alter_role_resolver.h"
#include "sql/pl/ob_pl_package.h"
#include "sql/parser/sql_parser_base.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/ai_service/ob_ai_column_spec.h"
#include "share/ob_server_struct.h"
#include "sql/resolver/dml/ob_update_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"

namespace oceanbase
{
using namespace common;
using namespace share;
namespace sql
{
ObResolver::ObResolver(ObResolverParams &params)
    : params_(params)
{

}

ObResolver::~ObResolver()
{
}

template <typename ResolverType>
int ObResolver::stmt_resolver_func(ObResolverParams &params, const ParseNode &parse_tree, ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  HEAP_VAR(ResolverType, stmt_resolver, params) {
    if (OB_FAIL(SMART_CALL(stmt_resolver.resolve(parse_tree)))) {
    }
    stmt = stmt_resolver.get_basic_stmt();
  }
  return ret;
}

template <typename SelectResolverType>
int ObResolver::select_stmt_resolver_func(ObResolverParams &params, const ParseNode &parse_tree, ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  HEAP_VAR(SelectResolverType, stmt_resolver, params) {
    stmt_resolver.set_calc_found_rows(true);
    stmt_resolver.set_has_top_limit(true);
    if (OB_FAIL(SMART_CALL(stmt_resolver.resolve(parse_tree)))) {
    }
    stmt = stmt_resolver.get_basic_stmt();
  }
  return ret;
}

// Build the synthetic UPDATE statement that executes
//   ALTER TABLE t RETRY AI COLUMN c [WHERE ...]
// The update assigns NULL to the AI column. The resolver turns that assignment
// into the implicit reset assignment of the column, which invalidates the
// cached value and lets the engine schedule a new AI task for every row that
// matches the predicate.
static int build_ai_column_retry_update(const ParseNode &alter_node,
                                        const ParseNode &retry_node,
                                        common::ObIAllocator &allocator,
                                        ParseNode *&update_node)
{
  int ret = OB_SUCCESS;
  ParseNode *relation_node = NULL;
  ParseNode *column_name_node = NULL;
  ParseNode *where_node = NULL;
  ParseNode *table_refs = NULL;
  ParseNode *column_ref = NULL;
  ParseNode *null_node = NULL;
  ParseNode *assign_item = NULL;
  ParseNode *assign_list = NULL;
  update_node = NULL;
  if (OB_UNLIKELY(T_ALTER_TABLE != alter_node.type_) || 2 > alter_node.num_child_
      || OB_ISNULL(alter_node.children_)
      || OB_ISNULL(relation_node = alter_node.children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid alter table parse tree for ai column retry", K(ret));
  } else if (OB_UNLIKELY(T_AI_COLUMN_RETRY != retry_node.type_)
             || 2 > retry_node.num_child_
             || OB_ISNULL(retry_node.children_)
             || OB_ISNULL(column_name_node = retry_node.children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid ai column retry parse node", K(ret));
  } else {
    where_node = retry_node.children_[1];
  }
  if (OB_SUCC(ret)
      && (OB_ISNULL(table_refs = new_non_terminal_node(&allocator,
                                                       T_TABLE_REFERENCES,
                                                       1,
                                                       relation_node))
          || OB_ISNULL(null_node = new_terminal_node(&allocator, T_NULL))
          || OB_ISNULL(column_ref = new_non_terminal_node(&allocator,
                                                          T_COLUMN_REF,
                                                          3,
                                                          NULL,
                                                          NULL,
                                                          column_name_node))
          || OB_ISNULL(assign_item = new_non_terminal_node(&allocator,
                                                           T_ASSIGN_ITEM,
                                                           2,
                                                           column_ref,
                                                           null_node))
          || OB_ISNULL(assign_list = new_non_terminal_node(&allocator,
                                                           T_ASSIGN_LIST,
                                                           1,
                                                           assign_item))
          || OB_ISNULL(update_node = new_non_terminal_node(&allocator,
                                                           T_UPDATE,
                                                           11,
                                                           NULL,        /* 0. with clause */
                                                           table_refs,  /* 1. table node */
                                                           assign_list, /* 2. update list */
                                                           where_node,  /* 3. where node */
                                                           NULL,        /* 4. order by node */
                                                           NULL,        /* 5. limit node */
                                                           NULL,        /* 6. when node */
                                                           NULL,        /* 7. hint node */
                                                           NULL,        /* 8. ignore */
                                                           NULL,        /* 9. returning */
                                                           NULL)))) {   /* 10. error logging clause */
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate the ai column retry parse tree", K(ret));
  }
  return ret;
}

// Key of a column referenced by a statement, used to deduplicate the AI column
// strict read check.
struct ObAiStrictColumnKey
{
  ObAiStrictColumnKey() : table_id_(common::OB_INVALID_ID), column_id_(common::OB_INVALID_ID) {}
  ObAiStrictColumnKey(const uint64_t table_id, const uint64_t column_id)
      : table_id_(table_id), column_id_(column_id) {}
  bool operator==(const ObAiStrictColumnKey &other) const
  {
    return table_id_ == other.table_id_ && column_id_ == other.column_id_;
  }
  uint64_t table_id_;
  uint64_t column_id_;
};

// Returns true when at least one row of the AI column does not carry the READY
// state. The hidden state column is only readable from an inner session, which
// is exactly what the sql proxy session is.
static int ai_column_has_unfinished_rows(common::ObMySQLProxy &sql_proxy,
                                         const ObString &database_name,
                                         const ObString &table_name,
                                         const ObString &state_column_name,
                                         bool &incomplete)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  incomplete = false;
  if (OB_FAIL(sql.append_fmt("select 1 from `%.*s`.`%.*s` "
                             "where `%.*s` is null or `%.*s` <> %d limit 1",
                             database_name.length(),
                             database_name.ptr(),
                             table_name.length(),
                             table_name.ptr(),
                             state_column_name.length(),
                             state_column_name.ptr(),
                             state_column_name.length(),
                             state_column_name.ptr(),
                             static_cast<int>(share::AI_COLUMN_STATE_READY)))) {
    LOG_WARN("failed to build the ai column strict read query", K(ret));
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
        LOG_WARN("failed to execute the ai column strict read query", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null mysql result", K(ret));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("failed to fetch the ai column strict read result", K(ret));
        }
      } else {
        incomplete = true;
      }
    }
  }
  return ret;
}

// Collect the AI columns whose cached value the statement itself resets through
// an implicit assignment. Such a column is being regenerated, so its stale
// state must not reject the statement.
static int collect_implicit_ai_columns(ObDMLStmt &stmt,
                                       ObIArray<ObAiStrictColumnKey> &columns)
{
  int ret = OB_SUCCESS;
  if (stmt.is_update_stmt()) {
    const ObIArray<ObUpdateTableInfo*> &table_infos =
        static_cast<ObUpdateStmt&>(stmt).get_update_table_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < table_infos.count(); ++i) {
      const ObUpdateTableInfo *table_info = table_infos.at(i);
      if (OB_ISNULL(table_info)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int64_t j = 0; OB_SUCC(ret) && j < table_info->assignments_.count(); ++j) {
          const ObAssignment &assignment = table_info->assignments_.at(j);
          if (assignment.is_implicit_ && OB_NOT_NULL(assignment.column_expr_)) {
            if (OB_FAIL(columns.push_back(ObAiStrictColumnKey(
                    assignment.column_expr_->get_table_id(),
                    assignment.column_expr_->get_column_id())))) {
              LOG_WARN("failed to push back the implicit ai column", K(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

static int check_ai_column_strict_read_stmt(ObResolverParams &params,
                                            common::ObMySQLProxy &sql_proxy,
                                            ObDMLStmt &stmt,
                                            const ObIArray<ObAiStrictColumnKey> &implicit_columns,
                                            ObIArray<ObAiStrictColumnKey> &checked_columns);

// Walk the statement and its subqueries, and reject the statement when one of
// the AI columns it references is not completely READY.
static int check_ai_column_strict_read(ObResolverParams &params, ObDMLStmt &stmt)
{
  int ret = OB_SUCCESS;
  bool strict_read = false;
  common::ObMySQLProxy *sql_proxy = params.sql_proxy_;
  ObSQLSessionInfo *session_info = params.session_info_;
  ObSEArray<ObAiStrictColumnKey, 8> implicit_columns;
  ObSEArray<ObAiStrictColumnKey, 8> checked_columns;
  if (OB_ISNULL(session_info) || OB_ISNULL(params.schema_checker_)) {
    ret = OB_NOT_INIT;
  } else if (session_info->is_inner()) {
    // server side statements (ai task maintenance, ...) are not user reads
  } else if (OB_FAIL(session_info->get_ai_column_strict_read(strict_read))) {
    LOG_WARN("failed to get ai_column_strict_read", K(ret));
  } else if (!strict_read) {
    // strict reads are disabled
  } else if (OB_ISNULL(sql_proxy) && OB_ISNULL(sql_proxy = GCTX.sql_proxy_)) {
    LOG_WARN("no sql proxy available, skip the ai column strict read check");
  } else if (OB_FAIL(collect_implicit_ai_columns(stmt, implicit_columns))) {
    LOG_WARN("failed to collect the implicit ai columns", K(ret));
  } else if (OB_FAIL(check_ai_column_strict_read_stmt(params,
                                                      *sql_proxy,
                                                      stmt,
                                                      implicit_columns,
                                                      checked_columns))) {
    LOG_WARN("failed to check the ai column strict read", K(ret));
  }
  return ret;
}

static int check_ai_column_strict_read_stmt(ObResolverParams &params,
                                            common::ObMySQLProxy &sql_proxy,
                                            ObDMLStmt &stmt,
                                            const ObIArray<ObAiStrictColumnKey> &implicit_columns,
                                            ObIArray<ObAiStrictColumnKey> &checked_columns)
{
  int ret = OB_SUCCESS;
  ObSchemaChecker *schema_checker = params.schema_checker_;
  ObSQLSessionInfo *session_info = params.session_info_;
  const ObIArray<ColumnItem> &column_items = stmt.get_column_items();
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); ++i) {
    const ColumnItem &column_item = column_items.at(i);
    const TableItem *table_item = NULL;
    const TableItem *base_table_item = NULL;
    const ObTableSchema *table_schema = NULL;
    const ObColumnSchemaV2 *column_schema = NULL;
    const ObAiStrictColumnKey key(column_item.table_id_, column_item.column_id_);
    char spec_name_buf[512];
    char state_name_buf[512];
    ObString spec_name;
    ObString state_name;
    if (OB_ISNULL(table_item = stmt.get_table_item_by_id(column_item.table_id_))) {
      // the column belongs to a table item of another statement level
    } else if (!table_item->is_basic_table()) {
      // generated, joined or function tables have no ai column of their own
    } else if (FALSE_IT(base_table_item = &table_item->get_base_table_item())) {
    } else if (has_exist_in_array(implicit_columns, key)) {
      // the statement resets the ai column itself, there is nothing stale to read
    } else if (has_exist_in_array(checked_columns, key)) {
      // already checked
    } else if (OB_FAIL(schema_checker->get_table_schema(base_table_item->ref_id_,
                                                        table_schema))) {
      LOG_WARN("failed to get the table schema", K(ret), K(base_table_item->ref_id_));
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null table schema", K(ret));
    } else if (OB_ISNULL(column_schema = table_schema->get_column_schema(column_item.column_id_))) {
      // the column is not part of the base table schema
    } else if (column_schema->is_hidden()) {
      // the hidden state/spec columns are not user visible
    } else if (OB_FAIL(share::ai_column_make_hidden_name(share::AI_COLUMN_SPEC_PREFIX,
                                                         column_schema->get_column_name_str(),
                                                         spec_name_buf,
                                                         static_cast<int64_t>(sizeof(spec_name_buf)),
                                                         spec_name))) {
      LOG_WARN("failed to make the ai spec column name", K(ret));
    } else if (NULL == table_schema->get_column_schema(spec_name)) {
      // an ordinary column
    } else if (OB_FAIL(share::ai_column_make_hidden_name(share::AI_COLUMN_STATE_PREFIX,
                                                         column_schema->get_column_name_str(),
                                                         state_name_buf,
                                                         static_cast<int64_t>(sizeof(state_name_buf)),
                                                         state_name))) {
      LOG_WARN("failed to make the ai state column name", K(ret));
    } else {
      bool incomplete = false;
      const ObString &database_name = base_table_item->database_name_.empty()
          ? session_info->get_database_name() : base_table_item->database_name_;
      if (OB_FAIL(checked_columns.push_back(key))) {
        LOG_WARN("failed to push back the checked ai column", K(ret));
      } else if (OB_FAIL(ai_column_has_unfinished_rows(sql_proxy,
                                                       database_name,
                                                       base_table_item->table_name_,
                                                       state_name,
                                                       incomplete))) {
        LOG_WARN("failed to check the ai column state", K(ret),
                 K(database_name), K(base_table_item->table_name_), K(state_name));
      } else if (incomplete) {
        ret = OB_AI_COLUMN_STRICT_READ_INCOMPLETE;
        LOG_USER_ERROR(OB_AI_COLUMN_STRICT_READ_INCOMPLETE,
                       static_cast<int64_t>(base_table_item->ref_id_),
                       static_cast<int64_t>(column_item.column_id_));
      }
    }
  }
  if (OB_SUCC(ret)) {
    const ObIArray<ObQueryRefRawExpr*> &subqueries = stmt.get_subquery_exprs();
    for (int64_t i = 0; OB_SUCC(ret) && i < subqueries.count(); ++i) {
      ObQueryRefRawExpr *query_ref = subqueries.at(i);
      ObSelectStmt *subquery_stmt = NULL;
      if (OB_ISNULL(query_ref)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(subquery_stmt = query_ref->get_ref_stmt())) {
        // the subquery was not resolved
      } else if (OB_FAIL(SMART_CALL(check_ai_column_strict_read_stmt(params,
                                                                     sql_proxy,
                                                                     *subquery_stmt,
                                                                     implicit_columns,
                                                                     checked_columns)))) {
        LOG_WARN("failed to check the ai column strict read of a subquery", K(ret));
      }
    }
  }
  return ret;
}

int ObResolver::resolve(IsPrepared if_prepared, const ParseNode &parse_tree, ObStmt *&stmt)
{
#define REGISTER_STMT_RESOLVER(name)                                   \
  do {                                                                 \
    ret = stmt_resolver_func<Ob##name##Resolver>(params_, *real_parse_tree, stmt); \
  } while (0)

#define REGISTER_SELECT_STMT_RESOLVER(name)                                   \
  do {                                                                        \
    ret = select_stmt_resolver_func<Ob##name##Resolver>(params_, parse_tree, stmt); \
  } while (0)
  ACTIVE_SESSION_FLAG_SETTER_GUARD(in_resolve);
  int ret = OB_SUCCESS;
  const ParseNode *real_parse_tree = NULL;
  int64_t questionmark_count = 0;
  bool is_ai_column_retry = false;
  UNUSED(if_prepared);
  if (OB_ISNULL(params_.allocator_)
      || OB_ISNULL(params_.schema_checker_)
      || OB_ISNULL(params_.session_info_)
      || OB_ISNULL(params_.expr_factory_)
      || OB_ISNULL(params_.query_ctx_)) {
    ret = OB_NOT_INIT;
  } else {
    real_parse_tree = &parse_tree;
  }

  if (OB_SUCC(ret)) {
    params_.resolver_scope_stmt_type_ = parse_tree.type_;
    params_.session_info_->set_stmt_type(
      ObResolverUtils::get_stmt_type_by_item_type(parse_tree.type_));
    ObExecContext *exec_ctx = params_.session_info_->get_cur_exec_ctx();
    if (exec_ctx != nullptr && exec_ctx->get_sql_ctx() != nullptr) {
      //save the current stmt type on my context,
      //some other modules may need to get the current SQL stmt type through its context
      ObSqlCtx *sql_ctx = exec_ctx->get_sql_ctx();
      sql_ctx->stmt_type_ = ObResolverUtils::get_stmt_type_by_item_type(parse_tree.type_);
    }

    switch (real_parse_tree->type_) {
      case T_CREATE_TABLE: {
        REGISTER_STMT_RESOLVER(CreateTable);
        break;
      }
      case T_ALTER_TABLE: {
        const ParseNode *action_list = NULL;
        const ParseNode *retry_action = NULL;
        int64_t action_count = 0;
        if (OB_NOT_NULL(real_parse_tree->children_) && real_parse_tree->num_child_ > 1) {
          action_list = real_parse_tree->children_[1];
        }
        if (OB_NOT_NULL(action_list)) {
          action_count = action_list->num_child_;
          for (int64_t i = 0; NULL == retry_action && i < action_count; ++i) {
            if (OB_NOT_NULL(action_list->children_[i])
                && T_AI_COLUMN_RETRY == action_list->children_[i]->type_) {
              retry_action = action_list->children_[i];
            }
          }
        }
        if (NULL == retry_action) {
          REGISTER_STMT_RESOLVER(AlterTable);
        } else if (1 != action_count) {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("cannot combine AI COLUMN RETRY with another alter action", K(ret));
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
        } else {
          ParseNode *update_node = NULL;
          if (OB_FAIL(build_ai_column_retry_update(*real_parse_tree,
                                                   *retry_action,
                                                   *params_.allocator_,
                                                   update_node))) {
            LOG_WARN("failed to build the ai column retry statement", K(ret));
          } else if (OB_FAIL(stmt_resolver_func<ObAiColumnRetryResolver>(params_,
                                                                        *update_node,
                                                                        stmt))) {
            LOG_WARN("failed to resolve the ai column retry statement", K(ret));
          } else if (OB_ISNULL(stmt)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            is_ai_column_retry = true;
            // the retry executes as a DML statement, not as a DDL statement
            params_.session_info_->set_stmt_type(stmt->get_stmt_type());
            if (NULL != exec_ctx && NULL != exec_ctx->get_sql_ctx()) {
              exec_ctx->get_sql_ctx()->stmt_type_ = stmt->get_stmt_type();
            }
          }
        }
        break;
      }
      case T_CREATE_INDEX: {
        REGISTER_STMT_RESOLVER(CreateIndex);
        break;
      }
      case T_CREATE_VIEW: {
        REGISTER_STMT_RESOLVER(CreateView);
        break;
      }
      case T_ALTER_VIEW: {
#if 0
        ObAlterViewResolver stmt_resolver(params_);
        if (OB_FAIL(stmt_resolver.resolve(*node))) {
        }

        stmt = stmt_resolver.get_basic_stmt();
#endif
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("not support to alter view");
	      LOG_USER_ERROR(OB_NOT_SUPPORTED, "alter view");
        break;
      }
      case T_DROP_VIEW:
      case T_DROP_TABLE: {
        REGISTER_STMT_RESOLVER(DropTable);
        break;
      }
      case T_DROP_INDEX: {
        REGISTER_STMT_RESOLVER(DropIndex);
        break;
      }
      case T_CREATE_TABLE_LIKE: {
        REGISTER_STMT_RESOLVER(CreateTableLike);
        break;
      }
      case T_FORK_TABLE: {
        REGISTER_STMT_RESOLVER(ForkTable);
        break;
      }
      case T_FORK_DATABASE: {
        REGISTER_STMT_RESOLVER(ForkDatabase);
        break;
      }
      case T_DIFF_TABLE: {
        REGISTER_STMT_RESOLVER(DiffTable);
        break;
      }
      case T_MERGE_TABLE: {
        REGISTER_STMT_RESOLVER(MergeTable);
        break;
      }
      case T_SELECT: {
        REGISTER_SELECT_STMT_RESOLVER(Select);
        break;
      }
      case T_INSERT: {
        REGISTER_STMT_RESOLVER(Insert);
        break;
      }
      case T_UPDATE: {
        REGISTER_STMT_RESOLVER(Update);
        break;
      }
      case T_DELETE: {
        REGISTER_STMT_RESOLVER(Delete);
        break;
      }
      case T_BEGIN: {
        REGISTER_STMT_RESOLVER(StartTrans);
        break;
      }
      case T_ROLLBACK:
        //fall through
      case T_COMMIT: {
        REGISTER_STMT_RESOLVER(EndTrans);
        break;
      }
      case T_FREEZE: {
        REGISTER_STMT_RESOLVER(Freeze);
        break;
      }
      case T_FLUSH_CACHE: {
        REGISTER_STMT_RESOLVER(FlushCache);
        break;
      }
      case T_FLUSH_KVCACHE: {
        REGISTER_STMT_RESOLVER(FlushKVCache);
        break;
      }
      case T_FLUSH_ILOGCACHE: {
        REGISTER_STMT_RESOLVER(FlushIlogCache);
        break;
      }
      case T_FLUSH_DAG_WARNINGS: {
        REGISTER_STMT_RESOLVER(FlushDagWarnings);
        break;
      }
      case T_MERGE_CONTROL: {
        REGISTER_STMT_RESOLVER(AdminMerge);
        break;
      }
      case T_CANCEL_TASK: {
        REGISTER_STMT_RESOLVER(CancelTask);
        break;
      }
      case T_REFRESH_MEMORY_STAT: {
        REGISTER_STMT_RESOLVER(RefreshMemStat);
        break;
      }
      case T_REFRESH_IO_CALIBRATION: {
        REGISTER_STMT_RESOLVER(RefreshIOCalibration);
        break;
      }
      case T_SWITCHOVER_TO_STANDBY:
      case T_SWITCHOVER_TO_PRIMARY:
      case T_ACTIVATE_STANDBY: {
        REGISTER_STMT_RESOLVER(SwitchRole);
        break;
      }
      case T_ALTER_SYSTEM_SET_PARAMETER: {
        REGISTER_STMT_RESOLVER(SetConfig);
        break;
      }
      case T_ALTER_SYSTEM_SETTP: {
        REGISTER_STMT_RESOLVER(SetTP);
        break;
      }
      case T_CLEAR_MERGE_ERROR: {
        REGISTER_STMT_RESOLVER(ClearMergeError);
        break;
      }
      case T_CREATE_DATABASE: {
        REGISTER_STMT_RESOLVER(CreateDatabase);
        break;
      }
      case T_USE_DATABASE: {
        REGISTER_STMT_RESOLVER(UseDatabase);
        break;
      }
      case T_ALTER_DATABASE: {
        REGISTER_STMT_RESOLVER(AlterDatabase);
        break;
      }
      case T_DROP_DATABASE: {
        REGISTER_STMT_RESOLVER(DropDatabase);
        break;
      }
      case T_RENAME_TABLE: {
        REGISTER_STMT_RESOLVER(RenameTable);
        break;
      }
      case T_TRUNCATE_TABLE: {
        REGISTER_STMT_RESOLVER(TruncateTable);
        break;
      }
      case T_RECYCLEBIN_RESTORE_TABLE: {
        REGISTER_STMT_RESOLVER(RecyclebinRestoreTable);
        break;
      }
      case T_RECYCLEBIN_RESTORE_DATABASE: {
        REGISTER_STMT_RESOLVER(RecyclebinRestoreDatabase);
        break;
      }
      case T_PURGE_TABLE: {
        REGISTER_STMT_RESOLVER(PurgeTable);
        break;
      }
      case T_PURGE_INDEX: {
        REGISTER_STMT_RESOLVER(PurgeIndex);
        break;
      }
      case T_PURGE_DATABASE: {
        REGISTER_STMT_RESOLVER(PurgeDatabase);
        break;
      }
      case T_PURGE_RECYCLEBIN: {
        REGISTER_STMT_RESOLVER(PurgeRecycleBin);
        break;
      }
      case T_OPTIMIZE_TABLE: {
        REGISTER_STMT_RESOLVER(OptimizeTable);
        break;
      }
      case T_PREPARE: {
        if (params_.is_prepare_protocol_) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          REGISTER_STMT_RESOLVER(Prepare);
        }
        break;
      }
      case T_EXECUTE: {
        if (params_.is_prepare_protocol_) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          REGISTER_STMT_RESOLVER(Execute);
        }
        break;
      }
      case T_DEALLOCATE: {
        if (params_.is_prepare_protocol_) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          REGISTER_STMT_RESOLVER(Deallocate);
        }
        break;
      }
      case T_VARIABLE_SET: {
        REGISTER_STMT_RESOLVER(VariableSet);
        break;
      }
      case T_TRANSACTION: {
        REGISTER_STMT_RESOLVER(SetTransaction);
        break;
      }
      case T_SHOW_TABLES:
      case T_SHOW_DATABASES:
      case T_SHOW_VARIABLES:
      case T_SHOW_COLUMNS:
      case T_SHOW_SCHEMA:
      case T_SHOW_CREATE_DATABASE:
      case T_SHOW_CREATE_TABLE:
      case T_SHOW_CREATE_VIEW:
      case T_SHOW_CREATE_PROCEDURE:
      case T_SHOW_CREATE_FUNCTION:
      case T_SHOW_TABLE_STATUS:
      case T_SHOW_PARAMETERS:
      case T_SHOW_INDEXES:
      case T_SHOW_PROCESSLIST:
      case T_SHOW_WARNINGS:
      case T_SHOW_ERRORS:
      case T_SHOW_TRACE:
      case T_SHOW_ENGINES:
      case T_SHOW_PROFILE:
      case T_SHOW_ENGINE:
      case T_SHOW_OPEN_TABLES:
      case T_SHOW_PRIVILEGES:
      case T_SHOW_CHARSET:
      case T_SHOW_COLLATION:
      case T_SHOW_GRANTS:
      case T_SHOW_RECYCLEBIN:
      case T_SHOW_PROCEDURE_STATUS:
      case T_SHOW_FUNCTION_STATUS:
      case T_SHOW_TRIGGERS:
      case T_SHOW_STATUS:
      case T_SHOW_CREATE_TRIGGER:
      case T_SHOW_CHECK_TABLE:
      case T_SHOW_CREATE_USER: {
        REGISTER_STMT_RESOLVER(Show);
        break;
      }
      case T_CREATE_USER: {
        REGISTER_STMT_RESOLVER(CreateUser);
        break;
      }
      case T_DROP_USER: {
        REGISTER_STMT_RESOLVER(DropUser);
        break;
      }
      case T_RENAME_USER: {
        REGISTER_STMT_RESOLVER(RenameUser);
        break;
      }
      case T_SET_PASSWORD: {
        REGISTER_STMT_RESOLVER(SetPassword);
        break;
      }
      case T_LOCK_USER: {
        REGISTER_STMT_RESOLVER(LockUser);
        break;
      }
      case T_ALTER_USER_DEFAULT_ROLE:
      case T_SET_ROLE: {
        REGISTER_STMT_RESOLVER(AlterUserRole);
        break;
      }

      case T_GRANT_ROLE:
      case T_SYSTEM_GRANT:
      case T_GRANT: {
        REGISTER_STMT_RESOLVER(Grant);
        break;
      }
      case T_SYSTEM_REVOKE:
      case T_REVOKE:
      case T_REVOKE_ALL: {
        REGISTER_STMT_RESOLVER(Revoke);
        break;
      }
      case T_EXPLAIN: {
        REGISTER_STMT_RESOLVER(Explain);
        break;
      }
      case T_ALTER_SYSTEM_SET: {
        REGISTER_STMT_RESOLVER(AlterSystemSet);
        break;
      }
      case T_ALTER_SYSTEM_KILL: {
        REGISTER_STMT_RESOLVER(Kill);
        break;
      }
      case T_ALTER_SESSION_SET: {
        REGISTER_STMT_RESOLVER(AlterSessionSet);
        break;
      }
      case T_SET_NAMES:
        // fall through
      case T_SET_CHARSET: {
        REGISTER_STMT_RESOLVER(SetNames);
        break;
      }
      case T_KILL: {
        REGISTER_STMT_RESOLVER(Kill);
        break;
      }
      case T_EMPTY_QUERY: {
        REGISTER_STMT_RESOLVER(EmptyQuery);
        break;
      }
      case T_FLUSH_PRIVILEGES: {
        REGISTER_STMT_RESOLVER(EmptyQuery);
        break;
      }
      case T_LOCK_TABLE: {
        REGISTER_STMT_RESOLVER(LockTable);
        break;
      }
      case T_CREATE_OUTLINE: {
        REGISTER_STMT_RESOLVER(CreateOutline);
        break;
      }
      case T_ALTER_OUTLINE: {
        REGISTER_STMT_RESOLVER(AlterOutline);
        break;
      }
      case T_DROP_OUTLINE: {
        REGISTER_STMT_RESOLVER(DropOutline);
        break;
      }
      case T_SP_CREATE: {
        REGISTER_STMT_RESOLVER(CreateProcedure);
        break;
      }
      case T_SP_ALTER: {
        REGISTER_STMT_RESOLVER(AlterProcedure);
        break;
      }
      case T_SP_CALL_STMT: {
        REGISTER_STMT_RESOLVER(CallProcedure);
        break;
      }
      case T_SF_CREATE: {
        REGISTER_STMT_RESOLVER(CreateFunction);
        break;
      }
      case T_SF_ALTER: {
        REGISTER_STMT_RESOLVER(AlterFunction);
        break;
      }
      case T_SP_DROP: {
        REGISTER_STMT_RESOLVER(DropProcedure);
        break;
      }
      case T_SF_DROP: {
        REGISTER_STMT_RESOLVER(DropFunction);
        break;
      }
      case T_SP_ANONYMOUS_BLOCK: {
        REGISTER_STMT_RESOLVER(AnonymousBlock);
        break;
      }
      case T_PACKAGE_CREATE: {
        REGISTER_STMT_RESOLVER(CreatePackage);
        break;
      }
      case T_PACKAGE_CREATE_BODY: {
        REGISTER_STMT_RESOLVER(CreatePackageBody);
        break;
      }
      case T_PACKAGE_DROP: {
        REGISTER_STMT_RESOLVER(DropPackage);
        break;
      }
      case T_MYSQL_UPDATE_HISTOGRAM:
      case T_MYSQL_DROP_HISTOGRAM:
      case T_MYSQL_ANALYZE: {
        REGISTER_STMT_RESOLVER(AnalyzeStmt);
        break;
      }
      case T_LOAD_DATA_URL:
      case T_LOAD_DATA: {
        REGISTER_STMT_RESOLVER(LoadData);
        break;
      }
      case T_SET_TABLE_COMMENT:
      case T_SET_COLUMN_COMMENT: {
        REGISTER_STMT_RESOLVER(SetComment);
        break;
      }
      case T_CREATE_ROLE: {
        REGISTER_STMT_RESOLVER(CreateRole);
        break;
      }
      case T_DROP_ROLE: {
        REGISTER_STMT_RESOLVER(DropRole);
        break;
      }
      case T_ALTER_ROLE: {
        REGISTER_STMT_RESOLVER(AlterRole);
        break;
      }
      case T_CREATE_SAVEPOINT:
      case T_ROLLBACK_SAVEPOINT:
      case T_RELEASE_SAVEPOINT: {
        REGISTER_STMT_RESOLVER(SavePoint);
        break;
      }
      case T_TG_CREATE: {
        REGISTER_STMT_RESOLVER(Trigger);
        break;
      }
      case T_TG_DROP: {
        REGISTER_STMT_RESOLVER(Trigger);
        break;
      }
      case T_TG_ALTER: {
        REGISTER_STMT_RESOLVER(Trigger);
        break;
      }
      case T_DIAGNOSTICS: {
        REGISTER_STMT_RESOLVER(GetDiagnostics);
        break;
      }
      case T_ALTER_SYSTEM_RESET_PARAMETER: {
        REGISTER_STMT_RESOLVER(ResetConfig);
        break;
      }
      case T_ALTER_SYSTEM_RESET: {
        REGISTER_STMT_RESOLVER(AlterSystemReset);
        break;
      }
      default: {
        ret = OB_NOT_SUPPORTED;
        const char *type_name = get_type_name(parse_tree.type_);
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "statement type");
        break;
      }
    }  // end switch

    if (OB_SUCC(ret) && OB_NOT_NULL(stmt) && stmt->is_dml_stmt()
        && !stmt->is_explain_stmt() && !is_ai_column_retry) {
      if (OB_FAIL(check_ai_column_strict_read(params_, *static_cast<ObDMLStmt*>(stmt)))) {
        LOG_WARN("failed to check the ai column strict read", K(ret));
      }
    }

    if (OB_SUCC(ret) && stmt->is_dml_write_stmt()) {
      // todo yanli:check leader-follower database
    }

    if (OB_SUCC(ret) && stmt->is_dml_stmt() && !params_.session_info_->is_varparams_sql_prepare()) {
      ObDMLStmt *dml_stmt = static_cast<ObDMLStmt*>(stmt);
      ObRawExprWrapEnumSet enum_set_wrapper(*params_.expr_factory_, params_.session_info_);
      if (OB_FAIL(enum_set_wrapper.wrap_enum_set(*dml_stmt))) {
      }
    }

    if (OB_SUCC(ret) && stmt->is_dml_stmt() && !stmt->is_explain_stmt()) {
      if (OB_FAIL(params_.query_ctx_->query_hint_.init_query_hint(params_.allocator_,
                                                                  params_.session_info_,
                                                                  static_cast<ObDMLStmt*>(stmt)))) {
      } else if (OB_FAIL(params_.query_ctx_->query_hint_.check_and_set_params_from_hint(params_,
                                                         *static_cast<ObDMLStmt*>(stmt)))) {
      }
    }

    if (OB_SUCC(ret) && stmt->is_dml_stmt() && !stmt->is_explain_stmt()) {
      bool is_contain_inner_table = false;
      bool is_contain_select_for_update = false;
      ObDMLStmt *dml_stmt = static_cast<ObDMLStmt*>(stmt);
      if (OB_FAIL(dml_stmt->check_if_contain_inner_table(is_contain_inner_table))) {
      } else if (OB_FAIL(dml_stmt->check_if_contain_select_for_update(
                  is_contain_select_for_update))) {
      } else {
        params_.query_ctx_->is_contain_inner_table_ = is_contain_inner_table;
        params_.query_ctx_->is_contain_select_for_update_ = is_contain_select_for_update;
        params_.query_ctx_->has_dml_write_stmt_ = dml_stmt->is_dml_write_stmt();
      }

    }
    if (OB_SUCC(ret)) {
      stmt::StmtType stmt_type = stmt->get_stmt_type();
      if (ObStmt::is_ddl_stmt(stmt_type, stmt->has_global_variable()) || ObStmt::is_dcl_stmt(stmt_type)) {
        ObDDLStmt *ddl_stmt = static_cast<ObDDLStmt*>(stmt);
        obcall::ObDDLArg &ddl_arg = ddl_stmt->get_ddl_arg();

        if (OB_ISNULL(params_.query_ctx_)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ddl_arg.ddl_stmt_str_ = params_.query_ctx_->get_sql_stmt();
        }
      }
    }
  }  // end if
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
