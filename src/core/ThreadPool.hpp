#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <atomic>

class ThreadPool {
public:
    explicit ThreadPool(int n_threads = 4);
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> job);
    void clear();

    int pendingCount() const { return static_cast<int>(m_pending.load()); }

    /// Number of worker threads. Fixed at construction — m_workers is filled by the
    /// constructor and only joined by the destructor, so this needs no lock.
    int threadCount() const { return static_cast<int>(m_workers.size()); }

private:
    std::vector<std::thread>          m_workers;
    std::queue<std::function<void()>> m_queue;
    mutable std::mutex                m_mutex;
    std::condition_variable           m_cv;
    std::atomic<bool>                 m_stop{false};
    std::atomic<int>                  m_pending{0};
};
