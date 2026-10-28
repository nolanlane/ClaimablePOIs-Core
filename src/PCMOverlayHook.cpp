#include "pch.h"

#include "PCMOverlayHook.h"
#include "Forms.h"
#include "Diagnostics.h"
#include "GuardedCallValidation.h"
#include "TeardownProtection.h"

#include "SFSE/API.h"
#include "SFSE/Version.h"

#include "REX/FModule.h"
#include "REL/Relocation.h"
#include "REL/Trampoline.h"

#include <Windows.h>

#undef ERROR

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstring>
#include <excpt.h>
#include <filesystem>
#include <fstream>
#include <shared_mutex>
#include <vector>

namespace
{
	constexpr std::uint32_t kLocalClaimedBeaconsList{ 0x805 };
	constexpr std::uint32_t kLocalClaimRadius{ 0x807 };

	[[nodiscard]] RE::TESFormID ComposeLocalID(const RE::TESFile& a_file, std::uint32_t a_localID) noexcept
	{
		const auto& index = a_file.fileIndex;
		if (a_file.fileFlags.any(RE::TESFile::Flags::kSmall)) {
			return 0xFE000000u | (static_cast<std::uint32_t>(index.smallIndex) << 12) | (a_localID & 0x00000FFFu);
		}
		if (a_file.fileFlags.any(RE::TESFile::Flags::kMedium)) {
			return 0xFD000000u | (static_cast<std::uint32_t>(index.mediumIndex) << 16) | (a_localID & 0x0000FFFFu);
		}
		return (static_cast<std::uint32_t>(index.fullIndex) << 24) | (a_localID & 0x00FFFFFFu);
	}

	// Starfield 1.16.244.0 Relocation Targets
	// Worldspace/planet query (historically mislabelled occupancy): REL::ID 62351 / RVA 0x00AD3B20
	constexpr REL::ID kWorldspaceQueryID{ 62351 };

	// BGSPlanetContentManagerContentNode evaluator: ID 52267 / RVA 0x007C3FB0.
	// Pinned primary-vtable index 0x6B (byte offset 0x358).
	// Call site at offset 0xB8: RVA 0x007C4068
	constexpr REL::ID kContentNodeSlot110ID{ 52267 };
	constexpr std::uintptr_t kContentNodeCallOffset{ 0xB8 };

	// BGSPlanetContentManagerBranchNode::Placement: REL::ID 52192 / RVA 0x007BE0E0
	// Call site at offset 0xBB: RVA 0x007BE19B
	constexpr REL::ID kBranchNodePlacementID{ 52192 };
	constexpr std::uintptr_t kBranchNodeCallOffset{ 0xBB };

	// GetOverlayForCell_ThreadSafe: REL::ID 62345 / RVA 0x00AD3610
	constexpr REL::ID kGetOverlayForCell_ThreadSafeID{ 62345 };

	// WorkshopMode call sites calling GetOverlayForCell_ThreadSafe:
	// Function 1: REL::ID 73857 / RVA 0x00F47DE0
	//   Call site 1 at offset 0x18C: RVA 0x00F47F6C (faulting call site in minidump)
	//   Call site 2 at offset 0x2D6: RVA 0x00F480B6
	// Function 2: REL::ID 73788 / RVA 0x00F40EA0
	//   Call site 3 at offset 0x7B:  RVA 0x00F40F1B
	constexpr REL::ID kWorkshopModeFunc1_ID{ 73857 };
	constexpr std::uintptr_t kWorkshopModeCall1_Offset{ 0x18C };
	constexpr std::uintptr_t kWorkshopModeCall2_Offset{ 0x2D6 };

	constexpr REL::ID kWorkshopModeFunc2_ID{ 73788 };
	constexpr std::uintptr_t kWorkshopModeCall3_Offset{ 0x7B };

	using WorldspaceQuery_t = bool (*)(RE::TESWorldSpace*, std::uint32_t*, void*);
	WorldspaceQuery_t s_originalWorldspaceQuery{ nullptr };

