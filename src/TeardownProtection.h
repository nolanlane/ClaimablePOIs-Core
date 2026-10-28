#pragma once
#include <cstdint>
#include <vector>
#include "TeardownPolicy.h"

namespace RE { class BGSListForm; class TESObjectREFR; }
namespace ClaimablePOIs::TeardownProtection
{
    inline constexpr std::uint32_t kInheritedItems = 0x810;
    inline constexpr std::uint32_t kProtectedBeacons = 0x811;
    inline constexpr std::size_t kListLimit = kInheritedLedgerLimit;

    bool Install() noexcept;
    bool Ready() noexcept;
    void Fault() noexcept;
    void ResetLoad() noexcept;
    bool Snapshot(const RE::BGSListForm*, std::vector<std::uint32_t>&) noexcept;
    bool Contains(const RE::BGSListForm*, std::uint32_t, bool& a_valid) noexcept;
    bool Record(RE::BGSListForm*, RE::TESObjectREFR*) noexcept;
    bool WorkshopOwner(RE::TESObjectREFR*, RE::TESObjectREFR*&) noexcept;
    bool PreflightRemoval(RE::TESObjectREFR*) noexcept;
    bool EnqueueRemoval(RE::TESObjectREFR*, std::uint32_t) noexcept;
    std::uint32_t PendingToken() noexcept;
    std::uint32_t PendingBeacon(std::uint32_t) noexcept;
    bool Matches(std::uint32_t, std::uint32_t) noexcept;
    bool Authorize(std::uint32_t, std::uint32_t) noexcept;
    void Cancel(std::uint32_t) noexcept;
    bool Busy() noexcept;
    // -1 = failed, 0 = pending/unknown, 1 = returned stock removal, registry
    // absence and inherited reference state independently verified.
    int Completion(std::uint32_t) noexcept;
}
