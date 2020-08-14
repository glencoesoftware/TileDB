/**
 * @file   thread_pool.cc
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2018-2020 TileDB, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @section DESCRIPTION
 *
 * This file defines the ThreadPool class.
 */

#include <cassert>
#include <iostream>
#include <sstream>

#include "tiledb/sm/misc/logger.h"
#include "tiledb/sm/misc/thread_pool.h"

namespace tiledb {
namespace sm {

// Define the static ThreadPool member variables.
std::unordered_map<std::thread::id, ThreadPool*> ThreadPool::tp_index_;
std::mutex ThreadPool::tp_index_lock_;

ThreadPool::ThreadPool()
    : concurrency_level_(0)
    , should_terminate_(false) {
}

ThreadPool::~ThreadPool() {
  terminate();
}

Status ThreadPool::init(const uint64_t concurrency_level) {
  if (concurrency_level == 0) {
    return Status::ThreadPoolError(
        "Unable to initialize a thread pool with a concurrency level of 0.");
  }

  Status st = Status::Ok();

  // We allocate one less thread than `concurrency_level` because
  // the `wait_all*()` routines may service tasks concurrently with
  // the worker threads.
  const uint64_t num_threads = concurrency_level - 1;
  for (uint64_t i = 0; i < num_threads; i++) {
    try {
      threads_.emplace_back([this]() { worker(*this); });
    } catch (const std::exception& e) {
      st = Status::ThreadPoolError(
          "Error initializing thread pool of concurrencylevel " +
          std::to_string(concurrency_level) + "; " + e.what());
      LOG_STATUS(st);
      break;
    }
  }

  if (!st.ok()) {
    // Join any created threads on error.
    terminate();
    return st;
  }

  // Save the concurrency level.
  concurrency_level_ = concurrency_level;

  // Index this ThreadPool instance from all of its thread ids.
  add_tp_index();

  return st;
}

ThreadPool::Task ThreadPool::execute(
    std::function<Status()>&& function) {
  if (concurrency_level_ == 0) {
    Task invalid_future;
    LOG_ERROR("Cannot execute task; thread pool uninitialized.");
    return invalid_future;
  }

  std::unique_lock<std::mutex> lck(task_stack_mutex_);

  if (should_terminate_) {
    Task invalid_future;
    LOG_ERROR("Cannot execute task; thread pool has terminated.");
    return invalid_future;
  }

  PackagedTask task(std::move(function));
  ThreadPool::Task future = task.get_future();

  // When we have a concurrency level > 1, we will have at least
  // one thread available to pick up the task. For a concurrency
  // level == 1, we have no worker threads available. When no
  // worker threads are available, execute the task on this
  // thread.
  if (concurrency_level_ > 1) {
    task_stack_.push(std::move(task));
    task_stack_cv_.notify_one();
    lck.unlock();

    // TODO only if no idle threads
    // notify one blocked
    blocked_tasks_mutex_.lock();
    if (!blocked_tasks_.empty()) {
      auto iter = blocked_tasks_.begin();
      std::stringstream ss;
      ss << "JOE notifying task " << iter->get();
      ss << " on tp " << this;
      ss << std::endl;
      std::cerr << ss.str();
      {
        std::lock_guard<std::mutex> lg((*iter)->return_st_mutex_);
        (*iter)->check_task_stack_ = true;
      }
      (*iter)->cv_.notify_all();
      blocked_tasks_.erase(iter);
      /*
      std::shared_ptr<TaskState> blocked_task = *blocked_tasks_.begin();
      blocked_tasks_.erase(blocked_task);
      blocked_task->cv_.notify_all();*/
    }
    blocked_tasks_mutex_.unlock();
  } else {
    lck.unlock();
    task();
  }

  assert(future.valid());
  return future;
}

uint64_t ThreadPool::concurrency_level() const {
  return concurrency_level_;
}

Status ThreadPool::wait_all(std::vector<Task>& tasks) {
  auto statuses = wait_all_status(tasks);
  for (auto& st : statuses) {
    if (!st.ok()) {
      return st;
    }
  }
  return Status::Ok();
}

std::vector<Status> ThreadPool::wait_all_status(
    std::vector<Task>& tasks) {
  std::vector<Status> statuses;
  for (auto& task : tasks) {
    if (!task.valid()) {
      LOG_ERROR("Waiting on invalid task future.");
      statuses.push_back(Status::ThreadPoolError("Invalid task future"));
    } else {
      Status status = wait_or_work(std::move(task));
      if (!status.ok()) {
        LOG_STATUS(status);
      }
      statuses.push_back(status);
    }
  }
  return statuses;
}

Status ThreadPool::wait_or_work(Task&& task) {
  do {
    if (task.done())
      break;

    // Lookup the thread pool that this thread belongs to. If it
    // does not belong to a thread pool, `lookup_tp` will return
    // `this`.
    ThreadPool* const tp = lookup_tp();

    // Lock the `tp->task_stack_` to receive the next task to work on.
    tp->task_stack_mutex_.lock();

    // If there are no pending tasks, we will wait for `task` to complete.
    if (tp->task_stack_.empty()) {
      tp->task_stack_mutex_.unlock();

      // TODO: add to block list
      if (tp != this) {
        tp->blocked_tasks_mutex_.lock();
        tp->blocked_tasks_.insert(task.task_state_);
        tp->blocked_tasks_mutex_.unlock();
      }

      // todo although no tasks, one is added and notified before
      // entering wait. signal comes too early.
      // we could re-lock the task stack to check again.

      std::stringstream ss;
      ss << "JOE waiting on task " << task.task_state_;
      ss << " on tp " << this;
      ss << std::endl;
      std::cerr << ss.str();

      task.wait();

      // must reset that it has checked task stack

      std::stringstream ss1;
      ss1 << "JOE DONE waiting on task " << task.task_state_;
      ss1 << " on tp " << this;
      ss1 << std::endl;
      std::cerr << ss1.str();

      // TODO: remove task from block list
      if (tp != this) {
        tp->blocked_tasks_mutex_.lock();
        if (tp->blocked_tasks_.count(task.task_state_) > 0)
          tp->blocked_tasks_.erase(task.task_state_);
        tp->blocked_tasks_mutex_.unlock();
      }

      // After the task has been woken up, check to see if it has completed.
      if (task.done()) {
        break;
      }

      // reset flag to check task stack
      {
        std::lock_guard<std::mutex> lg(task.task_state_->return_st_mutex_);
        task.task_state_->check_task_stack_ = true;
      }

      // The task has not completed. It has been signaled because the `task_stack_`
      // has become non-empty. Lock the `task_stack_` again before checking for
      // the next task to service.
      tp->task_stack_mutex_.lock();
    }

    // Pull the next task off of the task stack. We specifically use a LIFO
    // ordering to prevent overflowing the call stack.
    PackagedTask inner_task = std::move(tp->task_stack_.top());
    tp->task_stack_.pop();

    // We're done mutating `tp->task_stack_`.
    tp->task_stack_mutex_.unlock();

    // Execute the inner task.
    if (inner_task.valid())
      inner_task();
  } while (true);

  // The task has completed and will not block.
  assert(task.done());
  return task.get();
}

void ThreadPool::terminate() {
  {
    std::unique_lock<std::mutex> lck(task_stack_mutex_);
    should_terminate_ = true;
    task_stack_cv_.notify_all();
  }

  remove_tp_index();

  for (auto& t : threads_) {
    t.join();
  }

  threads_.clear();
}

void ThreadPool::worker(ThreadPool& pool) {
  while (true) {
    PackagedTask task;

    {
      // Wait until there's work to do.
      std::unique_lock<std::mutex> lck(pool.task_stack_mutex_);
      pool.task_stack_cv_.wait(lck, [&pool]() {
        return pool.should_terminate_ || !pool.task_stack_.empty();
      });

      if (!pool.task_stack_.empty()) {
        task = std::move(pool.task_stack_.top());
        pool.task_stack_.pop();
      }
    }

    if (task.valid())
      task();

    if (pool.should_terminate_)
      break;
  }
}

void ThreadPool::add_tp_index() {
  std::lock_guard<std::mutex> lock(tp_index_lock_);
  for (const auto& thread : threads_)
    tp_index_[thread.get_id()] = this;
}

void ThreadPool::remove_tp_index() {
  std::lock_guard<std::mutex> lock(tp_index_lock_);
  for (const auto& thread : threads_)
    tp_index_.erase(thread.get_id());
}

ThreadPool* ThreadPool::lookup_tp() {
  const std::thread::id tid = std::this_thread::get_id();
  std::lock_guard<std::mutex> lock(tp_index_lock_);
  if (tp_index_.count(tid) == 1)
    return tp_index_[tid];
  return this;
}

}  // namespace sm
}  // namespace tiledb
