#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>

namespace fifo {

class Throttler {
    using Clock = std::chrono::steady_clock;

public:
    Throttler(int tokensPerSecond, int maxTokens) {
        if (tokensPerSecond <= 0) throw std::invalid_argument("Rate must be greater than 0");
        if (maxTokens <= 0) throw std::invalid_argument("Capacity must be greater than 0");

        timePerToken_ = Clock::duration(std::chrono::seconds(1)) / tokensPerSecond;
        maxTokens_ = maxTokens;
        tokens_ = maxTokens;
        countingThread_ = std::thread{&Throttler::countingThreadFunc, this};
    }

    ~Throttler() {
        {
            std::lock_guard runningLock{runningMtx_};
            isRunning_ = false;
            runningCV_.notify_one();
        }
        countingThread_.join();

        // отменить все запросы оставшиеся в очереди
        std::lock_guard lock{mtx_};
        while (!pendingRequests_.empty()) {
            pendingRequests_.front()->cancel();
            pendingRequests_.pop();
        }
    }

    void Throttle() {
        std::unique_lock lock{mtx_};

        if (tokens_ > 0) {
            --tokens_;
            return;
        }

        // положить запрос в очередь отложенных запросов и приостановить
        // отправивший запрос поток до появления свободного токена
        // или до вызова деструктора Throttler'a
        auto request = std::make_shared<PendingRequest>();
        pendingRequests_.push(request);
        request->cv_.wait(lock, [&request] { return request->awaken; });

        if (request->cancelled) {
            throw std::runtime_error("Throttler is shutting down, request aborted");
        }
    }

private:
    void countingThreadFunc() {
        auto tp = Clock::now();
        Clock::duration timeError{};
        std::unique_lock lock{runningMtx_};

        while (isRunning_) {
            runningCV_.wait_for(lock, timePerToken_ - timeError, [this] () {
                return !isRunning_;
            });

            if (!isRunning_) break;

            auto nextTp = Clock::now();
            {
                std::lock_guard lock{mtx_};
                tokens_ = std::min(maxTokens_, tokens_ + static_cast<int>((nextTp - tp) / timePerToken_));
                DrainPendingRequestsQueue();
            }
            timeError = (nextTp - tp) % timePerToken_;
            tp = nextTp - timeError;
        }
    }

    // может быть вызван только под mtx_ мьютексом
    void DrainPendingRequestsQueue() {
        while (!pendingRequests_.empty() && tokens_ > 0) {
            --tokens_;
            pendingRequests_.front()->wake();
            pendingRequests_.pop();
        }
    }

    struct PendingRequest {
        std::condition_variable cv_;
        bool awaken{false};
        bool cancelled{false};

        void wake() {
            awaken = true;
            cv_.notify_all();
        }

        void cancel() {
            cancelled = true;
            wake();
        }
    };
    // PendingRequest некопируемый, поэтому в очереди указатели
    using PendingRequestsQueue = std::queue<std::shared_ptr<PendingRequest>>;

    Clock::duration timePerToken_;
    int maxTokens_{};

    std::mutex mtx_;
    int tokens_{};
    PendingRequestsQueue pendingRequests_;

    std::condition_variable runningCV_;
    std::mutex runningMtx_;
    bool isRunning_{true};

    std::thread countingThread_;
};

} // namespace fifo