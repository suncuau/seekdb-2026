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

#ifndef OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_READER_H_
#define OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_READER_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_array.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_object.h"
#include "common/timezone/ob_time_convert.h"

namespace oceanbase
{
namespace sql
{

// column description of a table backed by a local file
struct ObFileColumnMeta
{
  ObFileColumnMeta() : name_(), type_(common::ObMaxType), nullable_(true) {}
  common::ObString name_;
  common::ObObjType type_;
  bool nullable_;
  TO_STRING_KV(K_(name), K_(type), K_(nullable));
};

// a raw cell is either a piece of text or NULL
struct ObFileCell
{
  ObFileCell() : str_(), is_null_(false) {}
  common::ObString str_;
  bool is_null_;
};

struct ObFileReadOptions
{
  ObFileReadOptions() : file_type_(0), has_header_(true), format_() {}
  int64_t file_type_;
  bool has_header_;
  common::ObString format_;
};

// Everything known about a table backed by a local file. It is kept on the
// TableItem so that DESCRIBE and COPY can reuse the already parsed schema.
struct ObFileTableInfo
{
  ObFileTableInfo() : path_(), options_(), columns_(), row_cnt_(0) {}
  common::ObString path_;
  ObFileReadOptions options_;
  common::ObArray<ObFileColumnMeta> columns_;
  int64_t row_cnt_;
};

// Read a local file (csv/json/parquet, or a glob pattern) and materialize its
// content as a set of column metadata plus a row-major value matrix.
// All the memory (names, strings, value matrix) is allocated from alloc, which
// must outlive the generated plan.
class ObFileTableReader
{
public:
  enum FileType
  {
    FILE_CSV = 1,
    FILE_JSON = 2,
    FILE_PARQUET = 3,
    FILE_GLOB = 4,
  };
  ObFileTableReader(common::ObIAllocator &alloc,
                    const common::ObTimeZoneInfo *tz_info,
                    const common::ObCollationType cs_type)
      : alloc_(alloc), tz_info_(tz_info), cs_type_(cs_type) {}
  ~ObFileTableReader() {}

  // values is filled row by row, i.e. values[col_idx + row_idx * columns.count()].
  int read(const common::ObString &path,
           const ObFileReadOptions &options,
           common::ObIArray<ObFileColumnMeta> &columns,
           common::ObIArray<common::ObObjParam> &values,
           int64_t &row_cnt);

private:
  int read_whole_file(const common::ObString &path, char *&buf, int64_t &len);
  int parse_csv_record(const char *buf, const int64_t len, int64_t &pos,
                       common::ObIArray<ObFileCell> &fields);

  int read_csv(const char *buf, const int64_t len, const bool has_header,
               common::ObIArray<ObFileColumnMeta> &columns,
               common::ObIArray<common::ObObjParam> &values,
               int64_t &row_cnt);
  int read_json(const char *buf, const int64_t len,
                common::ObIArray<ObFileColumnMeta> &columns,
                common::ObIArray<common::ObObjParam> &values,
                int64_t &row_cnt);
  int read_glob(const common::ObString &path,
                common::ObIArray<ObFileColumnMeta> &columns,
                common::ObIArray<common::ObObjParam> &values,
                int64_t &row_cnt);
  int read_parquet(const char *buf, const int64_t len,
                   common::ObIArray<ObFileColumnMeta> &columns,
                   common::ObIArray<common::ObObjParam> &values,
                   int64_t &row_cnt);

  // build the final column types out of the raw cell matrix
  int build_columns(const common::ObIArray<common::ObString> &names,
                    const common::ObIArray<ObFileCell> &cells,
                    const int64_t col_cnt,
                    const int64_t row_cnt,
                    common::ObIArray<ObFileColumnMeta> &columns,
                    common::ObIArray<common::ObObjParam> &values);

  int store_str(const common::ObString &src, common::ObString &dst);
  int make_varchar(const common::ObString &src, common::ObObjParam &obj);

private:
  common::ObIAllocator &alloc_;
  const common::ObTimeZoneInfo *tz_info_;
  common::ObCollationType cs_type_;
  DISALLOW_COPY_AND_ASSIGN(ObFileTableReader);
};

int ob_file_sql_type_name(const common::ObObjType type, common::ObString &name);

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_READER_H_
