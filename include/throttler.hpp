#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace throttler {

class Throttler {
public:
    // 1 токен- разрешение на выполнение кода, следующего за вызовом Throttle
    // tokensPerSecond- сколько новых токенов появляется в секунду
    // maxTokens- максимальное количество доступных токенов
    Throttler(int tokensPerSecond, int maxTokens) {
        if (tokensPerSecond <= 0) throw std::runtime_error("Rate must be greater than 0");
        if (maxTokens <= 0) throw std::runtime_error("Capacity must be greater than 0");

        currentTokensNum_ = maxTokens;
        const auto tokenAddInterval = std::chrono::duration<double>(1.0 / tokensPerSecond);

        tokenCreatorThread_ = std::thread{
            [this, tokenAddInterval, maxTokens] () {
                while (true) {
                    std::this_thread::sleep_for(tokenAddInterval);
                    {
                        std::unique_lock lock{mtx_};
                        if (!isRunning_) break; // while
                        currentTokensNum_ = std::min(maxTokens, currentTokensNum_ + 1);
                    }
                    cv_.notify_one();
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

        if (tokenCreatorThread_.joinable()) 
            tokenCreatorThread_.join();
    }

    void Throttle() {
        // если есть свободные токены- взять один и продолжить выполнение,
        // если нет- ждать пока не появится или пока не будет вызван деструктор Throttler'a
        std::unique_lock lock{mtx_};
        cv_.wait(lock, [this] () {return currentTokensNum_ > 0 || !isRunning_;});
        if (!isRunning_) {
            // отказ ожидающим потокам при уничтожении Throttler'a
            throw std::runtime_error("Throttler is shutting down, request aborted");
        }
        --currentTokensNum_;
     }

private:
    std::thread tokenCreatorThread_;
    bool isRunning_{true};

    std::mutex mtx_;
    std::condition_variable cv_;
    int currentTokensNum_;
};

} // namespace throttler