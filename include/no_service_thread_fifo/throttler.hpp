#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stdexcept>

namespace no_service_thread_fifo {

class Throttler {
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::duration<double>;

public:
    Throttler(int tokensPerSecond, int maxTokens) {
        if (tokensPerSecond <= 0) throw std::invalid_argument("Rate must be greater than 0");
        if (maxTokens <= 0) throw std::invalid_argument("Capacity must be greater than 0");

        currentTokensNum_ = maxTokensNum_ = maxTokens;
        tokenAddInterval_ = Duration(1.0 / tokensPerSecond);
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
        cv_.wait(lock, [this] { return activeThrottleCalls_ == 0; });
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

        const TicketNumber myTicket = nextTicket_++;
        queue_.push(myTicket);
        
        while (true) {
            tryAddTokens();

            if (queue_.front() == myTicket && currentTokensNum_ > 0) {
                --currentTokensNum_;
                queue_.pop();
                cv_.notify_all(); // разбудить следующего в голове очереди, если он есть
                return;
            }

            if (queue_.front() == myTicket) {
                // голова очереди, но нет токенов — ждать до времени добавления следующего токена или до нотификации на остановку
                const auto nextTokenTime = lastAddTokenTime_ + std::chrono::duration_cast<Clock::duration>(tokenAddInterval_);
                cv_.wait_until(lock, nextTokenTime, [this] { return !isRunning_; });
            } else {
                // не голова очереди, ждать пока не станет головой или не будет нотификации на остановку
                cv_.wait(lock, [this, myTicket] {
                    return !isRunning_ || (!queue_.empty() && queue_.front() == myTicket);
                });
            }

            if (!isRunning_) {
                throw std::runtime_error("Throttler is shutting down, request aborted");
            }
        }
    }

private:
    // добавить токены, если прошло достаточно времени и разбудить ожидающие потоки
    // может быть вызван только под mtx_ мьютексом
    void tryAddTokens() {
        const auto newTokens = static_cast<int>((Clock::now() - lastAddTokenTime_) / tokenAddInterval_);
        if (newTokens > 0) {
            currentTokensNum_ = std::min(currentTokensNum_ + newTokens, maxTokensNum_);
            lastAddTokenTime_ += std::chrono::duration_cast<Clock::duration>(newTokens * tokenAddInterval_);
            cv_.notify_all();
        }
    }

    std::mutex              mtx_;
    std::condition_variable cv_;
    Clock::time_point       lastAddTokenTime_;
    Duration                tokenAddInterval_;
    int                     maxTokensNum_{0};
    int                     currentTokensNum_{0};
    int                     activeThrottleCalls_{0};
    bool                    isRunning_{true};

    using TicketNumber = unsigned int; 
    std::queue<TicketNumber> queue_;
    TicketNumber             nextTicket_{0};
};

} // namespace no_service_thread_fifo
