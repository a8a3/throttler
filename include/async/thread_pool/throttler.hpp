#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

namespace async::thread_pool {

class Throttler {
    using Clock = std::chrono::steady_clock;

public:

    Throttler(int tokensPerSecond, int maxTokens) {
        if (tokensPerSecond <= 0) throw std::invalid_argument("Rate must be greater than 0");
        if (maxTokens <= 0) throw std::invalid_argument("Capacity must be greater than 0");

        timePerToken_ = Clock::duration(std::chrono::seconds(1)) / tokensPerSecond;
        maxTokens_ = maxTokens;
        tokens_ = maxTokens;

        countingThread_ = std::thread{&Throttler::CountingThreadFunc, this};
    }

    ~Throttler() {
        {
            std::lock_guard lock{mtx_};
            isRunning_ = false;
            cv_.notify_one();
        }
        countingThread_.join();
        pool_.join();
    }

    using Callback = std::function<void(bool isCancelled)>;
    void Throttle(Callback cb) {
        std::unique_lock lock{mtx_};
        if (isRunning_) {
            requestsQueue_.emplace(std::move(cb));
            cv_.notify_one();
        } else {
            lock.unlock();
            try { cb(true); } catch (...) {};
        }
    }

    Throttler(const Throttler&) = delete;
    Throttler& operator=(const Throttler&) = delete;

private:
    void CountingThreadFunc() {
        auto tp = Clock::now();
        Clock::duration timeErr{};

        std::unique_lock lock{mtx_};
        while(isRunning_) {
            if (requestsQueue_.empty()) {
                // если очередь пуста- спать, пока не появится коллбек на исполнение
                cv_.wait(lock, [this] () { 
                    return !requestsQueue_.empty() || !isRunning_; 
                });
            } else if (0 == tokens_) {
                // если нет токенов, спать пока нельзя будет добавить
                cv_.wait_until(lock, tp + timePerToken_, [this] () {
                    return !isRunning_;
                });
            }

            if (!isRunning_) break; // while

            auto endTp = Clock::now();
            timeErr = (endTp - tp) % timePerToken_;
            {
                // добавить токены и выполнить коллбеки ждущие исполнения
                tokens_ = std::min(maxTokens_, tokens_ + static_cast<int>((endTp - tp) / timePerToken_));
                TryDrainPendingRequestsQueue(lock);
            }
            tp = endTp - timeErr;
        }

        while (!requestsQueue_.empty()) {
            auto cb = std::move(requestsQueue_.front());
            requestsQueue_.pop();
            lock.unlock();
            boost::asio::post(pool_, [cb = std::move(cb)] () {
                try{cb(true);} catch (...) {}
            });
            lock.lock();
        }
    }

    // вызывается под мьютексом
    void TryDrainPendingRequestsQueue(std::unique_lock<std::mutex>& lock) {
        while (tokens_ > 0 && !requestsQueue_.empty()) {
            auto cb = std::move(requestsQueue_.front());
            requestsQueue_.pop();
            --tokens_;
            lock.unlock();
            boost::asio::post(pool_, [cb = std::move(cb)] () {
                try{cb(false);} catch (...) {}
            });
            lock.lock();
        }
    }

    std::mutex mtx_;
    int tokens_{};
    using PendingRequestsQueue = std::queue<Callback>;
    PendingRequestsQueue requestsQueue_;

    Clock::duration timePerToken_;
    int maxTokens_{};

    std::thread countingThread_;
    bool isRunning_{true};
    std::condition_variable cv_;

    // поддерживается FIFO порядок помещения в пул, но не исполнения
    boost::asio::thread_pool pool_{
        std::min(std::thread::hardware_concurrency(), 4U)
    };
};

} // namespace async::thread_pool