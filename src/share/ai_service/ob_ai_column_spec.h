/**
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

#ifndef OCEANBASE_SHARE_AI_SERVICE_OB_AI_COLUMN_SPEC_H_
#define OCEANBASE_SHARE_AI_SERVICE_OB_AI_COLUMN_SPEC_H_

#include <string.h>
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_iarray.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/ob_errno.h"

namespace oceanbase
{
namespace share
{

// Hidden physical columns that back every AI column. The suffix of the column
// name is the name of the user visible result column.
constexpr const char *AI_COLUMN_STATE_PREFIX = "__ai_state_";
constexpr const char *AI_COLUMN_REQUEST_PREFIX = "__ai_request_";
constexpr const char *AI_COLUMN_SPEC_PREFIX = "__ai_spec_";

// Separator used to encode the AI column specification inside the comment of
// the hidden __ai_spec_ column. The DDL layer decodes it when the metadata row
// of __all_ai_column is written, so the specification travels with the table
// schema and no extra RPC structure is needed.
constexpr char AI_COLUMN_SPEC_FIELD_SEPARATOR = '\x1e';

constexpr const char *AI_COLUMN_JSON_PROMPT_SUFFIX =
    "\nReturn exactly one valid JSON value that follows the column definition, with no extra text.";

// Integer AI columns ask the model for a plain base-10 integer so that the
// result can be converted to the declared column type without further parsing.
constexpr const char *AI_COLUMN_INTEGER_PROMPT_SUFFIX =
    "\nReturn exactly one base-10 integer that follows the column definition, with no extra text.";

enum ObAiColumnState
{
  AI_COLUMN_STATE_PENDING = 0,
  AI_COLUMN_STATE_READY = 1,
  AI_COLUMN_STATE_FAILED = 2,
};

enum ObAiColumnFuncType
{
  AI_COLUMN_FUNC_INVALID = 0,
  AI_COLUMN_FUNC_COMPLETE = 1,
  AI_COLUMN_FUNC_EMBED = 2,
};

constexpr int64_t AI_COLUMN_MAX_ARGS = 8;

inline bool ai_column_is_space(const char c)
{
  return ' ' == c || '\t' == c || '\n' == c || '\r' == c;
}

inline bool ai_column_is_ident_start(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || '_' == c || (c & 0x80);
}

inline bool ai_column_is_ident_char(const char c)
{
  return ai_column_is_ident_start(c) || (c >= '0' && c <= '9') || '$' == c;
}

inline void ai_column_skip_space(const common::ObString &str, int64_t &pos)
{
  while (pos < str.length() && ai_column_is_space(str.ptr()[pos])) {
    ++pos;
  }
}

// Read one identifier starting at pos. pos is advanced to the first character
// that does not belong to the identifier.
inline void ai_column_read_ident(const common::ObString &str, int64_t &pos, common::ObString &ident)
{
  ai_column_skip_space(str, pos);
  const int64_t start = pos;
  if (pos < str.length() && ai_column_is_ident_start(str.ptr()[pos])) {
    ++pos;
    while (pos < str.length() && ai_column_is_ident_char(str.ptr()[pos])) {
      ++pos;
    }
    ident.assign_ptr(str.ptr() + start, static_cast<int32_t>(pos - start));
  } else {
    ident.reset();
  }
}

inline bool ai_column_name_equal(const common::ObString &lhs, const common::ObString &rhs)
{
  bool equal = (lhs.length() == rhs.length());
  for (int64_t i = 0; equal && i < lhs.length(); ++i) {
    char l = lhs.ptr()[i];
    char r = rhs.ptr()[i];
    if (l >= 'A' && l <= 'Z') {
      l = static_cast<char>(l - 'A' + 'a');
    }
    if (r >= 'A' && r <= 'Z') {
      r = static_cast<char>(r - 'A' + 'a');
    }
    equal = (l == r);
  }
  return equal;
}

// Parse the top level arguments of a function call expression. The argument
// texts point into ``expr`` and are therefore valid as long as ``expr`` is.
struct ObAiColumnExprInfo
{
  ObAiColumnExprInfo() { reset(); }
  void reset()
  {
    func_type_ = AI_COLUMN_FUNC_INVALID;
    func_name_.reset();
    arg_count_ = 0;
    for (int64_t i = 0; i < AI_COLUMN_MAX_ARGS; ++i) {
      args_[i].reset();
    }
  }
  ObAiColumnFuncType func_type_;
  common::ObString func_name_;
  int64_t arg_count_;
  common::ObString args_[AI_COLUMN_MAX_ARGS];
};

// Split ``expr`` (the text inside AI COLUMN ( ... )) into the function name and
// its top level arguments. Only the textual shape is inspected here.
inline int ai_column_parse_expr(const common::ObString &expr, ObAiColumnExprInfo &info)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  info.reset();
  ai_column_skip_space(expr, pos);
  ai_column_read_ident(expr, pos, info.func_name_);
  if (info.func_name_.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ai_column_skip_space(expr, pos);
    if (pos < expr.length() && '(' == expr.ptr()[pos]) {
      ++pos;
      int64_t depth = 1;
      int64_t arg_start = pos;
      char quote = 0;
      while (OB_SUCC(ret) && pos < expr.length() && depth > 0) {
        const char c = expr.ptr()[pos];
        if (0 != quote) {
          if ('\\' == c && '`' != quote) {
            pos += 2;
            continue;
          } else if (c == quote) {
            quote = 0;
          }
        } else if ('\'' == c || '"' == c || '`' == c) {
          quote = c;
        } else if ('(' == c) {
          ++depth;
        } else if (')' == c) {
          --depth;
          if (0 == depth && info.arg_count_ < AI_COLUMN_MAX_ARGS) {
            info.args_[info.arg_count_].assign_ptr(expr.ptr() + arg_start,
                                                   static_cast<int32_t>(pos - arg_start));
            ++info.arg_count_;
          }
        } else if (',' == c && 1 == depth) {
          if (info.arg_count_ < AI_COLUMN_MAX_ARGS) {
            info.args_[info.arg_count_].assign_ptr(expr.ptr() + arg_start,
                                                   static_cast<int32_t>(pos - arg_start));
            ++info.arg_count_;
          }
          arg_start = pos + 1;
        }
        ++pos;
      }
      if (depth > 0) {
        ret = OB_INVALID_ARGUMENT;
      }
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    if (OB_SUCC(ret)) {
      if (ai_column_name_equal(info.func_name_, common::ObString::make_string("ai_complete"))) {
        info.func_type_ = AI_COLUMN_FUNC_COMPLETE;
      } else if (ai_column_name_equal(info.func_name_, common::ObString::make_string("ai_embed"))) {
        info.func_type_ = AI_COLUMN_FUNC_EMBED;
      } else {
        info.func_type_ = AI_COLUMN_FUNC_INVALID;
      }
    }
  }
  return ret;
}

// Strip the surrounding quotes of a string literal argument, the result points
// into the argument text.
inline int ai_column_parse_string_literal(const common::ObString &arg, common::ObString &value)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  ai_column_skip_space(arg, pos);
  if (pos < arg.length() && ('\'' == arg.ptr()[pos] || '"' == arg.ptr()[pos])) {
    const char quote = arg.ptr()[pos];
    const int64_t start = ++pos;
    int64_t end = -1;
    while (pos < arg.length()) {
      if ('\\' == arg.ptr()[pos]) {
        pos += 2;
        continue;
      } else if (arg.ptr()[pos] == quote) {
        end = pos;
        break;
      }
      ++pos;
    }
    if (end < 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      value.assign_ptr(arg.ptr() + start, static_cast<int32_t>(end - start));
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

// Parse an integer literal argument, returns false when the argument is not a
// plain integer.
inline bool ai_column_parse_int_literal(const common::ObString &arg, int64_t &value)
{
  int64_t pos = 0;
  bool ok = true;
  bool negative = false;
  ai_column_skip_space(arg, pos);
  if (pos < arg.length() && ('-' == arg.ptr()[pos] || '+' == arg.ptr()[pos])) {
    negative = ('-' == arg.ptr()[pos]);
    ++pos;
  }
  const int64_t start = pos;
  int64_t v = 0;
  while (pos < arg.length() && arg.ptr()[pos] >= '0' && arg.ptr()[pos] <= '9') {
    v = v * 10 + (arg.ptr()[pos] - '0');
    ++pos;
  }
  ai_column_skip_space(arg, pos);
  if (start == pos || pos != arg.length()) {
    ok = false;
  } else {
    value = negative ? -v : v;
  }
  return ok;
}

inline void ai_column_trim(const common::ObString &str, common::ObString &out)
{
  int64_t start = 0;
  int64_t end = str.length();
  while (start < end && ai_column_is_space(str.ptr()[start])) {
    ++start;
  }
  while (end > start && ai_column_is_space(str.ptr()[end - 1])) {
    --end;
  }
  out.assign_ptr(str.ptr() + start, static_cast<int32_t>(end - start));
}

// Returns true when ``column_name`` appears in ``expr`` as a bare identifier
// (not inside a string literal and not used as a function name).
inline bool ai_column_expr_references_column(const common::ObString &expr,
                                             const common::ObString &column_name)
{
  bool found = false;
  if (!column_name.empty()) {
    int64_t pos = 0;
    char quote = 0;
    while (!found && pos < expr.length()) {
      const char c = expr.ptr()[pos];
      if (0 != quote) {
        if ('\\' == c && '`' != quote) {
          pos += 2;
          continue;
        } else if (c == quote) {
          quote = 0;
        }
        ++pos;
      } else if ('\'' == c || '"' == c || '`' == c) {
        quote = c;
        ++pos;
      } else if (ai_column_is_ident_start(c)) {
        const int64_t start = pos;
        while (pos < expr.length() && ai_column_is_ident_char(expr.ptr()[pos])) {
          ++pos;
        }
        const common::ObString ident(static_cast<int32_t>(pos - start), expr.ptr() + start);
        if (ai_column_name_equal(ident, column_name)) {
          // a bare identifier followed by '(' is a function call, not a column
          int64_t next = pos;
          ai_column_skip_space(expr, next);
          if (next >= expr.length() || '(' != expr.ptr()[next]) {
            found = true;
          }
        }
      } else {
        ++pos;
      }
    }
  }
  return found;
}

// Encode/decode the AI column specification that is stored in the comment of
// the hidden __ai_spec_ column.
struct ObAiColumnSpec
{
  ObAiColumnSpec() { reset(); }
  void reset()
  {
    max_retries_ = 0;
    sync_mode_.reset();
    result_type_.reset();
    model_name_.reset();
    dependency_columns_.reset();
    raw_expr_.reset();
    normalized_expr_.reset();
  }
  bool is_valid() const { return !normalized_expr_.empty() || !raw_expr_.empty(); }

  static int append_field(common::ObSqlString &buf,
                          const char *key,
                          const common::ObString *str_value,
                          const int64_t *int_value)
  {
    int ret = OB_SUCCESS;
    if (0 != buf.length() && OB_FAIL(buf.append_fmt("%c", AI_COLUMN_SPEC_FIELD_SEPARATOR))) {
    } else if (OB_FAIL(buf.append(key))) {
    } else if (OB_FAIL(buf.append("="))) {
    } else if (nullptr != str_value && OB_FAIL(buf.append(*str_value))) {
    } else if (nullptr != int_value && OB_FAIL(buf.append_fmt("%ld", *int_value))) {
    }
    return ret;
  }

  int encode(common::ObIAllocator &alloc, common::ObString &out) const
  {
    int ret = OB_SUCCESS;
    common::ObSqlString buf;
    const common::ObString version = common::ObString::make_string("1");
    if (OB_FAIL(append_field(buf, "v", &version, nullptr))) {
    } else if (OB_FAIL(append_field(buf, "max_retries", nullptr, &max_retries_))) {
    } else if (OB_FAIL(append_field(buf, "sync_mode", &sync_mode_, nullptr))) {
    } else if (OB_FAIL(append_field(buf, "result_type", &result_type_, nullptr))) {
    } else if (OB_FAIL(append_field(buf, "model", &model_name_, nullptr))) {
    } else if (OB_FAIL(append_field(buf, "deps", &dependency_columns_, nullptr))) {
    } else if (OB_FAIL(append_field(buf, "raw_expr", &raw_expr_, nullptr))) {
    } else if (OB_FAIL(append_field(buf, "normalized_expr", &normalized_expr_, nullptr))) {
    } else if (OB_FAIL(common::ob_write_string(alloc, common::ObString(static_cast<int32_t>(buf.length()), buf.ptr()), out))) {
    }
    return ret;
  }

  int decode(const common::ObString &in)
  {
    int ret = OB_SUCCESS;
    reset();
    int64_t pos = 0;
    while (OB_SUCC(ret) && pos <= in.length()) {
      int64_t end = pos;
      while (end < in.length() && AI_COLUMN_SPEC_FIELD_SEPARATOR != in.ptr()[end]) {
        ++end;
      }
      const common::ObString field(static_cast<int32_t>(end - pos), in.ptr() + pos);
      int64_t eq = 0;
      while (eq < field.length() && '=' != field.ptr()[eq]) {
        ++eq;
      }
      if (eq < field.length()) {
        const common::ObString key(static_cast<int32_t>(eq), field.ptr());
        const common::ObString value(field.length() - static_cast<int32_t>(eq + 1), field.ptr() + eq + 1);
        if (ai_column_name_equal(key, common::ObString::make_string("max_retries"))) {
          int64_t v = 0;
          if (ai_column_parse_int_literal(value, v)) {
            max_retries_ = v;
          }
        } else if (ai_column_name_equal(key, common::ObString::make_string("sync_mode"))) {
          sync_mode_ = value;
        } else if (ai_column_name_equal(key, common::ObString::make_string("result_type"))) {
          result_type_ = value;
        } else if (ai_column_name_equal(key, common::ObString::make_string("model"))) {
          model_name_ = value;
        } else if (ai_column_name_equal(key, common::ObString::make_string("deps"))) {
          dependency_columns_ = value;
        } else if (ai_column_name_equal(key, common::ObString::make_string("raw_expr"))) {
          raw_expr_ = value;
        } else if (ai_column_name_equal(key, common::ObString::make_string("normalized_expr"))) {
          normalized_expr_ = value;
        }
      }
      pos = end + 1;
    }
    return ret;
  }

  TO_STRING_KV(K_(max_retries), K_(sync_mode), K_(result_type), K_(model_name),
               K_(dependency_columns), K_(raw_expr), K_(normalized_expr));

  int64_t max_retries_;
  common::ObString sync_mode_;
  common::ObString result_type_;
  common::ObString model_name_;
  common::ObString dependency_columns_;
  common::ObString raw_expr_;
  common::ObString normalized_expr_;
};

inline int ai_column_make_hidden_name(const char *prefix,
                                      const common::ObString &column_name,
                                      char *buf,
                                      const int64_t buf_len,
                                      common::ObString &out)
{
  int ret = OB_SUCCESS;
  const int64_t prefix_len = static_cast<int64_t>(strlen(prefix));
  if (nullptr == buf || buf_len <= prefix_len + column_name.length()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(buf, prefix, prefix_len);
    MEMCPY(buf + prefix_len, column_name.ptr(), column_name.length());
    out.assign_ptr(buf, static_cast<int32_t>(prefix_len + column_name.length()));
  }
  return ret;
}

// Extract the dimension of a vector column from its extended type info, e.g.
// "VECTOR(4)" -> 4. Returns 0 when the type info does not describe a vector.
inline int64_t ai_column_vector_dim(const common::ObIArray<common::ObString> &type_info)
{
  int64_t dim = 0;
  if (type_info.count() > 0) {
    const common::ObString &str = type_info.at(0);
    int64_t pos = 0;
    while (pos < str.length() && '(' != str.ptr()[pos]) {
      ++pos;
    }
    if (pos < str.length()) {
      ++pos;
      while (pos < str.length() && str.ptr()[pos] >= '0' && str.ptr()[pos] <= '9') {
        dim = dim * 10 + (str.ptr()[pos] - '0');
        ++pos;
      }
    }
  }
  return dim;
}

// Build and encode the specification of an AI column. ``table_schema`` (any
// class with column_begin()/column_end() over ObColumnSchemaV2) is only used to
// compute the dependency columns of the expression.
template <typename TableSchema>
inline int ai_column_build_spec(const TableSchema &table_schema,
                                const common::ObString &raw_expr,
                                const common::ObString &column_name,
                                const bool is_json_result,
                                const bool is_integer_result,
                                const int64_t vector_dim,
                                const int64_t max_retries,
                                const common::ObString &sync_mode,
                                common::ObIAllocator &alloc,
                                common::ObString &encoded_spec)
{
  int ret = OB_SUCCESS;
  ObAiColumnSpec spec;
  ObAiColumnExprInfo expr_info;
  common::ObSqlString normalized_expr;
  common::ObSqlString dependency_columns;
  common::ObSqlString result_type;
  if (OB_FAIL(ai_column_parse_expr(raw_expr, expr_info))) {
  } else if (OB_FAIL(normalized_expr.append(raw_expr))) {
  } else if (is_json_result && OB_FAIL(normalized_expr.append(AI_COLUMN_JSON_PROMPT_SUFFIX))) {
  } else if (is_integer_result && OB_FAIL(normalized_expr.append(AI_COLUMN_INTEGER_PROMPT_SUFFIX))) {
  } else {
    if (expr_info.arg_count_ > 0) {
      common::ObString model_name;
      if (OB_SUCCESS == ai_column_parse_string_literal(expr_info.args_[0], model_name)) {
        spec.model_name_ = model_name;
      }
    }
    if (is_json_result) {
      spec.result_type_ = common::ObString::make_string("json");
    } else if (is_integer_result) {
      spec.result_type_ = common::ObString::make_string("integer");
    } else if (vector_dim > 0) {
      if (OB_FAIL(result_type.append_fmt("vector(%ld)", vector_dim))) {
      } else {
        spec.result_type_ = common::ObString(static_cast<int32_t>(result_type.length()),
                                             result_type.ptr());
      }
    } else {
      spec.result_type_ = common::ObString::make_string("string");
    }
    spec.max_retries_ = max_retries;
    spec.sync_mode_ = sync_mode;
    spec.raw_expr_ = raw_expr;
    spec.normalized_expr_ = common::ObString(static_cast<int32_t>(normalized_expr.length()),
                                             normalized_expr.ptr());
  }
  // dependency columns: every plain column referenced by the expression
  for (typename TableSchema::const_column_iterator it = table_schema.column_begin();
       OB_SUCC(ret) && it != table_schema.column_end();
       ++it) {
    const auto *column = *it;
    if (nullptr == column) {
      ret = OB_ERR_UNEXPECTED;
    } else if (column->is_hidden() || ai_column_name_equal(column->get_column_name_str(), column_name)) {
      // skip hidden columns and the AI column itself
    } else if (ai_column_expr_references_column(raw_expr, column->get_column_name_str())) {
      if (dependency_columns.length() > 0 && OB_FAIL(dependency_columns.append(","))) {
      } else if (OB_FAIL(dependency_columns.append(column->get_column_name_str()))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    spec.dependency_columns_ = common::ObString(static_cast<int32_t>(dependency_columns.length()),
                                                dependency_columns.ptr());
    if (OB_FAIL(spec.encode(alloc, encoded_spec))) {
    }
  }
  return ret;
}

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_SERVICE_OB_AI_COLUMN_SPEC_H_
