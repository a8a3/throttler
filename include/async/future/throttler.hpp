#pragma once

#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>

namespace async::future {

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

        std::lock_guard lock{mtx_};
        while (!requestsQueue_.empty()) {
            requestsQueue_.front().set_exception(
                std::make_exception_ptr(std::runtime_error("Throttler is shutting down, request aborted"))
            );
            requestsQueue_.pop();
        }
    }

    std::future<void> Throttle() {
        PendingRequest request;
        auto fut = request.get_future();

        std::unique_lock lock{mtx_};
        if (isRunning_) {
            requestsQueue_.emplace(std::move(request));
            TryDrainPendingRequestsQueue();
        } else {
            request.set_exception(
                std::make_exception_ptr(std::runtime_error("Throttler is shutting down, request aborted")
            ));
        }
        return fut;
    }

    Throttler(const Throttler&) = delete;
    Throttler& operator=(const Throttler&) = delete;

private:
    void CountingThreadFunc() {
        auto tp = Clock::now();
        Clock::duration timeErr{};

        std::unique_lock lock{mtx_};
        while(isRunning_) {
            if (tokens_ == maxTokens_) {
                // если токены пока никому не нужны- спать, пока кто-то не возьмет токен
                // клиент, взявший токен пронотифицирует cv_
                cv_.wait(lock, [this] () { 
                    return tokens_ < maxTokens_ || !isRunning_; 
                });

                // обновить данные времени, т.к поток мог спать больше интервала для 
                // добавления токена при полной корзине токенов. Ситуация аналогична
                // первому старту троттлера
                tp = Clock::now();
                timeErr = {};
                continue;

            } else {
                // если можно добавить токен, спать до времени следующего пополнения
                cv_.wait_for(lock, timePerToken_ - timeErr, [this] () {
                    return !isRunning_;
                });
            }

            if (!isRunning_) break;

            auto endTp = Clock::now();
            timeErr = (endTp - tp) % timePerToken_;
            {
                // добавить токены и вычитать из очереди ждущие promice
                tokens_ = std::min(maxTokens_, tokens_ + static_cast<int>((endTp - tp) / timePerToken_));
                TryDrainPendingRequestsQueue();
            }
            tp = endTp - timeErr;
        }
    }

    // вызывается под мьютексом
    void TryDrainPendingRequestsQueue() {
        while (tokens_ > 0 && !requestsQueue_.empty()) {
            requestsQueue_.front().set_value();
            requestsQueue_.pop();
            --tokens_;
            if (tokens_ == maxTokens_ - 1) {
                // в этой ситуации служебный поток спит на cv_
                cv_.notify_one();
            }
        }
    }

    std::mutex mtx_;
    int tokens_{};
    using PendingRequest = std::promise<void>;
    using PendingRequestsQueue = std::queue<PendingRequest>;
    PendingRequestsQueue requestsQueue_;

    Clock::duration timePerToken_;
    int maxTokens_{};

    std::thread countingThread_;
    bool isRunning_{true};
    std::condition_variable cv_;
};

} // namespace async::future