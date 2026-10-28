#include "pch.h"
#include "ProceduralLifecycleDiagnostics.h"
#include "GuardedCallValidation.h"
#include "PCMOverlayHook.h"
#include "REL/Trampoline.h"
#include "REL/Utility.h"
#include "REX/FModule.h"
#include "SFSE/Version.h"
#include <Windows.h>
#undef ERROR
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
	namespace PD = ClaimablePOIs::ProceduralLifecycleDiagnostics;
	// Exact 1.16.244 call/vtable boundaries; no entry detour/guessed stack arguments.
	using Create = void* (*)(void*, void*, const void*, const void*);
	using Lookup = void* (*)(void*, const void*);
	using Associate = void (*)(void*, std::uint32_t, const void*);
	using Retire = bool (*)(void*, std::uint32_t);
	using Content = bool (*)(void*, void*);
	using Dispatch = void (*)(void*, const void*, const void*);
	Create s_create{};
	Lookup s_lookup{};
	Associate s_associate{};
	Retire s_retire{};
	Content s_content{};
	Dispatch s_dispatch{};
	std::uintptr_t s_runtimeBase{};
	constexpr std::uintptr_t kContentVtable = 0x04B417D8, kContentSlot = 0x04B41B30;
	thread_local std::uint64_t s_parentInvocation{}, s_parentToken{};
	struct InvocationScope
	{
		std::uint64_t prior{ s_parentInvocation }, priorToken{ s_parentToken };
		InvocationScope(std::uint64_t a_token, std::uint64_t a_invocation) { s_parentToken = a_token; s_parentInvocation = a_invocation; }
		~InvocationScope() { s_parentToken = priorToken; s_parentInvocation = prior; }
		std::uint64_t Parent(std::uint64_t a_token) const { return priorToken == a_token ? prior : 0; }
	};
	std::mutex s_mutex;
	PD::Session s_session{};
	std::atomic<std::uint64_t> s_activeToken{}, s_faultToken{};
	std::atomic<std::uint64_t> s_nextInvocation{};
	std::array<std::atomic<float>, 3> s_origin{};
	// Negative-only hints avoid locking for unrelated worker traffic. Positive
	// matches are checked against exact keys/IDs under the diagnostic mutex.
	constexpr std::size_t kHintWords = 512;
	std::array<std::atomic<std::uint64_t>, kHintWords> s_keyHints{}, s_referenceHints{};
	std::size_t s_publishedKeys{}, s_publishedReferences{}; // under diagnostic mutex
	static_assert(std::atomic<float>::is_always_lock_free);
	std::uint64_t s_nextToken{}; // under diagnostic mutex
	bool s_installed{};
	std::uint64_t Hash(const PD::Key& a_key) noexcept
	{
		std::uint64_t hash = 14695981039346656037ull;
		for (const auto part : a_key) hash = (hash ^ part) * 1099511628211ull;
		return hash;
	}
	void Hint(std::array<std::atomic<std::uint64_t>, kHintWords>& a_hints, std::uint64_t a_hash) noexcept
	{
		for (const auto shift : { 0u, 32u }) {
			const auto bit = (a_hash >> shift) % (kHintWords * 64);
			a_hints[bit / 64].fetch_or(1ull << (bit % 64), std::memory_order_release);
		}
	}
	bool Maybe(const std::array<std::atomic<std::uint64_t>, kHintWords>& a_hints, std::uint64_t a_hash) noexcept
	{
		for (const auto shift : { 0u, 32u }) {
			const auto bit = (a_hash >> shift) % (kHintWords * 64);
			if ((a_hints[bit / 64].load(std::memory_order_acquire) & (1ull << (bit % 64))) == 0) return false;
		}
		return true;
	}
	void Publish() noexcept
	{
		while (s_publishedKeys < s_session.keyCount) Hint(s_keyHints, Hash(s_session.keys[s_publishedKeys++]));
		while (s_publishedReferences < s_session.referenceCount) Hint(s_referenceHints, Hash({ s_session.references[s_publishedReferences++], 0, 0 }));
	}
	bool Candidate(const PD::Event& a_event) noexcept
	{
		if (PD::Spatial(a_event.kind)) {
			double distance = 0;
			for (std::size_t i = 0; i < (PD::Planar(a_event.kind) ? 2u : a_event.position.size()); ++i) {
				if (!std::isfinite(a_event.position[i]) || std::fabs(a_event.position[i]) > 10000000.0F) return false;
				const double delta = static_cast<double>(a_event.position[i]) - s_origin[i].load(std::memory_order_acquire);
				distance += delta * delta;
			}
			return distance <= PD::kRadius * PD::kRadius;
		}
		return (PD::Session::Nonzero(a_event.key) && Maybe(s_keyHints, Hash(a_event.key))) ||
			(a_event.reference && Maybe(s_referenceHints, Hash({ a_event.reference, 0, 0 })));
	}

	void Fault(std::uint64_t a_token) noexcept
	{
		auto prior = s_faultToken.load(std::memory_order_relaxed);
		while (prior < a_token && !s_faultToken.compare_exchange_weak(prior, a_token, std::memory_order_release)) {}
	}
	void Record(std::uint64_t a_token, PD::Event a_event) noexcept
	{
		if (!a_token || s_activeToken.load(std::memory_order_acquire) != a_token || !Candidate(a_event)) return;
		try {
			// Never wait while vanilla holds a manager lock; lost observation fails
			// the trace instead of changing engine scheduling/lock ordering.
			std::unique_lock lock(s_mutex, std::try_to_lock);
			if (!lock.owns_lock()) { Fault(a_token); return; }
			if (s_session.token != a_token || s_activeToken.load(std::memory_order_acquire) != a_token) return;
			if (!PD::Planar(a_event.kind)) a_event.parentInvocation = s_parentToken == a_token ? s_parentInvocation : 0;
			a_event.thread = GetCurrentThreadId();
			s_session.Observe(a_event);
			Publish();
		} catch (...) { Fault(a_token); }
	}
	bool ReadKey(const void* a_key, PD::Key& a_value) noexcept
	{
		__try { std::memcpy(a_value.data(), a_key, sizeof(a_value)); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	bool ReadCreation(const void* a_base, const void* a_placement, const void* a_key, PD::Event& a_event) noexcept
	{
		// Target body reads base ID/type at 28/2E and copies placement+30 to
		// reference position+8C. Read inputs synchronously; no form acquisition.
		__try {
			std::memcpy(&a_event.base, static_cast<const std::uint8_t*>(a_base) + 0x28, 4);
			a_event.type = *(static_cast<const std::uint8_t*>(a_base) + 0x2E);
			std::memcpy(a_event.position.data(), static_cast<const std::uint8_t*>(a_placement) + 0x30, 12);
			std::memcpy(a_event.key.data(), a_key, 12);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	bool ReadLookup(const void* a_result, std::uint32_t& a_reference) noexcept
	{
		// Borrowed caller-owned output, first DWORD only. Its ownership flag at
		// +4 is left untouched; no dereference of the returned reference itself.
		__try { std::memcpy(&a_reference, a_result, 4); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	enum class ContentRead { Failed, OtherType, Copied };
	ContentRead ReadContent(const void* a_node, const void* a_request, PD::Event& a_event) noexcept
	{
		// Exact content-node vtable/BE form type; evaluator reads content+60,
		// request stamp+70 and XY+B8/BC. No smart pointer acquisition or request
		// retention. These XY inputs alone do not identify the world or owner.
		__try {
			const auto* node = static_cast<const std::uint8_t*>(a_node);
			if (*reinterpret_cast<const std::uintptr_t*>(node) != s_runtimeBase + kContentVtable ||
				node[0x2E] != 0xBE) return ContentRead::Failed;
			const auto* content = *reinterpret_cast<const std::uint8_t* const*>(node + 0x60);
			if (!content || (content[0x2E] != 0x2B && content[0x2E] != 0x34)) return ContentRead::OtherType;
			std::memcpy(&a_event.node, node + 0x28, 4);
			std::memcpy(&a_event.base, content + 0x28, 4);
			a_event.type = content[0x2E];
			std::memcpy(&a_event.stamp, static_cast<const std::uint8_t*>(a_request) + 0x70, 4);
			std::memcpy(a_event.position.data(), static_cast<const std::uint8_t*>(a_request) + 0xB8, 8);
			return ContentRead::Copied;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return ContentRead::Failed; }
	}
	bool ObserveContent(void* a_node, void* a_request)
	{
		const auto token = s_activeToken.load(std::memory_order_acquire);
		if (!token) return s_content(a_node, a_request);
		PD::Event event{ .kind = PD::Kind::ContentBegin };
		const auto copied = ReadContent(a_node, a_request, event);
		if (copied == ContentRead::Failed) Fault(token);
		if (copied != ContentRead::Copied || !Candidate(event)) return s_content(a_node, a_request);
		event.invocation = s_nextInvocation.fetch_add(1, std::memory_order_relaxed) + 1;
		InvocationScope scope(token, event.invocation);
		event.parentInvocation = scope.Parent(token);
		Record(token, event);
		const bool result = s_content(a_node, a_request); // exactly once; original exceptions propagate
		event.kind = PD::Kind::ContentEnd;
		event.result = result;
		Record(token, event);
		return result; // evaluator result, not proof of placement/materialization
	}
	bool ReadInventory(std::uint64_t a_token, const void* a_iterator, const void* a_context, PD::Event& a_summary) noexcept
	{
		// Caller keeps the collected placement blocks alive across this call.
		// Mirror only the dispatcher's borrowed, forward iterator. Bound every
		// header/index before entry arithmetic and detect cycles without allocation.
		__try {
			const auto* block = *reinterpret_cast<const std::uint8_t* const*>(a_iterator);
			std::uint32_t cursor{};
			std::memcpy(&cursor, static_cast<const std::uint8_t*>(a_iterator) + 0x10, 4);
			const auto mask = *(static_cast<const std::uint8_t*>(a_context) + 0x70);
			std::array<const std::uint8_t*, PD::kPlacementBlockCap> visited{};
			std::size_t blocks{}, entries{};
			while (block) {
				if (blocks == visited.size() || std::find(visited.begin(), visited.begin() + blocks, block) != visited.begin() + blocks) return false;
				visited[blocks++] = block;
				std::uint32_t first{}, end{};
				std::memcpy(&first, block + 0x1FF0, 4);
				std::memcpy(&end, block + 0x1FF4, 4);
				if (!PD::ValidPlacementBlock(cursor, first, end)) return false;
				for (; cursor < end; ++cursor) {
					if (++entries > PD::kPlacementEntryCap) return false;
					const auto* entry = block + static_cast<std::size_t>(cursor) * 0x60;
					PD::Event event{ .kind = PD::Kind::InventoryEntry, .invocation = a_summary.invocation, .parentInvocation = a_summary.parentInvocation };
					std::memcpy(&event.flags, entry, 4);
					event.category = entry[0x4C];
					event.mask = mask;
					std::memcpy(event.position.data(), entry + 0x50, 8);
					std::memcpy(&event.base, entry + 0x58, 4);
					std::memcpy(&event.seed, entry + 0x5C, 4);
					event.block = static_cast<std::uint32_t>(blocks - 1);
					event.entry = cursor;
					if (!Candidate(event)) continue;
					if (event.category >= 8) return false; // no inferred shift/category semantics
					event.result = (mask & (1u << event.category)) != 0; // category gate only
					if (!a_summary.count) {
						a_summary.position = event.position;
						a_summary.mask = mask;
						Record(a_token, a_summary);
					}
					++a_summary.count;
					Record(a_token, event);
				}
				block = *reinterpret_cast<const std::uint8_t* const*>(block + 0x1FE8);
				if (block) std::memcpy(&cursor, block + 0x1FF0, 4);
			}
			a_summary.stamp = static_cast<std::uint32_t>(entries); // total entries traversed, not request stamp
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	void ObserveDispatch(void* a_manager, const void* a_iterator, const void* a_context)
	{
		const auto token = s_activeToken.load(std::memory_order_acquire);
		if (!token) { s_dispatch(a_manager, a_iterator, a_context); return; }
		PD::Event summary{ .kind = PD::Kind::DispatchBegin, .invocation = s_nextInvocation.fetch_add(1, std::memory_order_relaxed) + 1 };
		InvocationScope scope(token, summary.invocation);
		summary.parentInvocation = scope.Parent(token);
		const bool complete = ReadInventory(token, a_iterator, a_context, summary);
		if (!complete) Fault(token);
		s_dispatch(a_manager, a_iterator, a_context); // outside read guard; engine exceptions propagate
		if (summary.count) {
			summary.kind = PD::Kind::DispatchEnd;
			summary.result = complete;
			Record(token, summary);
		}
	}
	template <std::uint32_t Site>
	void* ObserveCreation(void* a_context, void* a_base, const void* a_placement, const void* a_key)
	{
		const auto token = s_activeToken.load(std::memory_order_acquire);
		PD::Event event{ .kind = PD::Kind::CreateBegin, .site = Site };
		if (token) event.invocation = s_nextInvocation.fetch_add(1, std::memory_order_relaxed) + 1;
		const bool copied = token && ReadCreation(a_base, a_placement, a_key, event);
		if (token && !copied) Fault(token);
		if (copied) Record(token, event);
		void* result = s_create(a_context, a_base, a_placement, a_key); // exactly once; engine exceptions propagate
		if (copied) {
			event.kind = PD::Kind::CreateEnd;
			event.result = result != nullptr;
			Record(token, event); // never inspect/acquire the borrowed result
		}
		return result;
	}
	void* ObserveLookup(void* a_output, const void* a_key)
	{
		const auto token = s_activeToken.load(std::memory_order_acquire);
		PD::Event event{ .kind = PD::Kind::Lookup };
		const bool copied = token && ReadKey(a_key, event.key);
		if (token && !copied) Fault(token);
		void* result = s_lookup(a_output, a_key);
		if (copied) {
			if (!ReadLookup(result, event.reference)) Fault(token);
			else { event.result = event.reference != 0; Record(token, event); }
		}
		return result;
	}
	void ObserveAssociation(void* a_holder, std::uint32_t a_reference, const void* a_key)
	{
		const auto token = s_activeToken.load(std::memory_order_acquire);
		PD::Event event{ .kind = PD::Kind::Associate, .reference = a_reference };
		const bool copied = token && ReadKey(a_key, event.key);
		if (token && !copied) Fault(token);
		s_associate(a_holder, a_reference, a_key);
		if (copied) { event.result = true; Record(token, event); }
	}
	bool ObserveRetirement(void* a_holder, std::uint32_t a_reference)
	{
		const auto token = s_activeToken.load(std::memory_order_acquire);
		PD::Event event{ .kind = PD::Kind::RetireBegin, .reference = a_reference };
		if (token) event.invocation = s_nextInvocation.fetch_add(1, std::memory_order_relaxed) + 1;
		Record(token, event);
		const bool result = s_retire(a_holder, a_reference);
		event.kind = PD::Kind::RetireEnd;
		event.result = result;
		Record(token, event);
		return result;
	}

	struct Gate { std::uintptr_t rva; std::size_t size; std::uint64_t hash; };
	constexpr std::array<Gate, 15> kGates{{
		{ 0x007A2A10, 0x24B, 0x4B2C3AB7A1C2DFEFull }, { 0x007A2C60, 0x18C, 0x5F4B335F46C86028ull },
		{ 0x007A6700, 0x6E8, 0xAA6D8A0F7DF1C3E9ull }, { 0x007A6DED, 0x5, 0x4F17C57031EB4F80ull },
		{ 0x007A7060, 0x52, 0x6FE6237562CAF332ull }, { 0x007A70B7, 0x9B5, 0xDB9449E2158028CAull },
		{ 0x005DE450, 0x117, 0xE1619EC8F9A969ADull }, { 0x02409C80, 0x183, 0x09CE9D17AD42B9FDull },
		{ 0x02409E10, 0xEC, 0xDC8D3FA6D29CF1A0ull },
		{ 0x007A4B90, 0xC6, 0x8E89C18C93994B2Full }, { 0x007A4C5B, 0x62, 0x06A60C6B2D58F57Eull },
		{ 0x007A4CC0, 0x143, 0x0CDEC83765B97E6Aull }, { 0x007A38C0, 0x234, 0x420DCF5178B0F258ull },
		{ 0x007C3FB0, 0xB8, 0x22BBF500F12080CFull }, { 0x007C406D, 0x792, 0x1C51236CA1516FCEull }
	}};
	struct Site { std::uintptr_t rva, target, hook, stub{}; std::uint8_t opcode; std::array<std::uint8_t, 5> original{}; };
	std::array<Site, 6> s_sites{{
		{ 0x007A2C5B, 0x007A7060, reinterpret_cast<std::uintptr_t>(&ObserveCreation<0>), 0, 0xE8 },
		{ 0x007A6DE8, 0x007A7060, reinterpret_cast<std::uintptr_t>(&ObserveCreation<1>), 0, 0xE8 },
		{ 0x007A70B2, 0x005DE450, reinterpret_cast<std::uintptr_t>(&ObserveLookup), 0, 0xE8 },
		{ 0x0059FE30, 0x02409C80, reinterpret_cast<std::uintptr_t>(&ObserveAssociation), 0, 0xE9 },
		{ 0x0059FE40, 0x02409E10, reinterpret_cast<std::uintptr_t>(&ObserveRetirement), 0, 0xE9 },
		{ 0x007A4C56, 0x007A4CC0, reinterpret_cast<std::uintptr_t>(&ObserveDispatch), 0, 0xE8 }
	}};
	bool Owned(const Site& a_site, std::uintptr_t a_base) noexcept
	{
		__try {
			if (!a_site.stub) return false;
			std::array<std::uint8_t, 5> branch{};
			std::memcpy(branch.data(), reinterpret_cast<const void*>(a_base + a_site.rva), 5);
			if (branch[0] != a_site.opcode) return false;
			branch[0] = 0xE8; // Normalize a local copy only, after checking E8/E9.
			return ClaimablePOIs::MatchesOwnedGuardCall(branch, a_base + a_site.rva,
				std::span<const std::uint8_t, 14>(reinterpret_cast<const std::uint8_t*>(a_site.stub), 14), a_site.stub, a_site.hook);
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	bool RestoreSites(std::uintptr_t a_base) noexcept
	{
		__try {
			bool restored = true;
			for (const auto& site : s_sites) {
				if (site.original[0] == 0) continue; // Allocation failed before writes.
				if (Owned(site, a_base)) restored = REL::WriteSafeData(a_base + site.rva, site.original) && restored;
				else if (std::memcmp(reinterpret_cast<const void*>(a_base + site.rva), site.original.data(), 5) != 0) restored = false;
			}
			const auto slot = *reinterpret_cast<const std::uintptr_t*>(a_base + kContentSlot);
			if (slot == reinterpret_cast<std::uintptr_t>(&ObserveContent))
				restored = REL::WriteSafeData(a_base + kContentSlot, a_base + 0x007C3FB0) && restored;
			else if (slot != a_base + 0x007C3FB0) restored = false;
			return restored;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	bool Verify(std::uintptr_t a_base, bool a_owned) noexcept
	{
		__try {
			const auto expectedSlot = a_owned ? reinterpret_cast<std::uintptr_t>(&ObserveContent) : a_base + 0x007C3FB0;
			if (*reinterpret_cast<const std::uintptr_t*>(a_base + kContentSlot) != expectedSlot ||
				*reinterpret_cast<const std::uintptr_t*>(a_base + kContentVtable - 8) != a_base + 0x04FD9100 ||
				!ClaimablePOIs::PCMOverlayHook::VerifyOwnedContentQueryCall(a_base + 0x007C4068)) return false;
			constexpr char contentType[] = ".?AVBGSPlanetContentManagerContentNode@@";
			if (std::memcmp(reinterpret_cast<const void*>(a_base + 0x0598B238), contentType, sizeof(contentType)) != 0) return false;
			for (const auto& gate : kGates) {
				const auto* code = reinterpret_cast<const std::uint8_t*>(a_base + gate.rva);
				std::uint64_t hash = 14695981039346656037ull;
				for (std::size_t i = 0; i < gate.size; ++i) hash = (hash ^ code[i]) * 1099511628211ull;
				if (hash != gate.hash) return false;
			}
			for (const auto& site : s_sites) {
				std::array<std::uint8_t, 5> branch{};
				std::memcpy(branch.data(), reinterpret_cast<const void*>(a_base + site.rva), 5);
				if (branch[0] != site.opcode) return false;
				if (a_owned) {
					if (!Owned(site, a_base)) return false;
				} else {
					std::int32_t displacement{};
					std::memcpy(&displacement, branch.data() + 1, 4);
					if (a_base + site.rva + 5 + displacement != a_base + site.target) return false;
				}
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	void LogEvents(std::span<const PD::Event> a_events, std::uint64_t a_epoch, std::uint64_t a_token)
	{
		constexpr std::array names{ "create-begin", "lookup", "create-end", "associate-return", "retire-begin", "retire-end",
			"content-begin", "content-end", "dispatch-begin", "inventory-entry", "dispatch-end" };
		for (const auto& event : a_events) {
			REX::INFO("MiniSelection-v1 event epoch={} token={} interval={} sequence={} invocation={} thread={} kind={} site={} key=({:08X},{:08X},{:08X}) ref={:08X} base={:08X} type={} pos=({:.2f},{:.2f},{:.2f}) result={} parentInvocation={} node={:08X} stamp={} flags=0x{:08X} category={} mask=0x{:02X} seed={} block={} entry={} count={} xyOnly={} worldProven=false loaded3DProven=false readOnly=true",
				a_epoch, a_token, event.interval, event.sequence, event.invocation, event.thread, names[static_cast<std::size_t>(event.kind)], event.site,
				event.key[0], event.key[1], event.key[2], event.reference, event.base, event.type,
				event.position[0], event.position[1], event.position[2], event.result, event.parentInvocation, event.node, event.stamp,
				event.flags, event.category, event.mask, event.seed, event.block, event.entry, event.count, PD::Planar(event.kind));
		}
	}
}

namespace ClaimablePOIs::ProceduralLifecycleDiagnostics
{
	bool Install() noexcept
	{
		const auto module = REX::FModule::GetExecutingModule();
		const auto base = module.GetBaseAddress();
		if (module.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244 ||
			REL::ID(51952).address() != base + 0x007A2A10 || REL::ID(51969).address() != base + 0x007A6700 ||
			REL::ID(51971).address() != base + 0x007A7060 || REL::ID(47402).address() != base + 0x005DE450 ||
			REL::ID(126719).address() != base + 0x02409C80 || REL::ID(126720).address() != base + 0x02409E10 ||
			REL::ID(51957).address() != base + 0x007A4B90 || REL::ID(51958).address() != base + 0x007A4CC0 ||
			REL::ID(51954).address() != base + 0x007A38C0 || REL::ID(52267).address() != base + 0x007C3FB0 ||
			REL::ID(403095).address() != base + kContentVtable ||
			!Verify(base, s_installed)) return false;
		if (s_installed) return true;
		try {
			s_runtimeBase = base;
			s_create = reinterpret_cast<Create>(base + 0x007A7060);
			s_lookup = reinterpret_cast<Lookup>(base + 0x005DE450);
			s_associate = reinterpret_cast<Associate>(base + 0x02409C80);
			s_retire = reinterpret_cast<Retire>(base + 0x02409E10);
			s_content = reinterpret_cast<Content>(base + 0x007C3FB0);
			s_dispatch = reinterpret_cast<Dispatch>(base + 0x007A4CC0);
			auto& trampoline = REL::GetTrampoline();
			// Preflight every boundary and allocate every stub before code writes.
			for (auto& site : s_sites) {
				std::memcpy(site.original.data(), reinterpret_cast<const void*>(base + site.rva), 5);
				site.stub = trampoline.allocate_branch5(site.hook);
			}
			for (const auto& site : s_sites) {
				if (site.opcode == 0xE8) trampoline.write_call<5>(base + site.rva, site.hook);
				else trampoline.write_jmp<5>(base + site.rva, site.hook);
			}
			if (!REL::WriteSafeData(base + kContentSlot, reinterpret_cast<std::uintptr_t>(&ObserveContent))) {
				const bool restored = RestoreSites(base) && Verify(base, false);
				REX::ERROR("MiniSelection-v1 vtable installation failed restored={} stopTrial=true", restored);
				return false;
			}
			s_installed = Verify(base, true);
			if (!s_installed) {
				const bool restored = RestoreSites(base) && Verify(base, false);
				REX::ERROR("MiniSelection-v1 installation verification failed restored={} stopTrial=true", restored);
				return false;
			}
			REX::INFO("MiniSelection-v1 installed={} hooks=7 inactiveUntilCensus=true engineResultsPreserved=true", s_installed);
			return s_installed;
		} catch (...) {
			const bool restored = RestoreSites(base) && Verify(base, false);
			REX::ERROR("MiniSelection-v1 installation failed restored={} stopTrial=true", restored);
			return false;
		}
	}
	void Reset(std::uint64_t a_epoch) noexcept
	{
		s_activeToken.store(0, std::memory_order_release);
		std::lock_guard lock(s_mutex);
		s_session.Reset(a_epoch);
		s_publishedKeys = s_publishedReferences = 0;
		for (auto& word : s_keyHints) word.store(0, std::memory_order_release);
		for (auto& word : s_referenceHints) word.store(0, std::memory_order_release);
	}
	void Abort() noexcept
	{
		s_activeToken.store(0, std::memory_order_release);
		try {
			std::vector<Event> events;
			events.reserve(kEventCap);
			std::uint64_t epoch{}, token{};
			{
				std::lock_guard lock(s_mutex);
				epoch = s_session.epoch;
				token = s_session.token;
				if (!token) return;
				events.assign(s_session.events.begin(), s_session.events.begin() + s_session.eventCount);
				s_session.eventCount = 0;
				s_session.fault = true;
			}
			LogEvents(events, epoch, token);
			REX::WARN("MiniSelection-v1 aborted epoch={} token={} events={} complete=false stopTrial=true", epoch, token, events.size());
		} catch (...) { REX::ERROR("MiniSelection-v1 abort logging failed; stop trial and preserve logs"); }
	}
	bool Checkpoint(std::uint64_t a_epoch, std::uint32_t a_world, const Position& a_origin,
		std::uint32_t a_checkpoint, std::span<const Seed> a_seeds) noexcept
	{
		try {
			if (!s_installed || !Verify(REX::FModule::GetExecutingModule().GetBaseAddress(), true)) return false;
			std::vector<Event> events;
			events.reserve(kEventCap);
			std::array<std::uint64_t, static_cast<std::size_t>(Kind::Count)> calls{};
			std::uint64_t token{};
			std::size_t keys{}, references{};
			bool complete = true;
			{
				std::lock_guard lock(s_mutex);
				if (a_checkpoint == 0) {
					if (s_activeToken.load(std::memory_order_acquire) || s_session.epoch != a_epoch || !a_world) return false;
					s_session.world = a_world;
					s_session.origin = a_origin;
					if (!s_session.InRange(a_origin)) return false;
					for (std::size_t i = 0; i < a_origin.size(); ++i) s_origin[i].store(a_origin[i], std::memory_order_release);
					s_session.token = ++s_nextToken;
				} else if (a_checkpoint > 2 || s_session.epoch != a_epoch || s_session.world != a_world ||
					s_session.interval + 1 != a_checkpoint || s_activeToken.load(std::memory_order_acquire) != s_session.token) return false;
				token = s_session.token;
				for (const auto& seed : a_seeds) complete = s_session.Add(seed) && complete;
				Publish();
				complete = complete && !s_session.fault && s_session.keyCount != 0 && s_session.referenceCount != 0;
				events.assign(s_session.events.begin(), s_session.events.begin() + s_session.eventCount);
				calls = s_session.calls;
				keys = s_session.keyCount;
				references = s_session.referenceCount;
				s_session.eventCount = 0;
				s_session.calls.fill(0);
				s_session.interval = a_checkpoint;
				complete = complete && s_faultToken.load(std::memory_order_acquire) != token;
				s_activeToken.store(complete && a_checkpoint < 2 ? token : 0, std::memory_order_release);
			}
			// No logger or engine call while holding the diagnostic or manager lock.
			LogEvents(events, a_epoch, token);
			complete = complete && s_faultToken.load(std::memory_order_acquire) != token;
			if (!complete) s_activeToken.store(0, std::memory_order_release);
			REX::INFO("MiniSelection-v1 checkpoint epoch={} token={} checkpoint={} anchorWorld={:08X} origin=({:.2f},{:.2f},{:.2f}) events={} keys={} references={} filteredCalls=({},{},{},{},{},{}) selectionCalls=({},{},{},{},{}) complete={} armed={} spatialCandidates=true readOnly=true",
				a_epoch, token, a_checkpoint, a_world, a_origin[0], a_origin[1], a_origin[2], events.size(), keys, references,
				calls[0], calls[1], calls[2], calls[3], calls[4], calls[5], calls[6], calls[7], calls[8], calls[9], calls[10], complete, complete && a_checkpoint < 2);
			if (!complete) REX::WARN("MiniSelection-v1 incomplete: bounded buffer/identity limit, read failure or contention; stop trial");
			return complete;
		} catch (...) { Abort(); return false; }
	}
}
