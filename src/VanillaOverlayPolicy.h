#pragma once

#include <cmath>
#include <cstdint>

namespace ClaimablePOIs
{
	struct InitialOverlayClaim
	{
		std::uint64_t token{};
		std::uint32_t helper{}, cellWorld{}, overlay{};
		float x{}, y{}, z{}, radius{};
	};

	// Plain values only: a registration result belongs to one live claim and beacon.
	struct InitialOverlayBinding
	{
		std::uint64_t token{};
		std::uint32_t helper{}, beacon{}, original{}, selected{}, created{};
	};

	[[nodiscard]] inline bool HasVerifiedInitialBinding(const InitialOverlayBinding& a_binding,
		std::uint64_t a_token, std::uint32_t a_helper, std::uint32_t a_beacon) noexcept
	{
		return a_token && a_binding.token == a_token && a_helper && a_binding.helper == a_helper &&
			a_beacon && a_binding.beacon == a_beacon && a_binding.original &&
			a_binding.original == a_binding.selected && !a_binding.created;
	}

	[[nodiscard]] inline bool MatchesInitialOverlayClaim(const InitialOverlayClaim& a_claim,
		std::uint32_t a_base, std::uint32_t a_cellWorld, float a_x, float a_y, float a_z) noexcept
	{
		if (!a_claim.token || !a_claim.helper || !a_claim.cellWorld || !a_claim.overlay ||
			a_base != 0x002C60E9 || a_cellWorld != a_claim.cellWorld ||
			!std::isfinite(a_claim.radius) || a_claim.radius <= 0 || a_claim.radius > 500) {
			return false;
		}
		const double dx = static_cast<double>(a_x) - a_claim.x;
		const double dy = static_cast<double>(a_y) - a_claim.y;
		const double dz = static_cast<double>(a_z) - a_claim.z;
		const double distanceSquared = dx * dx + dy * dy + dz * dz;
		return std::isfinite(distanceSquared) && distanceSquared <= static_cast<double>(a_claim.radius) * a_claim.radius;
	}

	[[nodiscard]] inline bool SelectSeparateVanillaOverlay(const InitialOverlayClaim& a_claim,
		std::uint64_t a_currentToken, std::uint32_t a_selectedOverlay, std::uint32_t a_parentWorld,
		bool a_enableSeparation = false) noexcept
	{
		if (!a_enableSeparation) {
			return false;
		}
		return a_claim.token && a_currentToken == a_claim.token && a_claim.overlay &&
			a_selectedOverlay == a_claim.overlay && a_parentWorld && a_parentWorld != a_selectedOverlay;
	}
}
