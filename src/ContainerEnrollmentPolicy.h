#pragma once

#include <cstdint>

namespace ClaimablePOIs
{
	// CPOI safety-mask bits, not engine flags. Adopt non-constructible compound
	// storage, including targets of incoming linked refs. Vanilla-buildable
	// CONT (especially the active placement preview) must never be adopted.
	// No ownership, outgoing links, workshop or procedural carriers are admitted.
	inline constexpr std::uint32_t kCompoundContainerAllowedFlags = (1u << 3) | (1u << 12);
	inline constexpr std::uint32_t kCompoundContainerRequiredFlags = 1u << 3;

	[[nodiscard]] constexpr bool IsCompoundContainerMaskAllowed(std::uint32_t a_flags) noexcept
	{
		return (a_flags & kCompoundContainerRequiredFlags) == kCompoundContainerRequiredFlags &&
			(a_flags & ~kCompoundContainerAllowedFlags) == 0;
	}
}
