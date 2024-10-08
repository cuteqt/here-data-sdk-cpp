/*
 * Copyright (C) 2019-2021 HERE Europe B.V.
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
 *
 * SPDX-License-Identifier: Apache-2.0
 * License-Filename: LICENSE
 */
#include "olp/core/utils/Thread.h"
#include "olp/core/thread/ThreadPoolTaskScheduler.h"

#if defined(PORTING_PLATFORM_QNX)
#include <process.h>
#elif defined(PORTING_PLATFORM_MAC)
#include <pthread.h>
#elif defined(PORTING_PLATFORM_LINUX) || defined(PORTING_PLATFORM_ANDROID)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <pthread.h>
#endif
#include <string>

#include "olp/core/logging/Log.h"
#include "olp/core/logging/LogContext.h"
#include "olp/core/porting/platform.h"
#include "olp/core/thread/SyncQueue.h"
#include "thread/PriorityQueueExtended.h"

namespace olp {
namespace thread {

namespace {
constexpr auto kLogTag = "ThreadPoolTaskScheduler";

struct PrioritizedTask {
  TaskScheduler::CallFuncType function;
  uint32_t priority;
};

struct ComparePrioritizedTask {
  bool operator()(const PrioritizedTask& lhs,
                  const PrioritizedTask& rhs) const {
    return lhs.priority < rhs.priority;
  }
};

void SetExecutorName(size_t idx) {
  std::string thread_name = "OLPSDKPOOL_" + std::to_string(idx);
  olp::utils::Thread::SetCurrentThreadName(thread_name);
  OLP_SDK_LOG_INFO_F(kLogTag, "Starting thread '%s'", thread_name.c_str());
}

}  // namespace

class ThreadPoolTaskScheduler::QueueImpl {
 public:
  using ElementType = PrioritizedTask;

  bool Pull(ElementType& element) { return sync_queue_.Pull(element); }
  void Push(ElementType&& element) { sync_queue_.Push(std::move(element)); }
  void Close() { sync_queue_.Close(); }

 private:
  using PriorityQueue =
      PriorityQueueExtended<ElementType, ComparePrioritizedTask>;
  SyncQueue<ElementType, PriorityQueue> sync_queue_;
};

ThreadPoolTaskScheduler::ThreadPoolTaskScheduler(size_t thread_count)
    :queue_{std::make_unique<QueueImpl>()} {
  thread_pool_.reserve(thread_count);

  for (size_t idx = 0; idx < thread_count; ++idx) {
    std::thread executor([this, idx]() {
      // Set thread name for easy profiling and debugging
      SetExecutorName(idx);

      for (;;) {
        PrioritizedTask task;
        if (!queue_->Pull(task)) {
          return;
        }
        task.function();
      }
    });

    thread_pool_.push_back(std::move(executor));
  }
}

ThreadPoolTaskScheduler::~ThreadPoolTaskScheduler() {
  queue_->Close();
  for (auto& thread : thread_pool_) {
    thread.join();
  }
  thread_pool_.clear();
}

void ThreadPoolTaskScheduler::EnqueueTask(TaskScheduler::CallFuncType&& func) {
  EnqueueTask(std::move(func), thread::NORMAL);
}

void ThreadPoolTaskScheduler::EnqueueTask(TaskScheduler::CallFuncType&& func,
                                          uint32_t priority) {
  auto logContext = logging::GetContext();

#if __cplusplus >= 201402L
  // At least C++14, use generalized lambda capture
  auto funcWithCapturedLogContext = [logContext = std::move(logContext),
                                     func = std::move(func)]() {
    olp::logging::ScopedLogContext scopedContext(logContext);
    func();
  };
#else
  // C++11 does not support generalized lambda capture :(
  auto funcWithCapturedLogContext = std::bind(
      [](std::shared_ptr<const olp::logging::LogContext>& logContext,
         TaskScheduler::CallFuncType& func) {
        olp::logging::ScopedLogContext scopedContext(logContext);
        func();
      },
      std::move(logContext), std::move(func));
#endif

  queue_->Push({std::move(funcWithCapturedLogContext), priority});
}

}  // namespace thread
}  // namespace olp
