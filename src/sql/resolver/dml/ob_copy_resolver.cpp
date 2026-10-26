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
#include "sql/resolver/dml/ob_copy_resolver.h"

#include <algorithm>
#include <cstdio>

#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/cmd/ob_empty_query_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/dml/ob_file_table_reader.h"
#include "sql/session/ob_basic_session_info.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace
{

struct CopyRowCompare
{
  CopyRowCompare(const ObValuesTableDef &table_def,
                 const int64_t column_cnt,
                 const ObIArray<int64_t> &key_columns,
                 const ObIArray<bool> &key_ascending,
                 const ObIArray<bool> &key_null_first,
                 const ObCollationType cs_type)
      : table_def_(table_def),
        column_cnt_(column_cnt),
        key_columns_(key_columns),
        key_ascending_(key_ascending),
        key_null_first_(key_null_first),
        cs_type_(cs_type) {}
  bool operator()(const int64_t lhs, const int64_t rhs) const
  {
    bool less = false;
    for (int64_t i = 0; i < key_columns_.count(); ++i) {
      const ObObj &left = table_def_.access_objs_.at(lhs * column_cnt_ + key_columns_.at(i));
      const ObObj &right = table_def_.access_objs_.at(rhs * column_cnt_ + key_columns_.at(i));
      int cmp = 0;
      if (left.is_null() && right.is_null()) {
        cmp = 0;
      } else if (left.is_null()) {
        cmp = key_null_first_.at(i) ? -1 : 1;
      } else if (right.is_null()) {
        cmp = key_null_first_.at(i) ? 1 : -1;
      } else {
        cmp = left.compare(right, cs_type_);
      }
      if (0 != cmp) {
        less = key_ascending_.at(i) ? (cmp < 0) : (cmp > 0);
        break;
      }
    }
    return less;
  }
  const ObValuesTableDef &table_def_;
  const int64_t column_cnt_;
  const ObIArray<int64_t> &key_columns_;
  const ObIArray<bool> &key_ascending_;
  const ObIArray<bool> &key_null_first_;
  const ObCollationType cs_type_;
};

int write_csv_field(FILE *fp, const char *ptr, const int64_t len)
{
  int ret = OB_SUCCESS;
  bool need_quote = false;
  for (int64_t i = 0; i < len && !need_quote; ++i) {
    const char c = ptr[i];
    need_quote = (',' == c || '"' == c || '\n' == c || '\r' == c);
  }
  if (need_quote) {
    if (EOF == ::fputc('"', fp)) {
      ret = OB_IO_ERROR;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
        if ('"' == ptr[i] && EOF == ::fputc('"', fp)) {
          ret = OB_IO_ERROR;
        } else if (EOF == ::fputc(ptr[i], fp)) {
          ret = OB_IO_ERROR;
        }
      }
      if (OB_SUCC(ret) && EOF == ::fputc('"', fp)) {
        ret = OB_IO_ERROR;
      }
    }
  } else if (len > 0 && len != ::fwrite(ptr, 1, len, fp)) {
    ret = OB_IO_ERROR;
  }
  return ret;
}

} // anonymous namespace

int ObCopyResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObEmptyQueryStmt *copy_stmt = NULL;
  bool enable_file_sql = false;
  if (OB_UNLIKELY(T_COPY_TO != parse_tree.type_ || 3 != parse_tree.num_child_) ||
      OB_ISNULL(parse_tree.children_) ||
      OB_ISNULL(parse_tree.children_[0]) || OB_ISNULL(parse_tree.children_[1])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid copy statement", K(ret), K(parse_tree.type_), K(parse_tree.num_child_));
  } else if (OB_ISNULL(session_info_) || OB_ISNULL(allocator_) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid resolver params", K(ret));
  } else if (OB_FAIL(session_info_->get_enable_file_sql(enable_file_sql))) {
  } else if (!enable_file_sql) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy while enable_file_sql is off");
    LOG_WARN("file sql is disabled", K(ret));
  } else if (OB_ISNULL(copy_stmt = create_stmt<ObEmptyQueryStmt>())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to create empty query stmt", K(ret));
  } else if (FALSE_IT(session_info_->set_stmt_type(stmt::T_EMPTY_QUERY))) {
  } else if (OB_FAIL(do_copy(*parse_tree.children_[0],
                             *parse_tree.children_[1],
                             parse_tree.children_[2]))) {
    LOG_WARN("failed to copy query result to file", K(ret));
  }
  return ret;
}

