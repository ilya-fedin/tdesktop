/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include <crl/crl.h>

#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

namespace {

constexpr auto kRepeats = 7;

[[nodiscard]] const char *BackendName() {
#if defined CRL_USE_DISPATCH
    return "dispatch (GCD)";
#elif defined CRL_USE_TMC
    return "TooManyCooks";
#elif defined CRL_USE_QT
    return "QThreadPool";
#else
    return "unknown";
#endif
}

[[nodiscard]] int64_t NowUs() {
    return crl::profile();
}

struct Result {
    std::string name;
    int threads = 0;
    int64_t count = 0;
    double bestMs = 0.0;
    double medianMs = 0.0;
    double spreadPct = 0.0;
    double throughput = 0.0;
};

template <typename Run>
[[nodiscard]] Result RunRepeated(Run &&run) {
    std::vector<double> times;
    Result result;
    for (auto r = 0; r < kRepeats; ++r) {
        result = run();
        times.push_back(result.bestMs);
    }
    std::sort(times.begin(), times.end());
    const auto best = times.front();
    const auto worst = times.back();
    const auto median = times[times.size() / 2];
    result.bestMs = best;
    result.medianMs = median;
    result.spreadPct = (median > 0.0) ? (worst - best) / median * 100.0 : 0.0;
    result.throughput = (best > 0.0) ? double(result.count) * 1000.0 / best : 0.0;
    return result;
}

[[nodiscard]] Result BenchBurst(int64_t count) {
    std::atomic<int64_t> remaining{count};
    crl::semaphore sem;
    const auto start = NowUs();
    for (auto i = 0; i < count; ++i) {
        crl::async([&] {
            if (remaining.fetch_sub(1, std::memory_order_relaxed) == 1) {
                sem.release();
            }
        });
    }
    sem.acquire();
    const auto elapsed = NowUs() - start;
    return {
        "burst",
        1,
        count,
        double(elapsed) / 1000.0,
        double(count) / (double(elapsed) / 1000000.0)
    };
}

[[nodiscard]] Result BenchMultiProducer(
        int threads,
        int64_t perThread) {
    const auto total = int64_t(threads) * perThread;
    std::atomic<int64_t> remaining{total};
    crl::semaphore sem;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    for (auto t = 0; t < threads; ++t) {
        producers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (auto i = 0; i < perThread; ++i) {
                crl::async([&] {
                    if (remaining.fetch_sub(
                            1,
                            std::memory_order_relaxed) == 1) {
                        sem.release();
                    }
                });
            }
        });
    }
    // Start timing once every producer is spawned and parked, keeping thread
    // creation cost out of the measured window.
    while (ready.load(std::memory_order_acquire) < threads) {
        std::this_thread::yield();
    }
    const auto start = NowUs();
    go.store(true, std::memory_order_release);
    sem.acquire();
    const auto elapsed = NowUs() - start;
    for (auto &t : producers) {
        t.join();
    }
    auto name = "multi-" + std::to_string(threads);
    return {
        name,
        threads,
        total,
        double(elapsed) / 1000.0,
        double(total) / (double(elapsed) / 1000000.0)
    };
}

