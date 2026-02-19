#include <gtest/gtest.h>
#include "../include/throttler.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace throttler;

using ThrottlerTypes = ::testing::Types<Throttler>;

template <typename ThrottlerType>
class ThrottlerTypedTest : public ::testing::Test {
protected:
    ThrottlerTypedTest() = default;
    ThrottlerType throttler{5, 10};
};

TYPED_TEST_SUITE(ThrottlerTypedTest, ThrottlerTypes);

TYPED_TEST(ThrottlerTypedTest, ConsumeSingleToken) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 10; ++i) {
        this->throttler.Throttle();
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime);
    
    EXPECT_LT(duration.count(), 10) << "All 10 tokens should be consumed without blocking"; 
}

TYPED_TEST(ThrottlerTypedTest, TokenReplenishment) {
    for (int i = 0; i < 10; ++i) {
        this->throttler.Throttle();
    }

    // Wait for tokens (~0.4 seconds for 2 tokens at 5/sec)
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    auto startTime = std::chrono::high_resolution_clock::now();
    this->throttler.Throttle();
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime);
    
    EXPECT_LT(duration.count(), 10) << "Token should be added"; 
}

TYPED_TEST(ThrottlerTypedTest, ConcurrentConsumption) {
    const int numThreads = 3;
    const int tokensPerThread = 3;
    std::atomic_int counter{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &counter, tokensPerThread]() {
            for (int j = 0; j < tokensPerThread; ++j) {
                this->throttler.Throttle();
                ++counter;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(counter, numThreads * tokensPerThread);
}

TYPED_TEST(ThrottlerTypedTest, ConcurrentStress) {
    const int numThreads = 8;
    const int tokensPerThread = 4;
    std::atomic_int totalConsumed{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &totalConsumed, tokensPerThread]() {
            for (int j = 0; j < tokensPerThread; ++j) {
                this->throttler.Throttle();
                ++totalConsumed;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(totalConsumed, numThreads * tokensPerThread);
}

TYPED_TEST(ThrottlerTypedTest, ThrottleBlocksWhenNoTokens) {
    // consume all tokens
    for (int i = 0; i < 10; ++i) {
        this->throttler.Throttle();
    }

    // try to consume when no tokens available
    std::atomic_bool blockDetected{false};
    std::thread consumer([this, &blockDetected]() {
        auto startTime = std::chrono::high_resolution_clock::now();
        this->throttler.Throttle();
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime - startTime);
        
        if (duration.count() > 50) {
            blockDetected = true;
        }
    });
    consumer.join();

    EXPECT_TRUE(blockDetected);
}