	using GetOverlayForCell_ThreadSafe_t = void* (*)(RE::TESObjectCELL*);
	GetOverlayForCell_ThreadSafe_t s_originalGetOverlayForCell_ThreadSafe{ nullptr };

	std::shared_mutex s_registryMutex;
	std::vector<ClaimablePOIs::PCMOverlayHook::ClaimedPOIRecord> s_claimedSites;

	std::atomic<bool> s_hookInstalled{ false };
	std::uintptr_t s_overlayResolverCallSite{ 0 };
	std::uintptr_t s_overlayResolverStub{ 0 };
	std::uintptr_t s_contentQueryCallSite{ 0 }, s_contentQueryStub{ 0 };
	std::atomic<std::int64_t> s_lastNullCellLogTickMs{ 0 };

	[[nodiscard]] bool VerifyCallInstruction(std::uintptr_t a_callSite, std::uintptr_t a_expectedTarget) noexcept
	{
		__try {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_callSite);
			if (bytes[0] != 0xE8) {
				REX::ERROR("PCMOverlayHook: byte gate failed at 0x{:016X}; opcode=0x{:02X}, expected 0xE8",
					a_callSite, bytes[0]);
				return false;
			}
			std::int32_t disp = 0;
			std::memcpy(&disp, bytes + 1, sizeof(disp));
			const auto target = a_callSite + 5 + disp;
			if (target != a_expectedTarget) {
				REX::ERROR("PCMOverlayHook: target gate failed at 0x{:016X}; target=0x{:016X}, expected 0xE8 to 0x{:016X}",
					a_callSite, target, a_expectedTarget);
				return false;
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			REX::ERROR("PCMOverlayHook: exception reading memory at 0x{:016X}", a_callSite);
			return false;
		}
	}

	// These hooks observe the verified three-argument worldspace query only.
	// The scalar is not established as an occupancy count; the 16-byte output
	// follows the planet-coordinate producer (two doubles), not local XYZ.
	struct QueryCounters
	{
		std::uint64_t calls{ 0 };
		std::uint64_t resolved{ 0 };
		std::uint64_t nonzeroScalar{ 0 };
		std::uint64_t exactWorldMatches{ 0 };
		std::array<std::uint32_t, 8> samples{};
	};
	std::mutex s_queryMutex;
	std::array<QueryCounters, 2> s_queryCounters{};
	std::uint64_t s_queryEpoch{ 0 };
	std::atomic<std::uint64_t> s_queryReadFailures{ 0 };

	void RawObserveQuery(std::size_t a_site, RE::TESWorldSpace* a_world,
		const std::uint32_t* a_scalar, const void* a_coordinates, bool a_result)
	{
		const auto worldID = a_world ? a_world->formID : 0;
		const auto scalar = a_scalar ? *a_scalar : 0;
		std::array<double, 2> angular{};
		if (a_coordinates) {
			std::memcpy(angular.data(), a_coordinates, sizeof(angular));
		}
		RE::TESFormID matchedBeacon = 0;
		std::size_t sites = 0;
		{
			std::shared_lock lock(s_registryMutex);
			sites = s_claimedSites.size();
			for (const auto& site : s_claimedSites) {
				if (worldID != 0 && site.worldspaceID == worldID) {
					matchedBeacon = site.beaconRefID;
					break;
				}
			}
		}
		std::lock_guard lock(s_queryMutex);
		auto& counters = s_queryCounters[a_site];
		++counters.calls;
		counters.resolved += a_result ? 1 : 0;
		counters.nonzeroScalar += scalar != 0 ? 1 : 0;
		counters.exactWorldMatches += matchedBeacon != 0 ? 1 : 0;
		const std::size_t bucket = (sites != 0 ? 4u : 0u) | (matchedBeacon != 0 ? 2u : 0u) | (a_result ? 1u : 0u);
		if (counters.samples[bucket] >= 8) {
			return;
		}
		++counters.samples[bucket];
		REX::INFO("PCMQueryAudit-v1 sample epoch={} site={} world={:08X} result={} scalar={:08X} angular=({:.9f},{:.9f}) outputPresent={} registrySites={} exactWorldBeacon={:08X} bucket={} sample={} passthrough=true",
			s_queryEpoch, a_site == 0 ? "content" : "branch", worldID, a_result, scalar,
			angular[0], angular[1], a_scalar && a_coordinates, sites, matchedBeacon, bucket, counters.samples[bucket]);
	}

