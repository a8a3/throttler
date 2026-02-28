#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace no_service_thread {

class Throttler {
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::duration<double>;

public:
    Throttler(int tokensPerSecond, int maxTokens) {
        if (tokensPerSecond <= 0) throw std::invalid_argument("Rate must be greater than 0");
        if (maxTokens <= 0) throw std::invalid_argument("Capacity must be greater than 0");

        currentTokensNum_ = maxTokensNum_ = maxTokens;
        tokenAddInterval_ = Duration(1.0 / tokensPerSecond);  // интервал в секундах
        lastAddTokenTime_ = Clock::now();
    }

    Throttler(const Throttler&) = delete;
    Throttler& operator=(const Throttler&) = delete;

    ~Throttler() {
        {
            std::unique_lock lock{mtx_};
            isRunning_ = false;
        }
        cv_.notify_all();

        std::unique_lock lock{mtx_};
        cv_.wait(lock, [this] { return 0 == activeThrottleCalls_; });
    }

    void Throttle() {
        std::unique_lock lock{mtx_};

        if (!isRunning_) {
            throw std::runtime_error("Throttler is shutting down, request aborted");
        }

        // учет количества активных потоков, вызвавших Throttle
        struct Guard {
            Throttler* self_;
            Guard(Throttler* t) : self_{t} { ++self_->activeThrottleCalls_; }
            ~Guard() {
                --self_->activeThrottleCalls_;
                if (self_->activeThrottleCalls_ == 0)
                    self_->cv_.notify_all();
            }
        } guard{this};

        while (true) {
            // сколько токенов можно добавить с момента последнего добавления
            const auto newTokens = static_cast<int>((Clock::now() - lastAddTokenTime_) / tokenAddInterval_);
            if (newTokens > 0) {
                currentTokensNum_ = std::min(currentTokensNum_ + newTokens, maxTokensNum_);
                // остаток времени от округления учитывается
                lastAddTokenTime_ += std::chrono::duration_cast<Clock::duration>(newTokens * tokenAddInterval_);
            }

            if (currentTokensNum_ > 0) {
                --currentTokensNum_;
                return;
            }

            // ожидать до времени возможного добавления следующего токена или до
            // нотификации на остановку
            const auto nextTokenTime = lastAddTokenTime_ + tokenAddInterval_;
            cv_.wait_until(lock, nextTokenTime, [this] { return !isRunning_; });

            if (!isRunning_) {
                throw std::runtime_error("Throttler is shutting down, request aborted");
            }
        }
    }

private:
    std::mutex              mtx_;
    std::condition_variable cv_;
    Clock::time_point       lastAddTokenTime_;
    Duration                tokenAddInterval_;
    int                     maxTokensNum_{0};
    int                     currentTokensNum_{0};
    int                     activeThrottleCalls_{0};
    bool                    isRunning_{true};
};

} // namespace no_service_thread