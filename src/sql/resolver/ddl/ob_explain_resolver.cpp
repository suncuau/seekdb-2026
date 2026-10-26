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
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_file_table_reader.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;

namespace
{

// Appends a SQL string literal, quoting it so that it can be parsed back.
int append_sql_literal(ObSqlString &sql, const ObString &str)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  } else {
    for (int32_t i = 0; OB_SUCC(ret) && i < str.length(); ++i) {
      const char c = str.ptr()[i];
      if ('\'' == c) {
        ret = sql.append("''");
      } else if ('\\' == c) {
        ret = sql.append("\\\\");
      } else {
        ret = sql.append(&c, 1);
      }
    }
    if (OB_SUCC(ret)) {
      ret = sql.append("'");
    }
  }
  return ret;
}

int find_file_table(ObSelectStmt &select_stmt, const TableItem *&file_table)
{
  int ret = OB_SUCCESS;
  file_table = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && OB_ISNULL(file_table) &&
                      i < select_stmt.get_table_size(); ++i) {
    const TableItem *item = select_stmt.get_table_item(i);
    if (OB_ISNULL(item)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null table item", K(ret));
    } else if (item->is_file_table()) {
      file_table = item;
    }
  }
  return ret;
}

// Builds the DuckDB style DESCRIBE result: one row per output column of the
// child query, with the name, the type name and whether NULLs were seen.
int gen_file_table_describe_sql(ObSelectStmt &select_stmt,
                                const TableItem &file_table,
                                ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const ObFileTableInfo *file_info = file_table.get_file_table_info();
  if (OB_ISNULL(file_info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file table info is null", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt.get_select_item_size(); ++i) {
    const SelectItem &select_item = select_stmt.get_select_item(i);
    const ObRawExpr *expr = select_item.expr_;
    const ObFileColumnMeta *meta = NULL;
    ObString column_name;
    ObString type_name;
    bool nullable = true;
    if (!select_item.alias_name_.empty()) {
      column_name = select_item.alias_name_;
    } else if (!select_item.expr_name_.empty()) {
      column_name = select_item.expr_name_;
    }
    if (OB_NOT_NULL(expr) && T_REF_COLUMN == expr->get_expr_type()) {
      const ObColumnRefRawExpr *column_expr = static_cast<const ObColumnRefRawExpr *>(expr);
      const int64_t column_idx =
          static_cast<int64_t>(column_expr->get_column_id()) - OB_APP_MIN_COLUMN_ID;
      if (column_expr->get_table_id() == file_table.table_id_ &&
          column_idx >= 0 && column_idx < file_info->columns_.count()) {
        meta = &file_info->columns_.at(column_idx);
      }
    }
    if (OB_NOT_NULL(meta)) {
      nullable = meta->nullable_;
      if (OB_FAIL(ob_file_sql_type_name(meta->type_, type_name))) {
      } else if (column_name.empty()) {
        column_name = meta->name_;
      }
    } else if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null select expr", K(ret));
    } else if (OB_FAIL(ob_file_sql_type_name(expr->get_result_type().get_type(), type_name))) {
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append(0 == i ? "select " : " union all select "))) {
    } else if (OB_FAIL(append_sql_literal(sql, column_name))) {
    } else if (OB_FAIL(sql.append(" as column_name, "))) {
    } else if (OB_FAIL(append_sql_literal(sql, type_name))) {
    } else if (OB_FAIL(sql.append(" as column_type, "))) {
    } else if (OB_FAIL(sql.append(nullable ? "'YES'" : "'NO'"))) {
    } else if (OB_FAIL(sql.append(" as `null`, NULL as `key`, "
                                  "NULL as `default`, NULL as extra"))) {
    }
  }
  return ret;
}

int resolve_generated_sql(ObResolverParams &params,
                          const ObString &sql_text,
                          ObStmt *&new_stmt)
{
  int ret = OB_SUCCESS;
  ObParser parser(*params.allocator_,
                  params.session_info_->get_sql_mode(),
                  params.session_info_->get_charsets4parser());
  ParseResult parse_result;
  new_stmt = NULL;
  if (OB_FAIL(parser.parse(sql_text, parse_result))) {
    LOG_WARN("failed to parse the describe statement", K(ret), K(sql_text));
  } else if (OB_ISNULL(parse_result.result_tree_) ||
             1 != parse_result.result_tree_->num_child_ ||
             OB_ISNULL(parse_result.result_tree_->children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected describe parse result", K(ret));
  } else {
    ObResolver resolver(params);
    if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                 *parse_result.result_tree_->children_[0],
                                 new_stmt))) {
      LOG_WARN("failed to resolve the describe statement", K(ret), K(sql_text));
    }
  }
  return ret;
}

} // anonymous namespace

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format && T_DESCRIBE == format->type_ && child_stmt->is_select_stmt()) {
          // DuckDB style: DESCRIBE <query> describes the columns of the query
          // result instead of the plan of the query.
          const TableItem *file_table = NULL;
          ObSqlString describe_sql;
          ObString sql_text;
          ObStmt *describe_stmt = NULL;
          if (OB_FAIL(find_file_table(*static_cast<ObSelectStmt *>(child_stmt), file_table))) {
          } else if (OB_ISNULL(file_table)) {
            explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
          } else if (OB_FAIL(gen_file_table_describe_sql(*static_cast<ObSelectStmt *>(child_stmt),
                                                         *file_table,
                                                         describe_sql))) {
            LOG_WARN("failed to generate the describe statement", K(ret));
          } else if (OB_FAIL(ob_write_string(*allocator_,
                                             ObString(describe_sql.length(), describe_sql.ptr()),
                                             sql_text,
                                             true))) {
          } else if (OB_FAIL(resolve_generated_sql(params_, sql_text, describe_stmt))) {
          } else if (OB_ISNULL(describe_stmt)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("failed to resolve the describe statement", K(ret));
          } else {
            set_basic_stmt(describe_stmt);
          }
        } else if (FALSE_IT(explain_stmt->set_explain_query_stmt(
                        static_cast<ObDMLStmt *>(child_stmt)))) {
        }
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          case T_DESCRIBE: {
            // a DESCRIBE marker: the statement has been rewritten already
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