	void ObserveQuery(std::size_t a_site, RE::TESWorldSpace* a_world,
		const std::uint32_t* a_scalar, const void* a_coordinates, bool a_result) noexcept
	{
		if (!ClaimablePOIs::Diagnostics::Enabled()) return;
		__try {
			RawObserveQuery(a_site, a_world, a_scalar, a_coordinates, a_result);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			s_queryReadFailures.fetch_add(1, std::memory_order_relaxed);
		}
	}

	template <std::size_t Site>
	bool HookedWorldspaceQuery(RE::TESWorldSpace* a_world, std::uint32_t* a_scalar, void* a_coordinates)
	{
		// Do not catch engine exceptions or synthesize a result. Observation cannot
		// modify the return, scalar, or coordinate outputs used by either caller.
		const bool result = s_originalWorldspaceQuery(a_world, a_scalar, a_coordinates);
		ObserveQuery(Site, a_world, a_scalar, a_coordinates, result);
		return result;
	}

	void* RawGetOverlayForCell_ThreadSafe(RE::TESObjectCELL* a_cell) noexcept
	{
		if (!a_cell) {
			const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			const auto lastMs = s_lastNullCellLogTickMs.load(std::memory_order_relaxed);
			if (nowMs - lastMs >= 2000) {
				s_lastNullCellLogTickMs.store(nowMs, std::memory_order_relaxed);
				REX::INFO("PCMOverlayHook: guarded GetOverlayForCell_ThreadSafe from null cell pointer");
			}
			return nullptr;
		}
		if (!s_originalGetOverlayForCell_ThreadSafe) {
			return nullptr;
		}
		return s_originalGetOverlayForCell_ThreadSafe(a_cell);
	}

	void* HookedGetOverlayForCell_ThreadSafe(RE::TESObjectCELL* a_cell) noexcept
	{
		__try {
			return RawGetOverlayForCell_ThreadSafe(a_cell);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			REX::ERROR("PCMOverlayHook: exception in HookedGetOverlayForCell_ThreadSafe");
			return nullptr;
		}
	}

	struct PersistedRecord
	{
		RE::TESFormID beaconRefID{ 0 };
		RE::TESFormID worldspaceID{ 0 };
		RE::TESFormID parentWorldID{ 0 };
		RE::TESFormID locationID{ 0 };
		float posX{ 0.0f };
		float posY{ 0.0f };
		float posZ{ 0.0f };
		float radius{ 250.0f };
	};

	constexpr std::uint32_t kPersistMagic{ 0x494F5043 };  // 'CPOI'
	constexpr std::uint32_t kPersistVersion{ 1 };

	[[nodiscard]] std::filesystem::path GetSitePersistencePath()
	{
		const auto modulePath = std::filesystem::path{ REX::FModule::GetCurrentModule().GetFileName() };
		return modulePath.parent_path() / "ClaimablePOIs_claimed_sites.bin";
	}

	void SavePersistentSitesLocked() noexcept
	{
		try {
			const auto path = GetSitePersistencePath();
			std::ofstream stream(path, std::ios::out | std::ios::binary | std::ios::trunc);
			if (!stream.is_open()) {
				return;
			}
			const std::uint32_t magic = kPersistMagic;
			const std::uint32_t version = kPersistVersion;
			const std::uint32_t count = static_cast<std::uint32_t>(s_claimedSites.size());
			stream.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
			stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
			stream.write(reinterpret_cast<const char*>(&count), sizeof(count));
			for (const auto& site : s_claimedSites) {
				PersistedRecord rec{
					.beaconRefID = site.beaconRefID,
					.worldspaceID = site.worldspaceID,
					.parentWorldID = site.parentWorldID,
					.locationID = site.locationID,
					.posX = site.posX,
					.posY = site.posY,
					.posZ = site.posZ,
					.radius = site.radius
				};
				stream.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
			}
		} catch (const std::exception& e) {
			REX::ERROR("PCMOverlayHook: exception during SavePersistentSitesLocked: {}", e.what());
		} catch (...) {
			REX::ERROR("PCMOverlayHook: unknown exception during SavePersistentSitesLocked");
		}
	}

