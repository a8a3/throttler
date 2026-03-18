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
        std::unique_lock lock{mtx_};
        isRunning_ = false;

        // отменить все запросы оставшиеся в очереди
        while (!pendingRequests_.empty()) {
            pendingRequests_.front()->cancel();
            pendingRequests_.pop();
        }

        // дождаться пока все активные вызовы Throttle завершатся
        guardCv_.wait(lock, [this] { return 0 == activeThrottleCalls_; });
    }

    void Throttle() {
        PendingRequest request;
        {
            std::unique_lock lock{mtx_};
            if (!isRunning_) {
                throw std::runtime_error("Throttler is shutting down, request aborted");
            }
            ++activeThrottleCalls_;
            pendingRequests_.push(&request);
        }

        struct Guard {
            Throttler* self_;
            Guard(Throttler* t) : self_{t} {}
            ~Guard() {
                std::lock_guard lock{self_->mtx_};
                if (--self_->activeThrottleCalls_ == 0)
                    self_->guardCv_.notify_one();
            }
        } guard{this};
        
        while (true) {
            std::unique_lock lock{mtx_};
            tryAddTokens();

            std::unique_lock requestLock{request.mtx_};
            if (pendingRequests_.front() == &request) {
                 if (currentTokensNum_ > 0) {
                    --currentTokensNum_;
                    pendingRequests_.pop();
                    
                    // разбудить следующего в очереди, независимо от наличия токенов, 
                    // теперь он ответственен за продвижение времени и добавление токенов
                    if (!pendingRequests_.empty()) {
                        pendingRequests_.front()->wake();
                    }
                    return;
                }
                auto nextTokenTime = lastAddTokenTime_ + tokenAddInterval_;
                lock.unlock();

                // голова очереди ждет до появления следующего токена или отмены
                request.cv_.wait_until(
                    requestLock, 
                    nextTokenTime,
                    [&request] { return request.cancelled_; });
            } else {
                lock.unlock();

                // хвост очереди ждет пока не станет головой или не будет отменен
                request.cv_.wait(requestLock, [&request] {
                    return request.cancelled_ || request.awaken_;
                });
            }

            if (request.cancelled_) {
                throw std::runtime_error("Throttler is shutting down, request aborted");
            }
        }
    }

private:
    // добавить токены, если прошло достаточно времени и разбудить поток из головы очереди
    // может быть вызван только под mtx_ мьютексом
    void tryAddTokens() {
        const auto newTokens = static_cast<int>((Clock::now() - lastAddTokenTime_) / tokenAddInterval_);
        if (newTokens > 0) {
            currentTokensNum_ = std::min(currentTokensNum_ + newTokens, maxTokensNum_);
            lastAddTokenTime_ += std::chrono::duration_cast<Clock::duration>(newTokens * tokenAddInterval_);
            if (!pendingRequests_.empty()) {
                // разбудить текущую голову очереди
                pendingRequests_.front()->wake();
            }
        }
    }

    std::mutex              mtx_;
    std::condition_variable guardCv_;
    Clock::time_point       lastAddTokenTime_;
    Duration                tokenAddInterval_;
    int                     maxTokensNum_{0};
    int                     currentTokensNum_{0};
    int                     activeThrottleCalls_{0};
    bool                    isRunning_{true};

    struct PendingRequest {
        std::condition_variable cv_;
        std::mutex mtx_;
        bool awaken_{false};
        bool cancelled_{false};

        void wake() {
            {
                std::lock_guard lock{mtx_};
                awaken_ = true;
            }
            cv_.notify_one();
        }

        void cancel() {
            {
                std::lock_guard lock{mtx_};
                cancelled_ = true;
            }
            cv_.notify_one();
        }
    };
    // PendingRequest некопируемый, поэтому в очереди указатели
    using PendingRequestsQueue = std::queue<PendingRequest*>;
    PendingRequestsQueue pendingRequests_;
};

} // namespace no_service_thread_fifo
