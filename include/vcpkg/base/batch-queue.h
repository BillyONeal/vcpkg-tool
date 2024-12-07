#pragma once

#include <condition_variable>
#include <mutex>
#include <vector>

template<class T>
class BatchCollector
{
public:
    template<class... Args>
    void push(Args&&... args)
    {
        m_batch.emplace_back(std::forward<Args>(args)...);
    }

    bool empty() const { return m_batch.empty(); }

    void extract_all(std::vector<T>& out)
    {
        out.clear();
        swap(out, m_batch);
    }

private:
    std::vector<T> m_batch;
};

template<class WorkItem>
struct BGThreadBatchQueue
{
    template<class... Args>
    void push(Args&&... args)
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_tasks.push(std::forward<Args>(args)...);
        m_cv.notify_all();
    }

    void wait_for_items(std::vector<WorkItem>& out)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_cv.wait(lock, [this]() { return !m_tasks.empty() || !m_running; });
        m_tasks.extract_all(out);
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_running = false;
        m_cv.notify_all();
    }

    bool stopped()
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        return !m_running;
    }

private:
    std::mutex m_mtx;
    std::condition_variable m_cv;
    BatchCollector<WorkItem> m_tasks;
    bool m_running = true;
};
