#include "RE/Starfield.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
    // Independent model of the verified engine release arithmetic, for live
    // objects with other owners (the virtual zero-owner callbacks do not run).
    constexpr std::uint64_t kExternalField = std::uint64_t{1} << 21;
    constexpr std::uint64_t kTotalField = std::uint64_t{1} << 43;
    constexpr std::uint64_t kSeed = 17 + 23 * kExternalField + 29 * kTotalField;
    void ReleaseStrong(volatile std::uint64_t& a_count)
    {
        REX::TAtomicRef count{ a_count };
        count.fetch_sub(1);
        count.fetch_sub(kExternalField + kTotalField);
    }
    void ReleaseExternal(volatile std::uint64_t& a_count)
    {
        REX::TAtomicRef count{ a_count };
        count.fetch_sub(kExternalField);
        count.fetch_sub(kTotalField);
    }
}

int main()
{
    using Counter = RE::TESFormRefCount;
    static_assert(Counter::kRuntimeRefCountABI == 116244);
    static_assert(Counter::kRefCountMask == (1u << 21) - 1);
    volatile std::uint64_t count = kSeed;
    bool passed = Counter::AcquireStrong(count) == kSeed + 1 + kExternalField + kTotalField;
    passed &= Counter::AcquireExternal(count) == kSeed + 1 + 2 * kExternalField + 2 * kTotalField;
    ReleaseExternal(count);
    ReleaseStrong(count);
    passed &= count == kSeed;

    // A population-size sequence of balanced temporary acquisitions must not
    // consume the engine's other owners. Exercise the actual patched functions.
    for (int i = 0; i < 16384; ++i) {
        Counter::AcquireStrong(count);
        Counter::AcquireStrong(count);
        Counter::AcquireExternal(count);
        ReleaseStrong(count);
        ReleaseExternal(count);
        ReleaseStrong(count);
    }
    passed &= count == kSeed;
    std::vector<std::jthread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back([&] {
        for (int j = 0; j < 16384; ++j) {
            Counter::AcquireStrong(count);
            Counter::AcquireExternal(count);
            ReleaseStrong(count);
            ReleaseExternal(count);
        }
    });
    threads.clear();
    passed &= count == kSeed;

    // Regression controls: both old acquire implementations corrupt the same
    // initial state when paired with this runtime's releases.
    volatile std::uint64_t oldStrong = kSeed + 1;
    ReleaseStrong(oldStrong);
    volatile std::uint64_t oldExternal = kSeed + 0x400000;
    ReleaseExternal(oldExternal);
    passed &= oldStrong != kSeed && oldExternal != kSeed;
    std::cout << (passed ? "Form refcount balance/regression/concurrency tests passed\n" : "Form refcount tests FAILED\n");
    return passed ? 0 : 1;
}