	[[nodiscard]] std::vector<PersistedRecord> LoadPersistentSites() noexcept
	{
		std::vector<PersistedRecord> records;
		try {
			const auto path = GetSitePersistencePath();
			if (!std::filesystem::exists(path)) {
				return records;
			}
			std::ifstream stream(path, std::ios::in | std::ios::binary);
			if (!stream.is_open()) {
				return records;
			}
			std::uint32_t magic = 0;
			std::uint32_t version = 0;
			std::uint32_t count = 0;
			stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
			stream.read(reinterpret_cast<char*>(&version), sizeof(version));
			stream.read(reinterpret_cast<char*>(&count), sizeof(count));
			if (magic != kPersistMagic || version != kPersistVersion || count > 1024) {
				return records;
			}
			records.resize(count);
			stream.read(reinterpret_cast<char*>(records.data()), count * sizeof(PersistedRecord));
		} catch (const std::exception& e) {
			REX::ERROR("PCMOverlayHook: exception during LoadPersistentSites: {}", e.what());
			records.clear();
		} catch (...) {
			REX::ERROR("PCMOverlayHook: unknown exception during LoadPersistentSites");
			records.clear();
		}
		return records;
	}

	void RawSyncFromSave() noexcept
	{
		const auto* file = ClaimablePOIs::Forms::FindPluginFile();
		if (!file) {
			REX::WARN("PCMOverlayHook: SyncFromSave cannot resolve plugin file");
			return;
		}

		const auto beaconsListID = ComposeLocalID(*file, kLocalClaimedBeaconsList);
		auto* beaconsList = RE::TESForm::LookupByID<RE::BGSListForm>(beaconsListID);
		if (!beaconsList) {
			REX::WARN("PCMOverlayHook: CPOI_ClaimedBeacons (0x{:08X}) not found", beaconsListID);
			return;
		}

		float radius = 250.0f;
		const auto radiusID = ComposeLocalID(*file, kLocalClaimRadius);
		auto* radiusGlobal = RE::TESForm::LookupByID<RE::TESGlobal>(radiusID);
		if (radiusGlobal && radiusGlobal->value > 0.0f) {
			radius = radiusGlobal->value + 50.0f;
		}

		// Collect all beacon formIDs authorized by this save's CPOI_ClaimedBeacons list.
		std::vector<RE::TESFormID> authorizedBeaconIDs;
        if (!ClaimablePOIs::TeardownProtection::Snapshot(beaconsList,authorizedBeaconIDs)) {
            REX::ERROR("PCMOverlayHook: unreadable save-local beacon list; synchronization aborted");
            return;
        }

		// Load disk-cached records for fallback resolution of unloaded cells
		const auto diskRecords = LoadPersistentSites();

		std::vector<ClaimablePOIs::PCMOverlayHook::ClaimedPOIRecord> newSites;

		for (const auto beaconID : authorizedBeaconIDs) {
			auto* refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(beaconID);
			if (refr && refr->IsDeleted()) {
				continue;
			}

			// 1. Try resolving live worldspace from loaded reference
			RE::TESWorldSpace* world = refr ? refr->GetParentWorldSpace() : nullptr;
			if (world) {
				const auto pos = refr->GetPosition();
				auto* loc = refr->GetCurrentLocation();
				newSites.push_back(ClaimablePOIs::PCMOverlayHook::ClaimedPOIRecord{
					.worldspaceID = world->formID,
					.parentWorldID = world->parentWorld ? world->parentWorld->formID : 0,
					.beaconRefID = beaconID,
					.locationID = loc ? loc->formID : 0,
					.posX = pos.x,
					.posY = pos.y,
					.posZ = pos.z,
					.radius = radius
				});
				continue;
			}

			// 2. Beacon cell is unloaded. Check if existing in-memory registry already has verified data.
			bool resolved = false;
			{
				std::shared_lock lock(s_registryMutex);
				for (const auto& existing : s_claimedSites) {
					if (existing.beaconRefID == beaconID) {
						auto kept = existing;
						kept.radius = radius;
						if (refr) {
							const auto pos = refr->GetPosition();
							if (pos.x != 0.0f || pos.y != 0.0f || pos.z != 0.0f) {
								kept.posX = pos.x;
								kept.posY = pos.y;
								kept.posZ = pos.z;
							}
						}
						newSites.push_back(kept);
						resolved = true;
						break;
					}
				}
			}
			if (resolved) {
				continue;
			}

			// 3. Fall back to disk persistence (for clean process restarts)
			for (const auto& diskRec : diskRecords) {
				if (diskRec.beaconRefID == beaconID) {
					newSites.push_back(ClaimablePOIs::PCMOverlayHook::ClaimedPOIRecord{
						.worldspaceID = diskRec.worldspaceID,
						.parentWorldID = diskRec.parentWorldID,
						.beaconRefID = diskRec.beaconRefID,
						.locationID = diskRec.locationID,
						.posX = diskRec.posX,
						.posY = diskRec.posY,
						.posZ = diskRec.posZ,
						.radius = radius
					});
					resolved = true;
					break;
				}
			}

			if (!resolved) {
				REX::WARN("PCMOverlayHook: beacon 0x{:08X} in CPOI_ClaimedBeacons could not be spatially resolved (cell unloaded, no cached site)", beaconID);
			}
		}

		{
			std::unique_lock lock(s_registryMutex);
			s_claimedSites = std::move(newSites);
			SavePersistentSitesLocked();
		}

		REX::INFO("PCMOverlayHook: SyncFromSave synchronized {} claimed POI site(s) from {} authorized beacon(s)",
			s_claimedSites.size(), authorizedBeaconIDs.size());
	}

