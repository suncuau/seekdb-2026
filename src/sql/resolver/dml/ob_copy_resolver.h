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

#ifndef OCEANBASE_SQL_RESOLVER_DML_OB_COPY_RESOLVER_H_
#define OCEANBASE_SQL_RESOLVER_DML_OB_COPY_RESOLVER_H_

#include "sql/resolver/ob_stmt_resolver.h"

namespace oceanbase
{
namespace sql
{
class ObSelectStmt;
struct TableItem;

// COPY (query) TO 'file' (format csv, header, nullstr 'NULL')
class ObCopyResolver : public ObStmtResolver
{
public:
  explicit ObCopyResolver(ObResolverParams &params) : ObStmtResolver(params) {}
  virtual ~ObCopyResolver() {}
  virtual int resolve(const ParseNode &parse_tree);
private:
  int do_copy(const ParseNode &select_node,
              const ParseNode &path_node,
              const ParseNode *option_node);
  int resolve_copy_options(const ParseNode *option_node,
                           bool &with_header,
                           ObString &null_str);
  int find_file_table(ObSelectStmt &select_stmt, const TableItem *&file_table);
  int write_csv_file(const ObString &path,
                     const TableItem &file_table,
                     ObSelectStmt &select_stmt,
                     const bool with_header,
                     const ObString &null_str);
  DISALLOW_COPY_AND_ASSIGN(ObCopyResolver);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_DML_OB_COPY_RESOLVER_H_
