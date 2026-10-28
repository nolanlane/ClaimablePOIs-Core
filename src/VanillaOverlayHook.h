#pragma once
#include <cstdint>

namespace RE { class TESObjectREFR; class TESWorldSpace; }

namespace ClaimablePOIs::VanillaOverlayHook
{
	[[nodiscard]] bool Install() noexcept;
	[[nodiscard]] bool ArmInitialClaim(RE::TESObjectREFR* a_helper, RE::TESWorldSpace* a_overlay, float a_radius) noexcept;
	void ClearInitialClaim() noexcept;
	[[nodiscard]] bool IsInitialClaimBound(std::uint32_t a_helper, std::uint32_t a_beacon) noexcept;
}