int ObCopyResolver::do_copy(const ParseNode &select_node,
                            const ParseNode &path_node,
                            const ParseNode *option_node)
{
  int ret = OB_SUCCESS;
  ObResolver resolver(params_);
  ObStmt *child_stmt = NULL;
  ObSelectStmt *select_stmt = NULL;
  const TableItem *file_table = NULL;
  ObString path;
  ObString null_str;
  bool with_header = false;
  if (OB_UNLIKELY(T_VARCHAR != path_node.type_ && T_CHAR != path_node.type_) ||
      OB_ISNULL(path_node.str_value_) || path_node.str_len_ <= 0) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "non constant file path of copy");
    LOG_WARN("copy target must be a constant string", K(ret));
  } else if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, select_node, child_stmt))) {
  } else if (OB_ISNULL(child_stmt) || !child_stmt->is_select_stmt()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy source which is not a select");
    LOG_WARN("unexpected copy source", K(ret));
  } else if (FALSE_IT(select_stmt = static_cast<ObSelectStmt *>(child_stmt))) {
  } else if (OB_FAIL(resolve_copy_options(option_node, with_header, null_str))) {
  } else if (OB_FAIL(find_file_table(*select_stmt, file_table))) {
  } else {
    char *buf = static_cast<char *>(allocator_->alloc(path_node.str_len_));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, path_node.str_value_, path_node.str_len_);
      path.assign_ptr(buf, static_cast<int32_t>(path_node.str_len_));
      ret = write_csv_file(path, *file_table, *select_stmt, with_header, null_str);
    }
  }
  return ret;
}

int ObCopyResolver::resolve_copy_options(const ParseNode *option_node,
                                         bool &with_header,
                                         ObString &null_str)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ParseNode *, 4> options;
  const ParseNode *cur = option_node;
  with_header = false;
  null_str.reset();
  while (OB_SUCC(ret) && OB_NOT_NULL(cur)) {
    if (T_LINK_NODE == cur->type_) {
      if (OB_ISNULL(cur->children_) || OB_ISNULL(cur->children_[1])) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(options.push_back(cur->children_[1]))) {
      } else {
        cur = cur->children_[0];
      }
    } else if (T_COPY_OPTION == cur->type_) {
      if (OB_FAIL(options.push_back(cur))) {
      } else {
        cur = NULL;
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid copy option node", K(ret), K(cur->type_));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < options.count(); ++i) {
    const ParseNode *option = options.at(i);
    if (OB_ISNULL(option) || 2 != option->num_child_ || OB_ISNULL(option->children_) ||
        OB_ISNULL(option->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const ParseNode *name_node = option->children_[0];
      const ParseNode *value_node = option->children_[1];
      ObString name(name_node->str_len_, name_node->str_value_);
      if (0 == name.case_compare("format")) {
        ObString format;
        if (OB_NOT_NULL(value_node)) {
          format.assign_ptr(value_node->str_value_, static_cast<int32_t>(value_node->str_len_));
        }
        if (0 != format.case_compare("csv")) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy format other than csv");
          LOG_WARN("unsupported copy format", K(ret), K(format));
        }
      } else if (0 == name.case_compare("header")) {
        with_header = true;
      } else if (0 == name.case_compare("nullstr")) {
        if (OB_NOT_NULL(value_node) && OB_NOT_NULL(value_node->str_value_)) {
          null_str.assign_ptr(value_node->str_value_,
                              static_cast<int32_t>(value_node->str_len_));
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy option");
        LOG_WARN("unsupported copy option", K(ret), K(name));
      }
    }
  }
  return ret;
}

int ObCopyResolver::find_file_table(ObSelectStmt &select_stmt, const TableItem *&file_table)
{
  int ret = OB_SUCCESS;
  file_table = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && OB_ISNULL(file_table) && i < select_stmt.get_table_size(); ++i) {
    const TableItem *item = select_stmt.get_table_item(i);
    if (OB_ISNULL(item)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!item->is_file_table()) {
      // only a query over a file table can be copied
    } else if (OB_NOT_NULL(file_table)) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy from more than one file table");
      LOG_WARN("more than one file table in copy source", K(ret));
    } else {
      file_table = item;
    }
  }
  if (OB_SUCC(ret) && OB_ISNULL(file_table)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy from a query without a file table");
    LOG_WARN("no file table found in copy source", K(ret));
  }
  return ret;
}

