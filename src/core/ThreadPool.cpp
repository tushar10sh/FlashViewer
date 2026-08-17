#include "core/ThreadPool.hpp"

ThreadPool::ThreadPool(int n_threads) {
    for (int i = 0; i < n_threads; ++i) {
        m_workers.emplace_back([this] {
            while (true) {
                std::function<void()> job;
                {
                    std::unique_lock lock(m_mutex);
                    m_cv.wait(lock, [this] {
                        return m_stop.load() || !m_queue.empty();
                    });
                    if (m_stop && m_queue.empty()) return;
                    job = std::move(m_queue.front());
                    m_queue.pop();
                }
                job();
                --m_pending;
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    m_stop = true;
    m_cv.notify_all();
    for (auto& w : m_workers) w.join();
}

void ThreadPool::submit(std::function<void()> job) {
    ++m_pending;
    {
        std::lock_guard lock(m_mutex);
        m_queue.push(std::move(job));
    }
    m_cv.notify_one();
}

void ThreadPool::clear() {
    std::lock_guard lock(m_mutex);
    while (!m_queue.empty()) {
        m_queue.pop();
        --m_pending;
    }
}

