#pragma once

#include <array>
#include <cstdint>

namespace RE { class TESObjectREFR; }

namespace ClaimablePOIs::ProceduralKeyDiagnostics
{
	constexpr std::uint32_t kNodeCap = 128;
	constexpr std::uint32_t kReadCap = 2048;
	constexpr std::uint32_t kChildLogCap = 32;
	constexpr std::uint32_t kSelectedCap = 32;
	struct Key { std::array<std::uint32_t, 3> parts{}; };
	struct Snapshot
	{
		Key key{};
		bool readable{}, present{};
		std::uint32_t exception{};
	};
	struct Node
	{
		std::uintptr_t next{}, vtable{};
		std::uint8_t type{};
		Key key{};
	};

	// Caller holds the list lock. Reader copies values only, never returns a node.
	// Reject inconsistent presence bits, duplicate extras, cycles/cap and bad reads.
	template <class Reader>
	Snapshot CopyBounded(std::uintptr_t a_head, std::uintptr_t a_vtable,
		bool a_expectedPresence, Reader&& a_reader)
	{
		Snapshot result{};
		std::uint32_t count = 0;
		while (a_head) {
			Node node{};
			if (++count > kNodeCap || a_head % alignof(std::uintptr_t) != 0 ||
				!a_reader(a_head, node)) return {};
			if (node.type == 0xEF) {
				if (result.present || !a_vtable || node.vtable != a_vtable) return {};
				result.present = true;
				result.key = node.key;
			}
			a_head = node.next;
		}
		if (result.present != a_expectedPresence) return {};
		result.readable = true;
		return result;
	}

	[[nodiscard]] bool VerifyRuntime() noexcept;
	// Synchronous observation only; caller owns a live reference for this call.
	[[nodiscard]] Snapshot Observe(RE::TESObjectREFR* a_reference, std::uint32_t a_expectedID) noexcept;
}