	void RawRegisterBeacon(RE::TESObjectREFR* a_beacon) noexcept
	{
		if (!a_beacon) {
			return;
		}

		RE::TESWorldSpace* world = a_beacon->GetParentWorldSpace();
		if (!world) {
			return;
		}

		float radius = 250.0f;
		const auto* file = ClaimablePOIs::Forms::FindPluginFile();
		if (file) {
			const auto radiusID = ComposeLocalID(*file, kLocalClaimRadius);
			auto* radiusGlobal = RE::TESForm::LookupByID<RE::TESGlobal>(radiusID);
			if (radiusGlobal && radiusGlobal->value > 0.0f) {
				radius = radiusGlobal->value + 50.0f;
			}
		}

		const auto pos = a_beacon->GetPosition();
		auto* loc = a_beacon->GetCurrentLocation();
		ClaimablePOIs::PCMOverlayHook::ClaimedPOIRecord record{
			.worldspaceID = world->formID,
			.parentWorldID = world->parentWorld ? world->parentWorld->formID : 0,
			.beaconRefID = a_beacon->formID,
			.locationID = loc ? loc->formID : 0,
			.posX = pos.x,
			.posY = pos.y,
			.posZ = pos.z,
			.radius = radius
		};

		{
			std::unique_lock lock(s_registryMutex);
			bool updated = false;
			for (auto& existing : s_claimedSites) {
				if (existing.beaconRefID == record.beaconRefID) {
					existing = record;
					updated = true;
					break;
				}
			}
			if (!updated) {
				s_claimedSites.push_back(record);
			}
			SavePersistentSitesLocked();
		}

		REX::INFO("PCMOverlayHook: registered claimed POI beacon 0x{:08X} world=0x{:08X} parentWorld=0x{:08X} pos=({:.1f}, {:.1f}, {:.1f}) radius={:.1f}",
			record.beaconRefID, record.worldspaceID, record.parentWorldID, record.posX, record.posY, record.posZ, record.radius);
	}
}

