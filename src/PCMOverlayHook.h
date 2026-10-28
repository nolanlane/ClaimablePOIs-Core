#pragma once

#include "RE/Starfield.h"

#include <cstddef>
#include <cstdint>

namespace ClaimablePOIs::PCMOverlayHook
{
	struct ClaimedPOIRecord
	{
		RE::TESFormID worldspaceID{ 0 };
		RE::TESFormID parentWorldID{ 0 };
		RE::TESFormID beaconRefID{ 0 };
		RE::TESFormID locationID{ 0 };
		float posX{ 0.0f };
		float posY{ 0.0f };
		float posZ{ 0.0f };
		float radius{ 250.0f };
	};

	[[nodiscard]] bool Install() noexcept;
	// Read-only check of the specific guard installed in SetOutpostOverlay.
	[[nodiscard]] bool VerifyOwnedOverlayResolverCall(std::uintptr_t a_callSite) noexcept;
	// The selection diagnostic excludes this already patched call from its
	// complete evaluator fingerprint only after verifying allocator ownership.
	[[nodiscard]] bool VerifyOwnedContentQueryCall(std::uintptr_t a_callSite) noexcept;
	void ResetQueryDiagnostics() noexcept;
	void LogQuerySummary(std::int32_t a_stage) noexcept;
	void SyncFromSave() noexcept;
	void RegisterBeacon(RE::TESObjectREFR* a_beacon) noexcept;
	void Clear() noexcept;
	[[nodiscard]] std::size_t GetRegisteredSiteCount() noexcept;
}
