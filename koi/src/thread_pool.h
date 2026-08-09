#ifndef THREADPOOL_H_
#define THREADPOOL_H_

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

class Task {
 public:
  using TaskCallback = std::function<void()>;

  Task() = default;

  template <typename Func, typename... Args>
  auto Create(Func&& func, Args&&... args) {
    auto inner_call = [func = std::forward<Func>(func), ... args = std::forward<Args>(args)]() {
      return func(args...);
    };

    std::packaged_task task{std::move(inner_call)};

    auto task_ptr = std::make_shared<decltype(task)>(std::move(task));
    auto outer_call = [task_ptr]() { (*task_ptr)(); };
    functor_ = std::move(outer_call);
    return task_ptr->get_future();
  }

  void Run() {
    if (functor_) {
      functor_();
    }
  }

 private:
  TaskCallback functor_;
};

class ThreadPool {
 public:
  static ThreadPool& Instance() {
    static ThreadPool pool;
    return pool;
  }

  // Live worker threads. Callers use this to decide whether they still have a
  // pool, rather than remembering that they once called Init -- Shutdown makes
  // that memory wrong, and a task queued against a pool with no threads is
  // indistinguishable, from the outside, from a task that never finishes.
  std::size_t WorkerCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_.size();
  }

  void Init(int n_threads) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Cleared here as well as in Shutdown: Init after a Shutdown must produce a
    // pool that runs tasks, not threads that exit on their first predicate
    // check and a queue nobody ever drains.
    shutdown_requested_ = false;
    threads_.reserve(threads_.size() + static_cast<std::size_t>(n_threads));
    for (int i = 0; i < n_threads; i++) {
      threads_.emplace_back(Worker(this));
    }
  }

  void Shutdown() {
    // Taken out of `threads_` under the lock and joined out of it. Walking the
    // member vector unlocked, and clearing it unlocked, raced every other
    // member function -- WorkerCount reads it and Init grows it, both under the
    // mutex -- which is a data race on the vector itself, not merely a stale
    // count. Joining cannot happen under the lock: a worker needs the mutex to
    // see the flag and return.
    std::vector<std::thread> leaving;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (threads_.empty()) return;
      shutdown_requested_ = true;
      leaving.swap(threads_);
    }
    condvar_.notify_all();
    for (std::thread& t : leaving) {
      if (t.joinable()) {
        t.join();
      }
    }
    // A latched flag makes every later Init hand back a dead pool. Queued tasks
    // are deliberately left alone: the workers drain the queue before they
    // return (see Worker), and dropping a task already accepted by AddTask
    // strands a caller waiting on its side effects -- for a scan, on a `done`
    // flag that nothing would ever publish.
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_requested_ = false;
  }

  ~ThreadPool() { Shutdown(); }

  template <typename Func, typename... Args>
  auto AddTask(Func&& func, Args&&... args) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto& task = tasks_.emplace();
    auto future = task.Create(std::forward<Func>(func), std::forward<Args>(args)...);
    lock.unlock();
    condvar_.notify_one();
    return future;
  }

 private:
  ThreadPool() {}
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  class Worker {
   public:
    explicit Worker(ThreadPool* const pool) : pool_{pool} {}

    void operator()() {
      for (;;) {
        std::optional<Task> task;
        {
          std::unique_lock<std::mutex> lock(pool_->mutex_);
          pool_->condvar_.wait(
              lock, [this] { return pool_->shutdown_requested_ || !pool_->tasks_.empty(); });
          // Queue before flag: a task already accepted by AddTask has a caller
          // waiting on its side effects, and dropping it on the way out is
          // indistinguishable from a task that hung. Shutdown still terminates,
          // because nothing can enqueue once the caller has stopped asking.
          if (pool_->tasks_.empty()) {
            if (pool_->shutdown_requested_) return;
            continue;
          }
          task.emplace(std::move(pool_->tasks_.front()));
          pool_->tasks_.pop();
        }
        task->Run();
      }
    }

   private:
    ThreadPool* const pool_{nullptr};
  };

  std::vector<std::thread> threads_;
  std::condition_variable condvar_;
  std::mutex mutex_;

  std::queue<Task> tasks_{};
  bool shutdown_requested_{false};
};

#endif
