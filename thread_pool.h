#pragma once

#include <vector>
#include <queue>
#include <functional>
#include <atomic>
#include <type_traits>
#include <stdexcept>
#include <pthread.h>

#include "pthread_compat.h"
#include <future>

class ThreadPool {
public:
    explicit ThreadPool(size_t numWorkers) : stop_{false}, tasksRunning_{0} {
        pthread_mutex_init(&queueMutex_, nullptr);
        pthread_cond_init(&condition_, nullptr);

        for (size_t i = 0; i < numWorkers; ++i) {
            workers_.emplace_back();
            pthread_create(&workers_.back(), nullptr,
                [](void* arg) -> void* {
                    ThreadPool* pool = static_cast<ThreadPool*>(arg);
                    pool->workerLoop();
                    return nullptr;
                }, this);
        }
    }

    ~ThreadPool() {
        {
            pthread_mutex_lock(&queueMutex_);
            stop_ = true;
            pthread_mutex_unlock(&queueMutex_);
        }
        pthread_cond_broadcast(&condition_);
        for (auto& w : workers_) {
            if (w) pthread_join(w, nullptr);
        }
        pthread_mutex_destroy(&queueMutex_);
        pthread_cond_destroy(&condition_);
    }

    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ReturnType = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<ReturnType> result = task->get_future();
        {
            pthread_mutex_lock(&queueMutex_);
            if (stop_) {
                pthread_mutex_unlock(&queueMutex_);
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
            pthread_mutex_unlock(&queueMutex_);
        }
        pthread_cond_signal(&condition_);
        return result;
    }

    size_t workerCount() const { return workers_.size(); }

    void waitAll() {
        pthread_mutex_lock(&queueMutex_);
        while (!tasks_.empty() || tasksRunning_ > 0) {
            pthread_cond_wait(&condition_, &queueMutex_);
        }
        pthread_mutex_unlock(&queueMutex_);
    }

private:
    void workerLoop() {
        for (;;) {
            std::packaged_task<void()> task;
            {
                pthread_mutex_lock(&queueMutex_);
                while (!stop_ && tasks_.empty()) {
                    pthread_cond_wait(&condition_, &queueMutex_);
                }
                if (stop_ && tasks_.empty()) {
                    pthread_mutex_unlock(&queueMutex_);
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
                pthread_mutex_unlock(&queueMutex_);
            }
            tasksRunning_++;
            task();
            tasksRunning_--;
            pthread_cond_broadcast(&condition_);
        }
    }

    std::vector<pthread_t>        workers_;
    std::queue<std::packaged_task<void()>> tasks_;
    pthread_mutex_t               queueMutex_;
    pthread_cond_t               condition_;
    std::atomic<bool>             stop_;
    std::atomic<size_t>           tasksRunning_;
};