namespace ClaimablePOIs::PCMOverlayHook
{
	bool Install() noexcept
	{
		if (s_hookInstalled.load(std::memory_order_acquire)) {
			return true;
		}

		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			REX::ERROR("PCMOverlayHook: unsupported runtime {}; hook installation aborted",
				gameModule.GetFileVersion().string());
			return false;
		}

		const auto targetAddr = REL::ID(kWorldspaceQueryID).address();
		const auto callSite1 = REL::ID(kContentNodeSlot110ID).address() + kContentNodeCallOffset;
		const auto callSite2 = REL::ID(kBranchNodePlacementID).address() + kBranchNodeCallOffset;

		if (!VerifyCallInstruction(callSite1, targetAddr)) {
			REX::ERROR("PCMOverlayHook: ContentNode call site (0x{:016X}) verification failed", callSite1);
			return false;
		}
		if (!VerifyCallInstruction(callSite2, targetAddr)) {
			REX::ERROR("PCMOverlayHook: BranchNode call site (0x{:016X}) verification failed", callSite2);
			return false;
		}

		const auto getOverlayTarget = REL::ID(kGetOverlayForCell_ThreadSafeID).address();
		const auto workshopCall1 = REL::ID(kWorkshopModeFunc1_ID).address() + kWorkshopModeCall1_Offset;
		const auto workshopCall2 = REL::ID(kWorkshopModeFunc1_ID).address() + kWorkshopModeCall2_Offset;
		const auto workshopCall3 = REL::ID(kWorkshopModeFunc2_ID).address() + kWorkshopModeCall3_Offset;

		if (!VerifyCallInstruction(workshopCall1, getOverlayTarget)) {
			REX::ERROR("PCMOverlayHook: WorkshopMode call 1 (0x{:016X}) verification failed", workshopCall1);
			return false;
		}
		if (!VerifyCallInstruction(workshopCall2, getOverlayTarget)) {
			REX::ERROR("PCMOverlayHook: WorkshopMode call 2 (0x{:016X}) verification failed", workshopCall2);
			return false;
		}
		if (!VerifyCallInstruction(workshopCall3, getOverlayTarget)) {
			REX::ERROR("PCMOverlayHook: WorkshopMode call 3 (0x{:016X}) verification failed", workshopCall3);
			return false;
		}

		s_originalWorldspaceQuery = reinterpret_cast<WorldspaceQuery_t>(targetAddr);
		s_originalGetOverlayForCell_ThreadSafe = reinterpret_cast<GetOverlayForCell_ThreadSafe_t>(getOverlayTarget);

		auto& trampoline = REL::GetTrampoline();
		// Capture our allocator-owned stub, never infer ownership from patched bytes.
		s_overlayResolverCallSite = workshopCall3;
		s_overlayResolverStub = trampoline.allocate_branch5(
			reinterpret_cast<std::uintptr_t>(&HookedGetOverlayForCell_ThreadSafe));
		s_contentQueryCallSite = callSite1;
		s_contentQueryStub = trampoline.allocate_branch5(reinterpret_cast<std::uintptr_t>(&HookedWorldspaceQuery<0>));
		trampoline.write_call<5>(callSite1, &HookedWorldspaceQuery<0>);
		trampoline.write_call<5>(callSite2, &HookedWorldspaceQuery<1>);

		trampoline.write_call<5>(workshopCall1, &HookedGetOverlayForCell_ThreadSafe);
		trampoline.write_call<5>(workshopCall2, &HookedGetOverlayForCell_ThreadSafe);
		trampoline.write_call<5>(workshopCall3, &HookedGetOverlayForCell_ThreadSafe);

