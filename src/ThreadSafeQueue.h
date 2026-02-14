#pragma once
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <optional>

namespace jxr {

template <typename T>
class ThreadSafeQueue {
public:
    void push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(value));
        }
        cv_.notify_one();
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        T val = std::move(queue_.front());
        queue_.pop_front();
        return val;
    }

    // Waits up to `timeout` for an item. Returns nullopt on timeout.
    template <typename Rep, typename Period>
    std::optional<T> wait_and_pop(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            return std::nullopt;
        }
        T val = std::move(queue_.front());
        queue_.pop_front();
        return val;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // Re-queue an item at the front (for retries)
    void push_front(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_front(std::move(value));
        }
        cv_.notify_one();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
};

} // namespace jxr
