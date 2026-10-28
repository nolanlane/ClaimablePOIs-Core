#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace ClaimablePOIs::ProceduralLifecycleDiagnostics
{
	using Key = std::array<std::uint32_t, 3>;
	using Position = std::array<float, 3>;
	constexpr std::size_t kIdentityCap = 4096;
	constexpr std::size_t kEventCap = 16384;
	constexpr float kRadius = 64.0F;
	constexpr std::size_t kPlacementBlockCap = 64;
	constexpr std::size_t kPlacementEntryCap = 4096;
	constexpr std::uint32_t kEntriesPerBlock = 85;
	enum class Kind : std::uint8_t {
		CreateBegin, Lookup, CreateEnd, Associate, RetireBegin, RetireEnd,
		ContentBegin, ContentEnd, DispatchBegin, InventoryEntry, DispatchEnd, Count
	};
	constexpr bool Planar(Kind a_kind) { return a_kind >= Kind::ContentBegin && a_kind < Kind::Count; }
	constexpr bool Spatial(Kind a_kind) { return a_kind == Kind::CreateBegin || a_kind == Kind::CreateEnd || Planar(a_kind); }
	constexpr bool ValidPlacementBlock(std::uint32_t a_cursor, std::uint32_t a_first, std::uint32_t a_end)
	{
		return a_first <= a_cursor && a_cursor < a_end && a_end <= kEntriesPerBlock;
	}
	struct Seed { std::uint32_t reference{}; Key key{}; bool present{}; };
	struct Event
	{
		Kind kind{};
		Key key{};
		Position position{};
		std::uint64_t sequence{}, invocation{}, parentInvocation{};
		std::uint32_t interval{}, reference{}, base{}, type{}, site{}, thread{};
		std::uint32_t node{}, stamp{}, flags{}, category{}, mask{}, seed{}, block{}, entry{}, count{};
		bool result{};
	};

	// Plain-value session only. The runtime adapter serializes it without taking
	// engine locks. Spatial creation candidates have no inferred world/owner.
	struct Session
	{
		std::uint64_t epoch{}, token{}, nextSequence{};
		std::uint32_t world{}, interval{};
		Position origin{};
		std::array<Key, kIdentityCap> keys{};
		std::array<std::uint32_t, kIdentityCap> references{};
		std::array<Event, kEventCap> events{};
		std::array<std::uint64_t, static_cast<std::size_t>(Kind::Count)> calls{};
		std::size_t keyCount{}, referenceCount{}, eventCount{};
		bool fault{};

		void Reset(std::uint64_t a_epoch)
		{
			epoch = a_epoch;
			token = nextSequence = 0;
			world = interval = 0;
			origin = {};
			keyCount = referenceCount = eventCount = 0;
			calls.fill(0);
			fault = false;
		}
		static bool Nonzero(const Key& a_key) { return a_key != Key{}; }
		bool HasKey(const Key& a_key) const
		{
			return Nonzero(a_key) && std::find(keys.begin(), keys.begin() + keyCount, a_key) != keys.begin() + keyCount;
		}
		bool HasReference(std::uint32_t a_reference) const
		{
			return a_reference && std::find(references.begin(), references.begin() + referenceCount, a_reference) != references.begin() + referenceCount;
		}
		bool Add(const Seed& a_seed)
		{
			if (a_seed.present && Nonzero(a_seed.key) && !HasKey(a_seed.key)) {
				if (keyCount == keys.size()) { fault = true; return false; }
				keys[keyCount++] = a_seed.key;
			}
			if (a_seed.reference && !HasReference(a_seed.reference)) {
				if (referenceCount == references.size()) { fault = true; return false; }
				references[referenceCount++] = a_seed.reference;
			}
			return !fault;
		}
		bool InRange(const Position& a_position, bool a_planar = false) const
		{
			double distance = 0;
			for (std::size_t i = 0; i < (a_planar ? 2u : origin.size()); ++i) {
				if (!std::isfinite(a_position[i]) || std::fabs(a_position[i]) > 10000000.0F) return false;
				const double delta = static_cast<double>(a_position[i]) - origin[i];
				distance += delta * delta;
			}
			return distance <= kRadius * kRadius;
		}
		void Observe(Event a_event)
		{
			++calls[static_cast<std::size_t>(a_event.kind)];
			if (fault) return;
			if (!(Spatial(a_event.kind) ? InRange(a_event.position, Planar(a_event.kind)) : HasKey(a_event.key) || HasReference(a_event.reference))) return;
			if (!Add({ a_event.reference, a_event.key, Nonzero(a_event.key) })) return;
			if (eventCount == events.size()) { fault = true; return; }
			a_event.sequence = ++nextSequence;
			a_event.interval = interval;
			events[eventCount++] = a_event;
		}
	};

	[[nodiscard]] bool Install() noexcept;
	void Reset(std::uint64_t a_epoch) noexcept;
	void Abort() noexcept;
	// Initial complete census arms; later sequential censuses drain and extend
	// correlation. The third checkpoint ends the two-return observation.
	[[nodiscard]] bool Checkpoint(std::uint64_t a_epoch, std::uint32_t a_world,
		const Position& a_origin, std::uint32_t a_checkpoint, std::span<const Seed> a_seeds) noexcept;
}
