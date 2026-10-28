#pragma once

#include <cstdint>
#include <cstring>
#include <span>

namespace ClaimablePOIs
{
    // Six-byte FF 15 rel32 calls use an allocator-owned eight-byte target slot.
    [[nodiscard]] inline bool MatchesOwnedIndirectGuardCall(
        std::span<const std::uint8_t,6> a_call, std::uintptr_t a_site,
        std::span<const std::uint8_t,8> a_pointer, std::uintptr_t a_ownedPointer,
        std::uintptr_t a_hook) noexcept
    {
        if (!a_site || !a_ownedPointer || !a_hook || a_call[0]!=0xFF || a_call[1]!=0x15) return false;
        std::int32_t disp{}; std::uint64_t target{};
        std::memcpy(&disp,a_call.data()+2,4); std::memcpy(&target,a_pointer.data(),8);
        return a_site+6+static_cast<std::uintptr_t>(disp)==a_ownedPointer && target==a_hook;
    }

	// Pinned CommonLib REL::Trampoline::write_call<5> emits E8 rel32 to
	// its owned JMP14 stub: FF 25 00 00 00 00 followed by the hook address.
	[[nodiscard]] inline bool MatchesOwnedGuardCall(
		std::span<const std::uint8_t, 5> a_call,
		std::uintptr_t a_callSite,
		std::span<const std::uint8_t, 14> a_stub,
		std::uintptr_t a_ownedStub,
		std::uintptr_t a_hook) noexcept
	{
		if (!a_callSite || !a_ownedStub || !a_hook || a_call[0] != 0xE8) {
			return false;
		}
		std::int32_t displacement = 0;
		std::memcpy(&displacement, a_call.data() + 1, sizeof(displacement));
		if (a_callSite + 5 + static_cast<std::uintptr_t>(displacement) != a_ownedStub) {
			return false;
		}
		constexpr std::uint8_t jumpPrefix[]{ 0xFF, 0x25, 0, 0, 0, 0 };
		std::uint64_t target = 0;
		std::memcpy(&target, a_stub.data() + sizeof(jumpPrefix), sizeof(target));
		return std::memcmp(a_stub.data(), jumpPrefix, sizeof(jumpPrefix)) == 0 && target == a_hook;
	}
}
