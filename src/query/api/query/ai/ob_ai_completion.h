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

#ifndef OCEANBASE_QUERY_AI_OB_AI_COMPLETION_H_
#define OCEANBASE_QUERY_AI_OB_AI_COMPLETION_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace query
{

// Runs one completion request against an AI model endpoint without exposing
// the expression-layer provider and HTTP client to the callers.
class ObAiCompletion
{
public:
  // system_prompt is sent as the system message, user_prompt as the user
  // message. The result is copied into `result` with `allocator`.
  static int call(
      common::ObIAllocator &allocator,
      const common::ObString &model_name,
      const common::ObString &system_prompt,
      const common::ObString &user_prompt,
      common::ObString &result);
};

// Runs one dense embedding request against an AI model endpoint without
// exposing the expression-layer provider and HTTP client to the callers.
class ObAiEmbedding
{
public:
  // `input` is embedded as a single text. The result is the vector literal of
  // the provider answer, such as [0,0,0,0], copied with `allocator`.
  static int call(
      common::ObIAllocator &allocator,
      const common::ObString &model_name,
      const common::ObString &input,
      common::ObString &result);
};

} // namespace query
} // namespace oceanbase

#endif // OCEANBASE_QUERY_AI_OB_AI_COMPLETION_H_
