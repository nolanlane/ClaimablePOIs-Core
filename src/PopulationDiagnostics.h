#pragma once

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ClaimablePOIs::PopulationDiagnostics
{
	// Read-only signatures, not engine identity or proof of rendered geometry/navmesh.
	// Quantization can split nearby positions across a bin boundary. Keep the raw
	// FormID deltas alongside these heuristic results. Multiplicity must be preserved.
	constexpr std::size_t kSampleCap = 16384;
	constexpr float kBinSize = 0.1F;
	struct Key
	{
		std::uint32_t baseID{};
		std::uint32_t type{};
		std::int32_t x{}, y{}, z{};
		auto operator<=>(const Key&) const = default;
	};

	inline std::optional<Key> MakeKey(std::uint32_t a_baseID, std::uint32_t a_type,
		float a_x, float a_y, float a_z)
	{
		// Avoid undefined float-to-integer conversion on corrupt metadata.
		auto valid = [](float a_value) { return std::isfinite(a_value) && std::fabs(a_value) <= 10000000.0F; };
		if (a_baseID == 0 || !valid(a_x) || !valid(a_y) || !valid(a_z)) {
			return std::nullopt;
		}
		auto bin = [](float a_value) { return static_cast<std::int32_t>(std::round(static_cast<double>(a_value) * 10.0)); };
		return Key{ a_baseID, a_type, bin(a_x), bin(a_y), bin(a_z) };
	}

	struct Comparison
	{
		std::size_t matched{};
		std::vector<Key> unmatchedBefore;
		std::vector<Key> unmatchedAfter;
	};

	inline std::optional<Comparison> Compare(std::span<const Key> a_before, std::span<const Key> a_after)
	{
		if (a_before.size() > kSampleCap || a_after.size() > kSampleCap) {
			return std::nullopt;
		}
		std::vector<Key> before(a_before.begin(), a_before.end());
		std::vector<Key> after(a_after.begin(), a_after.end());
		std::ranges::sort(before);
		std::ranges::sort(after);
		Comparison result;
		std::size_t i = 0, j = 0;
		while (i < before.size() || j < after.size()) {
			if (j == after.size() || (i < before.size() && before[i] < after[j])) {
				result.unmatchedBefore.push_back(before[i++]);
			} else if (i == before.size() || after[j] < before[i]) {
				result.unmatchedAfter.push_back(after[j++]);
			} else {
				++result.matched;
				++i;
				++j;
			}
		}
		return result;
	}
}
