#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace basic {

class Throttler {
public:
    // 1 токен- разрешение на выполнение кода, следующего за вызовом Throttle
    // tokensPerSecond- сколько новых токенов появляется в секунду
    // maxTokens- максимальное количество доступных токенов
    Throttler(int tokensPerSecond, int maxTokens) {
        if (tokensPerSecond <= 0) throw std::invalid_argument("Rate must be greater than 0");
        if (maxTokens <= 0) throw std::invalid_argument("Capacity must be greater than 0");

        currentTokensNum_ = maxTokens;
        const auto tokenAddInterval = std::chrono::duration<double>(1.0 / tokensPerSecond);

        addTokenThread_ = std::thread{
            [this, tokenAddInterval, maxTokens] () {
                auto whenAdd = std::chrono::steady_clock::now() + tokenAddInterval;
                while (true) {
                    std::unique_lock lock{mtx_};
                    cv_.wait_until(lock, whenAdd, [this] {return !isRunning_;});
                    if (!isRunning_) break; // while

                    whenAdd += tokenAddInterval;
                    
                    if (currentTokensNum_ < maxTokens) {
                        ++currentTokensNum_;
                        lock.unlock();
                        cv_.notify_one();
                    }
                }
            }
        };
    }

    Throttler(const Throttler&) = delete;
    Throttler& operator=(const Throttler&) = delete;

    ~Throttler() {
        {
            std::unique_lock lock{mtx_};
            isRunning_ = false;
        }
        cv_.notify_all();

        {
            std::unique_lock lock{mtx_};
            guardCv_.wait(lock, [this] {return 0 == activeThrottleCalls_;});
        }

        if (addTokenThread_.joinable()) 
            addTokenThread_.join();
    }

    void Throttle() {
        // если есть свободные токены- взять один и продолжить выполнение,
        // если нет- ждать пока не появится или пока не будет вызван деструктор Throttler'a
        std::unique_lock lock{mtx_};

        if (!isRunning_) {
            throw std::runtime_error("Throttler is shutting down, request aborted");
        }

        // учет количества активных потоков, вызвавших Throttle
        struct Guard {
            Throttler* self_;

            Guard(Throttler* t) : self_{t} {
                ++self_->activeThrottleCalls_;
            }
            ~Guard() {
                --self_->activeThrottleCalls_;
                if (0 == self_->activeThrottleCalls_) {
                    self_->guardCv_.notify_all();
                }
            }
        } guard {this};

        cv_.wait(lock, [this] () {return currentTokensNum_ > 0 || !isRunning_;});
        if (!isRunning_) {
            // отказ ожидающим потокам при уничтожении Throttler'a
            throw std::runtime_error("Throttler is shutting down, request aborted");
        }
        --currentTokensNum_;
     }

private:
    std::thread addTokenThread_;
    bool isRunning_{true};

    std::mutex mtx_;
    std::condition_variable cv_;       // token availability + shutdown (consumers & service thread)
    std::condition_variable guardCv_;  // activeThrottleCalls_ counter (guard -> destructor only)
    int currentTokensNum_{0};
    int activeThrottleCalls_{0};
};

} // namespace basic