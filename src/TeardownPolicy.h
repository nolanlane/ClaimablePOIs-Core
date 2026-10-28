#pragma once
#include <cstdint>
#include <span>

namespace ClaimablePOIs
{
    // Shared engine registry, not the number of player-owned outposts. The
    // captured v3 run has 419 entries / capacity 512. Keep this independent of
    // the inherited ledger's separate cap and allocate the snapshot on the heap.
    inline constexpr std::uint32_t kTeardownRegistryLimit=65536;
    inline constexpr std::uint32_t kTeardownRegistryCapacityLimit=1048576;
    [[nodiscard]] constexpr bool IsBoundedTeardownRegistry(std::uint32_t a_size,
        std::uint32_t a_capacity,bool a_storage) noexcept
    {
        return a_size<=a_capacity && a_size<=kTeardownRegistryLimit &&
            a_capacity<=kTeardownRegistryCapacityLimit && (!a_capacity || a_storage);
    }
    // The native caller holds the real engine read lock and contains raw access
    // faults. Reject all metadata/buffer failures before the first output write.
    [[nodiscard]] inline bool CopyTeardownRegistry(const std::uint32_t* a_data,
        std::uint32_t a_size,std::uint32_t a_capacity,std::span<std::uint32_t> a_out) noexcept
    {
        if (!IsBoundedTeardownRegistry(a_size,a_capacity,a_data!=nullptr) || a_size>a_out.size()) return false;
        for (std::uint32_t i=0;i<a_size;++i) a_out[i]=a_data[i];
        return true;
    }
    // Compare the target's canonical, generation-bearing key. An unrelated
    // stale/unloaded handle does not make this exact membership read unknown.
    [[nodiscard]] inline bool ContainsTeardownRegistryKey(std::span<const std::uint32_t> a_keys,
        std::uint32_t a_key) noexcept
    { for (const auto key:a_keys) if (key==a_key) return true; return false; }
    // Completion is a transaction result, not the beacon's Papyrus delete bit.
    [[nodiscard]] constexpr bool ConfirmTeardown(bool a_returned, bool a_retired,
        bool a_wasRegistered, bool a_registryReadable, bool a_stillRegistered, bool a_survived) noexcept
    { return a_returned && a_retired && a_wasRegistered && a_registryReadable && !a_stillRegistered && a_survived; }
    enum class TeardownItemAction { Forward, Preserve, Fault };
    [[nodiscard]] constexpr TeardownItemAction ClassifyTeardownItem(std::uintptr_t a_savedPointer,
        std::uint32_t a_savedID, std::uintptr_t a_item, std::uint32_t a_id, std::uintptr_t a_current) noexcept
    {
        if (!a_savedPointer || !a_item || a_savedPointer!=a_item) return TeardownItemAction::Forward;
        return a_savedID==a_id && a_current==a_item ? TeardownItemAction::Preserve : TeardownItemAction::Fault;
    }

    inline constexpr std::uint32_t kInheritedLedgerLimit=4096;
    [[nodiscard]] constexpr bool IsBoundedTeardownList(std::uint32_t a_size,std::uint32_t a_capacity,bool a_data,
        std::uint32_t b_size,std::uint32_t b_capacity,bool b_data,std::uint32_t a_added) noexcept
    {
        return a_size<=a_capacity && a_capacity<=kInheritedLedgerLimit && (!a_capacity || a_data) &&
            b_size<=b_capacity && b_capacity<=kInheritedLedgerLimit && (!b_capacity || b_data) &&
            a_size+b_size<=kInheritedLedgerLimit && a_added<=a_size+b_size;
    }

    struct TeardownDetachResult { bool success{}, attempted{}; std::uint32_t detached{}; };
    template<class Range, class Current, class Linked, class Detach, class Detached>
    TeardownDetachResult DetachForTeardown(const Range& a_targets, Current a_current,
        Linked a_linked, Detach a_detach, Detached a_detached)
    {
        TeardownDetachResult result;
        if (!a_current()) return result;
        // Preflight the entire batch before making the first engine mutation.
        for (const auto& target:a_targets) if (!a_current() || !a_linked(target)) return result;
        for (const auto& target:a_targets) {
            if (!a_current() || !a_linked(target)) return result;
            result.attempted=true;
            if (!a_detach(target) || !a_detached(target)) return result;
            ++result.detached;
        }
        result.success=a_current();
        return result;
    }

    // Plain identities only. Runtime caller serializes access; no engine pointer
    // survives a request, load boundary, or deferred Papyrus callback.
    struct TeardownRequest
    {
        std::uint32_t sequence{}, token{}, beacon{};
        bool authorized{}, consumed{};

        void Reset() noexcept { token = beacon = 0; authorized = consumed = false; }
        std::uint32_t Queue(std::uint32_t a_beacon) noexcept
        {
            if (!a_beacon) return 0;
            if (token) return beacon == a_beacon ? token : 0;
            sequence = sequence == 0x7FFFFFFF ? 1 : sequence + 1;
            token = sequence;
            beacon = a_beacon;
            return token;
        }
        bool Matches(std::uint32_t a_token, std::uint32_t a_beacon) const noexcept
        { return token && token == a_token && beacon == a_beacon; }
        bool Authorize(std::uint32_t a_token, std::uint32_t a_beacon) noexcept
        {
            if (!Matches(a_token, a_beacon) || authorized || consumed) return false;
            authorized = true;
            return true;
        }
        bool Consume(std::uint32_t a_beacon) noexcept
        {
            if (!token || beacon != a_beacon || !authorized || consumed) return false;
            consumed = true;
            return true;
        }
    };
}
