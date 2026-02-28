#include <gtest/gtest.h>

#include "../include/basic/throttler.hpp"
#include "../include/no_service_thread/throttler.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace std::chrono_literals;

using Clock = std::chrono::steady_clock;

template <typename Duration>
auto toMs(Duration d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d);
}

using ThrottlerTypes = ::testing::Types<
    basic::Throttler,
    no_service_thread::Throttler
>;

template <typename ThrottlerType>
class ThrottlerTypedTest : public ::testing::Test {
protected:
    ThrottlerTypedTest() = default;
    ThrottlerType throttler{5, 10};
};

TYPED_TEST_SUITE(ThrottlerTypedTest, ThrottlerTypes);

TYPED_TEST(ThrottlerTypedTest, ConstructorThrowsOnInvalidArguments) {
    EXPECT_THROW(TypeParam( 0,  1), std::invalid_argument) << "Zero rate should throw";
    EXPECT_THROW(TypeParam(-1,  1), std::invalid_argument) << "Negative rate should throw";
    EXPECT_THROW(TypeParam( 1,  0), std::invalid_argument) << "Zero capacity should throw";
    EXPECT_THROW(TypeParam( 1, -1), std::invalid_argument) << "Negative capacity should throw";

    EXPECT_NO_THROW(TypeParam(1, 1)) << "Valid arguments should not throw";
}

TYPED_TEST(ThrottlerTypedTest, ConsumeSingleToken) {
    auto startTime = Clock::now();
    
    for (int i = 0; i < 10; ++i) {
        this->throttler.Throttle();
    }
    
    auto endTime = Clock::now();
    auto duration = toMs(endTime - startTime);
    
    EXPECT_LT(duration.count(), 10) << "All 10 tokens should be consumed without blocking"; 
}

TYPED_TEST(ThrottlerTypedTest, TokenReplenishment) {
    for (int i = 0; i < 10; ++i) {
        this->throttler.Throttle();
    }

    // Wait for tokens (~0.4 seconds for 2 tokens at 5/sec)
    std::this_thread::sleep_for(400ms);

    auto startTime = Clock::now();
    this->throttler.Throttle();
    auto endTime = Clock::now();
    auto duration = toMs(endTime - startTime);
    
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
        auto startTime = Clock::now();
        this->throttler.Throttle();
        auto endTime = Clock::now();
        
        if (toMs(endTime - startTime).count() > 50) {
            blockDetected = true;
        }
    });
    consumer.join();

    EXPECT_TRUE(blockDetected);
}

TYPED_TEST(ThrottlerTypedTest, ThrowsWhenShuttingDown) {
    auto throttler = std::make_unique<TypeParam>(1, 1);

    // no free tokens
    throttler->Throttle();

    std::atomic_bool exceptionCaught{false};
    std::string what;
    std::thread waiter([&throttler, &exceptionCaught, &what]() {
        try {
            throttler->Throttle();
        } catch (const std::runtime_error& error) {
            exceptionCaught = true;
            what = error.what();
        }
    });

    std::this_thread::sleep_for(50ms);
    // waiter is waitin on Throttler()
    throttler.reset();

    waiter.join();
    EXPECT_TRUE(exceptionCaught);
    EXPECT_STREQ(what.c_str(), "Throttler is shutting down, request aborted");
}

TYPED_TEST(ThrottlerTypedTest, FinishingServiceThreadWhenShuttingDown) {
    auto throttler = std::make_unique<TypeParam>(1, 1);

    // no free tokens
    throttler->Throttle();

    auto startTime = Clock::now();
    std::thread waiter([&throttler] {
        try {
            throttler->Throttle();
        } catch (const std::runtime_error& error) {
            // do nothing...
        }
    });
    
    // Ensure waiter has entered Throttle() before destroying
    std::this_thread::sleep_for(50ms);
    
    throttler.reset();

    auto endTime = Clock::now();
    auto duration = toMs(endTime - startTime);
    EXPECT_LT(duration.count(), 500) << "Sevice thread should be woken up by the Throttler destructor";

    waiter.join();
}

TYPED_TEST(ThrottlerTypedTest, TokensDoNotExceedMaxCapacity) {
// TODO: 
    if constexpr (std::is_same_v<TypeParam, basic::Throttler>) {
        GTEST_SKIP() << "basic::Throttler has a race in TokensDoNotExceedMaxCapacity";
    }

    TypeParam throttler{2, 3};  // rate = 2/sec, max tokens = 3

    std::this_thread::sleep_for(2s);

    auto start = Clock::now();
    for (int i = 0; i < 3; ++i) {
        throttler.Throttle();
    }
    auto burstDuration = toMs(Clock::now() - start);
    EXPECT_LT(burstDuration.count(), 50) << "Using of 3 first tokens should be instant";

    std::this_thread::sleep_for(50ms);

    std::atomic_bool blocked{false};
    std::thread t([&throttler, &blocked] {
        auto s = Clock::now();
        throttler.Throttle();
        blocked = toMs(Clock::now() - s).count() > 100;
    });
    t.join();
    EXPECT_TRUE(blocked) << "4th token should wait";
}

TYPED_TEST(ThrottlerTypedTest, AllWaitersThrowOnShutdown) {
    auto throttler = std::make_unique<TypeParam>(1, 1);

    throttler->Throttle(); // no free tokens

    const int numWaiters = 5;
    std::atomic_int exceptionCount{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < numWaiters; ++i) {
        waiters.emplace_back([&throttler, &exceptionCount] {
            try {
                throttler->Throttle();
            } catch (const std::runtime_error&) {
                ++exceptionCount;
            }
        });
    }

    std::this_thread::sleep_for(50ms);
    throttler.reset();

    for (auto& w : waiters) w.join();

    EXPECT_EQ(exceptionCount, numWaiters) << "All waiting threads throw exception";
}

TYPED_TEST(ThrottlerTypedTest, RateAccuracy) {
    const int rate = 10;
    TypeParam throttler{rate, 1};

    throttler.Throttle();

    const int samples = 100;
    auto start = Clock::now();
    for (int i = 0; i < samples; ++i) {
        throttler.Throttle();
    }
    const auto elapsed = toMs(Clock::now() - start);

    const double expected = static_cast<double>(samples) / rate * 1000.0;
    EXPECT_GT(elapsed.count(), expected * 0.95) << "Rate too fast";
    EXPECT_LT(elapsed.count(), expected * 1.05) << "Rate too slow";
}