int ObCopyResolver::write_csv_file(const ObString &path,
                                   const TableItem &file_table,
                                   ObSelectStmt &select_stmt,
                                   const bool with_header,
                                   const ObString &null_str)
{
  int ret = OB_SUCCESS;
  const ObFileTableInfo *file_info = file_table.get_file_table_info();
  const ObValuesTableDef *table_def = file_table.values_table_def_;
  ObSEArray<int64_t, 8> out_columns;
  ObSEArray<int64_t, 4> key_columns;
  ObSEArray<bool, 4> key_ascending;
  ObSEArray<bool, 4> key_null_first;
  ObSEArray<int64_t, 16> rows;
  ObCollationType cs_type = CS_TYPE_UTF8MB4_GENERAL_CI;
  char *path_buf = NULL;
  FILE *fp = NULL;
  int64_t column_cnt = 0;
  int64_t row_cnt = 0;
  if (OB_ISNULL(file_info) || OB_ISNULL(table_def)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(column_cnt = table_def->column_cnt_)) {
  } else if (OB_UNLIKELY(column_cnt <= 0) ||
             OB_UNLIKELY(table_def->access_objs_.count() % column_cnt != 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid values table of the file table", K(ret), K(column_cnt));
  } else if (FALSE_IT(row_cnt = table_def->access_objs_.count() / column_cnt)) {
  } else if (OB_ISNULL(session_info_) ||
             OB_FAIL(session_info_->get_collation_connection(cs_type))) {
  }
  // the selected columns of the file table
  for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt.get_select_item_size(); ++i) {
    const ObRawExpr *expr = select_stmt.get_select_item(i).expr_;
    int64_t column_idx = -1;
    if (OB_ISNULL(expr) || T_REF_COLUMN != expr->get_expr_type()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy of an expression which is not a file column");
      LOG_WARN("copy only supports plain columns of a file table", K(ret));
    } else {
      const ObColumnRefRawExpr *column_expr = static_cast<const ObColumnRefRawExpr *>(expr);
      column_idx = static_cast<int64_t>(column_expr->get_column_id()) - OB_APP_MIN_COLUMN_ID;
      if (column_expr->get_table_id() != file_table.table_id_ ||
          column_idx < 0 || column_idx >= column_cnt) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy of a column which is not a file column");
        LOG_WARN("copy only supports columns of the file table", K(ret), K(column_idx));
      } else if (OB_FAIL(out_columns.push_back(column_idx))) {
      }
    }
  }
  // the sort keys
  for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt.get_order_item_size(); ++i) {
    const OrderItem &order_item = select_stmt.get_order_item(i);
    const ObRawExpr *expr = order_item.expr_;
    int64_t column_idx = -1;
    if (OB_ISNULL(expr) || T_REF_COLUMN != expr->get_expr_type()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy ordered by an expression which is not a file column");
      LOG_WARN("copy only supports ordering by file columns", K(ret));
    } else {
      const ObColumnRefRawExpr *column_expr = static_cast<const ObColumnRefRawExpr *>(expr);
      column_idx = static_cast<int64_t>(column_expr->get_column_id()) - OB_APP_MIN_COLUMN_ID;
      if (column_expr->get_table_id() != file_table.table_id_ ||
          column_idx < 0 || column_idx >= column_cnt) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("copy only supports ordering by columns of the file table", K(ret));
      } else if (OB_FAIL(key_columns.push_back(column_idx))) {
      } else if (OB_FAIL(key_ascending.push_back(order_item.is_ascending()))) {
      } else if (OB_FAIL(key_null_first.push_back(order_item.is_null_first()))) {
      }
    }
  }
  if (OB_SUCC(ret) && out_columns.count() <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("no column to copy", K(ret));
  }
  // sort the rows
  for (int64_t i = 0; OB_SUCC(ret) && i < row_cnt; ++i) {
    if (OB_FAIL(rows.push_back(i))) {
    }
  }
  if (OB_SUCC(ret) && key_columns.count() > 0 && rows.count() > 1) {
    CopyRowCompare compare(*table_def, column_cnt, key_columns, key_ascending,
                           key_null_first, cs_type);
    std::sort(&rows.at(0), &rows.at(0) + rows.count(), compare);
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(path_buf = static_cast<char *>(allocator_->alloc(path.length() + 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (FALSE_IT(MEMCPY(path_buf, path.ptr(), path.length()))) {
  } else if (FALSE_IT(path_buf[path.length()] = '\0')) {
  } else if (OB_ISNULL(fp = ::fopen(path_buf, "wb"))) {
    ret = OB_IO_ERROR;
    LOG_WARN("failed to open copy target file", K(ret), K(path), K(errno));
  }
  // write the header
  if (OB_SUCC(ret) && with_header) {
    for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt.get_select_item_size(); ++i) {
      const SelectItem &select_item = select_stmt.get_select_item(i);
      const ObString &name = !select_item.alias_name_.empty() ?
                             select_item.alias_name_ : select_item.expr_name_;
      if (i > 0 && EOF == ::fputc(',', fp)) {
        ret = OB_IO_ERROR;
      } else if (OB_FAIL(write_csv_field(fp, name.ptr(), name.length()))) {
      }
    }
    if (OB_SUCC(ret) && EOF == ::fputc('\n', fp)) {
      ret = OB_IO_ERROR;
    }
  }
  // write the rows
  for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
    const int64_t row = rows.at(i);
    for (int64_t j = 0; OB_SUCC(ret) && j < out_columns.count(); ++j) {
      const ObObj &obj = table_def->access_objs_.at(row * column_cnt + out_columns.at(j));
      char *buf = NULL;
      int64_t pos = 0;
      if (j > 0 && EOF == ::fputc(',', fp)) {
        ret = OB_IO_ERROR;
      } else if (obj.is_null()) {
        if (EOF == ::fwrite(null_str.ptr(), 1, null_str.length(), fp) && null_str.length() > 0) {
          ret = OB_IO_ERROR;
        }
      } else {
        const int64_t str_len = ob_is_string_type(obj.get_type()) ? obj.get_string().length() : 0;
        const int64_t buf_len = str_len * 2 + 256;
        if (OB_ISNULL(buf = static_cast<char *>(allocator_->alloc(buf_len)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(obj.print_plain_str_literal(buf, buf_len, pos))) {
          LOG_WARN("failed to print file table value", K(ret), K(obj));
        } else if (OB_FAIL(write_csv_field(fp, buf, pos))) {
        }
      }
    }
    if (OB_SUCC(ret) && EOF == ::fputc('\n', fp)) {
      ret = OB_IO_ERROR;
    }
  }
  if (NULL != fp) {
    if (0 != ::fclose(fp)) {
      ret = OB_SUCC(ret) ? OB_IO_ERROR : ret;
    }
    fp = NULL;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
