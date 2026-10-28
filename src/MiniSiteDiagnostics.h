#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace ClaimablePOIs::MiniSiteDiagnostics
{
	constexpr float kRadius = 64.0F;
	constexpr std::uint32_t kRootLogCap = 128;
	using Origin = std::array<float, 3>;

	// Volatile diagnostic coordinates only; never engine pointers or save data.
	struct Session
	{
		std::uint64_t epoch{};
		std::uint32_t world{}, nextCheckpoint{};
		Origin origin{};
		std::array<std::uint32_t, 4> attempts{};

		void Reset(std::uint64_t a_epoch) { *this = Session{ .epoch = a_epoch }; }

		std::optional<Origin> Begin(std::uint32_t a_checkpoint, std::uint32_t a_world,
			const Origin& a_position, std::uint64_t a_epoch)
		{
			if (a_epoch != epoch || !a_world || a_checkpoint >= attempts.size() ||
				a_checkpoint != nextCheckpoint || attempts[a_checkpoint] >= 2) return std::nullopt;
			for (const auto coordinate : a_position) {
				if (!std::isfinite(coordinate) || std::fabs(coordinate) > 10000000.0F) return std::nullopt;
			}
			if (a_checkpoint != 0) {
				if (a_world != world) return std::nullopt;
				double distanceSquared = 0;
				for (std::size_t i = 0; i < origin.size(); ++i) {
					const double delta = static_cast<double>(a_position[i]) - origin[i];
					distanceSquared += delta * delta;
				}
				if (distanceSquared > kRadius * kRadius) return std::nullopt;
			}
			++attempts[a_checkpoint];
			return a_checkpoint == 0 ? a_position : origin;
		}

		void Complete(std::uint32_t a_world, const Origin& a_origin)
		{
			if (nextCheckpoint == 0) {
				world = a_world;
				origin = a_origin;
			}
			++nextCheckpoint;
		}
	};
}
