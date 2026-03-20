#include <gtest/gtest.h>

#include <future/throttler.hpp>

#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

using Clock = std::chrono::steady_clock;

namespace {

auto WaitStatus(const std::future<void>& fut, std::chrono::milliseconds timeout) {
    return fut.wait_for(timeout);
}

} // namespace

TEST(AsyncFutureThrottlerTest, ConstructorThrowsOnInvalidArguments) {
    using async::future::Throttler;

    EXPECT_THROW(Throttler(0, 1), std::invalid_argument);
    EXPECT_THROW(Throttler(-1, 1), std::invalid_argument);
    EXPECT_THROW(Throttler(1, 0), std::invalid_argument);
    EXPECT_THROW(Throttler(1, -1), std::invalid_argument);

    EXPECT_NO_THROW(Throttler(1, 1));
}

TEST(AsyncFutureThrottlerTest, ImmediateReadinessWithinInitialCapacity) {
    async::future::Throttler throttler{5, 3};

    auto f1 = throttler.Throttle();
    auto f2 = throttler.Throttle();
    auto f3 = throttler.Throttle();

    EXPECT_EQ(WaitStatus(f1, 1ms), std::future_status::ready);
    EXPECT_EQ(WaitStatus(f2, 1ms), std::future_status::ready);
    EXPECT_EQ(WaitStatus(f3, 1ms), std::future_status::ready);

    EXPECT_NO_THROW(f1.get());
    EXPECT_NO_THROW(f2.get());
    EXPECT_NO_THROW(f3.get());
}

TEST(AsyncFutureThrottlerTest, FutureStaysPendingWhenNoTokenYet) {
    async::future::Throttler throttler{2, 1}; // 2 per second

    auto immediate = throttler.Throttle();
    EXPECT_EQ(WaitStatus(immediate, 1ms), std::future_status::ready);
    immediate.get();

    auto pending = throttler.Throttle();
    EXPECT_EQ(WaitStatus(pending, 100ms), std::future_status::timeout);

    EXPECT_EQ(WaitStatus(pending, 700ms), std::future_status::ready);
    EXPECT_NO_THROW(pending.get());
}

TEST(AsyncFutureThrottlerTest, FifoCompletionOrder) {
    async::future::Throttler throttler{2, 1};

    auto first = throttler.Throttle();
    first.get(); // drain initial capacity

    auto f1 = throttler.Throttle();
    auto f2 = throttler.Throttle();
    auto f3 = throttler.Throttle();

    std::vector<int> readyOrder;
    readyOrder.reserve(3);
    bool seen1 = false;
    bool seen2 = false;
    bool seen3 = false;

    const auto deadline = Clock::now() + 2500ms;
    while (readyOrder.size() < 3 && Clock::now() < deadline) {
        if (!seen1 && WaitStatus(f1, 0ms) == std::future_status::ready) {
            seen1 = true;
            readyOrder.push_back(1);
        }
        if (!seen2 && WaitStatus(f2, 0ms) == std::future_status::ready) {
            seen2 = true;
            readyOrder.push_back(2);
        }
        if (!seen3 && WaitStatus(f3, 0ms) == std::future_status::ready) {
            seen3 = true;
            readyOrder.push_back(3);
        }
        std::this_thread::sleep_for(1ms);
    }

    ASSERT_EQ(readyOrder.size(), 3u);
    EXPECT_EQ(readyOrder[0], 1);
    EXPECT_EQ(readyOrder[1], 2);
    EXPECT_EQ(readyOrder[2], 3);

    EXPECT_NO_THROW(f1.get());
    EXPECT_NO_THROW(f2.get());
    EXPECT_NO_THROW(f3.get());
}

TEST(AsyncFutureThrottlerTest, ThroughputRespectsRate) {
    const int rate = 5;
    const int requests = 10;

    async::future::Throttler throttler{rate, 1};

    auto first = throttler.Throttle();
    first.get();

    std::vector<std::future<void>> futures;
    futures.reserve(requests);

    const auto start = Clock::now();
    for (int i = 0; i < requests; ++i) {
        futures.emplace_back(throttler.Throttle());
    }

    for (auto& fut : futures) {
        fut.get();
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    const double expectedMs = static_cast<double>(requests) / rate * 1000.0;

    EXPECT_GT(elapsedMs, expectedMs * 0.85);
    EXPECT_LT(elapsedMs, expectedMs * 1.25);
}

TEST(AsyncFutureThrottlerTest, ShutdownCancelsPendingFutures) {
    auto throttler = std::make_unique<async::future::Throttler>(1, 1);

    auto immediate = throttler->Throttle();
    immediate.get();

    auto pending1 = throttler->Throttle();
    auto pending2 = throttler->Throttle();

    throttler.reset();

    EXPECT_EQ(WaitStatus(pending1, 50ms), std::future_status::ready);
    EXPECT_EQ(WaitStatus(pending2, 50ms), std::future_status::ready);

    EXPECT_THROW(
        {
            try {
                pending1.get();
            } catch (const std::runtime_error& e) {
                EXPECT_STREQ(e.what(), "Throttler is shutting down, request aborted");
                throw;
            }
        },
        std::runtime_error
    );

    EXPECT_THROW(pending2.get(), std::runtime_error);
}

TEST(AsyncFutureThrottlerTest, ConcurrentSubmittingAllFuturesComplete) {
    async::future::Throttler throttler{20, 1};

    const int threads = 8;
    const int perThreadRequests = 10;

    std::vector<std::future<void>> futures;
    futures.reserve(threads * perThreadRequests);
    std::mutex futuresMtx;

    std::vector<std::thread> producers;
    producers.reserve(threads);

    for (int i = 0; i < threads; ++i) {
        producers.emplace_back([&] {
            for (int j = 0; j < perThreadRequests; ++j) {
                auto fut = throttler.Throttle();
                std::lock_guard lock{futuresMtx};
                futures.emplace_back(std::move(fut));
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    for (auto& fut : futures) {
        EXPECT_NO_THROW(fut.get());
    }
}

TEST(AsyncCallbackThrottlerTest, ConstructorThrowsOnInvalidArguments) {
    GTEST_SKIP() << "async::callback::Throttler is not implemented yet";
}

TEST(AsyncCallbackThrottlerTest, ImmediateExecutionWithinCapacity) {
    GTEST_SKIP() << "async::callback::Throttler is not implemented yet";
}

TEST(AsyncCallbackThrottlerTest, PreservesFifoOrder) {
    GTEST_SKIP() << "async::callback::Throttler is not implemented yet";
}

TEST(AsyncCallbackThrottlerTest, CancelsPendingOnShutdown) {
    GTEST_SKIP() << "async::callback::Throttler is not implemented yet";
}
