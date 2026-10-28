#include "pch.h"
#include "VanillaOverlayHook.h"
#include "VanillaOverlayPolicy.h"
#include "GuardedCallValidation.h"
#include "REL/Relocation.h"
#include "REL/Trampoline.h"
#include "REX/FModule.h"
#include "SFSE/Version.h"
#include <Windows.h>
#undef ERROR
#include <array>
#include <atomic>
#include <mutex>

namespace
{
	// 1.16.244: workshop root registration, not scanner permission or PCM.
	// Register(ref) finds the overlay at ref XY. An existing overlay is reused
	// at 17E236B..23A7; a missing lookup takes vanilla's complete new-overlay
	// path at 17E23A9..23BE. Only the in-flight initial claim may choose that path.
	using Register_t = void (*)(RE::TESObjectREFR*);
	using Lookup_t = RE::TESWorldSpace** (*)(void*, std::uint32_t, const std::int32_t*);
	using Create_t = std::uint32_t (*)(const RE::NiPoint3*);
	Register_t s_register{};
	Lookup_t s_lookup{};
	Create_t s_create{};
	std::mutex s_claimMutex;
	ClaimablePOIs::InitialOverlayClaim s_claim{};
	ClaimablePOIs::InitialOverlayBinding s_binding{};
	std::uint64_t s_nextToken{};
	std::atomic<std::uint64_t> s_activeToken{};
	std::atomic<std::uint32_t> s_registrationLogs{};
	bool s_installed{};

	struct Registration
	{
		ClaimablePOIs::InitialOverlayClaim claim{};
		std::uint32_t beacon{}, selected{}, created{};
		bool separated{};
	};
	// Stack-owned scope; no engine reference/pointer survives the vanilla call.
	thread_local Registration* s_registration{};
	struct RegistrationScope
	{
		Registration* previous{ s_registration };
		explicit RegistrationScope(Registration* a_current) { s_registration = a_current; }
		~RegistrationScope() { s_registration = previous; }
	};

