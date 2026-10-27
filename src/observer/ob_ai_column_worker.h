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

#ifndef OCEANBASE_OBSERVER_OB_AI_COLUMN_WORKER_H_
#define OCEANBASE_OBSERVER_OB_AI_COLUMN_WORKER_H_

#include "lib/ob_define.h"
#include "lib/container/ob_iarray.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/thread/threads.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "share/ai_service/ob_ai_column_spec.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
}
namespace share
{
namespace schema
{
class ObDatabaseSchema;
class ObTableSchema;
}
}
namespace observer
{

class ObInnerSQLConnection;

// One due row of __all_ai_column_task. The rowkey predicate is copied into the
// per-iteration allocator of the worker.
struct ObAiColumnTaskEntry
{
  ObAiColumnTaskEntry()
    : table_id_(common::OB_INVALID_ID),
      column_id_(common::OB_INVALID_ID),
      request_id_(0),
      retry_count_(0)
  {}
  uint64_t table_id_;
  uint64_t column_id_;
  uint64_t request_id_;
  int64_t retry_count_;
  common::ObString predicate_;
};

// One AI column of a task group together with everything the worker learned
// about it: the specification, the evaluated user prompt and the model answer.
struct ObAiColumnGroupItem
{
  ObAiColumnGroupItem()
    : column_id_(common::OB_INVALID_ID),
      max_retries_(0),
      retry_count_(0),
      is_json_(false),
      is_vector_(false),
      ok_(false),
      retryable_(true),
      row_missing_(false)
  {}
  uint64_t column_id_;
  common::ObString column_name_;
  common::ObString state_column_name_;
  common::ObString model_name_;
  common::ObString normalized_expr_;
  common::ObString raw_expr_;
  common::ObString result_type_;
  int64_t max_retries_;
  int64_t retry_count_;
  bool is_json_;
  bool is_vector_;
  bool ok_;
  bool retryable_;
  bool row_missing_;
  common::ObString prompt_;
  common::ObString result_;
  common::ObString error_msg_;
};

// Background worker of the AI columns. The DML layer records one row per
// invalidated AI column in __all_ai_column_task inside the transaction of the
// statement, so a committed change always carries its pending requests. The
// worker turns the due rows into column values:
//  * the tasks of one request (the AI columns of one row of one statement) are
//    handled as a group,
//  * every column of the group calls the model with its own system prompt,
//  * the results of the group are published by a single UPDATE guarded by the
//    request id, so a superseded request can never overwrite a newer value and
//    the columns of one row become visible together,
//  * a failed attempt is retried up to `max_retries` of the column, and a task
//    that exhausted its retries is dropped after the column is marked FAILED.
class ObAiColumnWorker : public lib::Threads
{
public:
  ObAiColumnWorker();
  virtual ~ObAiColumnWorker();
  int init();
  int start();
  void stop();
  void wait();
  void destroy();
  void run1() override;

  static const int64_t IDLE_INTERVAL_US = 50 * 1000LL;
  static const int64_t RETRY_DELAY_US = 600 * 1000LL;
  static const int64_t TASK_BATCH_SIZE = 64;

private:
  int do_work();
  int ensure_connection();
  void release_connection();
  int execute_write(const common::ObString &sql, int64_t &affected_rows);
  int execute_read(const common::ObString &sql,
                   common::ObISQLClient::ReadResult &res);
  int fetch_due_tasks(common::ObIAllocator &alloc,
                      common::ObIArray<ObAiColumnTaskEntry> &tasks);
  int process_group(const common::ObIArray<ObAiColumnTaskEntry> &tasks,
                    const int64_t begin,
                    const int64_t end);
  int load_group_items(common::ObIAllocator &alloc,
                       const share::schema::ObTableSchema &table_schema,
                       const common::ObIArray<ObAiColumnTaskEntry> &tasks,
                       const int64_t begin,
                       const int64_t end,
                       common::ObIArray<ObAiColumnGroupItem> &items);
  int evaluate_prompt(common::ObIAllocator &alloc,
                      const share::schema::ObDatabaseSchema &database_schema,
                      const share::schema::ObTableSchema &table_schema,
                      const common::ObString &predicate,
                      const common::ObString &raw_expr,
                      common::ObString &prompt,
                      bool &row_missing,
                      share::ObAiColumnFuncType &func_type);
  int call_model(common::ObIAllocator &alloc,
                 const common::ObString &model_name,
                 const common::ObString &system_prompt,
                 const common::ObString &user_prompt,
                 const bool is_vector,
                 common::ObString &result);
  int delete_tasks(const uint64_t table_id,
                   const uint64_t request_id,
                   const common::ObIArray<uint64_t> *column_ids);
  int delete_stale_tasks(const uint64_t table_id,
                         const uint64_t request_id,
                         const common::ObIArray<ObAiColumnTaskEntry> &tasks,
                         const int64_t begin,
                         const int64_t end,
                         const common::ObIArray<ObAiColumnGroupItem> &items);
private:
  common::sqlclient::ObISQLConnectionGuard conn_guard_;
  ObInnerSQLConnection *conn_;
  bool is_inited_;
};

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_OB_AI_COLUMN_WORKER_H_