		s_hookInstalled.store(true, std::memory_order_release);
		if (!VerifyOwnedOverlayResolverCall(workshopCall3)) {
			REX::ERROR("OverlayGuardCompatibility-v1: installed resolver guard verification failed");
			return false;
		}
		REX::INFO("OverlayGuardCompatibility-v1: owned resolver call and trampoline verified; overlay preflight recognizes this guard");
		REX::INFO("PCMOverlayHook: installed hooks at ContentNode (0x{:016X}), BranchNode (0x{:016X}), and WorkshopMode (0x{:016X}, 0x{:016X}, 0x{:016X})",
			callSite1, callSite2, workshopCall1, workshopCall2, workshopCall3);
		return true;
	}

	bool VerifyOwnedOverlayResolverCall(std::uintptr_t a_callSite) noexcept
	{
		if (!s_hookInstalled.load(std::memory_order_acquire) ||
			a_callSite != s_overlayResolverCallSite || !s_overlayResolverStub) {
			return false;
		}
		__try {
			return MatchesOwnedGuardCall(
				std::span<const std::uint8_t, 5>{ reinterpret_cast<const std::uint8_t*>(a_callSite), 5 },
				a_callSite,
				std::span<const std::uint8_t, 14>{ reinterpret_cast<const std::uint8_t*>(s_overlayResolverStub), 14 },
				s_overlayResolverStub,
				reinterpret_cast<std::uintptr_t>(&HookedGetOverlayForCell_ThreadSafe));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool VerifyOwnedContentQueryCall(std::uintptr_t a_callSite) noexcept
	{
		if (!s_hookInstalled.load(std::memory_order_acquire) ||
			a_callSite != s_contentQueryCallSite || !s_contentQueryStub) return false;
		__try {
			return MatchesOwnedGuardCall(
				std::span<const std::uint8_t, 5>{ reinterpret_cast<const std::uint8_t*>(a_callSite), 5 }, a_callSite,
				std::span<const std::uint8_t, 14>{ reinterpret_cast<const std::uint8_t*>(s_contentQueryStub), 14 },
				s_contentQueryStub, reinterpret_cast<std::uintptr_t>(&HookedWorldspaceQuery<0>));
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	void ResetQueryDiagnostics() noexcept
	{
		std::lock_guard lock(s_queryMutex);
		s_queryCounters = {};
		s_queryReadFailures.store(0, std::memory_order_relaxed);
		++s_queryEpoch;
		REX::INFO("PCMQueryAudit-v1 load epoch={} counters reset; all query outputs preserved", s_queryEpoch);
	}

	void LogQuerySummary(std::int32_t a_stage) noexcept
	{
		if (!ClaimablePOIs::Diagnostics::Enabled()) return;
		std::lock_guard lock(s_queryMutex);
		for (std::size_t i = 0; i < s_queryCounters.size(); ++i) {
			const auto& c = s_queryCounters[i];
			REX::INFO("PCMQueryAudit-v1 summary epoch={} stage={} site={} calls={} resolved={} nonzeroScalar={} exactWorldMatches={} readFailures={} passthrough=true",
				s_queryEpoch, a_stage, i == 0 ? "content" : "branch", c.calls, c.resolved,
				c.nonzeroScalar, c.exactWorldMatches, s_queryReadFailures.load(std::memory_order_relaxed));
		}
	}

	void SyncFromSave() noexcept
	{
		__try {
			RawSyncFromSave();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			REX::ERROR("PCMOverlayHook: exception during SyncFromSave");
		}
	}

	void RegisterBeacon(RE::TESObjectREFR* a_beacon) noexcept
	{
		__try {
			RawRegisterBeacon(a_beacon);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			REX::ERROR("PCMOverlayHook: exception during RegisterBeacon");
		}
	}

	void Clear() noexcept
	{
		std::unique_lock lock(s_registryMutex);
		s_claimedSites.clear();
		REX::INFO("PCMOverlayHook: cleared claimed POI registry");
	}

	std::size_t GetRegisteredSiteCount() noexcept
	{
		std::shared_lock lock(s_registryMutex);
		return s_claimedSites.size();
	}
}