	bool ReadClaim(RE::TESObjectREFR* a_helper, RE::TESWorldSpace* a_overlay,
		float a_radius, ClaimablePOIs::InitialOverlayClaim& a_claim) noexcept
	{
		__try {
			if (!a_helper || !a_overlay || !a_overlay->Is(RE::FormType::kWRLD)) return false;
			auto* world = a_helper->GetParentWorldSpace();
			if (!world) return false;
			const auto pos = a_helper->GetPosition();
			a_claim = { 1, a_helper->formID, world->formID, a_overlay->formID, pos.x, pos.y, pos.z, a_radius };
			return ClaimablePOIs::MatchesInitialOverlayClaim(a_claim, 0x002C60E9, world->formID, pos.x, pos.y, pos.z);
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool RawReadBeacon(RE::TESObjectREFR* a_beacon, Registration& a_registration)
	{
			if (!a_beacon) return false;
			const auto base = a_beacon->GetBaseObject();
			auto* world = a_beacon->GetParentWorldSpace();
			if (!base || !world) return false;
			const auto pos = a_beacon->GetPosition();
			a_registration.beacon = a_beacon->formID;
			return ClaimablePOIs::MatchesInitialOverlayClaim(a_registration.claim, base->formID, world->formID, pos.x, pos.y, pos.z);
	}

	bool ReadBeacon(RE::TESObjectREFR* a_beacon, Registration& a_registration) noexcept
	{
		__try { return RawReadBeacon(a_beacon, a_registration); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	std::uint32_t ReadOverlayID(RE::TESWorldSpace** a_result) noexcept
	{
		__try {
			return a_result && *a_result && (*a_result)->Is(RE::FormType::kWRLD) ? (*a_result)->formID : 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	void RegisterBeacon(RE::TESObjectREFR* a_beacon)
	{
		Registration registration{};
		{
			std::lock_guard lock(s_claimMutex);
			registration.claim = s_claim;
		}
		const bool eligible = ReadBeacon(a_beacon, registration) &&
			registration.claim.token == s_activeToken.load(std::memory_order_acquire);
		RegistrationScope scope(eligible ? &registration : nullptr);
		s_register(a_beacon); // Exactly once, including unrelated vanilla registrations.
		if (eligible) {
			std::lock_guard lock(s_claimMutex);
			if (s_claim.token == registration.claim.token && s_activeToken.load(std::memory_order_acquire) == s_claim.token) {
				// Publication is independent of the diagnostic budget. A later failed
				// registration replaces success, never inherits it from another beacon.
				s_binding = { registration.claim.token, registration.claim.helper, registration.beacon,
					registration.claim.overlay, registration.selected, registration.created };
			}
		}
		if (eligible && s_registrationLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
			const bool overlayBound = registration.selected != 0 && registration.selected == registration.claim.overlay;
			const bool distinct = registration.created != 0 && registration.created != 0xFFFFFFFF &&
				registration.created != registration.claim.overlay;
			REX::INFO("VanillaOverlayBinding-v1 registration helper={:08X} beacon={:08X} originalOverlay={:08X} selectedOverlay={:08X} bound={} reusePrevented={} createdOverlay={:08X} distinct={}",
				registration.claim.helper, registration.beacon, registration.claim.overlay, registration.selected,
				overlayBound, registration.separated, registration.created, distinct);
			if (!overlayBound && (!registration.separated || !distinct)) {
				REX::ERROR("VanillaOverlayBinding-v1: overlay binding was not verified; stop trial and preserve logs");
			}
		}
	}

	RE::TESWorldSpace** LookupOverlay(void* a_holder, std::uint32_t a_parentWorld, const std::int32_t* a_cellXY)
	{
		// AD30A0 returns a borrowed map-record address. Never edit/free that record.
		// The caller owns its manager read lock and releases it on both branches.
		auto** result = s_lookup(a_holder, a_parentWorld, a_cellXY);
		if (s_registration) {
			s_registration->selected = ReadOverlayID(result);
			if (ClaimablePOIs::SelectSeparateVanillaOverlay(s_registration->claim,
				s_activeToken.load(std::memory_order_acquire), s_registration->selected, a_parentWorld)) {
				s_registration->separated = true;
				return nullptr; // Vanilla then creates/binds/registers a real separate overlay.
			}
		}
		return result;
	}

	std::uint32_t CreateOverlay(const RE::NiPoint3* a_position)
	{
		const auto result = s_create(a_position);
		if (s_registration) s_registration->created = result;
		return result;
	}

	struct CallSite { std::uintptr_t rva, target, hook, stub{}; };
	std::array<CallSite, 3> s_calls{{
		{ 0x021B3CDC, 0x017E2180, reinterpret_cast<std::uintptr_t>(&RegisterBeacon) },
		{ 0x017E22BC, 0x00AD30A0, reinterpret_cast<std::uintptr_t>(&LookupOverlay) },
		{ 0x017E23B9, 0x017E0F40, reinterpret_cast<std::uintptr_t>(&CreateOverlay) }
	}};
	struct CodeGate { std::uintptr_t rva; std::size_t size; std::uint64_t hash; };
	// FNV-1a of the complete registration body excluding the two owned calls,
	// the root caller's argument move, and the complete lookup/creator bodies.
	constexpr std::array<CodeGate, 6> kCodeGates{{
		{ 0x017E2180, 0x13C, 0x43D8AD10CC343F80ull },
		{ 0x017E22C1, 0xF8, 0x91F431C83AF3FFD8ull },
		{ 0x017E23BE, 0x46, 0x027E4163E04EA353ull },
		{ 0x021B3CD9, 0x3, 0x3EC0C519C71557C3ull },
		{ 0x00AD30A0, 0xF8, 0x1C7B84FCDD04FD57ull },
		{ 0x017E0F40, 0x26B, 0x1A172D530CC7AB2Aull }
	}};

	bool VerifyLayout(std::uintptr_t a_base, bool a_owned) noexcept
	{
		__try {
			for (const auto& gate : kCodeGates) {
				const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_base + gate.rva);
				std::uint64_t hash = 14695981039346656037ull;
				for (std::size_t i = 0; i < gate.size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
				if (hash != gate.hash) return false;
			}
			for (const auto& call : s_calls) {
				const auto address = a_base + call.rva;
				const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
				if (a_owned) {
					if (!ClaimablePOIs::MatchesOwnedGuardCall(std::span<const std::uint8_t, 5>(bytes, 5), address,
						std::span<const std::uint8_t, 14>(reinterpret_cast<const std::uint8_t*>(call.stub), 14), call.stub, call.hook)) return false;
				} else {
					std::int32_t displacement{};
					std::memcpy(&displacement, bytes + 1, 4);
					if (bytes[0] != 0xE8 || address + 5 + displacement != a_base + call.target) return false;
				}
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
}

namespace ClaimablePOIs::VanillaOverlayHook
{
	bool Install() noexcept
	{
		const auto module = REX::FModule::GetExecutingModule();
		const auto base = module.GetBaseAddress();
		if (module.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244 ||
			REL::ID(97911).address() != base + 0x017E2180 || REL::ID(62337).address() != base + 0x00AD30A0 ||
			REL::ID(97909).address() != base + 0x017E0F40 || !VerifyLayout(base, s_installed)) return false;
		if (s_installed) return true;
		s_register = reinterpret_cast<Register_t>(base + 0x017E2180);
		s_lookup = reinterpret_cast<Lookup_t>(base + 0x00AD30A0);
		s_create = reinterpret_cast<Create_t>(base + 0x017E0F40);
		auto& trampoline = REL::GetTrampoline();
		for (auto& call : s_calls) call.stub = trampoline.allocate_branch5(call.hook);
		for (const auto& call : s_calls) trampoline.write_call<5>(base + call.rva, call.hook);
		s_installed = true;
		const bool verified = VerifyLayout(base, true);
		REX::INFO("SeparateVanillaOverlay-v1 installed ownedCallsVerified={}", verified);
		return verified;
	}

	bool ArmInitialClaim(RE::TESObjectREFR* a_helper, RE::TESWorldSpace* a_overlay, float a_radius) noexcept
	{
		if (!s_installed || !VerifyLayout(REX::FModule::GetExecutingModule().GetBaseAddress(), true)) return false;
		InitialOverlayClaim claim{};
		if (!ReadClaim(a_helper, a_overlay, a_radius, claim)) return false;
		std::lock_guard lock(s_claimMutex);
		claim.token = ++s_nextToken;
		s_claim = claim;
		s_binding = {};
		s_registrationLogs.store(0, std::memory_order_relaxed);
		s_activeToken.store(claim.token, std::memory_order_release);
		REX::INFO("SeparateVanillaOverlay-v1 armed token={} helper={:08X} cellWorld={:08X} originalOverlay={:08X} radius={:.1f}",
			claim.token, claim.helper, claim.cellWorld, claim.overlay, claim.radius);
		return true;
	}

	void ClearInitialClaim() noexcept
	{
		std::lock_guard lock(s_claimMutex);
		s_activeToken.store(0, std::memory_order_release);
		s_claim = {};
		s_binding = {};
	}

	bool IsInitialClaimBound(std::uint32_t a_helper, std::uint32_t a_beacon) noexcept
	{
		if (!s_installed || !VerifyLayout(REX::FModule::GetExecutingModule().GetBaseAddress(), true)) return false;
		std::lock_guard lock(s_claimMutex);
		return HasVerifiedInitialBinding(s_binding, s_activeToken.load(std::memory_order_acquire), a_helper, a_beacon);
	}
}