[[nodiscard]] Result BenchSyncContention(
        int threads,
        int64_t perThread) {
    const auto total = int64_t(threads) * perThread;
    std::atomic<int64_t> completed{0};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    for (auto t = 0; t < threads; ++t) {
        producers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (auto i = 0; i < perThread; ++i) {
                crl::sync([&] {
                    completed.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < threads) {
        std::this_thread::yield();
    }
    const auto start = NowUs();
    go.store(true, std::memory_order_release);
    for (auto &t : producers) {
        t.join();
    }
    const auto elapsed = NowUs() - start;
    auto name = "sync-" + std::to_string(threads);
    return {
        name,
        threads,
        total,
        double(elapsed) / 1000.0,
        double(total) / (double(elapsed) / 1000000.0)
    };
}

struct LatencyResult {
    std::string name;
    int64_t count = 0;
    int64_t p50us = 0;
    int64_t p99us = 0;
    int64_t maxUs = 0;
};

// Per-task dispatch latency: from just before crl::async() to task entry.
[[nodiscard]] LatencyResult BenchLatency(int64_t count) {
    std::atomic<int64_t> remaining{count};
    crl::semaphore sem;
    std::vector<int64_t> lat(count);
    for (auto i = 0; i < count; ++i) {
        const auto submit = NowUs();
        crl::async([&, i, submit] {
            lat[i] = NowUs() - submit;
            if (remaining.fetch_sub(1, std::memory_order_relaxed) == 1) {
                sem.release();
            }
        });
    }
    sem.acquire();
    std::sort(lat.begin(), lat.end());
    return {
        "latency",
        count,
        lat[count * 50 / 100],
        lat[count * 99 / 100],
        lat.back()
    };
}

void Warmup(int64_t count) {
    std::atomic<int64_t> remaining{count};
    crl::semaphore sem;
    for (auto i = 0; i < count; ++i) {
        crl::async([&] {
            if (remaining.fetch_sub(1, std::memory_order_relaxed) == 1) {
                sem.release();
            }
        });
    }
    sem.acquire();
}

void PrintHeader() {
    std::printf("  %-16s  %10s  %5s  %9s  %9s  %7s  %12s\n",
        "scenario", "tasks", "thrds", "best_ms", "med_ms", "spread%", "thrpt/s");
    std::printf("  %-16s  %10s  %5s  %9s  %9s  %7s  %12s\n",
        "--------", "-----", "-----", "-------", "------", "-------", "-------");
}

void PrintResult(const Result &r) {
    std::printf("  %-16s  %10lld  %5d  %9.3f  %9.3f  %7.1f  %12.0f\n",
        r.name.c_str(),
        (long long)r.count,
        r.threads,
        r.bestMs,
        r.medianMs,
        r.spreadPct,
        r.throughput);
}

void PrintLatency(const LatencyResult &r) {
    std::printf("  %-16s  %10lld  p50=%lldus  p99=%lldus  max=%lldus\n",
        r.name.c_str(),
        (long long)r.count,
        (long long)r.p50us,
        (long long)r.p99us,
        (long long)r.maxUs);
}

} // namespace

int main() {
    const auto hwThreads = std::max(1, (int)std::thread::hardware_concurrency());

    std::printf("===== crl::async contention bench =====\n");
    std::printf("  backend:  %s\n", BackendName());
    std::printf("  threads:  %d hw\n", hwThreads);
    std::printf("  repeats:  %d (best / median reported)\n", kRepeats);
    std::printf("\n");

    std::printf("--- warm-up: 100000 tasks ---\n");
    Warmup(100000);
    std::printf("  done.\n\n");

    // burst (single producer), various sizes
    std::printf("--- burst (single producer) ---\n");
    PrintHeader();
    for (const auto count : { 1000, 10000, 100000, 1000000 }) {
        PrintResult(RunRepeated([&] { return BenchBurst(count); }));
    }
    std::printf("\n");

    // multi-producer: N threads, 10000 each
    const auto mpTotal = int64_t(hwThreads) * 10000;
    std::printf("--- multi-producer (%d threads, %lld each) ---\n",
        hwThreads, (long long)mpTotal / hwThreads);
    PrintHeader();
    PrintResult(RunRepeated([&] { return BenchMultiProducer(hwThreads, 10000); }));
    std::printf("\n");

    // oversubscription: 4N threads, 2500 each = N*10000 total
    const auto overThreads = hwThreads * 4;
    const auto overTotal = int64_t(overThreads) * 2500;
    std::printf("--- oversubscription (%d threads, %lld each) ---\n",
        overThreads, (long long)overTotal / overThreads);
    PrintHeader();
    PrintResult(RunRepeated([&] { return BenchMultiProducer(overThreads, 2500); }));
    std::printf("\n");

    // sync contention: N threads, 1000 syncs each
    const auto syncTotal = int64_t(hwThreads) * 1000;
    std::printf("--- sync contention (%d threads, %lld each) ---\n",
        hwThreads, (long long)syncTotal / hwThreads);
    PrintHeader();
    PrintResult(RunRepeated([&] { return BenchSyncContention(hwThreads, 1000); }));
    std::printf("\n");

    // dispatch latency, single producer
    std::printf("--- dispatch latency (single producer) ---\n");
    PrintLatency(BenchLatency(200000));
    std::printf("\n");

    std::printf("===== done =====\n");
    return 0;
}
