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

#define USING_LOG_PREFIX SERVER

#include "observer/ob_ai_column_worker.h"

#include "lib/allocator/page_arena.h"
#include "lib/string/ob_sql_string.h"
#include "lib/thread/ob_thread_name.h"
#include "lib/utility/ob_mod_define.h"
#include "lib/utility/utility.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "observer/ob_inner_sql_connection.h"
#include "query/ai/ob_ai_completion.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/ai_service/ob_ai_column_spec.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "share/ob_errno.h"
#include "share/ob_server_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_struct.h"

namespace oceanbase
{
namespace observer
{

using namespace oceanbase::common;

namespace
{

// One pending regeneration task is stored per (table, ai column, request) in
// the __all_ai_column_task system table.
const char *const AI_COLUMN_TASK_TABLE = "oceanbase.__all_ai_column_task";

// Escape a value so that it can be used as a mysql string literal. The values
// of the model are arbitrary text and must not be able to change the statement.
int append_sql_string_literal(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("'");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value.ptr()[i]);
    switch (c) {
      case 0: ret = sql.append("\\0"); break;
      case '\n': ret = sql.append("\\n"); break;
      case '\r': ret = sql.append("\\r"); break;
      case '\\': ret = sql.append("\\\\"); break;
      case '\'': ret = sql.append("\\'"); break;
      case 0x1a: ret = sql.append("\\Z"); break;
      default: {
        const char ch = static_cast<char>(c);
        ret = sql.append(&ch, 1);
        break;
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

// Append a vector literal such as [0,0,0,0]. The text is a model answer, so
// only the characters of a numeric array are forwarded: the value can not
// change the statement it is published in.
int append_vector_literal(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  bool has_number = false;
  if (value.empty() || '[' != value.ptr()[0] || ']' != value.ptr()[value.length() - 1]) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if ((c >= '0' && c <= '9') || '.' == c || '-' == c || '+' == c
        || 'e' == c || 'E' == c) {
      has_number = true;
    } else if ('[' != c && ']' != c && ',' != c && ' ' != c) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  if (OB_SUCC(ret) && !has_number) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_SUCC(ret) && OB_FAIL(sql.append(value))) {
  }
  return ret;
}

// Print an identifier quoted with backticks.
int append_sql_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if ('`' == name.ptr()[i]) {
      ret = sql.append("``");
    } else {
      ret = sql.append(name.ptr() + i, 1);
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`");
  }
  return ret;
}

int hex_digit_value(const char c)
{
  int value = -1;
  if (c >= '0' && c <= '9') {
    value = c - '0';
  } else if (c >= 'a' && c <= 'f') {
    value = c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    value = c - 'A' + 10;
  }
  return value;
}

// The task table stores the rowkey predicate as X'<hex>' so that the text of
// the predicate can not be altered by the character set of the server. It is
// read back with hex() and decoded here.
int decode_hex_string(ObIAllocator &alloc, const ObString &hex, ObString &out)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  out.reset();
  if (0 != hex.length() % 2) {
    ret = OB_INVALID_ARGUMENT;
  } else if (hex.length() > 0) {
    const int64_t len = hex.length() / 2;
    if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(len)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
        const int high = hex_digit_value(hex.ptr()[2 * i]);
        const int low = hex_digit_value(hex.ptr()[2 * i + 1]);
        if (high < 0 || low < 0) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          buf[i] = static_cast<char>((high << 4) | low);
        }
      }
      if (OB_SUCC(ret)) {
        out.assign_ptr(buf, static_cast<int32_t>(len));
      }
    }
  }
  return ret;
}

// Build the error text that is kept in the last_error column of the task row.
int build_error_text(ObIAllocator &alloc, const int err, ObString &error_msg)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  if (OB_FAIL(buf.append_fmt("ret=%d %s", err, ob_error_name(err)))) {
    LOG_WARN("failed to build the ai column error text", K(ret), K(err));
  } else if (OB_FAIL(ob_write_string(
                 alloc, ObString(static_cast<int32_t>(buf.length()), buf.ptr()), error_msg))) {
    LOG_WARN("failed to copy the ai column error text", K(ret));
  }
  return ret;
}

}  // namespace

ObAiColumnWorker::ObAiColumnWorker()
  : lib::Threads(1),
    conn_guard_(),
    conn_(NULL),
    is_inited_(false)
{
}

ObAiColumnWorker::~ObAiColumnWorker()
{
  destroy();
}

int ObAiColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("the ai column worker is already inited", K(ret));
  } else if (OB_FAIL(lib::Threads::init())) {
    LOG_WARN("failed to init the ai column worker thread", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAiColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("the ai column worker is not inited", K(ret));
  } else {
    set_run_wrapper(share::server_runtime());
    if (OB_FAIL(lib::Threads::start())) {
      LOG_WARN("failed to start the ai column worker thread", K(ret));
    }
  }
  return ret;
}

void ObAiColumnWorker::stop()
{
  if (is_inited_) {
    lib::Threads::stop();
  }
}

void ObAiColumnWorker::wait()
{
  if (is_inited_) {
    lib::Threads::wait();
  }
}

void ObAiColumnWorker::destroy()
{
  if (is_inited_) {
    lib::Threads::stop();
    lib::Threads::wait();
    release_connection();
    lib::Threads::destroy();
    is_inited_ = false;
  }
}

void ObAiColumnWorker::run1()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("the ai column worker is not inited", K(ret));
  } else {
    lib::set_thread_name("AIColWorker", get_thread_idx());
    LOG_INFO("ai column worker started");
    while (!has_set_stop()) {
      const int work_ret = do_work();
      if (OB_SUCCESS != work_ret) {
        LOG_WARN("failed to process the ai column tasks", K(work_ret));
        // the connection may be broken together with its session, build a
        // fresh one on the next round
        release_connection();
      }
      if (!has_set_stop()) {
        ob_usleep(static_cast<useconds_t>(IDLE_INTERVAL_US));
      }
    }
    release_connection();
    LOG_INFO("ai column worker stopped");
  }
}

int ObAiColumnWorker::ensure_connection()
{
  int ret = OB_SUCCESS;
  if (NULL != conn_) {
    // reuse the connection and its session
  } else if (OB_FAIL(ObInnerSQLConnection::create_connection_with_owned_session(
                 false /*use_static_engine*/, 0 /*group_id*/, conn_guard_))) {
    LOG_WARN("failed to create the ai column worker connection", K(ret));
  } else if (OB_ISNULL(conn_ = static_cast<ObInnerSQLConnection *>(conn_guard_.get_ptr()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the ai column worker connection is null", K(ret));
  }
  return ret;
}

void ObAiColumnWorker::release_connection()
{
  conn_ = NULL;
  conn_guard_.reset();
}

int ObAiColumnWorker::execute_write(const ObString &sql, int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  affected_rows = 0;
  if (OB_FAIL(ensure_connection())) {
  } else if (OB_FAIL(conn_->execute_write(sql, affected_rows))) {
    LOG_WARN("failed to execute the ai column worker statement", K(ret), K(sql));
  }
  return ret;
}

int ObAiColumnWorker::execute_read(const ObString &sql, ObISQLClient::ReadResult &res)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ensure_connection())) {
  } else if (OB_FAIL(conn_->execute_read(sql, res))) {
    LOG_WARN("failed to execute the ai column worker query", K(ret), K(sql));
  }
  return ret;
}

int ObAiColumnWorker::do_work()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                             OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<ObAiColumnTaskEntry, TASK_BATCH_SIZE> tasks;
  if (OB_FAIL(fetch_due_tasks(allocator, tasks))) {
    LOG_WARN("failed to fetch the due ai column tasks", K(ret));
  } else {
    int64_t begin = 0;
    while (OB_SUCC(ret) && begin < tasks.count()) {
      // the rows are ordered by (table_id, request_id), so the tasks of one
      // request are adjacent
      int64_t end = begin + 1;
      while (end < tasks.count()
             && tasks.at(end).table_id_ == tasks.at(begin).table_id_
             && tasks.at(end).request_id_ == tasks.at(begin).request_id_) {
        ++end;
      }
      const int tmp_ret = process_group(tasks, begin, end);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("failed to process the ai column task group", K(tmp_ret),
                 K(tasks.at(begin).table_id_), K(tasks.at(begin).request_id_));
      }
      begin = end;
    }
  }
  return ret;
}

int ObAiColumnWorker::fetch_due_tasks(ObIAllocator &alloc, ObIArray<ObAiColumnTaskEntry> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  // State 0 is a fresh request and state 3 is a request that waits for its
  // retry deadline. The eligible timestamp keeps a failing task from spinning.
  if (OB_FAIL(sql.append_fmt("select table_id, column_id, request_id, retry_count,"
                             " hex(rowkey_predicate) from %s"
                             " where state in (0, 3) and eligible_ts <= now(6)"
                             " order by table_id, request_id, column_id limit %ld",
                             AI_COLUMN_TASK_TABLE,
                             TASK_BATCH_SIZE))) {
    LOG_WARN("failed to build the due ai column task query", K(ret));
  } else {
    SMART_VAR(ObISQLClient::ReadResult, read_result) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(execute_read(sql.string(), read_result))) {
      } else if (OB_ISNULL(result = read_result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the ai column task result is null", K(ret));
      } else {
        int next_ret = OB_SUCCESS;
        while (OB_SUCC(ret) && OB_SUCCESS == (next_ret = result->next())) {
          ObAiColumnTaskEntry entry;
          ObString hex;
          // The key columns of the task table are int (signed), so they must be
          // read as integers; get_uint() only accepts an unsigned 64 bit cell.
          int64_t table_id = 0;
          int64_t column_id = 0;
          int64_t request_id = 0;
          if (OB_FAIL(result->get_int(static_cast<int64_t>(0), table_id))) {
            LOG_WARN("failed to read the ai column task table id", K(ret));
          } else if (OB_FAIL(result->get_int(static_cast<int64_t>(1), column_id))) {
            LOG_WARN("failed to read the ai column task column id", K(ret));
          } else if (OB_FAIL(result->get_int(static_cast<int64_t>(2), request_id))) {
            LOG_WARN("failed to read the ai column task request id", K(ret));
          } else if (OB_FAIL(result->get_int(static_cast<int64_t>(3), entry.retry_count_))) {
            LOG_WARN("failed to read the ai column task retry count", K(ret));
          } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(4), hex))) {
            LOG_WARN("failed to read the ai column task predicate", K(ret));
          } else {
            entry.table_id_ = static_cast<uint64_t>(table_id);
            entry.column_id_ = static_cast<uint64_t>(column_id);
            entry.request_id_ = static_cast<uint64_t>(request_id);
            if (OB_FAIL(decode_hex_string(alloc, hex, entry.predicate_))) {
              LOG_WARN("failed to decode the ai column task predicate", K(ret), K(hex));
            } else if (OB_FAIL(tasks.push_back(entry))) {
              LOG_WARN("failed to push back the ai column task", K(ret));
            }
          }
        }
        if (OB_SUCCESS != next_ret && OB_ITER_END != next_ret) {
          ret = next_ret;
          LOG_WARN("failed to iterate the ai column tasks", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::delete_tasks(const uint64_t table_id,
                                   const uint64_t request_id,
                                   const ObIArray<uint64_t> *column_ids)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("delete from %s where table_id = %lu and request_id = %lu",
                             AI_COLUMN_TASK_TABLE, table_id, request_id))) {
    LOG_WARN("failed to build the ai column task delete", K(ret));
  } else if (NULL != column_ids) {
    if (column_ids->empty()) {
      // nothing to delete
      sql.reset();
    } else if (OB_FAIL(sql.append(" and column_id in ("))) {
      LOG_WARN("failed to append the ai column task delete header", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_ids->count(); ++i) {
        if (OB_FAIL(sql.append_fmt("%s%lu", i > 0 ? "," : "", column_ids->at(i)))) {
          LOG_WARN("failed to append the ai column task column id", K(ret));
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(sql.append(")"))) {
        LOG_WARN("failed to close the ai column task delete", K(ret));
      }
    }
  }
  if (OB_SUCC(ret) && !sql.empty() && OB_FAIL(execute_write(sql.string(), affected_rows))) {
    LOG_WARN("failed to delete the ai column tasks", K(ret), K(sql));
  }
  return ret;
}

int ObAiColumnWorker::load_group_items(ObIAllocator &alloc,
                                       const share::schema::ObTableSchema &table_schema,
                                       const ObIArray<ObAiColumnTaskEntry> &tasks,
                                       const int64_t begin,
                                       const int64_t end,
                                       ObIArray<ObAiColumnGroupItem> &items)
{
  int ret = OB_SUCCESS;
  const int64_t prefix_len = static_cast<int64_t>(strlen(share::AI_COLUMN_SPEC_PREFIX));
  for (share::schema::ObTableSchema::const_column_iterator it = table_schema.column_begin();
       OB_SUCC(ret) && it != table_schema.column_end(); ++it) {
    const share::schema::ObColumnSchemaV2 *spec_column = *it;
    const share::schema::ObColumnSchemaV2 *result_column = NULL;
    const ObAiColumnTaskEntry *task = NULL;
    ObString result_name;
    if (OB_ISNULL(spec_column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null column", K(ret));
    } else if (!spec_column->is_hidden()) {
      // only the hidden __ai_spec_ columns carry an ai column specification
    } else if (spec_column->get_column_name_str().length() <= prefix_len
               || 0 != strncmp(spec_column->get_column_name_str().ptr(),
                               share::AI_COLUMN_SPEC_PREFIX, prefix_len)) {
      // not an ai column spec column
    } else {
      const ObString &spec_name = spec_column->get_column_name_str();
      result_name.assign_ptr(spec_name.ptr() + prefix_len,
                             static_cast<int32_t>(spec_name.length() - prefix_len));
      result_column = table_schema.get_column_schema(result_name);
      if (OB_ISNULL(result_column)) {
        // the metadata of a dropped ai column may outlive its result column
      } else {
        for (int64_t i = begin; NULL == task && i < end; ++i) {
          if (tasks.at(i).column_id_ == result_column->get_column_id()) {
            task = &tasks.at(i);
          }
        }
        if (NULL != task) {
          share::ObAiColumnSpec spec;
          ObAiColumnGroupItem item;
          char state_name_buf[512];
          ObString state_name;
          if (OB_FAIL(spec.decode(spec_column->get_comment_str()))) {
            LOG_WARN("failed to decode the ai column specification", K(ret), K(result_name));
          } else if (OB_FAIL(share::ai_column_make_hidden_name(
                         share::AI_COLUMN_STATE_PREFIX, result_name, state_name_buf,
                         static_cast<int64_t>(sizeof(state_name_buf)), state_name))) {
            LOG_WARN("failed to make the ai state column name", K(ret), K(result_name));
          } else if (OB_FAIL(ob_write_string(alloc, result_column->get_column_name_str(),
                                             item.column_name_))) {
            LOG_WARN("failed to copy the ai column name", K(ret));
          } else if (OB_FAIL(ob_write_string(alloc, state_name, item.state_column_name_))) {
            LOG_WARN("failed to copy the ai state column name", K(ret));
          } else if (OB_FAIL(ob_write_string(alloc, spec.model_name_, item.model_name_))) {
            LOG_WARN("failed to copy the ai model name", K(ret));
          } else if (OB_FAIL(ob_write_string(alloc, spec.normalized_expr_,
                                             item.normalized_expr_))) {
            LOG_WARN("failed to copy the ai column prompt", K(ret));
          } else if (OB_FAIL(ob_write_string(alloc, spec.raw_expr_, item.raw_expr_))) {
            LOG_WARN("failed to copy the ai column expression", K(ret));
          } else if (OB_FAIL(ob_write_string(alloc, spec.result_type_, item.result_type_))) {
            LOG_WARN("failed to copy the ai column result type", K(ret));
          } else {
            item.column_id_ = result_column->get_column_id();
            item.max_retries_ = spec.max_retries_;
            item.retry_count_ = task->retry_count_;
            item.is_json_ = share::ai_column_name_equal(
                spec.result_type_, ObString::make_string("json"));
            if (OB_FAIL(items.push_back(item))) {
              LOG_WARN("failed to push back the ai column group item", K(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::evaluate_prompt(ObIAllocator &alloc,
                                      const share::schema::ObDatabaseSchema &database_schema,
                                      const share::schema::ObTableSchema &table_schema,
                                      const ObString &predicate,
                                      const ObString &raw_expr,
                                      ObString &prompt,
                                      bool &row_missing,
                                      share::ObAiColumnFuncType &func_type)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  share::ObAiColumnExprInfo expr_info;
  ObString prompt_expr;
  ObString user_prompt;
  row_missing = false;
  func_type = share::AI_COLUMN_FUNC_INVALID;
  prompt.reset();
  if (OB_FAIL(share::ai_column_parse_expr(raw_expr, expr_info))) {
    LOG_WARN("failed to parse the ai column expression", K(ret), K(raw_expr));
  } else if ((share::AI_COLUMN_FUNC_COMPLETE != expr_info.func_type_
              && share::AI_COLUMN_FUNC_EMBED != expr_info.func_type_)
             || expr_info.arg_count_ < 2) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("the ai column expression is not supported by the worker", K(ret), K(raw_expr));
  } else {
    // The second argument of ai_complete/ai_embed is the text sent to the
    // model. It may reference columns of the row, so it is evaluated by the
    // sql engine.
    func_type = expr_info.func_type_;
    share::ai_column_trim(expr_info.args_[1], prompt_expr);
    if (prompt_expr.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("the ai column prompt expression is empty", K(ret), K(raw_expr));
    } else if (OB_FAIL(sql.append("select ("))) {
    } else if (OB_FAIL(sql.append(prompt_expr))) {
    } else if (OB_FAIL(sql.append(") is null, cast(("))) {
    } else if (OB_FAIL(sql.append(prompt_expr))) {
    } else if (OB_FAIL(sql.append(") as char) from "))) {
    } else if (OB_FAIL(append_sql_identifier(sql, database_schema.get_database_name_str()))) {
    } else if (OB_FAIL(sql.append("."))) {
    } else if (OB_FAIL(append_sql_identifier(sql, table_schema.get_table_name_str()))) {
    } else if (OB_FAIL(sql.append(" where "))) {
    } else if (OB_FAIL(sql.append(predicate))) {
    } else if (OB_FAIL(sql.append(" limit 1"))) {
      LOG_WARN("failed to build the ai column prompt query", K(ret));
    } else {
      SMART_VAR(ObISQLClient::ReadResult, read_result) {
        sqlclient::ObMySQLResult *result = NULL;
        if (OB_FAIL(execute_read(sql.string(), read_result))) {
        } else if (OB_ISNULL(result = read_result.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("the ai column prompt result is null", K(ret));
        } else {
          const int next_ret = result->next();
          int64_t is_null = 0;
          if (OB_ITER_END == next_ret) {
            // the row was deleted after the task was created
            row_missing = true;
          } else if (OB_SUCCESS != next_ret) {
            ret = next_ret;
            LOG_WARN("failed to fetch the ai column prompt row", K(ret), K(sql));
          } else if (OB_FAIL(result->get_int(static_cast<int64_t>(0), is_null))) {
            LOG_WARN("failed to read the ai column prompt null flag", K(ret));
          } else if (0 != is_null) {
            // a null prompt carries nothing to send to the model
          } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(1), user_prompt))) {
            LOG_WARN("failed to read the ai column prompt", K(ret));
          } else if (OB_FAIL(ob_write_string(alloc, user_prompt, prompt))) {
            LOG_WARN("failed to copy the ai column prompt", K(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::call_model(ObIAllocator &alloc,
                                 const ObString &model_name,
                                 const ObString &system_prompt,
                                 const ObString &user_prompt,
                                 const bool is_vector,
                                 ObString &result)
{
  int ret = OB_SUCCESS;
  // The normalized expression of the column is the system prompt: it carries
  // the result format the column definition asks for. The query API owns the
  // provider and the HTTP client, which the observer layer cannot reach.
  result.reset();
  if (is_vector) {
    // An embedding model reads one text and answers one vector; the column
    // definition carries no extra instruction for it.
    if (OB_FAIL(query::ObAiEmbedding::call(alloc, model_name, user_prompt, result))) {
      LOG_WARN("failed to call the ai embedding model", K(ret), K(model_name));
    }
  } else if (OB_FAIL(query::ObAiCompletion::call(alloc, model_name, system_prompt,
                                                 user_prompt, result))) {
    LOG_WARN("failed to call the ai model", K(ret), K(model_name));
  }
  return ret;
}

int ObAiColumnWorker::delete_stale_tasks(const uint64_t table_id,
                                         const uint64_t request_id,
                                         const ObIArray<ObAiColumnTaskEntry> &tasks,
                                         const int64_t begin,
                                         const int64_t end,
                                         const ObIArray<ObAiColumnGroupItem> &items)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 4> stale_column_ids;
  for (int64_t i = begin; OB_SUCC(ret) && i < end; ++i) {
    bool matched = false;
    for (int64_t j = 0; !matched && j < items.count(); ++j) {
      matched = items.at(j).column_id_ == tasks.at(i).column_id_;
    }
    if (!matched && OB_FAIL(stale_column_ids.push_back(tasks.at(i).column_id_))) {
      LOG_WARN("failed to push back the stale ai column id", K(ret));
    }
  }
  if (OB_SUCC(ret) && !stale_column_ids.empty()) {
    if (OB_FAIL(delete_tasks(table_id, request_id, &stale_column_ids))) {
      LOG_WARN("failed to delete the stale ai column tasks", K(ret));
    }
  }
  return ret;
}

int ObAiColumnWorker::process_group(const ObIArray<ObAiColumnTaskEntry> &tasks,
                                    const int64_t begin,
                                    const int64_t end)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                             OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<ObAiColumnGroupItem, 4> items;
  ObSEArray<uint64_t, 4> ready_column_ids;
  ObSEArray<uint64_t, 4> failed_column_ids;
  const uint64_t table_id = tasks.at(begin).table_id_;
  const uint64_t request_id = tasks.at(begin).request_id_;
  const ObString &predicate = tasks.at(begin).predicate_;
  share::schema::ObSchemaGetterGuard guard;
  const share::schema::ObTableSchema *table_schema = NULL;
  const share::schema::ObDatabaseSchema *database_schema = NULL;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("the schema service is not ready", K(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    LOG_WARN("failed to get the runtime schema guard", K(ret));
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
    LOG_WARN("failed to get the ai column table schema", K(ret), K(table_id));
  } else if (OB_ISNULL(table_schema)) {
    // the table is gone, the tasks refer to nothing that can be regenerated
    if (OB_FAIL(delete_tasks(table_id, request_id, NULL))) {
      LOG_WARN("failed to drop the ai column tasks of a dropped table", K(ret));
    }
  } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(),
                                               database_schema))) {
    LOG_WARN("failed to get the ai column database schema", K(ret));
  } else if (OB_ISNULL(database_schema)) {
    if (OB_FAIL(delete_tasks(table_id, request_id, NULL))) {
      LOG_WARN("failed to drop the ai column tasks of a dropped database", K(ret));
    }
  } else if (OB_FAIL(load_group_items(allocator, *table_schema, tasks, begin, end, items))) {
    LOG_WARN("failed to load the ai column group", K(ret));
  } else if (items.empty()) {
    // none of the tasks belongs to a current ai column
    if (OB_FAIL(delete_tasks(table_id, request_id, NULL))) {
      LOG_WARN("failed to drop the stale ai column tasks", K(ret));
    }
  } else if (OB_FAIL(delete_stale_tasks(table_id, request_id, tasks, begin, end, items))) {
    LOG_WARN("failed to drop the orphan ai column tasks", K(ret));
  } else {
    bool row_missing = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
      ObAiColumnGroupItem &item = items.at(i);
      int tmp_ret = OB_SUCCESS;
      share::ObAiColumnFuncType func_type = share::AI_COLUMN_FUNC_INVALID;
      if (OB_FAIL(evaluate_prompt(allocator, *database_schema, *table_schema, predicate,
                                  item.raw_expr_, item.prompt_, item.row_missing_,
                                  func_type))) {
        // the prompt expression can not be evaluated, retrying can not change that
        LOG_WARN("failed to evaluate the ai column prompt", K(item.column_name_));
        tmp_ret = ret;
        ret = OB_SUCCESS;
        item.retryable_ = false;
      } else if (item.row_missing_) {
        LOG_DEBUG("the ai column row is gone", K(table_id), K(request_id));
        row_missing = true;
      } else if (item.prompt_.empty()) {
        // a null prompt carries nothing to send to the model
        LOG_DEBUG("the ai column prompt is null", K(table_id), K(request_id),
                  K(item.column_name_));
        item.retryable_ = false;
      } else if (FALSE_IT(item.is_vector_ = share::AI_COLUMN_FUNC_EMBED == func_type)) {
      } else if (OB_FAIL(call_model(allocator, item.model_name_, item.normalized_expr_,
                                    item.prompt_, item.is_vector_, item.result_))) {
        LOG_WARN("failed to call the ai model of the column", K(ret), K(item.model_name_),
                 K(item.column_name_));
        tmp_ret = ret;
        ret = OB_SUCCESS;
        item.retryable_ = true;
      } else {
        item.ok_ = true;
      }
      if (!item.ok_ && !item.row_missing_) {
        if (OB_SUCCESS == tmp_ret) {
          tmp_ret = OB_INVALID_DATA;
        }
        build_error_text(allocator, tmp_ret, item.error_msg_);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (row_missing) {
      // the target row is gone, the whole group is useless
      if (OB_FAIL(delete_tasks(table_id, request_id, NULL))) {
        LOG_WARN("failed to drop the ai column tasks of a deleted row", K(ret));
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
        if (items.at(i).ok_ && OB_FAIL(ready_column_ids.push_back(items.at(i).column_id_))) {
          LOG_WARN("failed to push back the ready ai column id", K(ret));
        }
      }
      // Publish the results of the group with one UPDATE: a reader sees either
      // every regenerated column of the row or none of them. The exists() guard
      // makes the request id part of the update, so a slower request whose task
      // was already replaced by a newer one touches no row.
      if (OB_SUCC(ret) && !ready_column_ids.empty()) {
        ObSqlString sql;
        int64_t affected_rows = 0;
        bool first = true;
        if (OB_FAIL(sql.append("update "))) {
        } else if (OB_FAIL(append_sql_identifier(sql,
                                                 database_schema->get_database_name_str()))) {
        } else if (OB_FAIL(sql.append("."))) {
        } else if (OB_FAIL(append_sql_identifier(sql, table_schema->get_table_name_str()))) {
        } else if (OB_FAIL(sql.append(" set "))) {
          LOG_WARN("failed to build the ai column publish statement", K(ret));
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
          const ObAiColumnGroupItem &item = items.at(i);
          ObString value;
          if (!item.ok_) {
            // published through the assignment of a ready column only
          } else if (OB_FAIL(sql.append(first ? "" : ", "))) {
          } else if (FALSE_IT(first = false)) {
          } else if (OB_FAIL(append_sql_identifier(sql, item.column_name_))) {
          } else if (OB_FAIL(sql.append(" = "))) {
          } else if (item.is_vector_) {
            // a vector literal is written as an array, not as a string
            share::ai_column_trim(item.result_, value);
            if (OB_FAIL(append_vector_literal(sql, value))) {
              LOG_WARN("failed to append the ai column vector value", K(ret), K(value));
            }
          } else if (!item.is_json_
                     && share::ai_column_name_equal(item.result_type_,
                                                    ObString::make_string("integer"))) {
            // write integers as numbers so that the conversion of the column
            // type can not be confused by a textual answer
            int64_t int_value = 0;
            share::ai_column_trim(item.result_, value);
            if (!share::ai_column_parse_int_literal(value, int_value)) {
              ret = OB_INVALID_DATA;
              LOG_WARN("the ai model answered a non integer value", K(ret), K(value));
            } else if (OB_FAIL(sql.append_fmt("%ld", int_value))) {
              LOG_WARN("failed to append the ai column integer value", K(ret));
            }
          } else if (OB_FAIL(append_sql_string_literal(sql, item.result_))) {
            LOG_WARN("failed to append the ai column value", K(ret));
          }
          if (OB_SUCC(ret) && item.ok_) {
            if (OB_FAIL(sql.append(", "))) {
            } else if (OB_FAIL(append_sql_identifier(sql, item.state_column_name_))) {
            } else if (OB_FAIL(sql.append_fmt(" = %d", share::AI_COLUMN_STATE_READY))) {
              LOG_WARN("failed to append the ai column state", K(ret));
            }
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(sql.append(" where "))) {
        } else if (OB_SUCC(ret) && OB_FAIL(sql.append(predicate))) {
        } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(
                       " and exists (select 1 from %s where table_id = %lu and request_id = %lu)",
                       AI_COLUMN_TASK_TABLE, table_id, request_id))) {
          LOG_WARN("failed to build the ai column publish condition", K(ret));
        } else if (OB_SUCC(ret)) {
          const int publish_ret = execute_write(sql.string(), affected_rows);
          if (OB_SUCCESS == publish_ret) {
            // The results are visible (or already equal to the published ones).
            // A superseded or deleted row makes the update touch no row, which
            // is fine: the newer request owns its own tasks.
            if (OB_FAIL(delete_tasks(table_id, request_id, &ready_column_ids))) {
              LOG_WARN("failed to drop the published ai column tasks", K(ret));
            }
          } else {
            // The values could not be written. Retrying the publish is the only
            // way to make progress, so the columns are handled like a failed
            // model call instead of leaving their tasks at state 0, which the
            // next cycle would pick up and send to the model again.
            LOG_WARN("failed to publish the ai column results", K(publish_ret), K(sql));
            for (int64_t i = 0; i < items.count(); ++i) {
              ObAiColumnGroupItem &item = items.at(i);
              if (item.ok_) {
                item.ok_ = false;
                item.retryable_ = true;
                build_error_text(allocator, publish_ret, item.error_msg_);
              }
            }
          }
        }
      }
      // Handle the unsuccessful columns. A failure caused by the model or the
      // endpoint is retried until the retry budget of the column is exhausted,
      // everything else is final.
      for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
        const ObAiColumnGroupItem &item = items.at(i);
        int64_t affected_rows = 0;
        ObSqlString sql;
        if (item.ok_) {
        } else if (item.retryable_ && item.retry_count_ < item.max_retries_) {
          if (OB_FAIL(sql.append_fmt("update %s set state = 3, retry_count = %ld,"
                                     " eligible_ts = date_add(now(6), interval %ld microsecond),"
                                     " last_error = ",
                                     AI_COLUMN_TASK_TABLE,
                                     item.retry_count_ + 1,
                                     RETRY_DELAY_US))) {
          } else if (OB_FAIL(append_sql_string_literal(sql, item.error_msg_))) {
          } else if (OB_FAIL(sql.append_fmt(" where table_id = %lu and request_id = %lu"
                                            " and column_id = %lu",
                                            table_id, request_id, item.column_id_))) {
          } else if (OB_FAIL(execute_write(sql.string(), affected_rows))) {
            LOG_WARN("failed to schedule the ai column retry", K(ret), K(sql));
          }
          // A zero affected row count means the request was superseded in the
          // meantime, there is nothing left to retry then.
        } else {
          // the retry budget is exhausted (or the failure can not change): mark
          // the column FAILED and drop its task
          if (OB_FAIL(sql.append("update "))) {
          } else if (OB_FAIL(append_sql_identifier(
                         sql, database_schema->get_database_name_str()))) {
          } else if (OB_FAIL(sql.append("."))) {
          } else if (OB_FAIL(append_sql_identifier(sql, table_schema->get_table_name_str()))) {
          } else if (OB_FAIL(sql.append(" set "))) {
          } else if (OB_FAIL(append_sql_identifier(sql, item.state_column_name_))) {
          } else if (OB_FAIL(sql.append_fmt(" = %d where ", share::AI_COLUMN_STATE_FAILED))) {
          } else if (OB_FAIL(sql.append(predicate))) {
          } else if (OB_FAIL(sql.append_fmt(
                         " and exists (select 1 from %s where table_id = %lu and request_id = %lu)",
                         AI_COLUMN_TASK_TABLE, table_id, request_id))) {
          } else if (OB_FAIL(execute_write(sql.string(), affected_rows))) {
            LOG_WARN("failed to mark the ai column failed", K(ret), K(sql));
          } else if (OB_FAIL(failed_column_ids.push_back(item.column_id_))) {
            LOG_WARN("failed to push back the failed ai column id", K(ret));
          }
        }
      }
      if (OB_SUCC(ret) && !failed_column_ids.empty()) {
        if (OB_FAIL(delete_tasks(table_id, request_id, &failed_column_ids))) {
          LOG_WARN("failed to drop the failed ai column tasks", K(ret));
        }
      }
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
