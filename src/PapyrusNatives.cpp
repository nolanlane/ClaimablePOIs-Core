#include "PapyrusNatives.h"

#include <excpt.h>

// wingdi.h defines ERROR as 0
#undef ERROR

#include "Forms.h"
#include "Diagnostics.h"
#include "PCMOverlayHook.h"
#include "VanillaOverlayHook.h"
#include "PopulationDiagnostics.h"
#include "MiniSiteDiagnostics.h"
#include "ProceduralKeyDiagnostics.h"
#include "ProceduralLifecycleDiagnostics.h"
#include "FurnitureEnrollmentBatch.h"
#include "ContainerEnrollmentPolicy.h"
#include "TeardownProtection.h"
#include "TeardownPolicy.h"

#include "RE/A/Actor.h"
#include "RE/B/BGSConstructibleObject.h"
#include "RE/B/BGSKeyword.h"
#include "RE/B/BGSListForm.h"
#include "RE/B/BGSLocation.h"
#include "RE/B/BSLock.h"
#include "RE/B/BSTScatterTable.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/T/TESFaction.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESGlobal.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESObjectREFR.h"

#include "SFSE/Version.h"

#include "REX/FModule.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
	// Vanilla Starfield.esm FC07OutpostActivator. This is the temporary
	// workshop activator consumed by the SetOutpostOverlay path; it is
	// not the normal OutpostBeaconActivator that the player must place through
	// the scanner to create a genuine outpost.
	constexpr RE::TESFormID kOverlayActivatorID{ 0x00260ED4 };
	// First decoration gate candidate. This is intentionally the only base form
	// admitted by GetDecorationSafetyFlags until a live disable/restore trial
	// proves another form safe.
	constexpr RE::TESFormID kDecorationAllowlistedBaseID{ 0x00237543 };

	// GetDecorationSafetyFlags returns zero only for the currently allowlisted,
	// promoted STAT with a non-null extra-data list and no rejected extra types.
	// Every nonzero bit is a fail-closed rejection reason.
	enum DecorationSafetyFlag : std::uint32_t
	{
		kDecorationNullReference = 1u << 0,
		kDecorationBaseMissing = 1u << 1,
		kDecorationBaseNotSTAT = 1u << 2,
		kDecorationBaseNotAllowlisted = 1u << 3,
		kDecorationExtraDataMissing = 1u << 4,
		kDecorationPromotedRefMissing = 1u << 5,
		kDecorationKeywords = 1u << 6,
		kDecorationOwnership = 1u << 7,
		kDecorationEnableStateParent = 1u << 8,
		kDecorationEnableStateChildren = 1u << 9,
		kDecorationLinkedRef = 1u << 10,
		kDecorationLinkedRefChildren = 1u << 11,
		kDecorationLocation = 1u << 12,
		kDecorationRadius = 1u << 13,
		kDecorationObjectInstance = 1u << 14,
		kDecorationWorkshop = 1u << 15,
		kDecorationWorkshopSnapping = 1u << 16,
		kDecorationFurnitureEntryData = 1u << 17,
		kDecorationSnapLinks = 1u << 18,
		kDecorationGroupedPackin = 1u << 19,
		kDecorationCalcedPackIn = 1u << 20,
		kDecorationSourcePackIn = 1u << 21,
		kDecorationOwningInstance = 1u << 22,
		kDecorationProceduralInstanceData = 1u << 23,
		kDecorationPlacedPlanetContent = 1u << 24,
		kDecorationProbeException = 1u << 25,
		kDecorationUnsupportedRuntime = 1u << 26
	};

	constexpr RE::TESFormID kWorkshopReplacementChairBaseID{ 0x0002F423 };
	constexpr RE::TESFormID kWorkshopCommitTrialBaseID{ 0x0001A21B };
	constexpr RE::TESFormID kClaimTimeVanillaBeaconBaseID{ 0x002C60E9 };
	// Exact animation/AI-only FURN bases proven unrelated to player-placeable
	// workshop furniture. Do not broaden this into a metadata heuristic: the
	// first such trial excluded 18 objects and regressed workbench movement.
	constexpr RE::TESFormID kWorkshopExcludedNPCClipboardBaseID{ 0x0025EB0F };
	constexpr RE::TESFormID kWorkshopExcludedFlyerPerchBaseID{ 0x003DE5A7 };
	// GrazeMarkerExclusion-v1: installed Starfield.esm identifies these as animal
	// grazing markers. Logs prove they were promoted by non-COBJ FURN enrollment.
	constexpr RE::TESFormID kWorkshopExcludedCreatureGrazeLowBaseID{ 0x0012B4B1 };
	constexpr RE::TESFormID kWorkshopExcludedCreatureGrazeBaseID{ 0x0021ED5E };
	constexpr RE::TESFormID kWorkshopExcludedCreatureGrazeTreeBaseID{ 0x001EA1DA };
	enum WorkshopFurnitureCompatibilityFlag : std::uint32_t
	{
		kWorkshopFurnitureKnownIncompatibleBase = 1u << 0,
		kWorkshopFurnitureProbeException = 1u << 1
	};

	// Claim-time attachment trial. This is intentionally gated to the exact
	// Starfield 1.16.244.0 mutator and its verified entry prefix. The call is
	// isolated behind SEH; an unknown runtime must never receive this call.
	constexpr std::uintptr_t kClaimTimeWorkshopAttachRva{ 0x00F3CF40 };
	constexpr std::array<std::uint8_t, 33> kClaimTimeWorkshopAttachPrefix{
		0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x89, 0x44,
		0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
		0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C,
		0x24, 0xD9, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00,
		0x00
	};
	// The attach routine's postcondition is the vanilla predicate at this
	// exact RVA, not the presence of an ExtraWorkshop node.  Keep the same
	// runtime and prefix gate as the mutator: this must never be called against
	// an unverified executable.
	constexpr std::uintptr_t kClaimTimeWorkshopPredicateRva{ 0x00F3CC90 };
	constexpr std::array<std::uint8_t, 16> kClaimTimeWorkshopPredicatePrefix{
		0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xC0,
		0x48, 0x8B, 0xD9, 0x80, 0x79, 0x2E, 0x4B, 0x48
	};
	// Active-session engine ensure path for a missing Workshop::ExtraData node.
	// This is experimental and is callable only from the single-chair session
	// reattach below; it is never used by claim-time or batch enrollment.
	constexpr std::uintptr_t kActiveWorkshopEnsureRva{ 0x00F51350 };
	constexpr std::array<std::uint8_t, 16> kActiveWorkshopEnsurePrefix{
		0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
		0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xEA, 0x48
	};
	// F51350 calls this read-only prerequisite before it queries or creates the
	// Workshop node.  The optional filter is loaded from the exact global below
	// only when its runtime form-type byte is 0x04, matching vanilla.
	constexpr std::uintptr_t kActiveWorkshopEligibilityRva{ 0x0047FCE0 };
	constexpr std::uintptr_t kActiveWorkshopEligibilityFilterRva{ 0x05F0B380 };
	constexpr std::array<std::uint8_t, 16> kActiveWorkshopEligibilityPrefix{
		0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83,
		0xC1, 0x68, 0x48, 0x8B, 0x01, 0x48, 0xC7, 0x44
	};
	// Canonical engine BaseExtraList::Remove routine verified against Starfield 1.16.244.0,
	// Each supplied attached cell is independently required to fit this verified
	// raw-slot bound. The multi-cell pass also has separate total-cell,
	// in-radius-reference, and candidate bounds so it can fail closed without
	// silently reducing the claimed POI to the beacon cell.
	constexpr std::uint32_t kClaimTimeCellReferenceRawSlotCap{ 16384 };
	constexpr std::uint32_t kClaimTimeReferenceScanCap{ 65536 };
	constexpr std::uint32_t kClaimTimeLoadedCellCap{ 128 };
	// Read-only population diagnostics retain plain reference metadata, never
	// engine pointers. The baseline survives travel and in-process save loads so
	// later stages can identify exactly which references disappeared or changed.
	// It is deliberately not serialized into the save and is replaced by the
	// next pre-overlay snapshot.
	constexpr std::uint32_t kPopulationDiagnosticReferenceCap{ 16384 };
	constexpr std::uint32_t kPopulationDiagnosticMissingLogCap{ 512 };
	constexpr std::uint32_t kPopulationDiagnosticAddedLogCap{ 128 };
	constexpr std::uint32_t kPopulationDiagnosticChangedLogCap{ 256 };
	// FormList stores both plugin/static entries and Papyrus AddForm entries.
	// This cap bounds metadata reads without imposing a project-specific list size.
	constexpr std::uint32_t kClaimTimeExclusionFormCap{ 4096 };

	// The CommonLibSF TESObjectCELL declaration is not the layout used by the
	// shipped 1.16.244.0 executable for this traversal. These offsets were
	// verified against that executable and are intentionally kept local here;
	// do not move them into the shared headers without re-verifying the runtime.
	constexpr std::uintptr_t kClaimTimeCellReferencesSizeOffset{ 0x80 };
	constexpr std::uintptr_t kClaimTimeCellReferencesCapacityOffset{ 0x84 };
	constexpr std::uintptr_t kClaimTimeCellReferencesDataOffset{ 0x88 };
	// TESObjectREFR::GetParentWorldSpace (ID 63417, RVA 0xB4EB40 on
	// 1.16.244.0) obtains the parent cell from ref+0xB0 and reads its
	// worldspace at cell+0x110. The reference lock remains at +0x128.
	// Keep these raw offsets local to the bounded diagnostic traversal.
	constexpr std::uintptr_t kClaimTimeCellWorldspaceOffset{ 0x110 };
	constexpr std::uintptr_t kClaimTimeCellReferencesLockOffset{ 0x128 };

	using ClaimTimeWorkshopAttachFunction = void (*)(
		RE::TESObjectREFR*, bool, RE::TESObjectREFR*);
	using ClaimTimeWorkshopPredicateFunction = bool (*)(RE::TESObjectREFR*);
	using ActiveWorkshopEnsureFunction = RE::BSExtraData* (*)(RE::TESObjectREFR*, bool);
	using ActiveWorkshopEligibilityFunction = bool (*)(RE::TESObjectREFR*, void*);
	using EnsurePromotedReferenceFunction = void (*)(RE::ExtraDataList*, RE::TESForm*, bool);
	using WorkshopCommitFunction = void (*)(void*, RE::TESObjectREFR*, const std::uint8_t*);
	// Starfield 1.16.244.0: the only engine caller passes
	// (reference->extraDataList, reference->parentCell, false). The routine owns
	// allocation, ExtraDataList insertion, locking, and the parent-cell entry in
	// ExtraPromotedRef; the plugin never constructs or links that node itself.
	constexpr REL::ID kEnsurePromotedReferenceFunctionID{ 45048 };
	constexpr std::array<std::uint8_t, 16> kEnsurePromotedReferenceFunctionPrefix{
		0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
		0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
	};
	constexpr REL::ID kWorkshopCommitFunctionID{ 121616 };
	constexpr REL::ID kWorkshopCommitContextID{ 928921 };
	constexpr std::array<std::uint8_t, 16> kWorkshopCommitFunctionPrefix{
		0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56,
		0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41
	};

	// BaseExtraList::Remove verified against Starfield 1.16.244.0 (Address Library ID 44738).
	// Unlinks the node of the requested ExtraDataType, updates m_flags and m_tail, and
	// invokes the node's deleting destructor in Starfield.exe to safely free the memory.
	using BaseExtraListRemoveFunction = bool (*)(void*, RE::ExtraDataType, RE::BSExtraData*);
	constexpr REL::ID kBaseExtraListRemoveFunctionID{ 44738 };
	constexpr std::uintptr_t kBaseExtraListRemoveRva{ 0x00501230 };
	constexpr std::array<std::uint8_t, 16> kBaseExtraListRemovePrefix{
		0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B,
		0x51, 0x10, 0x49, 0x8B, 0xF8, 0x4C, 0x8B, 0xC1
	};

	// 0 = no-op/no active transaction, 1 = one or more candidates attached, -1 =
	// rejected. The status distinguishes a valid empty scan from failure while
	// the NiPointers retain the engine objects through Commit/Rollback.
	constexpr std::uint32_t kBatchEnrollmentMaxItems{ 1024 };
	using FurnitureEnrollmentBatch = ClaimablePOIs::FurnitureEnrollmentBatch<kBatchEnrollmentMaxItems>;
	std::mutex g_furnitureEnrollmentMutex;
	std::atomic<bool> g_furnitureEnrollmentFailed{ false };
	// Inspection-only failure evidence, with no retained engine pointers. Neither
	// save/load nor overlay restoration clears the process-wide fault latch.
	FurnitureEnrollmentBatch g_failedFurnitureBatch{};
	RE::TESFormID g_failedFurnitureBeaconID{};
	std::int32_t g_claimTimeEnrollmentStatus{ 0 };
	std::array<RE::NiPointer<RE::TESObjectREFR>, kBatchEnrollmentMaxItems> g_claimTimeEnrollmentCandidates{};
	std::uint32_t g_claimTimeEnrollmentCandidateCount{ 0 };
	RE::NiPointer<RE::TESObjectREFR> g_claimTimeEnrollmentBeacon{};
	RE::NiPointer<RE::TESObjectREFR> g_claimTimeEnrollmentHelper{};
	RE::NiPointer<RE::TESObjectCELL> g_claimTimeEnrollmentCell{};

	// FURN linkage is valid only while the claimed workshop manager is live.
	// Retain exactly the references attached by this session so normal exit,
	// save, and load boundaries never have to rediscover or guess ownership.
	std::array<RE::NiPointer<RE::TESObjectREFR>, kBatchEnrollmentMaxItems> g_sessionFurniture{};
	std::uint32_t g_sessionFurnitureCount{ 0 };
	RE::NiPointer<RE::TESObjectREFR> g_sessionFurnitureBeacon{};

	// GetWorkshopReplacementSafetyFlags returns zero only for an original whose
	// base is runtime-enumerated from the outpost build menu and which has none
	// of the state carriers that could make hiding it unsafe. FURN additionally
	// requires its promoted/furniture entry data.
	enum WorkshopReplacementSafetyFlag : std::uint32_t
	{
		kWorkshopReplacementNullReference = 1u << 0,
		kWorkshopReplacementBaseMissing = 1u << 1,
		kWorkshopReplacementBaseNotEnrollmentEligible = 1u << 2,
		kWorkshopReplacementBaseNotAllowlisted = 1u << 3,
		kWorkshopReplacementExtraDataMissing = 1u << 4,
		kWorkshopReplacementPromotedRefMissing = 1u << 5,
		kWorkshopReplacementFurnitureEntryDataMissing = 1u << 6,
		kWorkshopReplacementKeywords = 1u << 7,
		kWorkshopReplacementOwnership = 1u << 8,
		kWorkshopReplacementEnableStateParent = 1u << 9,
		kWorkshopReplacementEnableStateChildren = 1u << 10,
		kWorkshopReplacementLinkedRef = 1u << 11,
		kWorkshopReplacementLinkedRefChildren = 1u << 12,
		kWorkshopReplacementLocation = 1u << 13,
		kWorkshopReplacementRadius = 1u << 14,
		kWorkshopReplacementObjectInstance = 1u << 15,
		kWorkshopReplacementWorkshop = 1u << 16,
		kWorkshopReplacementWorkshopSnapping = 1u << 17,
		kWorkshopReplacementSnapLinks = 1u << 18,
		kWorkshopReplacementGroupedPackin = 1u << 19,
		kWorkshopReplacementCalcedPackIn = 1u << 20,
		kWorkshopReplacementSourcePackIn = 1u << 21,
		kWorkshopReplacementOwningInstance = 1u << 22,
		kWorkshopReplacementProceduralInstanceData = 1u << 23,
		kWorkshopReplacementPlacedPlanetContent = 1u << 24,
		kWorkshopReplacementProbeException = 1u << 25,
		kWorkshopReplacementUnsupportedRuntime = 1u << 26
	};
	// Enrollment is deliberately more permissive than destructive replacement.
	// The exact type/lifecycle/location/worldspace/radius gates run separately;
	// only state that prevents safe attachment remains fatal here.
	constexpr std::uint32_t kWorkshopEnrollmentHardSafetyFlags{
		kWorkshopReplacementNullReference |
		kWorkshopReplacementBaseMissing |
		kWorkshopReplacementBaseNotEnrollmentEligible |
		kWorkshopReplacementExtraDataMissing |
		kWorkshopReplacementWorkshop
	};
	static_assert(ClaimablePOIs::kCompoundContainerAllowedFlags ==
		(kWorkshopReplacementBaseNotAllowlisted | kWorkshopReplacementLinkedRefChildren));
	static_assert(ClaimablePOIs::kCompoundContainerRequiredFlags == kWorkshopReplacementBaseNotAllowlisted);

	[[nodiscard]] bool IsConstructibleBase(RE::TESForm* a_form) noexcept;
	[[nodiscard]] bool EnsureEngineWorkshopExtraForBuildableFurniture(
		std::uintptr_t a_moduleBase,
		RE::TESObjectREFR* a_item,
		bool& a_created,
		std::uint32_t& a_exceptionCode) noexcept;
	[[nodiscard]] bool VerifyWorkshopCommitTarget(std::uintptr_t a_target) noexcept;
	[[nodiscard]] bool ReadWorkshopCommitContextBeacon(
		void* a_context,
		RE::TESObjectREFR*& a_beacon,
		std::uint32_t& a_exceptionCode) noexcept;
	[[nodiscard]] bool CallWorkshopCommit(
		std::uintptr_t a_target,
		void* a_context,
		RE::TESObjectREFR* a_item,
		std::uint32_t& a_exceptionCode) noexcept;

	// SetOutpostOverlay console command handler verified against Starfield 1.16.244.0,
	// Starfield.exe SHA-256 7E9ADB1414A8E1B325E5E1F097B9B17B78DEB7EEBEDA37A333351A43A60F9D28.
	constexpr std::uintptr_t kSetOutpostOverlayHandlerRva{ 0x00F40EA0 };
	constexpr std::array<std::uint8_t, 16> kSetOutpostOverlayHandlerPrefix{
		0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B,
		0xD8, 0x4D, 0x85, 0xC0, 0x0F, 0x84, 0xA1, 0x00
	};

	// Verified handler resolver RVAs and layout constants for Starfield 1.16.244.0:
	// 1. Cell resolver returns the cell overlay pointer.
	constexpr std::uintptr_t kCellResolverRva{ 0x00AD3610 };
	constexpr std::array<std::uint8_t, 16> kCellResolverPrefix{
		0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
		0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83
	};

	// 2. Global singleton getter function supplies manager at offset +0x8B0.
	constexpr std::uintptr_t kSingletonRva{ 0x023FF640 };
	constexpr std::array<std::uint8_t, 16> kSingletonPrefix{
		0x48, 0x83, 0xEC, 0x28, 0xBA, 0xB8, 0x00, 0x00,
		0x00, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00
	};
	constexpr std::ptrdiff_t kSingletonManagerOffset{ 0x8B0 };

	// 3. Lookup takes manager in RCX, ignored/null RDX, and address of overlay pointer in R8.
	constexpr std::uintptr_t kPropertyLookupRva{ 0x00ADA560 };
	constexpr std::array<std::uint8_t, 16> kPropertyLookupPrefix{
		0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
		0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x56
	};

	// 4. Exact handler instruction sites within SetOutpostOverlay:
	constexpr std::uintptr_t kSiteF40F14Rva{ 0x00F40F14 };
	constexpr std::array<std::uint8_t, 7> kSiteF40F14Bytes{ 0x48, 0x8B, 0x8B, 0xB0, 0x00, 0x00, 0x00 };

	constexpr std::uintptr_t kSiteF40F1BRva{ 0x00F40F1B };
	constexpr std::array<std::uint8_t, 5> kSiteF40F1BBytes{ 0xE8, 0xF0, 0x26, 0xB9, 0xFF };

	constexpr std::uintptr_t kSiteF40F25Rva{ 0x00F40F25 };
	constexpr std::array<std::uint8_t, 5> kSiteF40F25Bytes{ 0xE8, 0x16, 0xE7, 0x4B, 0x01 };

	constexpr std::uintptr_t kSiteF40F2FRva{ 0x00F40F2F };
	constexpr std::array<std::uint8_t, 7> kSiteF40F2FBytes{ 0x48, 0x8B, 0x88, 0xB0, 0x08, 0x00, 0x00 };

	constexpr std::uintptr_t kSiteF40F36Rva{ 0x00F40F36 };
	constexpr std::array<std::uint8_t, 5> kSiteF40F36Bytes{ 0xE8, 0x25, 0x96, 0xB9, 0xFF };

	constexpr std::uintptr_t kSiteF40F40Rva{ 0x00F40F40 };
	constexpr std::array<std::uint8_t, 4> kSiteF40F40Bytes{ 0x80, 0x48, 0x20, 0x01 };

	constexpr std::uintptr_t kSiteF40F44Rva{ 0x00F40F44 };
	constexpr std::array<std::uint8_t, 7> kSiteF40F44Bytes{ 0xC6, 0x05, 0x7A, 0x31, 0xF9, 0x04, 0x01 };

	// 5. ActivatedOverlayProperty flag byte offset and bit 0.
	constexpr std::ptrdiff_t kPropertyFlagOffset{ 0x20 };
	constexpr std::uint8_t   kPropertyFlagBit{ 0x01 };
	// 6. Engine dirty byte RVA set on overlay mutation.
	constexpr std::uintptr_t kDirtyByteRva{ 0x05ED40C5 };

	enum class UnlockPurpose : std::uint8_t
	{
		None = 0,
		InitialClaim = 1,
		ClaimedWorkshop = 2
	};

	// Captured tracking identity for active temporary POI overlay unlock.
	std::atomic<RE::TESFormID>  g_trackedHelperFormID{ 0 };
	std::atomic<RE::TESFormID>  g_trackedCellFormID{ 0 };
	std::atomic<std::uintptr_t> g_trackedPropertyAddress{ 0 };
	std::atomic<UnlockPurpose>  g_trackedPurpose{ UnlockPurpose::None };
	std::atomic<bool>           g_trackedRecoveryOnly{ false };
	std::atomic<bool>           g_trackedUnlockActive{ false };

	void ClearPOIOverlayUnlockTrackingInternal() noexcept
	{
		ClaimablePOIs::VanillaOverlayHook::ClearInitialClaim();
		g_trackedUnlockActive.store(false, std::memory_order_release);
		g_trackedPurpose.store(UnlockPurpose::None, std::memory_order_release);
		g_trackedRecoveryOnly.store(false, std::memory_order_release);
		g_trackedHelperFormID.store(0, std::memory_order_release);
		g_trackedCellFormID.store(0, std::memory_order_release);
		g_trackedPropertyAddress.store(0, std::memory_order_release);
	}

	// Local FormIDs in ClaimablePOIs.esm (must match Generator.cs output order).
	constexpr RE::TESFormID kLocalClaimableKeyword{ 0x803 };
	constexpr RE::TESFormID kLocalClaimedPOIsList{ 0x804 };
	constexpr RE::TESFormID kLocalClaimedCount{ 0x806 };
	constexpr RE::TESFormID kLocalClaimRadius{ 0x807 };

	// Plugin file index — set during ResolveData so we can compose runtime FormIDs.
	std::uint8_t g_pluginFullIndex{ 0xFF }; // sentinel: unset
	bool         g_pluginIsSmall{ false };
	bool         g_pluginIsMedium{ false };
	std::uint16_t g_pluginSmallIndex{ 0 };
	std::uint16_t g_pluginMediumIndex{ 0 };

	void SetPluginIndex(const RE::TESFile& a_file) noexcept
	{
		g_pluginIsSmall = a_file.fileFlags.any(RE::TESFile::Flags::kSmall);
		g_pluginIsMedium = a_file.fileFlags.any(RE::TESFile::Flags::kMedium);
		g_pluginFullIndex = a_file.fileIndex.fullIndex;
		g_pluginSmallIndex = a_file.fileIndex.smallIndex;
		g_pluginMediumIndex = a_file.fileIndex.mediumIndex;
	}

	[[nodiscard]] RE::TESFormID ComposeLocalID(std::uint32_t a_local) noexcept
	{
		if (g_pluginIsSmall) {
			return 0xFE000000u | (static_cast<std::uint32_t>(g_pluginSmallIndex) << 12) | (a_local & 0x00000FFFu);
		}
		if (g_pluginIsMedium) {
			return 0xFD000000u | (static_cast<std::uint32_t>(g_pluginMediumIndex) << 16) | (a_local & 0x0000FFFFu);
		}
		return (static_cast<std::uint32_t>(g_pluginFullIndex) << 24) | (a_local & 0x00FFFFFFu);
	}

	[[nodiscard]] RE::BGSKeyword* GetClaimableKeyword() noexcept
	{
		return RE::TESForm::LookupByID<RE::BGSKeyword>(ComposeLocalID(kLocalClaimableKeyword));
	}

	[[nodiscard]] RE::BGSListForm* GetClaimedList() noexcept
	{
		return RE::TESForm::LookupByID<RE::BGSListForm>(ComposeLocalID(kLocalClaimedPOIsList));
	}

	[[nodiscard]] RE::TESGlobal* GetClaimedCountGlobal() noexcept
	{
		return RE::TESForm::LookupByID<RE::TESGlobal>(ComposeLocalID(kLocalClaimedCount));
	}

	[[nodiscard]] RE::TESGlobal* GetClaimRadiusGlobal() noexcept
	{
		return RE::TESForm::LookupByID<RE::TESGlobal>(ComposeLocalID(kLocalClaimRadius));
	}

	struct ReferenceDiagnosticState
	{
		RE::TESFormID refID{ 0 };
		RE::TESFormID baseID{ 0 };
		RE::TESFormID cellID{ 0 };
		RE::TESFormID locationID{ 0 };
		RE::TESFormID worldspaceID{ 0 };
		float x{ 0.0F };
		float y{ 0.0F };
		float z{ 0.0F };
		std::uint32_t baseType{ 0 };
		bool deleted{ false };
		bool disabled{ false };
		bool hasExtraDataList{ false };
		bool hasWorkshop{ false };
		bool hasProceduralInstanceData{ false };
		bool hasPlacedPlanetContent{ false };
		bool hasRadius{ false };
		bool hasLinkedRef{ false };
		bool hasLocation{ false };
		bool hasPromotedRef{ false };
		bool hasSourcePackIn{ false };
		bool hasOwningInstance{ false };
		bool hasKeywords{ false };
		bool hasOwnership{ false };
		bool hasEnableStateParent{ false };
		bool hasEnableStateChildren{ false };
		bool hasLinkedRefChildren{ false };
		bool hasObjectInstance{ false };
		bool hasWorkshopSnapping{ false };
		bool hasFurnitureEntryData{ false };
		bool hasSnapLinks{ false };
		bool hasGroupedPackin{ false };
		bool hasCalcedPackIn{ false };
	};

	[[nodiscard]] bool RawCollectReferenceDiagnosticState(
		RE::TESObjectREFR* a_reference,
		ReferenceDiagnosticState& a_state) noexcept
	{
		if (!a_reference) {
			return false;
		}

		a_state.refID = a_reference->formID;
		if (const auto base = a_reference->GetBaseObject()) {
			a_state.baseID = base->formID;
			a_state.baseType = static_cast<std::uint32_t>(base->GetFormType());
		}
		if (a_reference->parentCell) {
			a_state.cellID = a_reference->parentCell->formID;
		}
		if (const auto* location = a_reference->GetCurrentLocation()) {
			a_state.locationID = location->formID;
		}
		if (const auto* worldspace = a_reference->GetParentWorldSpace()) {
			a_state.worldspaceID = worldspace->formID;
		}

		const auto position = a_reference->GetPosition();
		a_state.x = position.x;
		a_state.y = position.y;
		a_state.z = position.z;
		a_state.deleted = a_reference->IsDeleted();
		a_state.disabled = a_reference->IsDisabled();

		const auto* extraData = a_reference->extraDataList.get();
		if (extraData) {
			a_state.hasExtraDataList = true;
			a_state.hasKeywords = extraData->HasType(RE::ExtraDataType::kKeywords);
			a_state.hasOwnership = extraData->HasType(RE::ExtraDataType::kOwnership);
			a_state.hasEnableStateParent = extraData->HasType(RE::ExtraDataType::kEnableStateParent);
			a_state.hasEnableStateChildren = extraData->HasType(RE::ExtraDataType::kEnableStateChildren);
			a_state.hasLinkedRefChildren = extraData->HasType(RE::ExtraDataType::kLinkedRefChildren);
			a_state.hasObjectInstance = extraData->HasType(RE::ExtraDataType::kObjectInstance);
			a_state.hasWorkshop = extraData->HasType(RE::ExtraDataType::kWorkshop);
			a_state.hasWorkshopSnapping = extraData->HasType(RE::ExtraDataType::kWorkshopSnapping);
			a_state.hasFurnitureEntryData = extraData->HasType(RE::ExtraDataType::kRefFurnitureEntryData);
			a_state.hasSnapLinks = extraData->HasType(RE::ExtraDataType::kSnapLinks);
			a_state.hasGroupedPackin = extraData->HasType(RE::ExtraDataType::kGroupedPackin);
			a_state.hasCalcedPackIn = extraData->HasType(RE::ExtraDataType::kCalcedPackIn);
			a_state.hasProceduralInstanceData = extraData->HasType(RE::ExtraDataType::kProceduralInstanceData);
			a_state.hasPlacedPlanetContent = extraData->HasType(RE::ExtraDataType::kPlacedPlanetContent);
			a_state.hasRadius = extraData->HasType(RE::ExtraDataType::kRadius);
			a_state.hasLinkedRef = extraData->HasType(RE::ExtraDataType::kLinkedRef);
			a_state.hasLocation = extraData->HasType(RE::ExtraDataType::kLocation);
			a_state.hasPromotedRef = extraData->HasType(RE::ExtraDataType::kPromotedRef);
			a_state.hasSourcePackIn = extraData->HasType(RE::ExtraDataType::kSourcePackIn);
			a_state.hasOwningInstance = extraData->HasType(RE::ExtraDataType::kOwningInstance);
		}
		return true;
	}

	[[nodiscard]] bool SafeCollectReferenceDiagnosticState(
		RE::TESObjectREFR* a_reference,
		ReferenceDiagnosticState& a_state) noexcept
	{
		__try {
			return RawCollectReferenceDiagnosticState(a_reference, a_state);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool RawCollectDecorationSafetyFlags(
		RE::TESObjectREFR* a_reference,
		std::uint32_t& a_flags,
		RE::TESFormID& a_referenceID,
		RE::TESFormID& a_baseID) noexcept
	{
		if (!a_reference) {
			a_flags |= kDecorationNullReference;
			return true;
		}

		a_referenceID = a_reference->formID;
		const auto base = a_reference->GetBaseObject();
		if (!base) {
			a_flags |= kDecorationBaseMissing;
		} else {
			a_baseID = base->formID;
			if (!base->Is(RE::FormType::kSTAT)) {
				a_flags |= kDecorationBaseNotSTAT;
			}
			if (a_baseID != kDecorationAllowlistedBaseID) {
				a_flags |= kDecorationBaseNotAllowlisted;
			}
		}

		const auto* extraData = a_reference->extraDataList.get();
		if (!extraData) {
			a_flags |= kDecorationExtraDataMissing;
		} else {
			if (!extraData->HasType(RE::ExtraDataType::kPromotedRef)) {
				a_flags |= kDecorationPromotedRefMissing;
			}
			if (extraData->HasType(RE::ExtraDataType::kKeywords)) {
				a_flags |= kDecorationKeywords;
			}
			if (extraData->HasType(RE::ExtraDataType::kOwnership)) {
				a_flags |= kDecorationOwnership;
			}
			if (extraData->HasType(RE::ExtraDataType::kEnableStateParent)) {
				a_flags |= kDecorationEnableStateParent;
			}
			if (extraData->HasType(RE::ExtraDataType::kEnableStateChildren)) {
				a_flags |= kDecorationEnableStateChildren;
			}
			if (extraData->HasType(RE::ExtraDataType::kLinkedRef)) {
				a_flags |= kDecorationLinkedRef;
			}
			if (extraData->HasType(RE::ExtraDataType::kLinkedRefChildren)) {
				a_flags |= kDecorationLinkedRefChildren;
			}
			if (extraData->HasType(RE::ExtraDataType::kLocation)) {
				a_flags |= kDecorationLocation;
			}
			if (extraData->HasType(RE::ExtraDataType::kRadius)) {
				a_flags |= kDecorationRadius;
			}
			if (extraData->HasType(RE::ExtraDataType::kObjectInstance)) {
				a_flags |= kDecorationObjectInstance;
			}
			if (extraData->HasType(RE::ExtraDataType::kWorkshop)) {
				a_flags |= kDecorationWorkshop;
			}
			if (extraData->HasType(RE::ExtraDataType::kWorkshopSnapping)) {
				a_flags |= kDecorationWorkshopSnapping;
			}
			if (extraData->HasType(RE::ExtraDataType::kRefFurnitureEntryData)) {
				a_flags |= kDecorationFurnitureEntryData;
			}
			if (extraData->HasType(RE::ExtraDataType::kSnapLinks)) {
				a_flags |= kDecorationSnapLinks;
			}
			if (extraData->HasType(RE::ExtraDataType::kGroupedPackin)) {
				a_flags |= kDecorationGroupedPackin;
			}
			if (extraData->HasType(RE::ExtraDataType::kCalcedPackIn)) {
				a_flags |= kDecorationCalcedPackIn;
			}
			if (extraData->HasType(RE::ExtraDataType::kSourcePackIn)) {
				a_flags |= kDecorationSourcePackIn;
			}
			if (extraData->HasType(RE::ExtraDataType::kOwningInstance)) {
				a_flags |= kDecorationOwningInstance;
			}
			if (extraData->HasType(RE::ExtraDataType::kProceduralInstanceData)) {
				a_flags |= kDecorationProceduralInstanceData;
			}
			if (extraData->HasType(RE::ExtraDataType::kPlacedPlanetContent)) {
				a_flags |= kDecorationPlacedPlanetContent;
			}
		}
		return true;
	}

	[[nodiscard]] bool SafeCollectDecorationSafetyFlags(
		RE::TESObjectREFR* a_reference,
		std::uint32_t& a_flags,
		RE::TESFormID& a_referenceID,
		RE::TESFormID& a_baseID) noexcept
	{
		__try {
			return RawCollectDecorationSafetyFlags(a_reference, a_flags, a_referenceID, a_baseID);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] std::int32_t SafeGetDecorationSafetyFlags(RE::TESObjectREFR* a_reference) noexcept
	{
		std::uint32_t flags = 0;
		RE::TESFormID referenceID = 0;
		RE::TESFormID baseID = 0;

		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			flags |= kDecorationUnsupportedRuntime;
		}

		if (!SafeCollectDecorationSafetyFlags(a_reference, flags, referenceID, baseID)) {
			flags |= kDecorationProbeException;
		}

		REX::INFO(
			"DecorationSafety ref=0x{:08X} base=0x{:08X} flags=0x{:08X}",
			referenceID,
			baseID,
			flags);
		return static_cast<std::int32_t>(flags);
	}

	[[nodiscard]] bool RawCollectWorkshopReplacementSafetyFlags(
		RE::TESObjectREFR* a_reference,
		std::uint32_t& a_flags,
		RE::TESFormID& a_referenceID,
		RE::TESFormID& a_baseID) noexcept
	{
		if (!a_reference) {
			a_flags |= kWorkshopReplacementNullReference;
			return true;
		}

		a_referenceID = a_reference->formID;
		const auto base = a_reference->GetBaseObject();
		if (!base) {
			a_flags |= kWorkshopReplacementBaseMissing;
		} else {
			a_baseID = base->formID;
			const bool isContainer = base->Is(RE::FormType::kCONT);
			const bool isFurniture = base->Is(RE::FormType::kFURN);
			const bool isOutpostBuildable = IsConstructibleBase(base.get());
			if (!isContainer && !isFurniture) {
				a_flags |= kWorkshopReplacementBaseNotEnrollmentEligible;
			}
			if (!isOutpostBuildable) {
				a_flags |= kWorkshopReplacementBaseNotAllowlisted;
			}
		}

		const auto* extraData = a_reference->extraDataList.get();
		if (!extraData) {
			a_flags |= kWorkshopReplacementExtraDataMissing;
			return true;
		}

		const bool isFurniture = base && base->Is(RE::FormType::kFURN);
		if (isFurniture && !extraData->HasType(RE::ExtraDataType::kPromotedRef)) {
			a_flags |= kWorkshopReplacementPromotedRefMissing;
		}
		if (isFurniture && !extraData->HasType(RE::ExtraDataType::kRefFurnitureEntryData)) {
			a_flags |= kWorkshopReplacementFurnitureEntryDataMissing;
		}
		if (extraData->HasType(RE::ExtraDataType::kKeywords)) {
			a_flags |= kWorkshopReplacementKeywords;
		}
		if (extraData->HasType(RE::ExtraDataType::kOwnership)) {
			a_flags |= kWorkshopReplacementOwnership;
		}
		if (extraData->HasType(RE::ExtraDataType::kEnableStateParent)) {
			a_flags |= kWorkshopReplacementEnableStateParent;
		}
		if (extraData->HasType(RE::ExtraDataType::kEnableStateChildren)) {
			a_flags |= kWorkshopReplacementEnableStateChildren;
		}
		if (extraData->HasType(RE::ExtraDataType::kLinkedRef)) {
			a_flags |= kWorkshopReplacementLinkedRef;
		}
		if (extraData->HasType(RE::ExtraDataType::kLinkedRefChildren)) {
			a_flags |= kWorkshopReplacementLinkedRefChildren;
		}
		if (extraData->HasType(RE::ExtraDataType::kLocation)) {
			a_flags |= kWorkshopReplacementLocation;
		}
		if (extraData->HasType(RE::ExtraDataType::kRadius)) {
			a_flags |= kWorkshopReplacementRadius;
		}
		if (extraData->HasType(RE::ExtraDataType::kObjectInstance)) {
			a_flags |= kWorkshopReplacementObjectInstance;
		}
		if (extraData->HasType(RE::ExtraDataType::kWorkshop)) {
			a_flags |= kWorkshopReplacementWorkshop;
		}
		if (extraData->HasType(RE::ExtraDataType::kWorkshopSnapping)) {
			a_flags |= kWorkshopReplacementWorkshopSnapping;
		}
		if (extraData->HasType(RE::ExtraDataType::kSnapLinks)) {
			a_flags |= kWorkshopReplacementSnapLinks;
		}
		if (extraData->HasType(RE::ExtraDataType::kGroupedPackin)) {
			a_flags |= kWorkshopReplacementGroupedPackin;
		}
		if (extraData->HasType(RE::ExtraDataType::kCalcedPackIn)) {
			a_flags |= kWorkshopReplacementCalcedPackIn;
		}
		if (extraData->HasType(RE::ExtraDataType::kSourcePackIn)) {
			a_flags |= kWorkshopReplacementSourcePackIn;
		}
		if (extraData->HasType(RE::ExtraDataType::kOwningInstance)) {
			a_flags |= kWorkshopReplacementOwningInstance;
		}
		if (extraData->HasType(RE::ExtraDataType::kProceduralInstanceData)) {
			a_flags |= kWorkshopReplacementProceduralInstanceData;
		}
		if (extraData->HasType(RE::ExtraDataType::kPlacedPlanetContent)) {
			a_flags |= kWorkshopReplacementPlacedPlanetContent;
		}
		return true;
	}

	[[nodiscard]] bool SafeCollectWorkshopReplacementSafetyFlags(
		RE::TESObjectREFR* a_reference,
		std::uint32_t& a_flags,
		RE::TESFormID& a_referenceID,
		RE::TESFormID& a_baseID) noexcept
	{
		__try {
			return RawCollectWorkshopReplacementSafetyFlags(a_reference, a_flags, a_referenceID, a_baseID);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] std::int32_t SafeGetWorkshopReplacementSafetyFlags(RE::TESObjectREFR* a_reference) noexcept
	{
		std::uint32_t flags = 0;
		RE::TESFormID referenceID = 0;
		RE::TESFormID baseID = 0;

		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			flags |= kWorkshopReplacementUnsupportedRuntime;
		}

		if (!SafeCollectWorkshopReplacementSafetyFlags(a_reference, flags, referenceID, baseID)) {
			flags |= kWorkshopReplacementProbeException;
		}

		REX::INFO(
			"WorkshopReplacementSafety ref=0x{:08X} base=0x{:08X} flags=0x{:08X}",
			referenceID,
			baseID,
			flags);
		return static_cast<std::int32_t>(flags);
	}

	[[nodiscard]] bool SafeReadProcessMemory(const void* a_src, void* a_dst, std::size_t a_size) noexcept
	{
		__try {
			std::memcpy(a_dst, a_src, a_size);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool VerifyClaimTimeWorkshopAttachTarget(std::uintptr_t a_moduleBase) noexcept
	{
		__try {
			return std::memcmp(
				reinterpret_cast<const void*>(a_moduleBase + kClaimTimeWorkshopAttachRva),
				kClaimTimeWorkshopAttachPrefix.data(),
				kClaimTimeWorkshopAttachPrefix.size()) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool VerifyClaimTimeWorkshopPredicateTarget(std::uintptr_t a_moduleBase) noexcept
	{
		__try {
			return std::memcmp(
				reinterpret_cast<const void*>(a_moduleBase + kClaimTimeWorkshopPredicateRva),
				kClaimTimeWorkshopPredicatePrefix.data(),
				kClaimTimeWorkshopPredicatePrefix.size()) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool CallClaimTimeWorkshopAttach(
		std::uintptr_t a_moduleBase,
		RE::TESObjectREFR* a_item,
		bool a_attach,
		RE::TESObjectREFR* a_beacon,
		std::uint32_t& a_exceptionCode) noexcept
	{
		a_exceptionCode = 0;
		if (!a_item || !a_beacon) {
			return false;
		}

		__try {
			auto* attach = reinterpret_cast<ClaimTimeWorkshopAttachFunction>(
				a_moduleBase + kClaimTimeWorkshopAttachRva);
			attach(a_item, a_attach, a_beacon);
			return true;
		} __except (a_exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool CallClaimTimeWorkshopPredicate(
		std::uintptr_t a_moduleBase,
		RE::TESObjectREFR* a_item,
		bool& a_result,
		std::uint32_t& a_exceptionCode) noexcept
	{
		a_result = false;
		a_exceptionCode = 0;
		if (!a_item) {
			return false;
		}

		__try {
			a_result = reinterpret_cast<ClaimTimeWorkshopPredicateFunction>(
				a_moduleBase + kClaimTimeWorkshopPredicateRva)(a_item);
			return true;
		} __except (a_exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool HasClaimTimeWorkshopState(RE::TESObjectREFR* a_reference) noexcept
	{
		__try {
			return a_reference && a_reference->extraDataList &&
			       a_reference->extraDataList->HasType(RE::ExtraDataType::kWorkshop);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// The engine's all-forms registry is a BSTHashMap2 followed by its read/write
	// lock. Starfield 1.16.244 callers load the registry from address-library ID
	// TESForm::AllFormsMap, lock +0x30, and access the table at +0x10. The address
	// library entry is a pointer-valued global; callers first load that pointer.
	// This is the verified runtime enumeration path; TESDataHandler::formArrays is not used
	// because its current CommonLib view reports every array as empty.
	struct RuntimeAllFormsRegistry
	{
		RE::BSTHashMap2<RE::TESFormID, RE::TESForm*> forms;  // 00
		RE::BSReadWriteLock lock;                             // 30
	};
	static_assert(offsetof(RuntimeAllFormsRegistry, lock) == 0x30);
	static_assert(sizeof(RuntimeAllFormsRegistry) == 0x38);

	// The first runtime trial observed 2,370,994 occupied entries. Keep a bounded
	// power-of-two ceiling with substantial mod/save headroom; the old 2,000,000
	// ceiling rejected a healthy registry before iteration.
	constexpr std::size_t kRuntimeAllFormsCap{ 8'388'608 };
	constexpr std::string_view kOutpostCategoryPrefix{ "OutpostCategory" };

	// Set of base FormIDs produced by a loaded outpost build-menu COBJ. Outpost
	// recipes use their OutpostCategory* keyword as BGSCraftableForm::benchKeyword.
	// Internal TestAssets, Quest, and Bulldoze categories are deliberately not
	// treated as ordinary player build-menu content.
	std::unordered_set<RE::TESFormID> g_constructibleBaseIDs{};

	[[nodiscard]] bool IsRuntimeOutpostCategoryPrefix(RE::BGSKeyword* a_keyword) noexcept
	{
		__try {
			if (!a_keyword) {
				return false;
			}
			const char* editorID = a_keyword->GetFormEditorID();
			if (!editorID) {
				return false;
			}
			const std::string_view value{ editorID };
			return value.starts_with(kOutpostCategoryPrefix);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	struct PopulationDiagnosticEntry
	{
		RE::TESFormID refID{ 0 };
		RE::TESFormID baseID{ 0 };
		RE::TESFormID cellID{ 0 };
		RE::TESFormID locationID{ 0 };
		std::uint32_t baseType{ 0 };
		std::uint32_t stateFlags{ 0 };
		float x{ 0.0F };
		float y{ 0.0F };
		float z{ 0.0F };
		ClaimablePOIs::ProceduralKeyDiagnostics::Snapshot proceduralKey{};
	};

	struct PopulationDiagnosticScan
	{
		struct CellOverlay
		{
			std::uint32_t cell{}, overlay{}, exception{};
			std::uint8_t flag{};
			bool resolved{};
		};
		// Plain values only. Populated while the existing scanner holds each cell.
		std::array<CellOverlay, 64> packInCellOverlays{};
		std::uint32_t packInCellCount{};
		bool packInCensus{}, packInLayoutVerified{}, packInCellsTruncated{};
		bool proceduralKeys{};
		std::uint32_t proceduralKeyReads{};
		std::vector<PopulationDiagnosticEntry> entries{};
		std::vector<RE::TESFormID> cellIDs{};
		std::uint32_t candidateCells{ 0 };
		std::uint32_t attachedExteriorCells{ 0 };
		std::uint32_t worldspaceMatchedCells{ 0 };
		std::uint32_t worldspaceMismatchedCells{ 0 };
		bool claimCellWorldspaceMatched{ false };
		std::uint32_t scanned{ 0 };
		std::uint32_t stateRejected{ 0 };
		bool truncated{ false };
		bool layoutRejected{ false };
		bool readRejected{ false };
	};

	std::vector<PopulationDiagnosticEntry> g_populationDiagnosticBaseline{};
	RE::TESFormID g_populationDiagnosticLocationID{ 0 };
	RE::TESFormID g_populationDiagnosticWorldspaceID{ 0 };
	float g_populationDiagnosticRadius{ 0.0F };
	std::vector<RE::TESFormID> g_populationDiagnosticCellIDs{};
	RE::NiPoint3A g_populationDiagnosticOrigin{};
	bool g_populationDiagnosticBaselineValid{ false };
	std::atomic<std::uint64_t> g_overlayDiagnosticEpoch{};
	std::array<std::atomic<std::uint32_t>, 8> g_overlayDiagnosticSamples{};
	std::atomic<std::uint32_t> g_selectedOverlayDiagnosticSamples{};
	std::uint32_t g_selectedProceduralKeySamples{};  // protected by mini-site mutex
	std::array<std::atomic<std::uint32_t>, 8> g_packInCensusSamples{};
	std::mutex g_miniSiteDiagnosticMutex;
	ClaimablePOIs::MiniSiteDiagnostics::Session g_miniSiteDiagnosticSession{};
	std::vector<RE::TESFormID> g_miniSiteDiagnosticCells{};

	[[nodiscard]] bool CollectPopulationDiagnostic(
		RE::TESObjectREFR* a_center,
		RE::TESObjectCELL* a_claimCell,
		const std::vector<RE::TESObjectCELL*>& a_loadedCells,
		RE::BGSLocation* a_location,
		float a_radius,
		const RE::NiPoint3A& a_origin,
		PopulationDiagnosticScan& a_scan) noexcept;
	[[nodiscard]] std::string_view GetPopulationDiagnosticStage(std::int32_t a_stage) noexcept;
	void LogPopulationDiagnosticEntry(
		std::string_view a_kind,
		std::int32_t a_stage,
		const PopulationDiagnosticEntry& a_entry) noexcept;

	[[nodiscard]] std::uint32_t BuildPopulationDiagnosticFlags(const ReferenceDiagnosticState& a_state) noexcept
	{
		std::uint32_t flags = 0;
		auto set = [&flags](bool a_value, std::uint32_t a_bit) {
			if (a_value) {
				flags |= 1u << a_bit;
			}
		};
		set(a_state.deleted, 0);
		set(a_state.disabled, 1);
		set(a_state.hasExtraDataList, 3);
		set(a_state.hasWorkshop, 4);
		set(a_state.hasProceduralInstanceData, 5);
		set(a_state.hasPlacedPlanetContent, 6);
		set(a_state.hasRadius, 7);
		set(a_state.hasLinkedRef, 8);
		set(a_state.hasLocation, 9);
		set(a_state.hasPromotedRef, 10);
		set(a_state.hasSourcePackIn, 11);
		set(a_state.hasOwningInstance, 12);
		set(a_state.hasKeywords, 13);
		set(a_state.hasOwnership, 14);
		set(a_state.hasEnableStateParent, 15);
		set(a_state.hasEnableStateChildren, 16);
		set(a_state.hasLinkedRefChildren, 17);
		set(a_state.hasObjectInstance, 18);
		set(a_state.hasWorkshopSnapping, 19);
		set(a_state.hasFurnitureEntryData, 20);
		set(a_state.hasSnapLinks, 21);
		set(a_state.hasGroupedPackin, 22);
		set(a_state.hasCalcedPackIn, 23);
		return flags;
	}

	[[nodiscard]] PopulationDiagnosticEntry MakePopulationDiagnosticEntry(
		const ReferenceDiagnosticState& a_state) noexcept
	{
		return PopulationDiagnosticEntry{
			.refID = a_state.refID,
			.baseID = a_state.baseID,
			.cellID = a_state.cellID,
			.locationID = a_state.locationID,
			.baseType = a_state.baseType,
			.stateFlags = BuildPopulationDiagnosticFlags(a_state),
			.x = a_state.x,
			.y = a_state.y,
			.z = a_state.z
		};
	}

	void LogProceduralKey(std::string_view a_kind, std::int32_t a_checkpoint, std::uint64_t a_epoch,
		std::uint32_t a_world, const PopulationDiagnosticEntry& a_entry)
	{
		const auto& key = a_entry.proceduralKey;
		REX::INFO("ProceduralKey-v1 kind={} epoch={} checkpoint={} world={:08X} ref={:08X} base={:08X} type={} cell={:08X} location={:08X} pos=({:.2f},{:.2f},{:.2f}) readable={} present={} parts=({:08X},{:08X},{:08X}) exception=0x{:08X} semanticsKnown=false ownershipProven=false readOnly=true",
			a_kind, a_epoch, a_checkpoint, a_world, a_entry.refID, a_entry.baseID, a_entry.baseType,
			a_entry.cellID, a_entry.locationID, a_entry.x, a_entry.y, a_entry.z,
			key.readable, key.present, key.key.parts[0], key.key.parts[1], key.key.parts[2], key.exception);
	}

	void LogPopulationFootprint(std::int32_t a_stage, const PopulationDiagnosticScan& a_scan)
	{
		namespace PD = ClaimablePOIs::PopulationDiagnostics;
		try {
			auto build = [](const std::vector<PopulationDiagnosticEntry>& a_entries, std::size_t& a_rejected) {
				std::vector<PD::Key> keys;
				keys.reserve(a_entries.size());
				for (const auto& entry : a_entries) {
					const auto type = static_cast<RE::FormType>(entry.baseType);
					if (type != RE::FormType::kSTAT && type != RE::FormType::kMSTT &&
						type != RE::FormType::kFURN && type != RE::FormType::kCONT && type != RE::FormType::kDOOR) {
						continue;
					}
					if ((entry.stateFlags & 3u) != 0) { // deleted/disabled are not live footprint samples
						continue;
					}
					if (auto key = PD::MakeKey(entry.baseID, entry.baseType, entry.x, entry.y, entry.z)) {
						keys.push_back(*key);
					} else {
						++a_rejected;
					}
				}
				return keys;
			};
			std::size_t rejected = 0;
			const auto before = build(g_populationDiagnosticBaseline, rejected);
			const auto after = build(a_scan.entries, rejected);
			const auto comparison = PD::Compare(before, after);
			if (!comparison || rejected != 0) {
				REX::WARN("PopulationFootprint unavailable stage={} invalidSamples={} cap={}", a_stage, rejected, PD::kSampleCap);
				return;
			}
			REX::INFO("PopulationFootprint trial=CellWorldspaceAudit-v1 stage={} worldspace={:08X} location={:08X} before={} after={} matchedBins={} unmatchedBefore={} unmatchedAfter={} binSize={} sameCellCoverage={} baselineCells={} currentCells={} readRejected={} heuristicOnly=true navmeshVerified=false",
				a_stage, g_populationDiagnosticWorldspaceID, g_populationDiagnosticLocationID,
				before.size(), after.size(), comparison->matched, comparison->unmatchedBefore.size(), comparison->unmatchedAfter.size(),
				PD::kBinSize, g_populationDiagnosticCellIDs == a_scan.cellIDs, g_populationDiagnosticCellIDs.size(), a_scan.cellIDs.size(), a_scan.stateRejected);
			constexpr std::size_t sampleCap = 16;
			for (std::size_t i = 0; i < std::min(sampleCap, comparison->unmatchedBefore.size()); ++i) {
				const auto& key = comparison->unmatchedBefore[i];
				REX::INFO("PopulationFootprintSample kind=unmatched-before stage={} base={:08X} type={} bin=({},{},{})", a_stage, key.baseID, key.type, key.x, key.y, key.z);
			}
		} catch (...) {
			REX::WARN("PopulationFootprint unavailable stage={} allocation failure", a_stage);
		}
	}

	void ClearSessionFurnitureTracking() noexcept
	{
		for (std::uint32_t index = 0; index < g_sessionFurnitureCount; ++index) {
			g_sessionFurniture[index].reset();
		}
		g_sessionFurnitureCount = 0;
		g_sessionFurnitureBeacon.reset();
	}

	[[nodiscard]] bool TrackSessionFurniture(
		RE::TESObjectREFR* a_item,
		RE::TESObjectREFR* a_beacon) noexcept
	{
		if (!a_item || !a_beacon) {
			return false;
		}
		if (g_sessionFurnitureBeacon && g_sessionFurnitureBeacon.get() != a_beacon) {
			REX::ERROR(
				"TrackSessionFurniture rejected beacon mismatch active={:08X} incoming={:08X}",
				g_sessionFurnitureBeacon->formID,
				a_beacon->formID);
			return false;
		}
		for (std::uint32_t index = 0; index < g_sessionFurnitureCount; ++index) {
			if (g_sessionFurniture[index].get() == a_item) {
				return true;
			}
		}
		if (g_sessionFurnitureCount >= g_sessionFurniture.size()) {
			REX::ERROR("TrackSessionFurniture rejected capacity overflow");
			return false;
		}
		g_sessionFurnitureBeacon.reset(a_beacon);
		g_sessionFurniture[g_sessionFurnitureCount++].reset(a_item);
		return true;
	}

	[[nodiscard]] bool DetachTrackedSessionFurniture(std::uint32_t& a_detachedCount) noexcept
	{
		a_detachedCount = 0;
		const auto trackedCount = g_sessionFurnitureCount;
		if (g_sessionFurnitureCount == 0) {
			ClearSessionFurnitureTracking();
			return true;
		}
		auto* beacon = g_sessionFurnitureBeacon.get();
		const auto gameModule = REX::FModule::GetExecutingModule();
		if (!beacon || gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244 ||
			!VerifyClaimTimeWorkshopAttachTarget(gameModule.GetBaseAddress()) ||
			!VerifyClaimTimeWorkshopPredicateTarget(gameModule.GetBaseAddress())) {
			REX::ERROR("DetachTrackedSessionFurniture rejected invalid beacon/runtime/layout");
			return false;
		}

		const auto moduleBase = gameModule.GetBaseAddress();
		bool allDetached = true;
		for (std::uint32_t index = 0; index < g_sessionFurnitureCount; ++index) {
			auto* item = g_sessionFurniture[index].get();
			if (!item || item->IsDeleted()) {
				++a_detachedCount;
				continue;
			}
			bool before = false;
			std::uint32_t predicateException = 0;
			if (!CallClaimTimeWorkshopPredicate(moduleBase, item, before, predicateException)) {
				allDetached = false;
				REX::ERROR(
					"SessionFurnitureDetach predicate failed ref={:08X} exception=0x{:08X}",
					item->formID,
					predicateException);
				continue;
			}
			if (!before) {
				++a_detachedCount;
				continue;
			}
			std::uint32_t detachException = 0;
			const bool detachCalled = CallClaimTimeWorkshopAttach(
				moduleBase, item, false, beacon, detachException);
			bool after = true;
			predicateException = 0;
			const bool predicateCalled = detachCalled && CallClaimTimeWorkshopPredicate(
				moduleBase, item, after, predicateException);
			if (!detachCalled || !predicateCalled || after) {
				allDetached = false;
				REX::ERROR(
					"SessionFurnitureDetach failed ref={:08X} detachException=0x{:08X} predicateCalled={} after={} predicateException=0x{:08X}",
					item->formID,
					detachException,
					predicateCalled,
					after,
					predicateException);
				continue;
			}
			++a_detachedCount;
		}

		if (allDetached) {
			ClearSessionFurnitureTracking();
		}
		REX::INFO(
			"SessionFurnitureDetach completed detached={}/{} success={}",
			a_detachedCount,
			trackedCount,
			allDetached);
		return allDetached;
	}

	[[nodiscard]] bool IsRuntimeOutpostCategory(RE::BGSKeyword* a_keyword) noexcept
	{
		__try {
			if (!IsRuntimeOutpostCategoryPrefix(a_keyword)) {
				return false;
			}
			const std::string_view value{ a_keyword->GetFormEditorID() };
			return value != "OutpostCategoryTestAssets" &&
			       value != "OutpostCategoryQuest" &&
			       value != "OutpostCategoryBulldoze";
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool IsConstructibleBase(RE::TESForm* a_form) noexcept
	{
		__try {
			if (!a_form) {
				return false;
			}
			return g_constructibleBaseIDs.contains(a_form->formID);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] std::uint32_t RawGetWorkshopFurnitureCompatibilityFlags(
		RE::TESObjectREFR* a_reference,
		RE::TESFormID& a_baseID)
	{
		a_baseID = 0;
		if (!a_reference) {
			return kWorkshopFurnitureProbeException;
		}
		const auto base = a_reference->GetBaseObject();
		if (!base || !(base->Is(RE::FormType::kCONT) || base->Is(RE::FormType::kFURN))) {
			return kWorkshopFurnitureProbeException;
		}
		a_baseID = base->formID;
		return a_baseID == kWorkshopExcludedNPCClipboardBaseID ||
				a_baseID == kWorkshopExcludedFlyerPerchBaseID ||
				a_baseID == kWorkshopExcludedCreatureGrazeLowBaseID ||
				a_baseID == kWorkshopExcludedCreatureGrazeBaseID ||
				a_baseID == kWorkshopExcludedCreatureGrazeTreeBaseID ?
			kWorkshopFurnitureKnownIncompatibleBase : 0;
	}

	[[nodiscard]] std::uint32_t GetWorkshopFurnitureCompatibilityFlags(
		RE::TESObjectREFR* a_reference,
		RE::TESFormID& a_baseID) noexcept
	{
		__try {
			return RawGetWorkshopFurnitureCompatibilityFlags(a_reference, a_baseID);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_baseID = 0;
			return kWorkshopFurnitureProbeException;
		}
	}

	struct WorkshopFurnitureAudit
	{
		RE::TESFormID referenceID{ 0 };
		RE::TESFormID baseID{ 0 };
		RE::TESFormID associatedFormID{ 0 };
		std::array<char, 128> editorID{};
		std::array<char, 128> displayName{};
		std::array<char, 256> modelPath{};
		std::uintptr_t nodeAddress{ 0 };
		float worldBoundRadius{ 0.0F };
		std::uint32_t markerCount{ 0 };
		std::uint32_t entryPointCount{ 0 };
		std::uint32_t workbenchType{ 0 };
		std::uint32_t furnitureFlags{ 0 };
		std::uint32_t safetyFlags{ 0 };
		bool loadedData{ false };
		bool loaded3D{ false };
		bool collision{ false };
	};

	template <std::size_t N>
	void CopyWorkshopAuditText(std::array<char, N>& a_destination, const char* a_source) noexcept
	{
		if (a_source) {
			(void)strncpy_s(a_destination.data(), a_destination.size(), a_source, _TRUNCATE);
		}
	}

	[[nodiscard]] bool RawCollectWorkshopFurnitureAudit(
		RE::TESObjectREFR* a_item,
		WorkshopFurnitureAudit& a_audit)
	{
		a_audit = {};
		if (!a_item) {
			return false;
		}
		a_audit.referenceID = a_item->formID;
		const auto base = a_item->GetBaseObject();
		if (!base || !base->Is(RE::FormType::kFURN)) {
			return false;
		}
		a_audit.baseID = base->formID;
		auto* furniture = static_cast<RE::TESFurniture*>(base.get());
		CopyWorkshopAuditText(a_audit.editorID, furniture->GetFormEditorID());
		CopyWorkshopAuditText(a_audit.displayName, a_item->GetDisplayFullName());
		CopyWorkshopAuditText(a_audit.modelPath, furniture->model.c_str());
		a_audit.markerCount = static_cast<std::uint32_t>(furniture->markersArray.size());
		a_audit.entryPointCount = static_cast<std::uint32_t>(furniture->entryPointDataArray.size());
		a_audit.workbenchType = static_cast<std::uint32_t>(furniture->workBenchData.type);
		a_audit.furnitureFlags = furniture->furnFlags;
		a_audit.associatedFormID = furniture->associatedForm ? furniture->associatedForm->formID : 0;

		RE::TESFormID safetyReferenceID = 0;
		RE::TESFormID safetyBaseID = 0;
		if (!SafeCollectWorkshopReplacementSafetyFlags(
				a_item, a_audit.safetyFlags, safetyReferenceID, safetyBaseID)) {
			a_audit.safetyFlags |= kWorkshopReplacementProbeException;
		}

		auto loadedGuard = a_item->loadedData.LockRead();
		auto* loaded = *loadedGuard;
		a_audit.loadedData = loaded != nullptr;
		auto* node = loaded ? loaded->data3D.get() : nullptr;
		a_audit.loaded3D = node != nullptr;
		if (node) {
			a_audit.nodeAddress = reinterpret_cast<std::uintptr_t>(node);
			a_audit.collision = node->collisionObject != nullptr;
			a_audit.worldBoundRadius = node->worldBound.radius;
		}
		return true;
	}

	[[nodiscard]] bool SafeCollectWorkshopFurnitureAudit(
		RE::TESObjectREFR* a_item,
		WorkshopFurnitureAudit& a_audit) noexcept
	{
		__try {
			return RawCollectWorkshopFurnitureAudit(a_item, a_audit);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_audit = {};
			return false;
		}
	}

	struct ActiveWorkshopExtraTelemetry
	{
		bool inspectionCompleted{ false };
		bool returnedNonNull{ false };
		std::uint32_t actualType{ 0 };
		std::uint32_t expectedType{ static_cast<std::uint32_t>(RE::ExtraDataType::kWorkshop) };
		std::uintptr_t actualVtable{ 0 };
		std::uintptr_t expectedVtable{ 0 };
		std::uintptr_t itemList{ 0 };
		std::uintptr_t listedWorkshop{ 0 };
		bool membershipEqual{ false };
		std::uint32_t inspectionException{ 0 };
	};

	[[nodiscard]] bool InspectActiveWorkshopExtra(
		RE::TESObjectREFR* a_item,
		RE::BSExtraData* a_returnedExtra,
		ActiveWorkshopExtraTelemetry& a_telemetry) noexcept
	{
		a_telemetry = {};
		a_telemetry.expectedType = static_cast<std::uint32_t>(RE::ExtraDataType::kWorkshop);
		a_telemetry.expectedVtable = REL::Relocation<std::uintptr_t>{ RE::VTABLE::Workshop__ExtraData[0] }.address();
		a_telemetry.returnedNonNull = a_returnedExtra != nullptr;
		a_telemetry.itemList = reinterpret_cast<std::uintptr_t>(a_item ? a_item->extraDataList.get() : nullptr);
		if (a_returnedExtra) {
			a_telemetry.actualType = static_cast<std::uint32_t>(a_returnedExtra->type.underlying());
			a_telemetry.actualVtable = *reinterpret_cast<std::uintptr_t*>(a_returnedExtra);
		}
		if (a_item && a_item->extraDataList) {
			a_telemetry.listedWorkshop = reinterpret_cast<std::uintptr_t>(
				a_item->extraDataList->GetByType(RE::ExtraDataType::kWorkshop));
		}
		a_telemetry.membershipEqual = a_telemetry.returnedNonNull &&
			a_telemetry.listedWorkshop != 0 &&
			a_telemetry.listedWorkshop == reinterpret_cast<std::uintptr_t>(a_returnedExtra);
		a_telemetry.inspectionCompleted = true;
		return true;
	}

	[[nodiscard]] bool ValidateActiveWorkshopExtra(
		RE::TESObjectREFR* a_item,
		RE::BSExtraData* a_returnedExtra,
		ActiveWorkshopExtraTelemetry& a_telemetry) noexcept
	{
		__try {
			return InspectActiveWorkshopExtra(a_item, a_returnedExtra, a_telemetry);
		} __except (a_telemetry.inspectionException = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool IsMeaningfulActiveWorkshopExtra(
		const ActiveWorkshopExtraTelemetry& a_telemetry) noexcept
	{
		return a_telemetry.inspectionCompleted &&
			a_telemetry.inspectionException == 0 &&
			a_telemetry.returnedNonNull &&
			a_telemetry.actualType == a_telemetry.expectedType &&
			a_telemetry.actualVtable != 0 &&
			a_telemetry.actualVtable == a_telemetry.expectedVtable &&
			a_telemetry.listedWorkshop != 0 &&
			a_telemetry.membershipEqual;
	}

	[[nodiscard]] bool CallActiveWorkshopEnsure(
		std::uintptr_t a_moduleBase,
		RE::TESObjectREFR* a_item,
		RE::BSExtraData*& a_result,
		std::uint32_t& a_exceptionCode) noexcept
	{
		a_result = nullptr;
		a_exceptionCode = 0;
		if (!a_item) {
			return false;
		}

		__try {
			a_result = reinterpret_cast<ActiveWorkshopEnsureFunction>(
				a_moduleBase + kActiveWorkshopEnsureRva)(a_item, true);
			return true;
		} __except (a_exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool VerifyActiveWorkshopEnsureTarget(std::uintptr_t a_moduleBase) noexcept
	{
		__try {
			return std::memcmp(
				reinterpret_cast<const void*>(a_moduleBase + kActiveWorkshopEnsureRva),
				kActiveWorkshopEnsurePrefix.data(),
				kActiveWorkshopEnsurePrefix.size()) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	struct WorkshopComparisonTelemetry
	{
		bool eligibilityPrefixMatched{ false };
		bool eligibilityCalled{ false };
		bool eligibilityResult{ false };
		std::uint32_t eligibilityException{ 0 };
		std::uintptr_t filter{ 0 };
		std::uint32_t filterType{ 0 };
		RE::TESFormID filterFormID{ 0 };
		bool filterApplied{ false };
		RE::TESFormID baseFormID{ 0 };
		bool baseKeywordChecked{ false };
		bool baseKeywordResult{ false };
		bool predicatePrefixMatched{ false };
		bool predicateCalled{ false };
		bool predicateResult{ false };
		std::uint32_t predicateException{ 0 };
		ActiveWorkshopExtraTelemetry workshopExtra{};
		bool workshopInspectionCompleted{ false };
	};

	[[nodiscard]] bool VerifyActiveWorkshopEligibilityTarget(std::uintptr_t a_moduleBase) noexcept
	{
		__try {
			return std::memcmp(
				reinterpret_cast<const void*>(a_moduleBase + kActiveWorkshopEligibilityRva),
				kActiveWorkshopEligibilityPrefix.data(),
				kActiveWorkshopEligibilityPrefix.size()) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	void CollectBaseWorkshopKeywordTelemetry(
		RE::TESObjectREFR* a_item,
		RE::BGSKeyword* a_keyword,
		WorkshopComparisonTelemetry& a_telemetry)
	{
		const auto base = a_item ? a_item->GetBaseObject() : nullptr;
		if (auto* furniture = base ? base->As<RE::TESFurniture>() : nullptr) {
			a_telemetry.baseFormID = furniture->formID;
			a_telemetry.baseKeywordResult = static_cast<RE::BGSKeywordForm*>(furniture)->HasKeyword(a_keyword);
			a_telemetry.baseKeywordChecked = true;
		}
	}

	[[nodiscard]] bool CallActiveWorkshopEligibility(
		std::uintptr_t a_moduleBase,
		RE::TESObjectREFR* a_item,
		WorkshopComparisonTelemetry& a_telemetry) noexcept
	{
		a_telemetry.eligibilityCalled = false;
		a_telemetry.eligibilityResult = false;
		a_telemetry.eligibilityException = 0;
		if (!a_item) {
			return false;
		}

		__try {
			auto* filter = *reinterpret_cast<void**>(
				a_moduleBase + kActiveWorkshopEligibilityFilterRva);
			a_telemetry.filter = reinterpret_cast<std::uintptr_t>(filter);
			if (filter) {
				a_telemetry.filterType = *(reinterpret_cast<std::uint8_t*>(filter) + 0x2E);
			}
			a_telemetry.filterApplied = filter && a_telemetry.filterType == 0x04;
			if (a_telemetry.filterApplied) {
				auto* keyword = reinterpret_cast<RE::BGSKeyword*>(filter);
				a_telemetry.filterFormID = keyword->formID;
				CollectBaseWorkshopKeywordTelemetry(a_item, keyword, a_telemetry);
			}
			a_telemetry.eligibilityResult = reinterpret_cast<ActiveWorkshopEligibilityFunction>(
				a_moduleBase + kActiveWorkshopEligibilityRva)(
				a_item,
				a_telemetry.filterApplied ? filter : nullptr);
			a_telemetry.eligibilityCalled = true;
			return true;
		} __except (a_telemetry.eligibilityException = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool CollectListedWorkshopExtraTelemetry(
		RE::TESObjectREFR* a_item,
		ActiveWorkshopExtraTelemetry& a_telemetry) noexcept
	{
		__try {
			auto* listed = a_item && a_item->extraDataList ?
				a_item->extraDataList->GetByType(RE::ExtraDataType::kWorkshop) : nullptr;
			return InspectActiveWorkshopExtra(a_item, listed, a_telemetry);
		} __except (a_telemetry.inspectionException = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	void LogWorkshopComparisonTelemetry(RE::TESObjectREFR* a_item, std::int32_t a_stage) noexcept
	{
		WorkshopComparisonTelemetry telemetry{};
		const auto gameModule = REX::FModule::GetExecutingModule();
		const bool runtimeMatched = gameModule.GetFileVersion() == SFSE::RUNTIME_SF_1_16_244;
		const auto moduleBase = gameModule.GetBaseAddress();
		telemetry.eligibilityPrefixMatched = runtimeMatched &&
			VerifyActiveWorkshopEligibilityTarget(moduleBase);
		telemetry.predicatePrefixMatched = runtimeMatched &&
			VerifyClaimTimeWorkshopPredicateTarget(moduleBase);
		if (telemetry.eligibilityPrefixMatched) {
			(void)CallActiveWorkshopEligibility(moduleBase, a_item, telemetry);
		}
		if (telemetry.predicatePrefixMatched) {
			telemetry.predicateCalled = CallClaimTimeWorkshopPredicate(
				moduleBase,
				a_item,
				telemetry.predicateResult,
				telemetry.predicateException);
		}
		telemetry.workshopInspectionCompleted = CollectListedWorkshopExtraTelemetry(
			a_item,
			telemetry.workshopExtra);

		REX::INFO(
			"WorkshopCompare stage={} ref={:08X} runtimeMatched={} eligibilityPrefixMatched={} filter=0x{:X} filterType=0x{:02X} filterFormID={:08X} filterApplied={} baseFormID={:08X} baseKeywordChecked={} baseKeywordResult={} eligibilityCalled={} eligibilityResult={} eligibilityException=0x{:08X} predicatePrefixMatched={} predicateCalled={} predicateResult={} predicateException=0x{:08X} workshopInspectionCompleted={} itemList=0x{:X} listedWorkshop=0x{:X} workshopType=0x{:02X} workshopVtable=0x{:X} expectedVtable=0x{:X} membershipEqual={} inspectionException=0x{:08X}",
			a_stage,
			a_item ? a_item->formID : 0,
			runtimeMatched,
			telemetry.eligibilityPrefixMatched,
			telemetry.filter,
			telemetry.filterType,
			telemetry.filterFormID,
			telemetry.filterApplied,
			telemetry.baseFormID,
			telemetry.baseKeywordChecked,
			telemetry.baseKeywordResult,
			telemetry.eligibilityCalled,
			telemetry.eligibilityResult,
			telemetry.eligibilityException,
			telemetry.predicatePrefixMatched,
			telemetry.predicateCalled,
			telemetry.predicateResult,
			telemetry.predicateException,
			telemetry.workshopInspectionCompleted,
			telemetry.workshopExtra.itemList,
			telemetry.workshopExtra.listedWorkshop,
			telemetry.workshopExtra.actualType,
			telemetry.workshopExtra.actualVtable,
			telemetry.workshopExtra.expectedVtable,
			telemetry.workshopExtra.membershipEqual,
			telemetry.workshopExtra.inspectionException);
	}

	void ClearClaimTimeWorkshopEnrollment() noexcept
	{
		for (auto& candidate : g_claimTimeEnrollmentCandidates) {
			candidate.reset();
		}
		g_claimTimeEnrollmentCandidateCount = 0;
		g_claimTimeEnrollmentBeacon.reset();
		g_claimTimeEnrollmentHelper.reset();
		g_claimTimeEnrollmentCell.reset();
		g_claimTimeEnrollmentStatus = 0;
	}

	struct ClaimTimeWorkshopScan
	{
		RE::TESObjectREFR* beacon{ nullptr };
		RE::TESObjectREFR* player{ nullptr };
		RE::TESWorldSpace* expectedWorldspace{ nullptr };
		RE::BGSLocation* expectedLocation{ nullptr };
		std::array<RE::NiPointer<RE::TESObjectREFR>, kBatchEnrollmentMaxItems> candidates{};
		std::uint32_t candidateCount{ 0 };
		std::uint32_t scanned{ 0 };
		std::uint32_t eligible{ 0 };
		std::uint32_t rejected{ 0 };
		std::uint32_t furnCount{ 0 };
		std::uint32_t contCount{ 0 };
		std::uint32_t outpostBuildableCount{ 0 };
		std::uint32_t nonEligibleRejected{ 0 };
		std::uint32_t firstNonFurnContFormType{ 0 };
		std::uint32_t firstNonFurnContBaseID{ 0 };
		bool truncated{ false };
		bool layoutRejected{ false };
		bool readRejected{ false };
	};

	[[nodiscard]] bool SafeRollbackClaimTimeWorkshopEnrollment() noexcept;
	[[nodiscard]] bool VerifyWorkshopCommitTarget(std::uintptr_t a_target) noexcept;
	[[nodiscard]] bool ReadWorkshopCommitContextBeacon(
		void* a_context,
		RE::TESObjectREFR*& a_beacon,
		std::uint32_t& a_exceptionCode) noexcept;
	[[nodiscard]] bool CallWorkshopCommit(
		std::uintptr_t a_target,
		void* a_context,
		RE::TESObjectREFR* a_item,
		std::uint32_t& a_exceptionCode) noexcept;

	[[nodiscard]] bool IsClaimTimeWorkshopCandidate(
		RE::TESObjectREFR* a_reference,
		RE::TESObjectCELL* a_cell,
		ClaimTimeWorkshopScan& a_scan) noexcept
	{
		if (!a_reference || a_reference == a_scan.beacon || a_reference == a_scan.player ||
			a_reference->parentCell != a_cell || a_reference->IsDeleted() || a_reference->IsDisabled()) {
			return false;
		}

		const auto base = a_reference->GetBaseObject();
		if (!base) {
			return false;
		}
		const bool isCont = base->Is(RE::FormType::kCONT);
		const bool isFurn = base->Is(RE::FormType::kFURN);
		const bool isBuildable = IsConstructibleBase(base.get());
		if (!isCont && !isFurn && !isBuildable) {
			if (a_scan.nonEligibleRejected == 0) {
				a_scan.firstNonFurnContFormType = static_cast<std::uint32_t>(base->GetFormType());
				a_scan.firstNonFurnContBaseID = base->formID;
			}
			++a_scan.nonEligibleRejected;
			return false;
		}
		if (!a_scan.expectedWorldspace ||
			a_reference->GetParentWorldSpace() != a_scan.expectedWorldspace) {
			return false;
		}

		if (base->Is(RE::FormType::kFURN)) {
			++a_scan.furnCount;
		} else if (isCont) {
			++a_scan.contCount;
		} else {
			++a_scan.outpostBuildableCount;
		}
		return true;
	}

	[[nodiscard]] bool SafeReadClaimTimeCellReferenceHeader(
		RE::TESObjectCELL* a_cell,
		std::uint32_t& a_size,
		std::uint32_t& a_capacity,
		const RE::NiPointer<RE::TESObjectREFR>*& a_data) noexcept
	{
		a_size = 0;
		a_capacity = 0;
		a_data = nullptr;
		__try {
			const auto base = reinterpret_cast<std::uintptr_t>(a_cell);
			a_size = *reinterpret_cast<const std::uint32_t*>(base + kClaimTimeCellReferencesSizeOffset);
			a_capacity = *reinterpret_cast<const std::uint32_t*>(base + kClaimTimeCellReferencesCapacityOffset);
			a_data = *reinterpret_cast<const RE::NiPointer<RE::TESObjectREFR> *const*>(base + kClaimTimeCellReferencesDataOffset);
			return a_size <= a_capacity && (a_size == 0 || a_data != nullptr);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_size = 0;
			a_capacity = 0;
			a_data = nullptr;
			return false;
		}
	}

	[[nodiscard]] bool SafeReadClaimTimeCellReferenceSlot(
		const RE::NiPointer<RE::TESObjectREFR>* a_data,
		std::uint32_t a_index,
		RE::NiPointer<RE::TESObjectREFR>& a_reference) noexcept
	{
		a_reference.reset();
		__try {
			a_reference = a_data[a_index];
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			// The assignment can have copied a malformed pointer before its
			// retain operation faulted. Clear the pointer representation so the
			// caller cannot dereference or release it after a failed read.
			std::memset(&a_reference, 0, sizeof(a_reference));
			return false;
		}
	}

	[[nodiscard]] bool SafeIsClaimTimeReferenceInRange(
		RE::TESObjectREFR* a_reference,
		const RE::NiPoint3A& a_origin,
		float a_squaredRadius) noexcept
	{
		__try {
			return a_reference &&
			       a_origin.GetSquaredDistance(a_reference->GetPosition()) <= a_squaredRadius;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool SafeIsClaimTimeWorkshopCandidate(
		RE::TESObjectREFR* a_reference,
		RE::TESObjectCELL* a_cell,
		ClaimTimeWorkshopScan& a_scan) noexcept
	{
		__try {
			return IsClaimTimeWorkshopCandidate(a_reference, a_cell, a_scan);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	struct ClaimTimeCandidateValidation
	{
		const char* reason{ "unknown" };
		std::uint32_t safetyFlags{ 0 };
		RE::TESFormID cellID{ 0 };
		RE::TESFormID baseID{ 0 };
		RE::TESFormID candidateLocationID{ 0 };
		RE::TESFormID expectedLocationID{ 0 };
		bool locationMatched{ false };
	};

	[[nodiscard]] RE::TESFormID SafeGetClaimTimeCandidateID(RE::TESObjectREFR* a_reference) noexcept
	{
		if (!a_reference) {
			return 0;
		}
		__try {
			return a_reference->formID;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	[[nodiscard]] bool RawValidateClaimTimeBeacon(
		RE::TESObjectREFR* a_beacon,
		RE::TESObjectREFR* a_helper,
		RE::BGSLocation* a_claimLocation,
		ClaimTimeCandidateValidation& a_validation) noexcept
	{
		a_validation = {};
		if (!a_beacon) {
				a_validation.reason = "null";
				return false;
		}
		a_validation.cellID = a_beacon->parentCell ? a_beacon->parentCell->formID : 0;
		if (a_beacon->IsDeleted()) {
				a_validation.reason = "deleted";
				return false;
		}
		if (a_beacon->IsDisabled()) {
				a_validation.reason = "disabled";
				return false;
		}
		const auto base = a_beacon->GetBaseObject();
		a_validation.baseID = base ? base->formID : 0;
		if (!base || base->formID != kClaimTimeVanillaBeaconBaseID) {
				a_validation.reason = "base-not-vanilla-beacon";
				return false;
		}
		if (!a_beacon->parentCell) {
				a_validation.reason = "no-parent-cell";
				return false;
		}
		if (!a_beacon->parentCell->IsAttached()) {
				a_validation.reason = "parent-cell-not-attached";
				return false;
		}

		const auto helperWorldspace = a_helper ? a_helper->GetParentWorldSpace() : nullptr;
		const auto beaconWorldspace = a_beacon->GetParentWorldSpace();
		const auto helperLocation = a_helper ? a_helper->GetCurrentLocation() : nullptr;
		const auto beaconLocation = a_beacon->GetCurrentLocation();
		if (!helperWorldspace || !helperLocation || !a_claimLocation || !beaconWorldspace || !beaconLocation) {
				a_validation.reason = "claim-context-missing";
				return false;
		}
		if (beaconWorldspace != helperWorldspace) {
				a_validation.reason = "worldspace-mismatch";
				return false;
		}
		if (helperLocation != a_claimLocation || beaconLocation != helperLocation) {
				a_validation.reason = "location-mismatch";
				return false;
		}
		a_validation.reason = "accepted";
		return true;
	}

	[[nodiscard]] bool SafeValidateClaimTimeBeacon(
		RE::TESObjectREFR* a_beacon,
		RE::TESObjectREFR* a_helper,
		RE::BGSLocation* a_claimLocation,
		ClaimTimeCandidateValidation& a_validation) noexcept
	{
		__try {
			return RawValidateClaimTimeBeacon(a_beacon, a_helper, a_claimLocation, a_validation);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_validation = {};
			a_validation.reason = "exception";
			return false;
		}
	}

	[[nodiscard]] bool RawValidateClaimTimeWorkshopCandidate(
		RE::TESObjectREFR* a_reference,
		RE::TESObjectREFR* a_helper,
		RE::TESObjectREFR* a_beacon,
		RE::TESObjectREFR* a_player,
		RE::TESWorldSpace* a_expectedWorldspace,
		RE::BGSLocation* a_expectedLocation,
		float a_radius,
		ClaimTimeCandidateValidation& a_validation) noexcept
	{
		a_validation = {};
		if (!a_reference) {
				a_validation.reason = "null";
				return false;
		}
		a_validation.cellID = a_reference->parentCell ? a_reference->parentCell->formID : 0;
		if (a_reference == a_beacon) {
				a_validation.reason = "beacon";
				return false;
		}
		if (a_reference == a_player) {
				a_validation.reason = "player";
				return false;
		}
		if (a_reference->IsDeleted()) {
				a_validation.reason = "deleted";
				return false;
		}
		if (a_reference->IsDisabled()) {
				a_validation.reason = "disabled";
				return false;
		}
		if (!a_reference->parentCell) {
				a_validation.reason = "no-parent-cell";
				return false;
		}
		if (!a_reference->parentCell->IsAttached()) {
				a_validation.reason = "parent-cell-not-attached";
				return false;
		}

		const auto base = a_reference->GetBaseObject();
		a_validation.baseID = base ? base->formID : 0;
		if (!base || !(base->Is(RE::FormType::kCONT) || base->Is(RE::FormType::kFURN) || IsConstructibleBase(base.get()))) {
				a_validation.reason = "base-not-cont-furn-or-outpost-buildable";
				return false;
		}
		if (!a_expectedWorldspace || !a_expectedLocation) {
				a_validation.reason = "claim-context-missing";
				return false;
		}
		const auto helperWorldspace = a_helper ? a_helper->GetParentWorldSpace() : nullptr;
		const auto candidateWorldspace = a_reference->GetParentWorldSpace();
		const auto beaconWorldspace = a_beacon ? a_beacon->GetParentWorldSpace() : nullptr;
		if (!helperWorldspace || !candidateWorldspace || !beaconWorldspace ||
				candidateWorldspace != a_expectedWorldspace || candidateWorldspace != helperWorldspace ||
				candidateWorldspace != beaconWorldspace) {
				a_validation.reason = "worldspace-mismatch";
				return false;
		}

		const auto helperLocation = a_helper ? a_helper->GetCurrentLocation() : nullptr;
		const auto candidateLocation = a_reference->GetCurrentLocation();
		const auto beaconLocation = a_beacon ? a_beacon->GetCurrentLocation() : nullptr;
		a_validation.candidateLocationID = candidateLocation ? candidateLocation->formID : 0;
		a_validation.expectedLocationID = a_expectedLocation ? a_expectedLocation->formID : 0;
		a_validation.locationMatched = candidateLocation && candidateLocation == a_expectedLocation;
		if (!helperLocation || !beaconLocation || helperLocation != a_expectedLocation || beaconLocation != helperLocation) {
			a_validation.reason = "location-mismatch";
			return false;
		}
		// Inherited POI furniture may retain its source Location after the
		// genuine beacon receives the claimed Location.  The helper/beacon
		// relationship remains authoritative; candidate Location is telemetry,
		// not an enrollment rejection.

		if (!std::isfinite(a_radius) || a_radius <= 0.0F || a_radius > 10000.0F) {
			a_validation.reason = "invalid-radius";
			return false;
		}

		const auto helperPosition = a_helper->GetPosition();
		const auto candidatePosition = a_reference->GetPosition();
		const RE::NiPoint3A helperOrigin{ helperPosition.x, helperPosition.y, helperPosition.z };
		const RE::NiPoint3A candidateOrigin{ candidatePosition.x, candidatePosition.y, candidatePosition.z };
		const float squaredRadius = a_radius * a_radius;
		if (!std::isfinite(squaredRadius) || helperOrigin.GetSquaredDistance(candidateOrigin) > squaredRadius) {
			a_validation.reason = "outside-radius";
			return false;
		}

		RE::TESFormID referenceID = 0;
		if (!SafeCollectWorkshopReplacementSafetyFlags(a_reference, a_validation.safetyFlags, referenceID, a_validation.baseID)) {
			a_validation.reason = "safety-flags";
			return false;
		}
		// Replacement hides one object and substitutes another, so its broad
		// carrier mask is intentionally stricter than enrollment. Enrollment admits
		// any live FURN in the exact claimed context, including linked, owned,
		// pack-in, and non-promoted furniture. Workshop ownership is opaque; any
		// existing node fails closed before mutation.
		const bool alreadyEnrolled = HasClaimTimeWorkshopState(a_reference);
		const auto rejectedSafetyFlags = a_validation.safetyFlags & kWorkshopEnrollmentHardSafetyFlags;
		if (rejectedSafetyFlags != 0) {
			a_validation.safetyFlags = rejectedSafetyFlags;
			a_validation.reason = alreadyEnrolled ? "already-workshop" : "safety-flags";
			return false;
		}
		a_validation.reason = "accepted";
		return true;
	}

	[[nodiscard]] bool SafeValidateClaimTimeWorkshopCandidate(
		RE::TESObjectREFR* a_reference,
		RE::TESObjectREFR* a_helper,
		RE::TESObjectREFR* a_beacon,
		RE::TESObjectREFR* a_player,
		RE::TESWorldSpace* a_expectedWorldspace,
		RE::BGSLocation* a_expectedLocation,
		float a_radius,
		ClaimTimeCandidateValidation& a_validation) noexcept
	{
		__try {
			return RawValidateClaimTimeWorkshopCandidate(a_reference, a_helper, a_beacon, a_player, a_expectedWorldspace, a_expectedLocation, a_radius, a_validation);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_validation = {};
			a_validation.reason = "exception";
			return false;
		}
	}

	[[nodiscard]] bool SafeHasUnaliasedContainerExtras(RE::TESObjectREFR* a_item) noexcept
	{
		__try {
			// Alias extras stay excluded. Missing promotion is handled by the same
			// verified engine promotion used by the FURN transaction, not a gate.
			const auto* extra = a_item ? a_item->extraDataList.get() : nullptr;
			return extra &&
				!extra->HasType(RE::ExtraDataType::kAliasInstanceArray) &&
				!extra->HasType(RE::ExtraDataType::kFromAlias) &&
				!extra->HasType(RE::ExtraDataType::kLinkedChildrenAliasInstanceArray);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	void RawScanClaimTimeCellReferences(
		RE::TESObjectCELL* a_cell,
		const RE::NiPoint3A& a_origin,
		float a_radius,
		ClaimTimeWorkshopScan& a_scan) noexcept
	{
		const auto base = reinterpret_cast<std::uintptr_t>(a_cell);
		a_scan.layoutRejected = false;
		a_scan.readRejected = false;
		RE::BSReadWriteLock* lock = reinterpret_cast<RE::BSReadWriteLock*>(base + kClaimTimeCellReferencesLockOffset);
		RE::BSAutoReadLock lockGuard(*lock);

		std::uint32_t size = 0;
		std::uint32_t capacity = 0;
		const RE::NiPointer<RE::TESObjectREFR>* data = nullptr;
		if (!SafeReadClaimTimeCellReferenceHeader(a_cell, size, capacity, data)) {
			a_scan.layoutRejected = true;
			REX::WARN("ClaimTimeEnrollment raw cell layout rejected cell={:08X}", a_cell->formID);
			return;
		}
		if (size > kClaimTimeCellReferenceRawSlotCap || capacity > kClaimTimeCellReferenceRawSlotCap) {
			a_scan.layoutRejected = true;
			REX::WARN("ClaimTimeEnrollment raw cell layout rejected: slot bound cell={:08X} size={} capacity={} bound={}", a_cell->formID, size, capacity, kClaimTimeCellReferenceRawSlotCap);
			return;
		}

		const auto squaredRadius = a_radius * a_radius;
		for (std::uint32_t index = 0; index < size; ++index) {
			RE::NiPointer<RE::TESObjectREFR> reference;
			if (!SafeReadClaimTimeCellReferenceSlot(data, index, reference)) {
				a_scan.readRejected = true;
				REX::WARN("ClaimTimeEnrollment raw cell slot read rejected cell={:08X} index={} size={} capacity={}", a_cell->formID, index, size, capacity);
				return;
			}
			if (!reference || !SafeIsClaimTimeReferenceInRange(reference.get(), a_origin, squaredRadius)) {
				continue;
			}
			if (a_scan.scanned >= kClaimTimeReferenceScanCap) {
				a_scan.truncated = true;
				return;
			}
			++a_scan.scanned;
			if (!SafeIsClaimTimeWorkshopCandidate(reference.get(), a_cell, a_scan)) {
				++a_scan.rejected;
				continue;
			}
			if (a_scan.candidateCount >= kBatchEnrollmentMaxItems) {
				a_scan.truncated = true;
				return;
			}
			++a_scan.eligible;
		// The fixed array keeps this noexcept scanner allocation-free. The
		// NiPointer retains the reference while the cell read lock is held.
			a_scan.candidates[a_scan.candidateCount++] = std::move(reference);
		}
	}

	[[nodiscard]] bool SafeGetClaimTimeCellState(
		RE::TESObjectCELL* a_cell,
		bool& a_attached,
		bool& a_exterior) noexcept
	{
		a_attached = false;
		a_exterior = false;
		if (!a_cell) {
			return false;
		}
		__try {
			a_attached = a_cell->IsAttached();
			a_exterior = a_cell->IsExterior();
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool SafeGetClaimTimeCellWorldspace(
		RE::TESObjectCELL* a_cell,
		RE::TESWorldSpace* a_expected,
		RE::TESWorldSpace*& a_matched) noexcept
	{
		a_matched = nullptr;
		if (!a_cell || !a_expected) {
			return false;
		}
		__try {
			const auto base = reinterpret_cast<std::uintptr_t>(a_cell);
			const auto runtimeWorldspace = *reinterpret_cast<RE::TESWorldSpace* const*>(base + kClaimTimeCellWorldspaceOffset);
			if (runtimeWorldspace == a_expected) {
				a_matched = runtimeWorldspace;
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_matched = nullptr;
			return false;
		}
	}

	[[nodiscard]] bool SafeFormListContainsReference(
        RE::BGSListForm* a_list, RE::TESObjectREFR* a_reference, bool& a_valid) noexcept
    {
        if (!a_list || !a_reference) { a_valid = true; return false; }
        return ClaimablePOIs::TeardownProtection::Contains(a_list,a_reference->formID,a_valid);
    }

	void ScanClaimTimeCellReferences(
		RE::TESObjectCELL* a_cell,
		const RE::NiPoint3A& a_origin,
		float a_radius,
		ClaimTimeWorkshopScan& a_scan) noexcept
	{
		if (!a_cell || !std::isfinite(a_radius) || a_radius <= 0.0F || a_radius > 10000.0F) {
			a_scan.layoutRejected = true;
			return;
		}
		// This wrapper has no C++ objects requiring unwinding. Keep the SEH
		// boundary outside RawScan's lock-owning RAII frame so MSVC accepts the
		// function while still converting any cell-layout/access fault to a
		// fail-closed read rejection.
		__try {
			RawScanClaimTimeCellReferences(a_cell, a_origin, a_radius, a_scan);
		} __except (a_scan.readRejected = true, EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	[[nodiscard]] bool CollectClaimTimeWorkshopCandidates(
		RE::TESObjectREFR* a_beacon,
		RE::TESObjectCELL* a_claimCell,
		const std::vector<RE::TESObjectCELL*>& a_loadedCells,
		float a_radius,
		ClaimTimeWorkshopScan& a_scan) noexcept
	{
		bool claimCellAttached = false;
		bool claimCellExterior = false;
		if (!a_beacon || !a_claimCell || a_loadedCells.empty() ||
			a_loadedCells.size() > kClaimTimeLoadedCellCap ||
			!SafeGetClaimTimeCellState(a_claimCell, claimCellAttached, claimCellExterior) ||
			!claimCellAttached || !claimCellExterior || !std::isfinite(a_radius) ||
			a_radius <= 0.0F || a_radius > 10000.0F) {
			return false;
		}

		const auto beaconPosition = a_beacon->GetPosition();
		const RE::NiPoint3A origin{ beaconPosition.x, beaconPosition.y, beaconPosition.z };
		std::array<RE::NiPointer<RE::TESObjectCELL>, kClaimTimeLoadedCellCap> cells{};
		std::uint32_t cellCount = 0;
		bool claimCellPresent = false;
		for (auto* candidateCell : a_loadedCells) {
			if (!candidateCell) {
				return false;
			}
			bool duplicate = false;
			for (std::uint32_t index = 0; index < cellCount; ++index) {
				if (cells[index].get() == candidateCell) {
					duplicate = true;
					break;
				}
			}
			if (duplicate) {
				continue;
			}
			bool attached = false;
			bool exterior = false;
			if (!SafeGetClaimTimeCellState(candidateCell, attached, exterior)) {
				return false;
			}
			if (!attached || !exterior) {
				continue;
			}
			if (candidateCell == a_claimCell) {
				claimCellPresent = true;
			}
			cells[cellCount++].reset(candidateCell);
		}
		if (!claimCellPresent || cellCount == 0) {
			return false;
		}

		a_scan.beacon = a_beacon;
		a_scan.player = RE::PlayerCharacter::GetSingleton();
		a_scan.expectedWorldspace = a_beacon->GetParentWorldSpace();
		a_scan.expectedLocation = a_beacon->GetCurrentLocation();
		if (!a_scan.expectedWorldspace || !a_scan.expectedLocation) {
			return false;
		}
		for (std::uint32_t index = 0; index < cellCount; ++index) {
			ScanClaimTimeCellReferences(cells[index].get(), origin, a_radius, a_scan);
			if (a_scan.layoutRejected || a_scan.readRejected || a_scan.truncated) {
				return false;
			}
		}
		return true;
	}

	void ProbeContainerWorkshopCandidates(
		RE::TESObjectREFR* a_beacon,
		RE::TESObjectCELL* a_claimCell,
		const std::vector<RE::TESObjectCELL*>& a_loadedCells,
		float a_radius,
		std::uintptr_t a_moduleBase) noexcept
	{
		if (!a_beacon || !a_claimCell || a_loadedCells.empty() || a_loadedCells.size() > kClaimTimeLoadedCellCap) {
			return;
		}
		const auto beaconPosition = a_beacon->GetPosition();
		const RE::NiPoint3A origin{ beaconPosition.x, beaconPosition.y, beaconPosition.z };
		const auto squaredRadius = a_radius * a_radius;
		const auto expectedWorldspace = a_beacon->GetParentWorldSpace();
		if (!expectedWorldspace) {
			return;
		}

		std::uint32_t contScanned = 0;
		std::uint32_t contInRange = 0;
		std::uint32_t contAttachOk = 0;
		std::uint32_t contPredicateTrue = 0;
		std::uint32_t contAttachFailed = 0;
		std::uint32_t contPredicateFailed = 0;
		RE::TESFormID firstContRef = 0;
		RE::TESFormID firstContBase = 0;
		RE::TESFormID firstAttachOkRef = 0;
		RE::TESFormID firstAttachOkBase = 0;
		RE::TESFormID firstPredTrueRef = 0;
		RE::TESFormID firstPredTrueBase = 0;

		for (auto* cell : a_loadedCells) {
			if (!cell) {
				continue;
			}
			bool attached = false;
			bool exterior = false;
			if (!SafeGetClaimTimeCellState(cell, attached, exterior) || !attached || !exterior) {
				continue;
			}
			std::uint32_t size = 0;
			std::uint32_t capacity = 0;
			const RE::NiPointer<RE::TESObjectREFR>* data = nullptr;
			if (!SafeReadClaimTimeCellReferenceHeader(cell, size, capacity, data) ||
				size > kClaimTimeCellReferenceRawSlotCap || capacity > kClaimTimeCellReferenceRawSlotCap) {
				continue;
			}
			for (std::uint32_t index = 0; index < size; ++index) {
				RE::NiPointer<RE::TESObjectREFR> reference;
				if (!SafeReadClaimTimeCellReferenceSlot(data, index, reference)) {
					break;
				}
				auto* item = reference.get();
				if (!item || item == a_beacon || item->IsDeleted() || item->IsDisabled() ||
					item->parentCell != cell) {
					continue;
				}
				const auto base = item->GetBaseObject();
				if (!base || !base->Is(RE::FormType::kCONT)) {
					continue;
				}
				++contScanned;
				if (!SafeIsClaimTimeReferenceInRange(item, origin, squaredRadius)) {
					continue;
				}
				if (item->GetParentWorldSpace() != expectedWorldspace) {
					continue;
				}
				++contInRange;
				if (firstContRef == 0) {
					firstContRef = item->formID;
					firstContBase = base->formID;
				}
				if (contInRange > 32) {
					continue;
				}
				std::uint32_t attachException = 0;
				if (!CallClaimTimeWorkshopAttach(a_moduleBase, item, true, a_beacon, attachException)) {
					++contAttachFailed;
					REX::INFO(
						"CONT probe attach failed ref={:08X} base={:08X} beacon={:08X} exception=0x{:08X}",
						item->formID, base->formID, a_beacon->formID, attachException);
					continue;
				}
				++contAttachOk;
				if (firstAttachOkRef == 0) {
					firstAttachOkRef = item->formID;
					firstAttachOkBase = base->formID;
				}
				bool predicateState = false;
				std::uint32_t predicateException = 0;
				if (!CallClaimTimeWorkshopPredicate(a_moduleBase, item, predicateState, predicateException)) {
					++contPredicateFailed;
					REX::INFO(
						"CONT probe predicate exception ref={:08X} base={:08X} exception=0x{:08X}",
						item->formID, base->formID, predicateException);
				} else if (predicateState) {
					++contPredicateTrue;
					if (firstPredTrueRef == 0) {
						firstPredTrueRef = item->formID;
						firstPredTrueBase = base->formID;
					}
				} else {
					++contPredicateFailed;
				}
				std::uint32_t detachException = 0;
				if (!CallClaimTimeWorkshopAttach(a_moduleBase, item, false, a_beacon, detachException)) {
					REX::INFO(
						"CONT probe detach failed ref={:08X} base={:08X} exception=0x{:08X}",
						item->formID, base->formID, detachException);
				}
			}
		}
		REX::INFO(
			"CONT probe completed scanned={} inRange={} attachOk={} predicateTrue={} attachFailed={} predicateFailed={} firstRef={:08X} firstBase={:08X} firstAttachOkRef={:08X} firstAttachOkBase={:08X} firstPredTrueRef={:08X} firstPredTrueBase={:08X}",
			contScanned, contInRange, contAttachOk, contPredicateTrue,
			contAttachFailed, contPredicateFailed,
			firstContRef, firstContBase,
			firstAttachOkRef, firstAttachOkBase,
			firstPredTrueRef, firstPredTrueBase);
	}

	[[nodiscard]] RE::TESObjectREFR* SafeBeginClaimTimeWorkshopEnrollment(
		RE::TESObjectCELL* a_cell,
		const std::vector<RE::TESObjectCELL*>& a_loadedCells,
		RE::BGSLocation* a_claimLocation,
		RE::TESObjectREFR* a_helper,
		RE::TESObjectREFR* a_beacon,
		float a_radius,
		RE::BGSListForm* a_excludedOriginals,
		RE::BGSListForm* a_excludedReplacements) noexcept
	{
		if (g_furnitureEnrollmentFailed.load(std::memory_order_acquire)) {
			REX::ERROR("FurnitureRollback-v1: claim-time enrollment blocked after failed furniture batch");
			g_claimTimeEnrollmentStatus = -1;
			return nullptr;
		}
		if (g_claimTimeEnrollmentStatus == 1 && g_claimTimeEnrollmentBeacon.get() == a_beacon &&
			g_claimTimeEnrollmentHelper.get() == a_helper && g_claimTimeEnrollmentCell.get() == a_cell) {
			return g_claimTimeEnrollmentCandidateCount ? g_claimTimeEnrollmentCandidates[0].get() : nullptr;
		}
		if (g_claimTimeEnrollmentStatus != 0) {
			REX::ERROR("ClaimTimeEnrollment begin blocked: retained transaction status={}", g_claimTimeEnrollmentStatus);
			g_claimTimeEnrollmentStatus = -1;
			return nullptr;
		}
		ClearClaimTimeWorkshopEnrollment();
		if (!a_cell || !a_helper || !a_beacon || a_helper->parentCell != a_cell ||
			a_helper->IsDeleted() || a_beacon->IsDeleted() || a_beacon->IsDisabled()) {
			g_claimTimeEnrollmentStatus = -1;
			return nullptr;
		}
		const auto helperBase = a_helper->GetBaseObject();
		if (!helperBase || helperBase->formID != kOverlayActivatorID) {
			g_claimTimeEnrollmentStatus = -1;
			return nullptr;
		}
		ClaimTimeCandidateValidation beaconValidation;
		if (!SafeValidateClaimTimeBeacon(a_beacon, a_helper, a_claimLocation, beaconValidation)) {
			g_claimTimeEnrollmentStatus = -1;
			return nullptr;
		}
		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			g_claimTimeEnrollmentStatus = -1;
			return nullptr;
		}
		const auto moduleBase = gameModule.GetBaseAddress();
		if (!VerifyClaimTimeWorkshopAttachTarget(moduleBase) ||
			!VerifyClaimTimeWorkshopPredicateTarget(moduleBase)) {
			g_claimTimeEnrollmentStatus = -1;
			return nullptr;
		}
		ClaimTimeWorkshopScan scan;
		if (!CollectClaimTimeWorkshopCandidates(a_beacon, a_cell, a_loadedCells, a_radius, scan)) {
			REX::ERROR("ClaimTimeEnrollment discovery failed closed beacon={:08X}", a_beacon->formID);
			g_claimTimeEnrollmentStatus = -1;
			return nullptr;
		}
		REX::INFO("ClaimTimeEnrollment scan breakdown: scanned={} eligible={} rejected={} furn={} cont={} outpostBuildable={} nonEligibleRejected={} firstRejectedFormType={} firstRejectedBaseID={:08X}",
			scan.scanned, scan.eligible, scan.rejected, scan.furnCount, scan.contCount,
			scan.outpostBuildableCount, scan.nonEligibleRejected,
			scan.firstNonFurnContFormType, scan.firstNonFurnContBaseID);

		std::uint32_t incompatibleFurnitureCount = 0;
		RE::TESFormID firstExcludedRef = 0;
		RE::TESFormID firstExcludedBase = 0;
		std::uint32_t firstCompatibilityFlags = 0;
		for (std::uint32_t index = 0; index < scan.candidateCount; ++index) {
			auto* item = scan.candidates[index].get();
			RE::TESFormID baseID = 0;
			const auto compatibilityFlags = GetWorkshopFurnitureCompatibilityFlags(item, baseID);
			if (compatibilityFlags != 0) {
				++incompatibleFurnitureCount;
				if (firstExcludedRef == 0) {
					firstExcludedRef = item ? item->formID : 0;
					firstExcludedBase = baseID;
					firstCompatibilityFlags = compatibilityFlags;
				}
				continue;
			}
			bool originalsValid = true;
			bool replacementsValid = true;
			const bool excluded = SafeFormListContainsReference(a_excludedOriginals, item, originalsValid) ||
				SafeFormListContainsReference(a_excludedReplacements, item, replacementsValid);
			if (!originalsValid || !replacementsValid) {
				(void)SafeRollbackClaimTimeWorkshopEnrollment();
				g_claimTimeEnrollmentStatus = -1;
				return nullptr;
			}
			if (excluded) {
				continue;
			}
			ClaimTimeCandidateValidation validation;
			if (!SafeValidateClaimTimeWorkshopCandidate(item, a_helper, a_beacon, scan.player,
				scan.expectedWorldspace, scan.expectedLocation, a_radius, validation)) {
				continue;
			}
			const auto itemBase = item->GetBaseObject();
			const bool isContainer = itemBase && itemBase->Is(RE::FormType::kCONT);
			if (!isContainer) {
				// Inherited FURN and other build-menu forms are intentionally not
				// enrolled. CONT is the only proven safe inherited-object path.
				continue;
			}
			bool beforePredicate = false;
			std::uint32_t exceptionCode = 0;
			if (!CallClaimTimeWorkshopPredicate(moduleBase, item, beforePredicate, exceptionCode)) {
				g_claimTimeEnrollmentStatus = -1;
				REX::ERROR("ClaimTimeEnrollment pre-attach predicate failed; stopping ref={:08X}", item->formID);
				return nullptr;
			}
			if (beforePredicate || HasClaimTimeWorkshopState(item)) {
				continue;
			}
			if (g_claimTimeEnrollmentCandidateCount >= kBatchEnrollmentMaxItems) {
				g_claimTimeEnrollmentStatus = -1;
				return nullptr;
			}
			// Retain the attempted candidate BEFORE the engine call: an exception
			// or failed predicate may follow partial mutation. Never lose its ledger.
			g_claimTimeEnrollmentCandidates[g_claimTimeEnrollmentCandidateCount++].reset(item);
			g_claimTimeEnrollmentBeacon.reset(a_beacon);
			g_claimTimeEnrollmentHelper.reset(a_helper);
			g_claimTimeEnrollmentCell.reset(a_cell);
			g_claimTimeEnrollmentStatus = 1;
			if (!CallClaimTimeWorkshopAttach(moduleBase, item, true, a_beacon, exceptionCode)) {
				g_claimTimeEnrollmentStatus = -1;
				REX::ERROR("ClaimTimeEnrollment attach failed; retaining ledger and stopping ref={:08X}", item->formID);
				return nullptr;
			}
			bool predicate = false;
			if (!CallClaimTimeWorkshopPredicate(moduleBase, item, predicate, exceptionCode) || !predicate) {
				g_claimTimeEnrollmentStatus = -1;
				REX::ERROR("ClaimTimeEnrollment verification failed; retaining ledger and stopping ref={:08X}", item->formID);
				return nullptr;
			}
			REX::INFO("ClaimTimeEnrollment attached candidate={:08X} beacon={:08X} count={}",
				item->formID, a_beacon->formID, g_claimTimeEnrollmentCandidateCount);
		}
		if (g_claimTimeEnrollmentCandidateCount == 0) {
			REX::INFO("ClaimTimeEnrollment completed zero eligible candidates");
		}
		if (incompatibleFurnitureCount != 0) {
			REX::INFO(
				"ClaimTimeEnrollment excluded incompatible furniture count={} firstRef={:08X} firstBase={:08X} firstFlags=0x{:X}",
				incompatibleFurnitureCount,
				firstExcludedRef,
				firstExcludedBase,
				firstCompatibilityFlags);
		}
		return g_claimTimeEnrollmentCandidateCount ? g_claimTimeEnrollmentCandidates[0].get() : nullptr;
	}

	[[nodiscard]] bool SafeCommitClaimTimeWorkshopEnrollment() noexcept
	{
		if (g_claimTimeEnrollmentStatus == 0) {
			return true;
		}
		if (g_claimTimeEnrollmentStatus < 0 || g_claimTimeEnrollmentCandidateCount == 0 || !g_claimTimeEnrollmentBeacon) {
			return false;
		}
		REX::INFO("ClaimTimeEnrollment commit count={} beacon={:08X}", g_claimTimeEnrollmentCandidateCount, g_claimTimeEnrollmentBeacon->formID);
		ClearClaimTimeWorkshopEnrollment();
		return true;
	}

	[[nodiscard]] bool SafeRollbackClaimTimeWorkshopEnrollment() noexcept
	{
		if (g_claimTimeEnrollmentStatus == 0) {
			return true;
		}
		if (g_claimTimeEnrollmentStatus < 0 || g_claimTimeEnrollmentCandidateCount == 0 || !g_claimTimeEnrollmentBeacon) {
			REX::ERROR("ClaimTimeEnrollment rollback blocked: unverified transaction; retaining ledger");
			return false;
		}

		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244 ||
			!VerifyClaimTimeWorkshopAttachTarget(gameModule.GetBaseAddress()) ||
			!VerifyClaimTimeWorkshopPredicateTarget(gameModule.GetBaseAddress())) {
			REX::ERROR("ClaimTimeEnrollment rollback blocked: runtime/attach/predicate signature mismatch");
			return false;
		}
		bool allDetached = true;
		for (std::uint32_t index = 0; index < g_claimTimeEnrollmentCandidateCount; ++index) {
			auto& candidateHold = g_claimTimeEnrollmentCandidates[index];
			auto* candidate = candidateHold.get();
			if (!candidate) continue;
			std::uint32_t exceptionCode = 0;
			const bool called = CallClaimTimeWorkshopAttach(gameModule.GetBaseAddress(), candidate, false, g_claimTimeEnrollmentBeacon.get(), exceptionCode);
			bool predicate = false;
			std::uint32_t predicateException = 0;
			const bool predicateCalled = CallClaimTimeWorkshopPredicate(
				gameModule.GetBaseAddress(),
				candidate,
				predicate,
				predicateException);
			const bool extra = HasClaimTimeWorkshopState(candidate);
			const bool detached = called && predicateCalled && !predicate;
			REX::INFO(
				"ClaimTimeEnrollment rollback candidate={:08X} beacon={:08X} called={} predicateCalled={} predicate={} extra={} detached={} exception=0x{:08X} predicateException=0x{:08X}",
				candidate->formID,
				g_claimTimeEnrollmentBeacon->formID,
				called,
				predicateCalled,
				predicate,
				extra,
				detached,
				exceptionCode,
				predicateException);
			if (!detached) {
				allDetached = false;
				g_claimTimeEnrollmentStatus = -1;
				break;
			}
		}
		if (allDetached) {
			ClearClaimTimeWorkshopEnrollment();
		}
		return allDetached;
	}

	[[nodiscard]] std::string_view GetReferenceDiagnosticStage(std::int32_t a_stage) noexcept
	{
		switch (a_stage) {
		case 0:
			return "helper-pre-overlay";
		case 1:
			return "helper-post-overlay";
		case 2:
			return "native-beacon-event";
		case 3:
			return "native-beacon-bound";
		case 4:
			return "player-load-player";
		case 5:
			return "claimed-location-enter-player";
		case 6:
			return "claimed-location-enter-beacon";
		case 7:
			return "player-load-beacon";
		case 8:
			return "native-beacon-event-helper";
		case 9:
			return "manual-reference-probe";
		case 10:
			return "decoration-pre-disable";
		case 11:
			return "decoration-pre-restore";
		case 12:
			return "decoration-post-restore";
		case 13:
			return "replacement-original-pre-hide";
		case 14:
			return "replacement-built-captured";
		case 15:
			return "workshop-compare-chair";
		case 16:
			return "workshop-compare-pharma";
		default:
			return "unknown";
		}
	}

	bool SafeIsLocationExplored(RE::BGSLocation* a_location) noexcept
	{
		__try {
			if (!a_location) {
				return false;
			}
			return a_location->explored;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool SafeIsSiteClaimed(RE::BGSLocation* a_location) noexcept
	{
		__try {
			if (!a_location) {
				return false;
			}
			auto* keyword = GetClaimableKeyword();
			if (keyword && a_location->HasKeyword(keyword)) {
				return true;
			}
			return false;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool SafeIsLocationClaimable(RE::BGSLocation* a_location) noexcept
	{
		__try {
			if (!a_location) {
				return false;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}

			if (player->IsInSpace(true)) {
				return false;
			}

			auto* cell = player->parentCell;
			if (!cell || cell->IsInterior()) {
				return false;
			}

			if (SafeIsSiteClaimed(a_location)) {
				return false;
			}

			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	std::int32_t SafeGetClaimedCount() noexcept
	{
		__try {
			auto* global = GetClaimedCountGlobal();
			if (!global) {
				return -1;
			}
			return static_cast<std::int32_t>(global->value);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}

	float SafeGetClaimRadius() noexcept
	{
		__try {
			auto* global = GetClaimRadiusGlobal();
			if (!global) {
				return 0.0f;
			}
			return global->value;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0.0f;
		}
	}

	[[nodiscard]] bool VerifyEngineLayout(std::uintptr_t a_moduleBase) noexcept
	{
		if (a_moduleBase == 0) {
			return false;
		}

		__try {
			const auto* handlerBytes = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kSetOutpostOverlayHandlerRva);
			if (!handlerBytes || std::memcmp(handlerBytes, kSetOutpostOverlayHandlerPrefix.data(), kSetOutpostOverlayHandlerPrefix.size()) != 0) {
				return false;
			}

			const auto* cellResolverBytes = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kCellResolverRva);
			if (!cellResolverBytes || std::memcmp(cellResolverBytes, kCellResolverPrefix.data(), kCellResolverPrefix.size()) != 0) {
				return false;
			}

			const auto* singletonBytes = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kSingletonRva);
			if (!singletonBytes || std::memcmp(singletonBytes, kSingletonPrefix.data(), kSingletonPrefix.size()) != 0) {
				return false;
			}

			const auto* propLookupBytes = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kPropertyLookupRva);
			if (!propLookupBytes || std::memcmp(propLookupBytes, kPropertyLookupPrefix.data(), kPropertyLookupPrefix.size()) != 0) {
				return false;
			}

			const auto* siteF40F14 = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kSiteF40F14Rva);
			if (!siteF40F14 || std::memcmp(siteF40F14, kSiteF40F14Bytes.data(), kSiteF40F14Bytes.size()) != 0) {
				return false;
			}

			const auto* siteF40F1B = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kSiteF40F1BRva);
			// v2 guards this exact call. Accept vanilla or our verified call/stub,
			// retaining all neighboring byte gates and rechecking ownership each time.
			if (std::memcmp(siteF40F1B, kSiteF40F1BBytes.data(), kSiteF40F1BBytes.size()) != 0 &&
				!ClaimablePOIs::PCMOverlayHook::VerifyOwnedOverlayResolverCall(a_moduleBase + kSiteF40F1BRva)) {
				REX::ERROR("OverlayGuardCompatibility-v1: resolver call is neither vanilla nor the owned guard");
				return false;
			}

			const auto* siteF40F25 = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kSiteF40F25Rva);
			if (!siteF40F25 || std::memcmp(siteF40F25, kSiteF40F25Bytes.data(), kSiteF40F25Bytes.size()) != 0) {
				return false;
			}

			const auto* siteF40F2F = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kSiteF40F2FRva);
			if (!siteF40F2F || std::memcmp(siteF40F2F, kSiteF40F2FBytes.data(), kSiteF40F2FBytes.size()) != 0) {
				return false;
			}

			const auto* siteF40F36 = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kSiteF40F36Rva);
			if (!siteF40F36 || std::memcmp(siteF40F36, kSiteF40F36Bytes.data(), kSiteF40F36Bytes.size()) != 0) {
				return false;
			}

			const auto* siteF40F40 = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kSiteF40F40Rva);
			if (!siteF40F40 || std::memcmp(siteF40F40, kSiteF40F40Bytes.data(), kSiteF40F40Bytes.size()) != 0) {
				return false;
			}

			const auto* siteF40F44 = reinterpret_cast<const std::uint8_t*>(a_moduleBase + kSiteF40F44Rva);
			if (!siteF40F44 || std::memcmp(siteF40F44, kSiteF40F44Bytes.data(), kSiteF40F44Bytes.size()) != 0) {
				return false;
			}

			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Keep the structured-exception boundary limited to trivial values. MSVC rejects
	// __try in a frame that owns C++ objects requiring unwinding.
	[[nodiscard]] bool DispatchSetOutpostOverlayHandler(
		std::uintptr_t a_handlerAddress,
		RE::TESObjectREFR* a_overlayActivator,
		std::uint32_t& a_exceptionCode) noexcept
	{
		using handler_t = bool (*)(void*, void*, RE::TESObjectREFR*);
		a_exceptionCode = 0;
		__try {
			const auto handler = reinterpret_cast<handler_t>(a_handlerAddress);
			return handler(nullptr, nullptr, a_overlayActivator);
		} __except (a_exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool RawResolveCellProperty(
		std::uintptr_t a_moduleBase,
		RE::TESObjectCELL* a_cell,
		void*& a_outProperty,
		std::uint8_t& a_outFlagByte,
		std::uint32_t& a_outExceptionCode,
		RE::TESWorldSpace** a_outOverlay = nullptr) noexcept
	{
		a_outProperty = nullptr;
		if (a_outOverlay) *a_outOverlay = nullptr;
		a_outFlagByte = 0;
		a_outExceptionCode = 0;

		__try {
			if (!a_cell) {
				return false;
			}

			using CellResolver_t = void* (*)(RE::TESObjectCELL*);
			const auto cellResolver = reinterpret_cast<CellResolver_t>(a_moduleBase + kCellResolverRva);
			void* cellOverlay = cellResolver(a_cell);
			if (!cellOverlay) {
				return false;
			}

			using SingletonGetter_t = void* (*)();
			const auto getSingleton = reinterpret_cast<SingletonGetter_t>(a_moduleBase + kSingletonRva);
			const auto* singletonInstance = static_cast<const std::byte*>(getSingleton());
			if (!singletonInstance) {
				return false;
			}

			const auto managerPtr = *reinterpret_cast<void* const*>(singletonInstance + kSingletonManagerOffset);
			if (!managerPtr) {
				return false;
			}

			using PropertyLookup_t = void* (*)(void*, void*, void**);
			const auto lookup = reinterpret_cast<PropertyLookup_t>(a_moduleBase + kPropertyLookupRva);
			void* overlayPtrStorage = cellOverlay;
			void* property = lookup(managerPtr, nullptr, &overlayPtrStorage);
			if (!property) {
				return false;
			}

			const auto flagBytePtr = static_cast<const std::uint8_t*>(property) + kPropertyFlagOffset;
			a_outProperty = property;
			a_outFlagByte = *flagBytePtr;
			if (a_outOverlay) *a_outOverlay = static_cast<RE::TESWorldSpace*>(cellOverlay);
			return true;
		} __except (a_outExceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool RawClearPropertyBit0AndDirty(
		std::uintptr_t a_moduleBase,
		void* a_property,
		std::uint8_t& a_outFlagBefore,
		std::uint8_t& a_outFlagAfter,
		std::uint32_t& a_outExceptionCode) noexcept
	{
		a_outFlagBefore = 0;
		a_outFlagAfter = 0;
		a_outExceptionCode = 0;

		__try {
			if (!a_property) {
				return false;
			}

			auto* flagBytePtr = static_cast<std::uint8_t*>(a_property) + kPropertyFlagOffset;
			a_outFlagBefore = *flagBytePtr;
			if ((a_outFlagBefore & kPropertyFlagBit) == 0) {
				a_outFlagAfter = a_outFlagBefore;
				return true;
			}

			const auto newFlag = static_cast<std::uint8_t>(a_outFlagBefore & ~kPropertyFlagBit);
			*flagBytePtr = newFlag;

			auto* dirtyBytePtr = reinterpret_cast<std::uint8_t*>(a_moduleBase + kDirtyByteRva);
			*dirtyBytePtr = 1;

			a_outFlagAfter = *flagBytePtr;
			return (a_outFlagAfter & kPropertyFlagBit) == 0;
		} __except (a_outExceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool RawSetPropertyBit0AndDirty(
		std::uintptr_t a_moduleBase,
		void* a_property,
		std::uint8_t& a_outFlagBefore,
		std::uint8_t& a_outFlagAfter,
		std::uint32_t& a_outExceptionCode) noexcept
	{
		a_outFlagBefore = 0;
		a_outFlagAfter = 0;
		a_outExceptionCode = 0;

		__try {
			if (!a_property) {
				return false;
			}

			auto* flagBytePtr = static_cast<std::uint8_t*>(a_property) + kPropertyFlagOffset;
			a_outFlagBefore = *flagBytePtr;
			if ((a_outFlagBefore & kPropertyFlagBit) != 0) {
				a_outFlagAfter = a_outFlagBefore;
				return true;
			}

			const auto newFlag = static_cast<std::uint8_t>(a_outFlagBefore | kPropertyFlagBit);
			*flagBytePtr = newFlag;

			auto* dirtyBytePtr = reinterpret_cast<std::uint8_t*>(a_moduleBase + kDirtyByteRva);
			*dirtyBytePtr = 1;

			a_outFlagAfter = *flagBytePtr;
			return (a_outFlagAfter & kPropertyFlagBit) != 0;
		} __except (a_outExceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// 0: rejected with no outstanding mutation; 1: verified unlock;
	// -1: rollback is unverified, so the caller must retain its helper.
	std::int32_t SafeBeginPOIOverlayUnlock(RE::TESObjectREFR* a_helper, UnlockPurpose a_purpose) noexcept
	{
		const char* purposeName = (a_purpose == UnlockPurpose::ClaimedWorkshop) ? "BeginClaimedPOIWorkshopUnlock" : "BeginPOIOverlayUnlock";

		if (g_trackedUnlockActive.load(std::memory_order_acquire)) {
			REX::WARN("{}: rejected: tracking already active", purposeName);
			return a_helper && g_trackedHelperFormID.load(std::memory_order_relaxed) == a_helper->formID ? -1 : 0;
		}
		if (g_furnitureEnrollmentFailed.load(std::memory_order_acquire)) {
			REX::ERROR("FurnitureRollback-v1: {} blocked after failed furniture enrollment; restart from a pre-claim save", purposeName);
			return 0;
		}

		if (!a_helper) {
			REX::WARN("{}: rejected null helper reference", purposeName);
			ClearPOIOverlayUnlockTrackingInternal();
			return false;
		}

		const auto baseObject = a_helper->GetBaseObject();
		if (!baseObject || baseObject->formID != kOverlayActivatorID) {
			REX::WARN("{}: rejected non-FC07 helper 0x{:08X}", purposeName, a_helper->formID);
			ClearPOIOverlayUnlockTrackingInternal();
			return false;
		}

		auto* cell = a_helper->parentCell;
		if (!cell) {
			REX::WARN("{}: rejected helper 0x{:08X} with null parent cell", purposeName, a_helper->formID);
			ClearPOIOverlayUnlockTrackingInternal();
			return false;
		}

		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			REX::ERROR("{}: blocked: unsupported runtime {}", purposeName, gameModule.GetFileVersion());
			ClearPOIOverlayUnlockTrackingInternal();
			return false;
		}

		const auto moduleBase = gameModule.GetBaseAddress();
		if (!VerifyEngineLayout(moduleBase)) {
			REX::ERROR("{}: blocked: layout signature mismatch", purposeName);
			ClearPOIOverlayUnlockTrackingInternal();
			return false;
		}

		[[maybe_unused]] const auto handlerAddress = moduleBase + kSetOutpostOverlayHandlerRva;

		// Resolve property before mutation to verify initial state and reject pre-existing bit 0.
		void* propertyBefore = nullptr;
		RE::TESWorldSpace* overlayBefore = nullptr;
		std::uint8_t flagByteBefore = 0;
		std::uint32_t resolveException = 0;
		if (!RawResolveCellProperty(moduleBase, cell, propertyBefore, flagByteBefore, resolveException, &overlayBefore)) {
			REX::ERROR(
				"{}: failed to resolve property for cell 0x{:08X} (exception 0x{:08X})",
				purposeName,
				cell->formID,
				resolveException);
			ClearPOIOverlayUnlockTrackingInternal();
			return false;
		}

		if ((flagByteBefore & kPropertyFlagBit) != 0) {
			REX::WARN(
				"{}: rejected pre-existing bit 0 on property 0x{:X} for cell 0x{:08X} (flag=0x{:02X})",
				purposeName,
				reinterpret_cast<std::uintptr_t>(propertyBefore),
				cell->formID,
				flagByteBefore);
			ClearPOIOverlayUnlockTrackingInternal();
			return false;
		}

		if (a_purpose == UnlockPurpose::InitialClaim &&
			!ClaimablePOIs::VanillaOverlayHook::ArmInitialClaim(a_helper, overlayBefore, SafeGetClaimRadius())) {
			REX::ERROR("SeparateVanillaOverlay-v1: initial claim blocked before mutation: registration gates or identity unavailable");
			ClearPOIOverlayUnlockTrackingInternal();
			return 0;
		}

		// Own the recovery anchor BEFORE mutation: either activation route can mutate and fault.
		g_trackedRecoveryOnly.store(true, std::memory_order_release);
		g_trackedHelperFormID.store(a_helper->formID, std::memory_order_release);
		g_trackedCellFormID.store(cell->formID, std::memory_order_release);
		g_trackedPropertyAddress.store(reinterpret_cast<std::uintptr_t>(propertyBefore), std::memory_order_release);
		g_trackedPurpose.store(a_purpose, std::memory_order_release);
		g_trackedUnlockActive.store(true, std::memory_order_release);

		// ProceduralPreservation-v1: use property-bit-only activation for both
		// InitialClaim and ClaimedWorkshop. This grants vanilla scanner beacon
		// placement permission without invoking DispatchSetOutpostOverlayHandler,
		// which permanently promotes 2,300+ cell references with ExtraPromotedRef.
		std::uint32_t mutationException = 0;
		bool mutationSucceeded = false;
		std::uint8_t bitOnlyBefore = 0;
		std::uint8_t bitOnlyAfter = 0;
		REX::INFO(
			"ProceduralPreservation-v1: using property-bit-only activation purpose={} helper=0x{:08X} cell=0x{:08X}",
			purposeName,
			a_helper->formID,
			cell->formID);
		mutationSucceeded = RawSetPropertyBit0AndDirty(
			moduleBase,
			propertyBefore,
			bitOnlyBefore,
			bitOnlyAfter,
			mutationException);
		REX::INFO(
			"ProceduralPreservation-v1: property-bit mutation result={} flagBefore=0x{:02X} flagAfter=0x{:02X} exception=0x{:08X}",
			mutationSucceeded,
			bitOnlyBefore,
			bitOnlyAfter,
			mutationException);
		if (mutationException != 0 || !mutationSucceeded) {
			REX::ERROR(
				"{}: activation mutation failed/faulted for helper 0x{:08X} (succeeded={} exception=0x{:08X})",
				purposeName,
				a_helper->formID,
				mutationSucceeded,
				mutationException);

			if (propertyBefore) {
				std::uint8_t rbBefore = 0;
				std::uint8_t rbAfter = 0;
				std::uint32_t rbEx = 0;
				const bool rollbackOk = RawClearPropertyBit0AndDirty(moduleBase, propertyBefore, rbBefore, rbAfter, rbEx);
				if (!rollbackOk) {
					REX::ERROR(
						"{}: activation-failure rollback failed for property 0x{:X} (exception 0x{:08X})",
						purposeName,
						reinterpret_cast<std::uintptr_t>(propertyBefore),
						rbEx);
					return -1;
				}
			}
			ClearPOIOverlayUnlockTrackingInternal();
			return false;
		}

		// Post-mutation resolution and verification: bit 0 must be set.
		void* propertyAfter = nullptr;
		std::uint8_t flagByteAfter = 0;
		std::uint32_t postResolveException = 0;
		const bool resolvedAfter = RawResolveCellProperty(moduleBase, cell, propertyAfter, flagByteAfter, postResolveException);
		const bool bit0Set = resolvedAfter && propertyAfter && ((flagByteAfter & kPropertyFlagBit) != 0);

		if (!bit0Set) {
			REX::ERROR(
				"{}: post-verification failed: resolved={} property=0x{:X} flag=0x{:02X} (exception 0x{:08X})",
				purposeName,
				resolvedAfter,
				reinterpret_cast<std::uintptr_t>(propertyAfter),
				flagByteAfter,
				postResolveException);

			void* rollbackTarget = propertyAfter ? propertyAfter : propertyBefore;
			g_trackedPropertyAddress.store(reinterpret_cast<std::uintptr_t>(rollbackTarget), std::memory_order_release);
			if (rollbackTarget) {
				std::uint8_t rbBefore = 0;
				std::uint8_t rbAfter = 0;
				std::uint32_t rbEx = 0;
				const bool rollbackOk = RawClearPropertyBit0AndDirty(moduleBase, rollbackTarget, rbBefore, rbAfter, rbEx);
				if (!rollbackOk) {
					REX::ERROR(
						"{}: post-verification rollback failed for property 0x{:X} (exception 0x{:08X})",
						purposeName,
						reinterpret_cast<std::uintptr_t>(rollbackTarget),
						rbEx);
					return -1;
				}
			}
			ClearPOIOverlayUnlockTrackingInternal();
			return false;
		}

		// Capture tracking identity.
		g_trackedRecoveryOnly.store(false, std::memory_order_release);
		g_trackedHelperFormID.store(a_helper->formID, std::memory_order_release);
		g_trackedCellFormID.store(cell->formID, std::memory_order_release);
		g_trackedPropertyAddress.store(reinterpret_cast<std::uintptr_t>(propertyAfter), std::memory_order_release);
		g_trackedPurpose.store(a_purpose, std::memory_order_release);
		g_trackedUnlockActive.store(true, std::memory_order_release);

		REX::INFO(
			"{}: succeeded: helper=0x{:08X} cell=0x{:08X} property=0x{:X} flagBefore=0x{:02X} flagAfter=0x{:02X} purpose={}",
			purposeName,
			a_helper->formID,
			cell->formID,
			reinterpret_cast<std::uintptr_t>(propertyAfter),
			flagByteBefore,
			flagByteAfter,
			static_cast<std::uint32_t>(a_purpose));
		return true;
	}

	bool SafeResumePOIOverlayUnlock(RE::TESObjectREFR* a_helper, bool a_recoveryOnly = false) noexcept
	{
		if (!a_recoveryOnly && g_furnitureEnrollmentFailed.load(std::memory_order_acquire)) {
			return false;
		}
		if (!a_helper) {
			REX::WARN("ResumePOIOverlayUnlock rejected null helper reference");
			return false;
		}

		const auto baseObject = a_helper->GetBaseObject();
		if (!baseObject || baseObject->formID != kOverlayActivatorID) {
			REX::WARN("ResumePOIOverlayUnlock rejected non-FC07 helper 0x{:08X}", a_helper->formID);
			return false;
		}

		auto* cell = a_helper->parentCell;
		if (!cell) {
			REX::WARN("ResumePOIOverlayUnlock rejected helper 0x{:08X} with null parent cell", a_helper->formID);
			return false;
		}

		// A live retry must keep the exact captured address, never replace it with
		// a fresh lookup or steal another transaction's recovery anchor.
		if (g_trackedUnlockActive.load(std::memory_order_acquire)) {
			return a_recoveryOnly &&
				g_trackedPurpose.load(std::memory_order_relaxed) == UnlockPurpose::InitialClaim &&
				g_trackedHelperFormID.load(std::memory_order_relaxed) == a_helper->formID &&
				g_trackedCellFormID.load(std::memory_order_relaxed) == cell->formID;
		}

		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			REX::ERROR("ResumePOIOverlayUnlock blocked: unsupported runtime {}", gameModule.GetFileVersion());
			return false;
		}

		const auto moduleBase = gameModule.GetBaseAddress();
		if (!VerifyEngineLayout(moduleBase)) {
			REX::ERROR("ResumePOIOverlayUnlock blocked: layout signature mismatch");
			return false;
		}

		void* property = nullptr;
		std::uint8_t flagByte = 0;
		std::uint32_t resolveException = 0;
		if (!RawResolveCellProperty(moduleBase, cell, property, flagByte, resolveException)) {
			REX::ERROR(
				"ResumePOIOverlayUnlock failed to resolve property for cell 0x{:08X} (exception 0x{:08X})",
				cell->formID,
				resolveException);
			return false;
		}

		if (!property || (!a_recoveryOnly && (flagByte & kPropertyFlagBit) == 0)) {
			REX::WARN(
				"ResumePOIOverlayUnlock rejected property without bit 0 set for cell 0x{:08X} (property=0x{:X} flag=0x{:02X})",
				cell->formID,
				reinterpret_cast<std::uintptr_t>(property),
				flagByte);
			return false;
		}

		g_trackedHelperFormID.store(a_helper->formID, std::memory_order_release);
		g_trackedCellFormID.store(cell->formID, std::memory_order_release);
		g_trackedPropertyAddress.store(reinterpret_cast<std::uintptr_t>(property), std::memory_order_release);
		g_trackedPurpose.store(UnlockPurpose::InitialClaim, std::memory_order_release);
		g_trackedRecoveryOnly.store(a_recoveryOnly, std::memory_order_release);
		g_trackedUnlockActive.store(true, std::memory_order_release);

		REX::INFO(
			"ResumePOIOverlayUnlock recaptured identity: helper=0x{:08X} cell=0x{:08X} property=0x{:X} flag=0x{:02X}",
			a_helper->formID,
			cell->formID,
			reinterpret_cast<std::uintptr_t>(property),
			flagByte);
		return true;
	}

	bool SafeRestorePOIOverlay(RE::TESObjectREFR* a_helper) noexcept
	{
		if (!a_helper) {
			REX::WARN("RestorePOIOverlay rejected null helper reference");
			return false;
		}

		const auto baseObject = a_helper->GetBaseObject();
		if (!baseObject || baseObject->formID != kOverlayActivatorID) {
			REX::WARN("RestorePOIOverlay rejected non-FC07 helper 0x{:08X}", a_helper->formID);
			return false;
		}

		auto* cell = a_helper->parentCell;
		if (!cell) {
			REX::WARN("RestorePOIOverlay rejected helper 0x{:08X} with null parent cell", a_helper->formID);
			return false;
		}

		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			REX::ERROR("RestorePOIOverlay blocked: unsupported runtime {}", gameModule.GetFileVersion());
			return false;
		}

		const auto moduleBase = gameModule.GetBaseAddress();
		if (!VerifyEngineLayout(moduleBase)) {
			REX::ERROR("RestorePOIOverlay blocked: layout signature mismatch");
			return false;
		}

		// Restore requires active tracking plus exact helper/cell match
		if (!g_trackedUnlockActive.load(std::memory_order_acquire)) {
			REX::ERROR("RestorePOIOverlay rejected: no active unlock tracking");
			return false;
		}

		const auto trackedHelper = g_trackedHelperFormID.load(std::memory_order_relaxed);
		const auto trackedCell = g_trackedCellFormID.load(std::memory_order_relaxed);
		if (trackedHelper == 0 || trackedHelper != a_helper->formID) {
			REX::ERROR(
				"RestorePOIOverlay helper mismatch: tracked=0x{:08X} provided=0x{:08X}",
				trackedHelper,
				a_helper->formID);
			return false;
		}
		if (trackedCell == 0 || trackedCell != cell->formID) {
			REX::ERROR(
				"RestorePOIOverlay cell mismatch: tracked=0x{:08X} provided=0x{:08X}",
				trackedCell,
				cell->formID);
			return false;
		}

		const auto capturedAddr = g_trackedPropertyAddress.load(std::memory_order_relaxed);
		if (capturedAddr == 0) {
			REX::ERROR("RestorePOIOverlay failed: captured property address is null");
			return false;
		}

		void* property = reinterpret_cast<void*>(capturedAddr);

		// Clear only bit 0 on captured property and set verified dirty byte.
		g_trackedRecoveryOnly.store(true, std::memory_order_release);
		// If save-suspended, bit 0 is already clear; RawClearPropertyBit0AndDirty treats already-clear as success.
		std::uint8_t flagBefore = 0;
		std::uint8_t flagAfter = 0;
		std::uint32_t clearException = 0;
		if (!RawClearPropertyBit0AndDirty(moduleBase, property, flagBefore, flagAfter, clearException)) {
			REX::ERROR(
				"RestorePOIOverlay failed to clear bit 0 on property 0x{:X} (exception 0x{:08X})",
				reinterpret_cast<std::uintptr_t>(property),
				clearException);
			return false;
		}

		// Verify clear.
		if ((flagAfter & kPropertyFlagBit) != 0) {
			REX::ERROR(
				"RestorePOIOverlay post-verification failed: flag still has bit 0 set (flag=0x{:02X})",
				flagAfter);
			return false;
		}

		REX::INFO(
			"RestorePOIOverlay succeeded: helper=0x{:08X} cell=0x{:08X} property=0x{:X} flagBefore=0x{:02X} flagAfter=0x{:02X}",
			a_helper->formID,
			cell->formID,
			reinterpret_cast<std::uintptr_t>(property),
			flagBefore,
			flagAfter);

		ClearPOIOverlayUnlockTrackingInternal();
		return true;
	}

	bool SafeRecoverClaimedPOIWorkshopUnlock(RE::TESObjectREFR* a_helper) noexcept
	{
		if (!a_helper) {
			REX::WARN("RecoverClaimedPOIWorkshopUnlock rejected null helper reference");
			return false;
		}

		const auto baseObject = a_helper->GetBaseObject();
		if (!baseObject || baseObject->formID != kOverlayActivatorID) {
			REX::WARN("RecoverClaimedPOIWorkshopUnlock rejected non-FC07 helper 0x{:08X}", a_helper->formID);
			return false;
		}

		auto* cell = a_helper->parentCell;
		if (!cell) {
			REX::WARN("RecoverClaimedPOIWorkshopUnlock rejected helper 0x{:08X} with null parent cell", a_helper->formID);
			return false;
		}

		// Never inspect or mutate a recovery target while a different transaction owns
		// the single native tracker.
		if (g_trackedUnlockActive.load(std::memory_order_acquire)) {
			const auto trackedHelper = g_trackedHelperFormID.load(std::memory_order_relaxed);
			const auto trackedCell = g_trackedCellFormID.load(std::memory_order_relaxed);
			const auto trackedPurpose = g_trackedPurpose.load(std::memory_order_relaxed);
			if (trackedPurpose != UnlockPurpose::ClaimedWorkshop || trackedHelper != a_helper->formID || trackedCell != cell->formID) {
				REX::WARN(
					"RecoverClaimedPOIWorkshopUnlock rejected unrelated active tracker: purpose={} trackedHelper=0x{:08X} trackedCell=0x{:08X} helper=0x{:08X} cell=0x{:08X}",
					static_cast<std::uint32_t>(trackedPurpose),
					trackedHelper,
					trackedCell,
					a_helper->formID,
					cell->formID);
				return false;
			}
			// A failed live restore must retry the captured property, not clear a
			// newly resolved property and silently discard the original anchor.
			return SafeRestorePOIOverlay(a_helper);
		}

		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			REX::ERROR("RecoverClaimedPOIWorkshopUnlock blocked: unsupported runtime {}", gameModule.GetFileVersion());
			return false;
		}

		const auto moduleBase = gameModule.GetBaseAddress();
		if (!VerifyEngineLayout(moduleBase)) {
			REX::ERROR("RecoverClaimedPOIWorkshopUnlock blocked: layout signature mismatch");
			return false;
		}

		void* property = nullptr;
		std::uint8_t flagByte = 0;
		std::uint32_t resolveException = 0;
		if (!RawResolveCellProperty(moduleBase, cell, property, flagByte, resolveException)) {
			REX::ERROR(
				"RecoverClaimedPOIWorkshopUnlock failed to resolve property for cell 0x{:08X} (exception 0x{:08X})",
				cell->formID,
				resolveException);
			return false;
		}

		std::uint8_t flagBefore = 0;
		std::uint8_t flagAfter = 0;
		std::uint32_t clearException = 0;
		if (!RawClearPropertyBit0AndDirty(moduleBase, property, flagBefore, flagAfter, clearException)) {
			REX::ERROR(
				"RecoverClaimedPOIWorkshopUnlock failed to clear bit 0 on property 0x{:X} (exception 0x{:08X})",
				reinterpret_cast<std::uintptr_t>(property),
				clearException);
			return false;
		}

		if ((flagAfter & kPropertyFlagBit) != 0) {
			REX::ERROR(
				"RecoverClaimedPOIWorkshopUnlock verification failed: bit 0 still set on property 0x{:X} (flag=0x{:02X})",
				reinterpret_cast<std::uintptr_t>(property),
				flagAfter);
			return false;
		}

		REX::INFO(
			"RecoverClaimedPOIWorkshopUnlock succeeded: helper=0x{:08X} cell=0x{:08X} property=0x{:X} flagBefore=0x{:02X} flagAfter=0x{:02X}",
			a_helper->formID,
			cell->formID,
			reinterpret_cast<std::uintptr_t>(property),
			flagBefore,
			flagAfter);

		ClearPOIOverlayUnlockTrackingInternal();

		return true;
	}

	bool SafeIsLocationNeverReset(RE::BGSLocation* a_location) noexcept
	{
		__try {
			return a_location && a_location->flags.any(RE::BGSLocation::Flags::kNoReset);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] const char* RawGetLocationName(RE::BGSLocation* a_location) noexcept
	{
		if (!a_location) {
			return nullptr;
		}

		__try {
			const char* name = a_location->GetFullName();
			if ((!name || !*name) && a_location->parentLocation) {
				name = a_location->parentLocation->GetFullName();
			}
			return name;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	[[nodiscard]] RE::BSFixedString SafeGetLocationName(RE::BGSLocation* a_location) noexcept
	{
		const char* name = RawGetLocationName(a_location);
		if (!name || !*name) {
			return {};
		}
		return RE::BSFixedString(name);
	}

	[[nodiscard]] bool VerifyBaseExtraListRemoveTarget(std::uintptr_t a_moduleBase) noexcept
	{
		__try {
			return std::memcmp(
				reinterpret_cast<const void*>(a_moduleBase + kBaseExtraListRemoveRva),
				kBaseExtraListRemovePrefix.data(),
				kBaseExtraListRemovePrefix.size()) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool CallBaseExtraListRemove(
		std::uintptr_t a_moduleBase,
		void* a_baseExtraList,
		RE::ExtraDataType a_type,
		bool& a_removed,
		std::uint32_t& a_exceptionCode) noexcept
	{
		a_removed = false;
		a_exceptionCode = 0;
		if (!a_baseExtraList) {
			return false;
		}
		__try {
			auto* removeFunc = reinterpret_cast<BaseExtraListRemoveFunction>(
				a_moduleBase + kBaseExtraListRemoveRva);
			a_removed = removeFunc(a_baseExtraList, a_type, nullptr);
			return true;
		} __except (a_exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool RawSanitizeClaimedBeacon(RE::TESObjectREFR* a_beacon) noexcept
	{
		if (!a_beacon || a_beacon->IsDeleted() || a_beacon->IsDisabled()) {
			REX::WARN("SanitizeClaimedBeacon rejected: null/deleted/disabled beacon");
			return false;
		}

		const auto baseObject = a_beacon->GetBaseObject();
		const RE::TESFormID baseID = baseObject ? baseObject->formID : 0;
		if (baseID != kClaimTimeVanillaBeaconBaseID) {
			REX::WARN(
				"SanitizeClaimedBeacon rejected: ref 0x{:08X} base 0x{:08X} is not vanilla beacon (expected 0x{:08X})",
				a_beacon->formID,
				baseID,
				static_cast<std::uint32_t>(kClaimTimeVanillaBeaconBaseID));
			return false;
		}

		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			REX::ERROR("SanitizeClaimedBeacon blocked: unsupported runtime {}", gameModule.GetFileVersion());
			return false;
		}

		const auto moduleBase = gameModule.GetBaseAddress();
		if (!VerifyBaseExtraListRemoveTarget(moduleBase)) {
			REX::ERROR("SanitizeClaimedBeacon blocked: BaseExtraList::Remove signature mismatch");
			return false;
		}

		auto* extraDataList = a_beacon->extraDataList.get();
		if (!extraDataList) {
			return true;
		}

		auto* lock = reinterpret_cast<RE::BSReadWriteLock*>(
			reinterpret_cast<std::uintptr_t>(extraDataList) + 0x20);
		lock->LockWrite();

		void* baseExtraList = reinterpret_cast<void*>(
			reinterpret_cast<std::uintptr_t>(extraDataList) + 0x08);

		bool removedSourcePackIn = false;
		bool removedLocation = false;

		if (extraDataList->HasType(RE::ExtraDataType::kSourcePackIn)) {
			bool removed = false;
			std::uint32_t ex = 0;
			if (CallBaseExtraListRemove(moduleBase, baseExtraList, RE::ExtraDataType::kSourcePackIn, removed, ex)) {
				removedSourcePackIn = removed;
			}
		}

		if (extraDataList->HasType(RE::ExtraDataType::kLocation)) {
			bool removed = false;
			std::uint32_t ex = 0;
			if (CallBaseExtraListRemove(moduleBase, baseExtraList, RE::ExtraDataType::kLocation, removed, ex)) {
				removedLocation = removed;
			}
		}

		lock->UnlockWrite();

		// PerimeterAnchor-v3: kGroupedPackin (0xDA) is deliberately NOT removed.
		// ExtraGroupedPackin links the beacon to its child meshes/components; removing it
		// caused the cell-border traversal CTD. Removing only kSourcePackIn and kLocation
		// disconnects the persistent beacon from the POI pack-in and location, matching
		// the positive control beacons (FF0C95F1, FF0C9AA9, FF0A66CA).
		REX::INFO(
			"SanitizeClaimedBeacon: beacon=0x{:08X} removed sourcePackIn={} location={} (groupedPackin intact)",
			a_beacon->formID,
			removedSourcePackIn,
			removedLocation);

		return true;
	}

	[[nodiscard]] bool SafeSanitizeClaimedBeacon(RE::TESObjectREFR* a_beacon) noexcept
	{
		__try {
			return RawSanitizeClaimedBeacon(a_beacon);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			REX::ERROR(
				"SanitizeClaimedBeacon: unhandled exception on beacon 0x{:08X}",
				a_beacon ? a_beacon->formID : 0);
			return false;
		}
	}

	// --- Native function entrypoints bound to Papyrus ---

	RE::BSFixedString GetLocationName(std::monostate, RE::BGSLocation* a_location)
	{
		return SafeGetLocationName(a_location);
	}

	bool SanitizeClaimedBeacon(std::monostate, RE::TESObjectREFR* a_beacon)
	{
		return SafeSanitizeClaimedBeacon(a_beacon);
	}

	bool IsLocationExplored(std::monostate, RE::BGSLocation* a_location)
	{
		return SafeIsLocationExplored(a_location);
	}

	bool IsSiteClaimed(std::monostate, RE::BGSLocation* a_location)
	{
		return SafeIsSiteClaimed(a_location);
	}

	bool IsLocationClaimable(std::monostate, RE::BGSLocation* a_location)
	{
		return SafeIsLocationClaimable(a_location);
	}

	bool IsLocationNeverReset(std::monostate, RE::BGSLocation* a_location)
	{
		return SafeIsLocationNeverReset(a_location);
	}

	std::int32_t GetClaimedCount(std::monostate)
	{
		return SafeGetClaimedCount();
	}

	float GetClaimRadius(std::monostate)
	{
		return SafeGetClaimRadius();
	}

	bool IsDiagnosticsEnabled(std::monostate)
	{
		return ClaimablePOIs::Diagnostics::Enabled();
	}

	bool ClaimSite(std::monostate, RE::BGSLocation*)
	{
		// Retained binding for old scripts; bypassing vanilla claim/ownership is unsupported.
		REX::WARN("ClaimSite: legacy direct mutation is disabled; use the Claim Beacon workflow");
		return false;
	}

	bool UnclaimSite(std::monostate, RE::BGSLocation*)
	{
		// Retained binding for old scripts; bypassing vanilla claim/ownership is unsupported.
		REX::WARN("UnclaimSite: legacy direct mutation is disabled; use the Claim Beacon workflow");
		return false;
	}

	std::int32_t BeginPOIOverlayUnlockNative(std::monostate, RE::TESObjectREFR* a_helper)
	{
        if (!ClaimablePOIs::TeardownProtection::Ready() || ClaimablePOIs::TeardownProtection::Busy()) return 0;
		return SafeBeginPOIOverlayUnlock(a_helper, UnlockPurpose::InitialClaim);
	}

	std::int32_t BeginClaimedPOIWorkshopUnlockNative(std::monostate, RE::TESObjectREFR* a_helper)
	{
		if (!ClaimablePOIs::TeardownProtection::Ready() || ClaimablePOIs::TeardownProtection::Busy()) return 0;
		return SafeBeginPOIOverlayUnlock(a_helper, UnlockPurpose::ClaimedWorkshop);
	}

	bool ResumePOIOverlayUnlockNative(std::monostate, RE::TESObjectREFR* a_helper)
	{
		return SafeResumePOIOverlayUnlock(a_helper);
	}

	bool RestorePOIOverlayNative(std::monostate, RE::TESObjectREFR* a_helper)
	{
		return SafeRestorePOIOverlay(a_helper);
	}

	bool IsInitialClaimBoundNative(std::monostate, RE::TESObjectREFR* a_helper, RE::TESObjectREFR* a_beacon)
	{
		__try {
			return a_helper && a_beacon && ClaimablePOIs::VanillaOverlayHook::IsInitialClaimBound(a_helper->formID, a_beacon->formID);
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

    RE::BGSListForm* TeardownList(std::uint32_t a_local) noexcept
    { return RE::TESForm::LookupByID<RE::BGSListForm>(ComposeLocalID(a_local)); }

    bool RawLiveVanillaBeacon(RE::TESObjectREFR* a_beacon)
    {
        if (!a_beacon || a_beacon->IsDeleted()) return false;
        const auto base=a_beacon->GetBaseObject();
        return base && base->formID==0x002C60E9;
    }
    bool LiveVanillaBeacon(RE::TESObjectREFR* a_beacon) noexcept
    {
        __try {
            return RawLiveVanillaBeacon(a_beacon);
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool CurrentReferenceIdentity(RE::TESObjectREFR* a_ref) noexcept
    {
        __try { return a_ref && RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref->formID)==a_ref; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int ClassifyProtectedBeacon(RE::TESObjectREFR* a_beacon) noexcept
    {
        // An old queued command must never resolve a reused ID in another save.
        // Compare the actual current object, not only its dynamic FormID.
        if (a_beacon && !CurrentReferenceIdentity(a_beacon)) return -1;
        if (!LiveVanillaBeacon(a_beacon)) return 0;
        bool valid{};
        const bool claimed=ClaimablePOIs::TeardownProtection::Contains(TeardownList(0x805),a_beacon->formID,valid);
        if (!valid) return -1;
        if (!claimed) return 0;
        const bool armed=ClaimablePOIs::TeardownProtection::Contains(
            TeardownList(ClaimablePOIs::TeardownProtection::kProtectedBeacons),a_beacon->formID,valid);
        return !valid ? -1 : armed ? 2 : 1;
    }
    bool RawInspectInherited(RE::TESObjectREFR* a_item,RE::TESObjectREFR* a_beacon,bool& a_owned)
    {
        a_owned=false;
        if (!a_item) return false; // Unknown/unloaded identity blocks, never guesses ownership.
        if (a_item->IsDeleted()) return true; // Never resurrect intentional scrapping.
        const auto base=a_item->GetBaseObject();
        if (!base || (!base->Is(RE::FormType::kFURN) && !base->Is(RE::FormType::kCONT))) return false;
        RE::TESObjectREFR* owner{};
        if (!ClaimablePOIs::TeardownProtection::WorkshopOwner(a_item,owner)) return false;
        const auto* extra=a_item->extraDataList.get();
        // A private controller can own the item independently of its link.
        // Never treat a cleared public link as proof that opaque state is safe.
        if (extra && (extra->HasType(RE::ExtraDataType::kWorkshop) ||
            extra->HasType(RE::ExtraDataType::kWorkshopSnapping))) return false;
        a_owned=owner==a_beacon;
        if (a_owned) {
            if (!CurrentReferenceIdentity(a_item) || !a_item->GetParentWorldSpace() ||
                a_item->GetParentWorldSpace()!=a_beacon->GetParentWorldSpace()) return false;
            // Do not reverse newly acquired quest/foreign/private ownership.
            // These states have no verified teardown contract for adoption.
            if (!SafeHasUnaliasedContainerExtras(a_item) || !extra ||
                extra->HasType(RE::ExtraDataType::kOwnership) ||
                extra->HasType(RE::ExtraDataType::kProceduralInstanceData) ||
                extra->HasType(RE::ExtraDataType::kPlacedPlanetContent) ||
                extra->HasType(RE::ExtraDataType::kGroupedPackin) ||
                extra->HasType(RE::ExtraDataType::kCalcedPackIn) ||
                extra->HasType(RE::ExtraDataType::kSourcePackIn) ||
                extra->HasType(RE::ExtraDataType::kOwningInstance) ||
                extra->HasType(RE::ExtraDataType::kObjectInstance)) return false;
        }
        return true;
    }
    bool InspectInherited(RE::TESObjectREFR* a_item,RE::TESObjectREFR* a_beacon,bool& a_owned) noexcept
    {
        __try { return RawInspectInherited(a_item,a_beacon,a_owned); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool AllInheritedDetached(RE::TESObjectREFR* a_beacon) noexcept
    {
        std::vector<std::uint32_t> ids;
        if (!ClaimablePOIs::TeardownProtection::Snapshot(
            TeardownList(ClaimablePOIs::TeardownProtection::kInheritedItems),ids)) return false;
        for (const auto id:ids) {
            if (!id) continue;
            auto* item=RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
            bool owned{};
            if (!InspectInherited(item,a_beacon,owned) || owned) return false;
        }
        return true;
    }
    bool InitializeInheritedProtection(std::monostate,RE::TESObjectREFR* a_beacon)
    {
        std::lock_guard lock(g_furnitureEnrollmentMutex);
        if (!ClaimablePOIs::TeardownProtection::Ready() || ClassifyProtectedBeacon(a_beacon)<1 ||
            ClaimablePOIs::TeardownProtection::Busy()) return false;
        // Only the fresh-claim finalizer calls this. Existing claims are not
        // retroactively certified: their already-linked references have no ledger.
        return ClaimablePOIs::TeardownProtection::Record(
            TeardownList(ClaimablePOIs::TeardownProtection::kProtectedBeacons),a_beacon);
    }
    bool HasInheritedProtection(std::monostate,RE::TESObjectREFR* a_beacon)
    { return ClaimablePOIs::TeardownProtection::Ready() && ClassifyProtectedBeacon(a_beacon)==2; }
    bool IsInheritedTeardownReady(std::monostate)
    { return ClaimablePOIs::TeardownProtection::Ready(); }
    std::int32_t GetPendingInheritedTeardown(std::monostate)
    { return static_cast<std::int32_t>(ClaimablePOIs::TeardownProtection::PendingToken()); }
    std::int32_t GetInheritedTeardownBeacon(std::monostate,std::int32_t a_token)
    { return static_cast<std::int32_t>(ClaimablePOIs::TeardownProtection::PendingBeacon(static_cast<std::uint32_t>(a_token))); }
    std::int32_t GetInheritedTeardownCompletion(std::monostate,std::int32_t a_token)
    {
        // The completed beacon may no longer resolve. Query the load-invalidated
        // receipt using its already beacon-bound token, never a retired pointer.
        return ClaimablePOIs::TeardownProtection::Completion(static_cast<std::uint32_t>(a_token));
    }
    void CancelInheritedTeardown(std::monostate,std::int32_t a_token,bool a_fault)
    {
        if (a_fault) ClaimablePOIs::TeardownProtection::Fault();
        ClaimablePOIs::TeardownProtection::Cancel(static_cast<std::uint32_t>(a_token));
    }
    std::int32_t PrepareInheritedTeardown(std::monostate,RE::TESObjectREFR* a_beacon,std::int32_t a_token)
    {
        namespace TP=ClaimablePOIs::TeardownProtection;
        std::lock_guard lock(g_furnitureEnrollmentMutex);
        const auto token=static_cast<std::uint32_t>(a_token);
        if (!TP::Ready() || !LiveVanillaBeacon(a_beacon) || !TP::Matches(token,a_beacon->formID) ||
            ClassifyProtectedBeacon(a_beacon)!=2 || g_trackedUnlockActive.load() ||
            g_claimTimeEnrollmentCandidateCount || g_sessionFurnitureCount ||
            g_furnitureEnrollmentFailed.load()) return 0;
        auto* player=RE::PlayerCharacter::GetSingleton();
        const auto epoch=g_overlayDiagnosticEpoch.load();
        if (!player || !player->GetParentWorldSpace() || player->GetParentWorldSpace()!=a_beacon->GetParentWorldSpace()) return 0;
        const RE::NiPointer<RE::TESObjectREFR> beacon(a_beacon);
        std::vector<std::uint32_t> ids;
        if (!TP::Snapshot(TeardownList(TP::kInheritedItems),ids)) {
            REX::ERROR("InheritedTeardown-v4: unreadable ownership list; no detach attempted");
            return 0;
        }
        std::vector<RE::NiPointer<RE::TESObjectREFR>> targets;
        try { targets.reserve(ids.size()); } catch (...) { return 0; }
        // Complete preflight before any detach. Foreign workshop links and built
        // objects never enter the list; deleted references stay deleted.
        for (const auto id:ids) {
            if (!id) continue;
            auto* item=RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
            bool owned{};
            if (!InspectInherited(item,a_beacon,owned)) {
                REX::WARN("InheritedTeardown-v4: unresolved/protected ownership ref={:08X}; no detach attempted",id);
                return 0;
            }
            if (owned) targets.emplace_back(item);
        }
        const auto base=REX::FModule::GetExecutingModule().GetBaseAddress();
        if (!VerifyClaimTimeWorkshopAttachTarget(base) || !VerifyClaimTimeWorkshopPredicateTarget(base)) return 0;
        // Registry and complete survivor capture must pass before detaching any
        // inherited link. The dispatch repeats them in its own synchronous scope.
        if (!TP::PreflightRemoval(a_beacon)) {
            REX::ERROR("InheritedTeardown-v4: preparation preflight rejected; zero objects detached, vanilla removal blocked");
            return 0;
        }
        const auto batch=ClaimablePOIs::DetachForTeardown(targets,
            [&] { return epoch==g_overlayDiagnosticEpoch.load() && TP::Ready() && TP::Matches(token,a_beacon->formID); },
            [&](const auto& target) { bool owned{}; return InspectInherited(target.get(),a_beacon,owned) && owned; },
            [&](const auto& target) { std::uint32_t exception{}; return CallClaimTimeWorkshopAttach(base,target.get(),false,a_beacon,exception); },
            [&](const auto& target) {
                std::uint32_t exception{}; bool after=true,owned=true;
                return CallClaimTimeWorkshopPredicate(base,target.get(),after,exception) && !after &&
                    InspectInherited(target.get(),a_beacon,owned) && !owned;
            });
        if (!batch.success) {
            if (batch.attempted) TP::Fault();
            REX::ERROR("InheritedTeardown-v4: detach batch blocked attempted={} detached={}; vanilla removal blocked",batch.attempted,batch.detached);
            return batch.attempted ? -1 : 0;
        }
        if (epoch!=g_overlayDiagnosticEpoch.load() || !AllInheritedDetached(a_beacon) || !TP::Authorize(token,a_beacon->formID)) {
            TP::Fault(); return -1;
        }
        REX::INFO("InheritedTeardown-v4: prepared beacon={:08X} token={} detached={} originalTransformsInventoryEnabledStateUntouched=true",
            a_beacon->formID,token,targets.size());
        if (!TP::EnqueueRemoval(a_beacon,token)) {
            const bool healthy=TP::Ready();
            TP::Cancel(token);
            return healthy ? -2 : -1;
        }
        return static_cast<std::int32_t>(targets.size()+1);
    }

	bool NotifyPlayerLoaded(std::monostate, RE::Actor* a_player)
	{
		if (!a_player || a_player != RE::PlayerCharacter::GetSingleton()) {
			return false;
		}
        ClaimablePOIs::TeardownProtection::ResetLoad();
		ClaimablePOIs::ProceduralLifecycleDiagnostics::Reset(
			g_overlayDiagnosticEpoch.load(std::memory_order_acquire) + 1);
		// OnPlayerLoadGame is delivered by the game's Papyrus VM. SFSE 0.2.21
		// does not install its save/load messaging hooks. The old property address
		// belongs to the world we left: discard metadata only, NEVER restore it here.
		const bool wasActive = g_trackedUnlockActive.load(std::memory_order_acquire);
		const auto oldHelper = g_trackedHelperFormID.load(std::memory_order_relaxed);
		const auto oldCell = g_trackedCellFormID.load(std::memory_order_relaxed);
		ClearPOIOverlayUnlockTrackingInternal();

		// Do not detach, release, or dereference retained engine objects after a
		// world replacement. An interrupted enrollment requires a clean process.
		const bool retainedReferences = g_claimTimeEnrollmentCandidateCount != 0 ||
			g_claimTimeEnrollmentBeacon.get() || g_claimTimeEnrollmentHelper.get() ||
			g_claimTimeEnrollmentCell.get() || g_sessionFurnitureCount != 0 ||
			g_sessionFurnitureBeacon.get() ||
			std::ranges::any_of(g_claimTimeEnrollmentCandidates, [](const auto& a_ref) { return a_ref.get() != nullptr; }) ||
			std::ranges::any_of(g_sessionFurniture, [](const auto& a_ref) { return a_ref.get() != nullptr; });
		const bool furnitureFailed = g_furnitureEnrollmentFailed.load(std::memory_order_acquire);
		REX::INFO("LoadBoundary source=Papyrus oldTracking={} oldHelper={:08X} oldCell={:08X} retainedReferences={} furnitureFailed={} ready={}",
			wasActive, oldHelper, oldCell, retainedReferences, furnitureFailed, !retainedReferences && !furnitureFailed);
		if (retainedReferences || furnitureFailed) {
			REX::ERROR("LoadBoundary blocked: interrupted/failed native enrollment; restart from a pre-claim save");
			return false;
		}
		g_claimTimeEnrollmentStatus = 0;

		// Dynamic FormIDs can be reused by another loaded save. A location ID alone
		// cannot authorize comparison against a baseline from that previous world.
		g_populationDiagnosticBaselineValid = false;
		g_populationDiagnosticBaseline.clear();
		g_overlayDiagnosticEpoch.fetch_add(1, std::memory_order_acq_rel);
		{
			std::scoped_lock lock(g_miniSiteDiagnosticMutex);
			g_miniSiteDiagnosticSession.Reset(g_overlayDiagnosticEpoch.load(std::memory_order_acquire));
			g_miniSiteDiagnosticCells.clear();
			g_selectedProceduralKeySamples = 0;
		}
		for (auto& samples : g_overlayDiagnosticSamples) samples.store(0, std::memory_order_relaxed);
		for (auto& samples : g_packInCensusSamples) samples.store(0, std::memory_order_relaxed);
		g_selectedOverlayDiagnosticSamples.store(0, std::memory_order_relaxed);
		REX::INFO("CellWorldspaceAudit-v1 load boundary: population baseline invalidated; no external cache read");

		// Registry is diagnostic context only; the query hooks preserve vanilla outputs.
		ClaimablePOIs::PCMOverlayHook::ResetQueryDiagnostics();
		ClaimablePOIs::PCMOverlayHook::SyncFromSave();

		return true; // Keep exact overlay recovery available even if teardown guards failed.
	}

	bool ResumePOIOverlayRecoveryNative(std::monostate, RE::TESObjectREFR* a_helper)
	{
		return SafeResumePOIOverlayUnlock(a_helper, true);
	}

	bool RecoverClaimedPOIWorkshopUnlockNative(std::monostate, RE::TESObjectREFR* a_helper)
	{
		return SafeRecoverClaimedPOIWorkshopUnlock(a_helper);
	}

	void LogSelectedOverlayDiagnostic(RE::TESObjectREFR* a_reference, const ReferenceDiagnosticState& a_state);

	bool LogSelectedProceduralKey(RE::TESObjectREFR* a_reference, const ReferenceDiagnosticState& a_state)
	{
		namespace PK = ClaimablePOIs::ProceduralKeyDiagnostics;
		const bool runtime = PK::VerifyRuntime();
		std::scoped_lock lock(g_miniSiteDiagnosticMutex);
		const auto epoch = g_overlayDiagnosticEpoch.load(std::memory_order_acquire);
		const auto& session = g_miniSiteDiagnosticSession;
		const double dx = static_cast<double>(a_state.x) - session.origin[0];
		const double dy = static_cast<double>(a_state.y) - session.origin[1];
		const double dz = static_cast<double>(a_state.z) - session.origin[2];
		const double distance = dx * dx + dy * dy + dz * dz;
		if (!runtime || session.epoch != epoch || !session.nextCheckpoint || session.world != a_state.worldspaceID ||
			!std::isfinite(distance) || distance > ClaimablePOIs::MiniSiteDiagnostics::kRadius * ClaimablePOIs::MiniSiteDiagnostics::kRadius ||
			g_selectedProceduralKeySamples >= PK::kSelectedCap) {
			REX::WARN("ProceduralKey-v1 selected rejected ref={:08X} reason=runtime-anchor-or-budget stopTrial=true readOnly=true", a_state.refID);
			return false;
		}
		++g_selectedProceduralKeySamples;
		auto entry = MakePopulationDiagnosticEntry(a_state);
		entry.proceduralKey = PK::Observe(a_reference, a_state.refID);
		if (!entry.proceduralKey.readable || entry.proceduralKey.present != a_state.hasProceduralInstanceData ||
			epoch != g_overlayDiagnosticEpoch.load(std::memory_order_acquire)) {
			REX::WARN("ProceduralKey-v1 selected failed ref={:08X} exception=0x{:08X} reason=read-state-or-epoch stopTrial=true readOnly=true",
				a_state.refID, entry.proceduralKey.exception);
			return false;
		}
		LogProceduralKey("selected", static_cast<std::int32_t>(session.nextCheckpoint - 1), epoch, session.world, entry);
		return true;
	}

	bool LogReferenceState(std::monostate, RE::TESObjectREFR* a_reference, std::int32_t a_stage)
	{
		if (!ClaimablePOIs::Diagnostics::Enabled()) return false;
		ReferenceDiagnosticState state{};
		if (!SafeCollectReferenceDiagnosticState(a_reference, state)) {
			REX::WARN(
				"ReferenceState stage={}({}) unavailable ref={}",
				a_stage,
				GetReferenceDiagnosticStage(a_stage),
				static_cast<const void*>(a_reference));
			return false;
		}

		if (a_stage == 15 && !LogSelectedProceduralKey(a_reference, state)) return false;
		REX::INFO(
			"ReferenceState stage={}({}) ref=0x{:08X} base=0x{:08X} type={} cell=0x{:08X} location=0x{:08X} "
			"worldspace=0x{:08X} pos=({:.2f},{:.2f},{:.2f}) state[deleted={} disabled={}] extras[list={} workshop={} proceduralInstance={} "
			"placedPlanetContent={} radius={} linkedRef={} location={} promotedRef={} sourcePackIn={} owningInstance={} "
			"keywords={} ownership={} enableStateParent={} enableStateChildren={} linkedRefChildren={} "
			"objectInstance={} workshopSnapping={} furnitureEntryData={} snapLinks={} groupedPackin={} calcedPackIn={}]",
			a_stage,
			GetReferenceDiagnosticStage(a_stage),
			state.refID,
			state.baseID,
			state.baseType,
			state.cellID,
			state.locationID,
			state.worldspaceID,
			state.x,
			state.y,
			state.z,
			state.deleted,
			state.disabled,
			state.hasExtraDataList,
			state.hasWorkshop,
			state.hasProceduralInstanceData,
			state.hasPlacedPlanetContent,
			state.hasRadius,
			state.hasLinkedRef,
			state.hasLocation,
			state.hasPromotedRef,
			state.hasSourcePackIn,
			state.hasOwningInstance,
			state.hasKeywords,
			state.hasOwnership,
			state.hasEnableStateParent,
			state.hasEnableStateChildren,
			state.hasLinkedRefChildren,
			state.hasObjectInstance,
			state.hasWorkshopSnapping,
			state.hasFurnitureEntryData,
			state.hasSnapLinks,
			state.hasGroupedPackin,
			state.hasCalcedPackIn);
		if (a_stage == 15) {
			LogSelectedOverlayDiagnostic(a_reference, state);
		}
		if (a_stage == 15 || a_stage == 16) {
			LogWorkshopComparisonTelemetry(a_reference, a_stage);
		}
		if (a_stage == 2 || a_stage == 3 || a_stage == 6 || a_stage == 7) {
			ClaimablePOIs::PCMOverlayHook::RegisterBeacon(a_reference);
		}
		return true;
	}

	// Observe the currently selected overlay, not the old captured property pointer.
	// ID and flag are copied before returning; no engine pointer survives this call.
	bool RawReadOverlayDiagnostic(std::uintptr_t a_moduleBase, RE::TESObjectCELL* a_cell, RE::BGSLocation* a_location,
		std::uint32_t& a_overlayID, std::uint8_t& a_flag, bool& a_neverResets, std::uint32_t& a_exception) noexcept
	{
		__try {
			void* property = nullptr;
			RE::TESWorldSpace* overlay = nullptr;
			if (!RawResolveCellProperty(a_moduleBase, a_cell, property, a_flag, a_exception, &overlay) ||
				!overlay || !overlay->Is(RE::FormType::kWRLD)) return false;
			a_overlayID = overlay->formID;
			a_neverResets = a_location->flags.any(RE::BGSLocation::Flags::kNoReset);
			return true;
		} __except (a_exception = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool RawReadSelectedOverlayDiagnostic(std::uintptr_t a_moduleBase, RE::TESObjectREFR* a_reference,
		const ReferenceDiagnosticState& a_state, std::uint32_t& a_overlayID, std::uint8_t& a_flag,
		bool& a_neverResets, std::uint32_t& a_exception) noexcept
	{
		__try {
			if (!a_reference || a_reference->formID != a_state.refID) return false;
			auto* cell = a_reference->parentCell;
			auto* location = a_reference->GetCurrentLocation();
			auto* world = a_reference->GetParentWorldSpace();
			if (!cell || !location || !world || cell->formID != a_state.cellID ||
				location->formID != a_state.locationID || world->formID != a_state.worldspaceID) return false;
			return RawReadOverlayDiagnostic(a_moduleBase, cell, location, a_overlayID, a_flag, a_neverResets, a_exception);
		} __except (a_exception = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	void LogSelectedOverlayDiagnostic(RE::TESObjectREFR* a_reference, const ReferenceDiagnosticState& a_state)
	{
		// NeighborOverlayControl-v1: manual probe only, at most 32 samples per load.
		// Reuse the verified cell resolver; it selects a cell overlay, not an object's owner.
		// No pointer is retained and no property, link, or PCM result is modified.
		const auto epoch = g_overlayDiagnosticEpoch.load(std::memory_order_acquire);
		const auto sample = g_selectedOverlayDiagnosticSamples.fetch_add(1, std::memory_order_relaxed);
		if (sample >= 32) return;
		const auto module = REX::FModule::GetExecutingModule();
		const bool layout = module.GetFileVersion() == SFSE::RUNTIME_SF_1_16_244 && VerifyEngineLayout(module.GetBaseAddress());
		std::uint32_t overlayID = 0, exception = 0;
		std::uint8_t flag = 0;
		bool neverResets = false;
		const bool resolved = layout && RawReadSelectedOverlayDiagnostic(module.GetBaseAddress(), a_reference, a_state,
			overlayID, flag, neverResets, exception);
		if (epoch != g_overlayDiagnosticEpoch.load(std::memory_order_acquire)) return;
		REX::INFO("NeighborOverlayControl-v1 epoch={} sample={}/32 ref={:08X} base={:08X} world={:08X} cell={:08X} location={:08X} pos=({:.2f},{:.2f},{:.2f}) layout={} resolved={} cellSelectedOverlay={:08X} flag=0x{:02X} locationNeverResets={} exception=0x{:08X} ownershipProven=false readOnly=true",
			epoch, sample + 1, a_state.refID, a_state.baseID, a_state.worldspaceID, a_state.cellID, a_state.locationID,
			a_state.x, a_state.y, a_state.z, layout, resolved, overlayID, flag, neverResets, exception);
	}

	void LogOverlayResolutionDiagnostic(RE::TESObjectCELL* a_cell, std::uint32_t a_worldID,
		RE::BGSLocation* a_location, std::int32_t a_stage)
	{
		// Same eight population stages, at most eight records each per actual load.
		// This diagnostic neither authorizes a claim nor changes an engine result.
		const auto epoch = g_overlayDiagnosticEpoch.load(std::memory_order_acquire);
		if (g_overlayDiagnosticSamples[a_stage].fetch_add(1, std::memory_order_relaxed) >= 8) return;
		const auto module = REX::FModule::GetExecutingModule();
		const bool layout = module.GetFileVersion() == SFSE::RUNTIME_SF_1_16_244 && VerifyEngineLayout(module.GetBaseAddress());
		std::uint32_t overlayID = 0, exception = 0;
		std::uint8_t flag = 0;
		bool neverResets = false;
		const bool resolved = layout && RawReadOverlayDiagnostic(module.GetBaseAddress(), a_cell, a_location, overlayID, flag, neverResets, exception);
		if (epoch != g_overlayDiagnosticEpoch.load(std::memory_order_acquire)) return;
		REX::INFO("OverlayReturnAudit-v1 epoch={} stage={} world={:08X} cell={:08X} location={:08X} layout={} resolved={} selectedOverlay={:08X} flag=0x{:02X} locationNeverResets={} exception=0x{:08X} readOnly=true",
			epoch, a_stage, a_worldID, a_cell->formID, a_location->formID, layout, resolved, overlayID, flag, neverResets, exception);
	}

	std::int32_t LogMiniSiteCensus(std::monostate, RE::TESObjectREFR* a_center,
		std::vector<RE::TESObjectCELL*> a_loadedCells, RE::BGSLocation* a_location, std::int32_t a_checkpoint)
	{
		if (!ClaimablePOIs::Diagnostics::Enabled()) return -1;
		namespace MD = ClaimablePOIs::MiniSiteDiagnostics;
		namespace PD = ClaimablePOIs::ProceduralLifecycleDiagnostics;
		struct FailureScope
		{
			bool complete{};
			~FailureScope() { if (!complete) PD::Abort(); }
		} traceScope;
		ReferenceDiagnosticState state{};
		if (a_checkpoint < 0 || a_checkpoint > 3 || a_center != RE::PlayerCharacter::GetSingleton() ||
			!a_location || !SafeCollectReferenceDiagnosticState(a_center, state) ||
			!state.worldspaceID || !state.cellID || state.locationID != a_location->formID) {
			REX::WARN("MiniSiteObservation-v1 rejected checkpoint={} reason=player-context readOnly=true", a_checkpoint);
			return -1;
		}
		const auto module = REX::FModule::GetExecutingModule();
		if (module.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244 || !VerifyEngineLayout(module.GetBaseAddress()) ||
			!ClaimablePOIs::ProceduralKeyDiagnostics::VerifyRuntime()) {
			REX::WARN("MiniSiteObservation-v1 rejected checkpoint={} reason=runtime-layout readOnly=true", a_checkpoint);
			return -1;
		}
		std::scoped_lock lock(g_miniSiteDiagnosticMutex);
		const auto epoch = g_overlayDiagnosticEpoch.load(std::memory_order_acquire);
		const auto origin = g_miniSiteDiagnosticSession.Begin(a_checkpoint, state.worldspaceID,
			MD::Origin{ state.x, state.y, state.z }, epoch);
		if (!origin) {
			REX::WARN("MiniSiteObservation-v1 rejected epoch={} checkpoint={} next={} world={:08X} reason=anchor-order-or-budget readOnly=true",
				epoch, a_checkpoint, g_miniSiteDiagnosticSession.nextCheckpoint, state.worldspaceID);
			return -1;
		}
		PopulationDiagnosticScan scan{};
		scan.packInCensus = true;
		scan.packInLayoutVerified = true;
		scan.proceduralKeys = true;
		const RE::NiPoint3A scanOrigin{ (*origin)[0], (*origin)[1], (*origin)[2] };
		if (!CollectPopulationDiagnostic(a_center, a_center->parentCell, a_loadedCells, a_location, MD::kRadius, scanOrigin, scan) ||
			epoch != g_overlayDiagnosticEpoch.load(std::memory_order_acquire)) {
			REX::WARN("MiniSiteObservation-v1 failed epoch={} checkpoint={} scanned={} captured={} rejected={} truncated={} layoutRejected={} readRejected={} epochChanged={} readOnly=true",
				epoch, a_checkpoint, scan.scanned, scan.entries.size(), scan.stateRejected, scan.truncated,
				scan.layoutRejected, scan.readRejected, epoch != g_overlayDiagnosticEpoch.load(std::memory_order_acquire));
			return -1;
		}
		std::uint32_t roots = 0, logged = 0;
		for (const auto& entry : scan.entries) {
			if (entry.baseType != static_cast<std::uint32_t>(RE::FormType::kPKIN) &&
				entry.baseType != static_cast<std::uint32_t>(RE::FormType::kLVLP)) continue;
			++roots;
			if (logged >= MD::kRootLogCap) continue;
			++logged;
			REX::INFO("MiniSiteObservation-v1 root epoch={} checkpoint={} world={:08X} ref={:08X} base={:08X} type={} cell={:08X} location={:08X} pos=({:.2f},{:.2f},{:.2f}) flags=0x{:06X} ownershipProven=false readOnly=true",
				epoch, a_checkpoint, state.worldspaceID, entry.refID, entry.baseID, entry.baseType,
				entry.cellID, entry.locationID, entry.x, entry.y, entry.z, entry.stateFlags);
			LogProceduralKey("root", a_checkpoint, epoch, state.worldspaceID, entry);
		}
		// Nearest structural sample before console selection; plain values only.
		std::vector<const PopulationDiagnosticEntry*> children;
		for (const auto& entry : scan.entries) {
			const auto type = static_cast<RE::FormType>(entry.baseType);
			if (type == RE::FormType::kSTAT || type == RE::FormType::kSCOL || type == RE::FormType::kACTI ||
				type == RE::FormType::kMSTT) children.push_back(&entry);
		}
		auto distance = [&](const PopulationDiagnosticEntry* entry) {
			const double dx = static_cast<double>(entry->x) - (*origin)[0];
			const double dy = static_cast<double>(entry->y) - (*origin)[1];
			const double dz = static_cast<double>(entry->z) - (*origin)[2];
			return dx * dx + dy * dy + dz * dz;
		};
		std::sort(children.begin(), children.end(), [&](const auto* left, const auto* right) {
			const auto lhs = distance(left), rhs = distance(right);
			return lhs != rhs ? lhs < rhs : left->refID < right->refID;
		});
		const auto childLogged = std::min<std::size_t>(children.size(), ClaimablePOIs::ProceduralKeyDiagnostics::kChildLogCap);
		for (std::size_t i = 0; i < childLogged; ++i) LogProceduralKey("structural-sample", a_checkpoint, epoch, state.worldspaceID, *children[i]);
		REX::INFO("ProceduralKey-v1 summary epoch={} checkpoint={} reads={} structuralCandidates={} structuralLogged={} spatialSample=true selectedBudget=32 reverseLookupCalled=false readOnly=true",
			epoch, a_checkpoint, scan.proceduralKeyReads, children.size(), childLogged);
		for (std::uint32_t i = 0; i < scan.packInCellCount; ++i) {
			const auto& cell = scan.packInCellOverlays[i];
			std::uint32_t members = 0, procedural = 0, promoted = 0, workshop = 0;
			for (const auto& entry : scan.entries) {
				if (entry.cellID != cell.cell) continue;
				++members;
				procedural += (entry.stateFlags & (1u << 5)) != 0;
				promoted += (entry.stateFlags & (1u << 10)) != 0;
				workshop += (entry.stateFlags & (1u << 4)) != 0;
			}
			REX::INFO("MiniSiteObservation-v1 cell epoch={} checkpoint={} world={:08X} cell={:08X} selectedOverlay={:08X} flag=0x{:02X} resolved={} exception=0x{:08X} inRadius={} procedural={} promoted={} workshop={} ownershipProven=false readOnly=true",
				epoch, a_checkpoint, state.worldspaceID, cell.cell, cell.overlay, cell.flag, cell.resolved,
				cell.exception, members, procedural, promoted, workshop);
		}
		bool complete = roots == logged && !scan.packInCellsTruncated && scan.stateRejected == 0 &&
			(a_checkpoint != 0 || roots != 0);
		std::vector<PD::Seed> seeds;
		seeds.reserve(scan.entries.size());
		std::uint32_t loot = 0;
		for (const auto& entry : scan.entries) {
			seeds.push_back({ entry.refID, entry.proceduralKey.key.parts, entry.proceduralKey.present });
			if (entry.baseType != static_cast<std::uint32_t>(RE::FormType::kCONT)) continue;
			if (++loot > 64) { complete = false; continue; }
			REX::INFO("MiniSelection-v1 container epoch={} checkpoint={} world={:08X} ref={:08X} base={:08X} cell={:08X} location={:08X} pos=({:.2f},{:.2f},{:.2f}) flags=0x{:06X} keyPresent={} key=({:08X},{:08X},{:08X}) ownershipProven=false readOnly=true",
				epoch, a_checkpoint, state.worldspaceID, entry.refID, entry.baseID, entry.cellID, entry.locationID,
				entry.x, entry.y, entry.z, entry.stateFlags, entry.proceduralKey.present,
				entry.proceduralKey.key.parts[0], entry.proceduralKey.key.parts[1], entry.proceduralKey.key.parts[2]);
		}
		if (complete) complete = PD::Checkpoint(epoch, state.worldspaceID, *origin,
			static_cast<std::uint32_t>(a_checkpoint), seeds);
		REX::INFO("MiniSiteObservation-v1 summary epoch={} checkpoint={} world={:08X} playerCell={:08X} location={:08X} origin=({:.2f},{:.2f},{:.2f}) playerPos=({:.2f},{:.2f},{:.2f}) radius={:.2f} roots={} logged={} rootsTruncated={} cells={} cellsTruncated={} sameCellCoverage={} scanned={} captured={} rejected={} complete={} ownershipProven=false readOnly=true",
			epoch, a_checkpoint, state.worldspaceID, state.cellID, state.locationID,
			(*origin)[0], (*origin)[1], (*origin)[2], state.x, state.y, state.z, MD::kRadius,
			roots, logged, roots != logged, scan.packInCellCount, scan.packInCellsTruncated,
			a_checkpoint != 0 && g_miniSiteDiagnosticCells == scan.cellIDs, scan.scanned, scan.entries.size(), scan.stateRejected, complete);
		if (!complete) {
			REX::WARN("MiniSiteObservation-v1 incomplete epoch={} checkpoint={} stopTrial=true readOnly=true", epoch, a_checkpoint);
			return -1;
		}
		if (a_checkpoint == 0) g_miniSiteDiagnosticCells = std::move(scan.cellIDs);
		g_miniSiteDiagnosticSession.Complete(state.worldspaceID, *origin);
		traceScope.complete = true;
		return static_cast<std::int32_t>(scan.entries.size());
	}

	std::int32_t LogReferencePopulation(
		std::monostate,
		RE::TESObjectREFR* a_center,
		RE::TESObjectCELL* a_claimCell,
		std::vector<RE::TESObjectCELL*> a_loadedCells,
		RE::BGSLocation* a_location,
		float a_radius,
		std::int32_t a_stage)
	{
		if (!ClaimablePOIs::Diagnostics::Enabled()) return -1;
		if (a_stage < 0 || a_stage > 7 || !a_center || !a_claimCell || !a_location) {
			REX::WARN("PopulationSnapshot rejected stage={} center={} cell={} location={}", a_stage, static_cast<void*>(a_center), static_cast<void*>(a_claimCell), static_cast<void*>(a_location));
			return -1;
		}

		if (a_stage == 0) {
			g_populationDiagnosticBaselineValid = false;
		}
		const auto* worldspace = a_center->GetParentWorldSpace();
		if (!worldspace || a_center->parentCell != a_claimCell) {
			REX::WARN("PopulationSnapshot rejected stage={} exterior worldspace/center-cell mismatch", a_stage);
			return -1;
		}
		LogOverlayResolutionDiagnostic(a_claimCell, worldspace->formID, a_location, a_stage);
		const bool baselineAvailable = a_stage != 0 && g_populationDiagnosticBaselineValid &&
			g_populationDiagnosticLocationID == a_location->formID &&
			g_populationDiagnosticWorldspaceID == worldspace->formID &&
			g_populationDiagnosticRadius == a_radius;
		const auto centerPosition = a_center->GetPosition();
		const RE::NiPoint3A currentOrigin{ centerPosition.x, centerPosition.y, centerPosition.z };
		const RE::NiPoint3A scanOrigin = baselineAvailable ? g_populationDiagnosticOrigin : currentOrigin;

		PopulationDiagnosticScan scan{};
		const auto censusEpoch = g_overlayDiagnosticEpoch.load(std::memory_order_acquire);
		// Two windows per relevant stage per actual load; workshop scans are excluded.
		scan.packInCensus = (a_stage == 0 || a_stage == 1 || a_stage == 6 || a_stage == 7) &&
			g_packInCensusSamples[a_stage].fetch_add(1, std::memory_order_relaxed) < 2;
		if (scan.packInCensus) {
			const auto module = REX::FModule::GetExecutingModule();
			scan.packInLayoutVerified = module.GetFileVersion() == SFSE::RUNTIME_SF_1_16_244 && VerifyEngineLayout(module.GetBaseAddress());
		}
		if (!CollectPopulationDiagnostic(a_center, a_claimCell, a_loadedCells, a_location, a_radius, scanOrigin, scan)) {
			REX::WARN(
				"PopulationSnapshot failed stage={}({}) candidates={} attachedExterior={} worldspaceMatched={} worldspaceMismatched={} claimCellMatched={} scanned={} captured={} rejected={} truncated={} layoutRejected={} readRejected={}",
				a_stage,
				GetPopulationDiagnosticStage(a_stage),
				scan.candidateCells,
				scan.attachedExteriorCells,
				scan.worldspaceMatchedCells,
				scan.worldspaceMismatchedCells,
				scan.claimCellWorldspaceMatched,
				scan.scanned,
				scan.entries.size(),
				scan.stateRejected,
				scan.truncated,
				scan.layoutRejected,
				scan.readRejected);
			return -1;
		}

		if (scan.packInCensus && censusEpoch == g_overlayDiagnosticEpoch.load(std::memory_order_acquire)) {
			// These are actual base types, not guessed SourcePackIn/instance payloads.
			// Root position/template changes distinguish reselection from child loss;
			// cell-selected overlay is still not proof of reference ownership.
			std::uint32_t roots = 0, logged = 0;
			for (const auto& entry : scan.entries) {
				if (entry.baseType != static_cast<std::uint32_t>(RE::FormType::kPKIN) &&
					entry.baseType != static_cast<std::uint32_t>(RE::FormType::kLVLP)) continue;
				++roots;
				if (logged >= 128) continue;
				++logged;
				REX::INFO("PackInCensus-v1 root epoch={} stage={} world={:08X} ref={:08X} base={:08X} type={} cell={:08X} location={:08X} pos=({:.2f},{:.2f},{:.2f}) flags=0x{:06X} ownershipProven=false readOnly=true",
					censusEpoch, a_stage, worldspace->formID, entry.refID, entry.baseID, entry.baseType,
					entry.cellID, entry.locationID, entry.x, entry.y, entry.z, entry.stateFlags);
			}
			for (std::uint32_t i = 0; i < scan.packInCellCount; ++i) {
				const auto& cell = scan.packInCellOverlays[i];
				std::uint32_t members = 0, procedural = 0, promoted = 0, workshop = 0;
				for (const auto& entry : scan.entries) {
					if (entry.cellID != cell.cell) continue;
					++members;
					procedural += (entry.stateFlags & (1u << 5)) != 0;
					promoted += (entry.stateFlags & (1u << 10)) != 0;
					workshop += (entry.stateFlags & (1u << 4)) != 0;
				}
				REX::INFO("PackInCensus-v1 cell epoch={} stage={} world={:08X} cell={:08X} selectedOverlay={:08X} flag=0x{:02X} resolved={} exception=0x{:08X} inRadius={} procedural={} promoted={} workshop={} ownershipProven=false readOnly=true",
					censusEpoch, a_stage, worldspace->formID, cell.cell, cell.overlay, cell.flag, cell.resolved, cell.exception,
					members, procedural, promoted, workshop);
			}
			REX::INFO("PackInCensus-v1 summary epoch={} stage={} world={:08X} origin=({:.2f},{:.2f},{:.2f}) radius={:.2f} roots={} logged={} rootsTruncated={} cells={} cellsTruncated={} layout={} readOnly=true",
				censusEpoch, a_stage, worldspace->formID, scanOrigin.x, scanOrigin.y, scanOrigin.z, a_radius,
				roots, logged, roots > logged, scan.packInCellCount, scan.packInCellsTruncated, scan.packInLayoutVerified);
		}

		REX::INFO("PopulationScope trial=CellWorldspaceAudit-v1 stage={} worldspace={:08X} location={:08X} centerCell={:08X} baselineComparable={} candidates={} attachedExterior={} worldspaceMatched={} worldspaceMismatched={} claimCellMatched={} cells={}",
			a_stage, worldspace->formID, a_location->formID, a_claimCell->formID, baselineAvailable,
			scan.candidateCells, scan.attachedExteriorCells, scan.worldspaceMatchedCells, scan.worldspaceMismatchedCells,
			scan.claimCellWorldspaceMatched, scan.cellIDs.size());
		for (const auto cellID : scan.cellIDs) {
			REX::INFO("PopulationCell stage={} worldspace={:08X} cell={:08X}", a_stage, worldspace->formID, cellID);
		}
		if (baselineAvailable) {
			LogPopulationFootprint(a_stage, scan);
		}

		std::array<std::uint32_t, 256> typeCounts{};
		std::uint32_t proceduralCount = 0;
		std::uint32_t sourcePackInCount = 0;
		std::uint32_t owningInstanceCount = 0;
		std::uint32_t enableParentCount = 0;
		std::uint32_t enableChildrenCount = 0;
		std::uint32_t groupedPackInCount = 0;
		std::uint32_t calcedPackInCount = 0;
		std::uint32_t disabledCount = 0;
		for (const auto& entry : scan.entries) {
			if (entry.baseType < typeCounts.size()) {
				++typeCounts[entry.baseType];
			}
			auto has = [&entry](std::uint32_t a_bit) { return (entry.stateFlags & (1u << a_bit)) != 0; };
			proceduralCount += has(5) ? 1u : 0u;
			sourcePackInCount += has(11) ? 1u : 0u;
			owningInstanceCount += has(12) ? 1u : 0u;
			enableParentCount += has(15) ? 1u : 0u;
			enableChildrenCount += has(16) ? 1u : 0u;
			groupedPackInCount += has(22) ? 1u : 0u;
			calcedPackInCount += has(23) ? 1u : 0u;
			disabledCount += has(1) ? 1u : 0u;
		}

		std::uint32_t missingCount = 0;
		std::uint32_t addedCount = 0;
		std::uint32_t changedCount = 0;
		std::uint32_t missingLogged = 0;
		std::uint32_t addedLogged = 0;
		std::uint32_t changedLogged = 0;
		if (baselineAvailable) {
			std::size_t baselineIndex = 0;
			std::size_t currentIndex = 0;
			while (baselineIndex < g_populationDiagnosticBaseline.size() || currentIndex < scan.entries.size()) {
				if (currentIndex >= scan.entries.size() ||
					(baselineIndex < g_populationDiagnosticBaseline.size() && g_populationDiagnosticBaseline[baselineIndex].refID < scan.entries[currentIndex].refID)) {
					const auto& missing = g_populationDiagnosticBaseline[baselineIndex++];
					++missingCount;
					if (missingLogged++ < kPopulationDiagnosticMissingLogCap) {
						LogPopulationDiagnosticEntry("missing", a_stage, missing);
					}
					continue;
				}
				if (baselineIndex >= g_populationDiagnosticBaseline.size() || scan.entries[currentIndex].refID < g_populationDiagnosticBaseline[baselineIndex].refID) {
					const auto& added = scan.entries[currentIndex++];
					++addedCount;
					if (addedLogged++ < kPopulationDiagnosticAddedLogCap) {
						LogPopulationDiagnosticEntry("added", a_stage, added);
					}
					continue;
				}

				const auto& baseline = g_populationDiagnosticBaseline[baselineIndex++];
				const auto& current = scan.entries[currentIndex++];
				const bool positionChanged = std::fabs(baseline.x - current.x) > 0.01F ||
					std::fabs(baseline.y - current.y) > 0.01F || std::fabs(baseline.z - current.z) > 0.01F;
				if (baseline.baseID != current.baseID || baseline.baseType != current.baseType ||
					baseline.cellID != current.cellID || baseline.locationID != current.locationID ||
					baseline.stateFlags != current.stateFlags || positionChanged) {
					++changedCount;
					if (changedLogged++ < kPopulationDiagnosticChangedLogCap) {
						LogPopulationDiagnosticEntry("changed-before", a_stage, baseline);
						LogPopulationDiagnosticEntry("changed-after", a_stage, current);
					}
				}
			}
		}

		REX::INFO(
			"PopulationSnapshot stage={}({}) location={:08X} center={:08X} currentOrigin=({:.2f},{:.2f},{:.2f}) scanOrigin=({:.2f},{:.2f},{:.2f}) radius={:.2f} scanned={} captured={} stateRejected={} baseline={} baselineCount={} missing={} added={} changed={} missingTruncated={} addedTruncated={} changedTruncated={} metadata[disabled={} procedural={} sourcePackIn={} owningInstance={} enableParent={} enableChildren={} groupedPackIn={} calcedPackIn={}]",
			a_stage,
			GetPopulationDiagnosticStage(a_stage),
			a_location->formID,
			a_center->formID,
			currentOrigin.x,
			currentOrigin.y,
			currentOrigin.z,
			scanOrigin.x,
			scanOrigin.y,
			scanOrigin.z,
			a_radius,
			scan.scanned,
			scan.entries.size(),
			scan.stateRejected,
			baselineAvailable,
			g_populationDiagnosticBaseline.size(),
			missingCount,
			addedCount,
			changedCount,
			missingCount > kPopulationDiagnosticMissingLogCap,
			addedCount > kPopulationDiagnosticAddedLogCap,
			changedCount > kPopulationDiagnosticChangedLogCap,
			disabledCount,
			proceduralCount,
			sourcePackInCount,
			owningInstanceCount,
			enableParentCount,
			enableChildrenCount,
			groupedPackInCount,
			calcedPackInCount);
		for (std::uint32_t type = 0; type < typeCounts.size(); ++type) {
			if (typeCounts[type] != 0) {
				REX::INFO("PopulationType stage={}({}) type={} count={}", a_stage, GetPopulationDiagnosticStage(a_stage), type, typeCounts[type]);
			}
		}

		const auto capturedCount = static_cast<std::int32_t>(scan.entries.size());
		if (a_stage == 0) {
			g_populationDiagnosticBaseline = std::move(scan.entries);
			g_populationDiagnosticLocationID = a_location->formID;
			g_populationDiagnosticWorldspaceID = worldspace->formID;
			g_populationDiagnosticRadius = a_radius;
			g_populationDiagnosticCellIDs = std::move(scan.cellIDs);
			g_populationDiagnosticOrigin = currentOrigin;
			g_populationDiagnosticBaselineValid = true;
			REX::INFO("PopulationBaseline captured location={:08X} count={} origin=({:.2f},{:.2f},{:.2f})", g_populationDiagnosticLocationID, g_populationDiagnosticBaseline.size(), currentOrigin.x, currentOrigin.y, currentOrigin.z);
		}
		ClaimablePOIs::PCMOverlayHook::LogQuerySummary(a_stage);
		return capturedCount;
	}

	std::int32_t GetDecorationSafetyFlags(std::monostate, RE::TESObjectREFR* a_reference)
	{
		return SafeGetDecorationSafetyFlags(a_reference);
	}

	std::int32_t GetWorkshopReplacementSafetyFlags(std::monostate, RE::TESObjectREFR* a_reference)
	{
		return SafeGetWorkshopReplacementSafetyFlags(a_reference);
	}

	bool IsRuntimeOutpostBuildable(std::monostate, RE::TESForm* a_base)
	{
		return IsConstructibleBase(a_base);
	}

	RE::TESObjectREFR* BeginClaimTimeWorkshopEnrollment(
		std::monostate,
		RE::TESObjectCELL* a_cell,
		std::vector<RE::TESObjectCELL*> a_loadedCells,
		RE::BGSLocation* a_claimLocation,
		RE::TESObjectREFR* a_helper,
		RE::TESObjectREFR* a_beacon,
		float a_radius,
		RE::BGSListForm* a_excludedOriginals,
		RE::BGSListForm* a_excludedReplacements)
	{
		return SafeBeginClaimTimeWorkshopEnrollment(
			a_cell,
			a_loadedCells,
			a_claimLocation,
			a_helper,
			a_beacon,
			a_radius,
			a_excludedOriginals,
			a_excludedReplacements);
	}

	std::int32_t GetClaimTimeWorkshopEnrollmentStatus(std::monostate)
	{
		return g_claimTimeEnrollmentStatus;
	}

	bool CommitClaimTimeWorkshopEnrollment(std::monostate)
	{
		return SafeCommitClaimTimeWorkshopEnrollment();
	}

	bool RollbackClaimTimeWorkshopEnrollment(std::monostate)
	{
		return SafeRollbackClaimTimeWorkshopEnrollment();
	}

	bool CommitSelectedInheritedWorkshopItem(
		std::monostate,
		RE::TESObjectREFR* a_item,
		RE::TESObjectREFR* a_beacon,
		RE::BGSLocation* a_claimLocation,
		float a_radius,
		RE::BGSListForm* a_excludedOriginals,
		RE::BGSListForm* a_excludedReplacements)
	{
		if (!a_item || !a_beacon || !a_claimLocation || a_item->IsDeleted() || a_item->IsDisabled() ||
			a_beacon->IsDeleted() || a_beacon->IsDisabled() || !a_item->parentCell || !a_beacon->parentCell ||
			!a_item->parentCell->IsAttached() || !a_beacon->parentCell->IsAttached() ||
			!std::isfinite(a_radius) || a_radius <= 0.0F || a_radius > 10000.0F) {
			REX::ERROR("WorkshopCommitTrial rejected invalid lifecycle, cells, or radius");
			return false;
		}
		const auto itemBase = a_item->GetBaseObject();
		const auto beaconBase = a_beacon->GetBaseObject();
		if (!itemBase || itemBase->formID != kWorkshopCommitTrialBaseID ||
			!beaconBase || beaconBase->formID != kClaimTimeVanillaBeaconBaseID ||
			a_beacon->GetCurrentLocation() != a_claimLocation) {
			REX::WARN(
				"WorkshopCommitTrial rejected exact base or beacon context item={:08X} base={:08X} beacon={:08X}",
				a_item->formID,
				itemBase ? itemBase->formID : 0,
				a_beacon->formID);
			return false;
		}
		if (SafeGetWorkshopReplacementSafetyFlags(a_item) != 0 || HasClaimTimeWorkshopState(a_item)) {
			REX::WARN("WorkshopCommitTrial rejected unsafe or pre-enrolled item={:08X}", a_item->formID);
			return false;
		}
		bool originalsValid = true;
		bool replacementsValid = true;
		if (SafeFormListContainsReference(a_excludedOriginals, a_item, originalsValid) ||
			SafeFormListContainsReference(a_excludedReplacements, a_item, replacementsValid) ||
			!originalsValid || !replacementsValid) {
			REX::WARN("WorkshopCommitTrial rejected replacement-ledger item={:08X}", a_item->formID);
			return false;
		}
		ClaimTimeCandidateValidation validation{};
		if (!SafeValidateClaimTimeWorkshopCandidate(
				a_item,
				a_beacon,
				a_beacon,
				RE::PlayerCharacter::GetSingleton(),
				a_beacon->GetParentWorldSpace(),
				a_beacon->GetCurrentLocation(),
				a_radius,
				validation)) {
			REX::WARN(
				"WorkshopCommitTrial rejected validation item={:08X} reason={} flags=0x{:08X}",
				a_item->formID,
				validation.reason,
				validation.safetyFlags);
			return false;
		}

		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			REX::ERROR("WorkshopCommitTrial rejected unsupported runtime");
			return false;
		}
		const auto target = kWorkshopCommitFunctionID.address();
		auto* context = reinterpret_cast<void*>(kWorkshopCommitContextID.address());
		if (!VerifyWorkshopCommitTarget(target) ||
			!VerifyClaimTimeWorkshopPredicateTarget(gameModule.GetBaseAddress())) {
			REX::ERROR("WorkshopCommitTrial rejected commit or predicate prefix mismatch");
			return false;
		}
		RE::TESObjectREFR* contextBeacon = nullptr;
		std::uint32_t contextException = 0;
		if (!ReadWorkshopCommitContextBeacon(context, contextBeacon, contextException) || contextBeacon != a_beacon) {
			REX::WARN(
				"WorkshopCommitTrial rejected active context item={:08X} expectedBeacon={:08X} contextBeacon={:08X} exception=0x{:08X}",
				a_item->formID,
				a_beacon->formID,
				contextBeacon ? contextBeacon->formID : 0,
				contextException);
			return false;
		}
		bool beforePredicate = false;
		std::uint32_t beforeException = 0;
		if (!CallClaimTimeWorkshopPredicate(
				gameModule.GetBaseAddress(), a_item, beforePredicate, beforeException) || beforePredicate) {
			REX::WARN(
				"WorkshopCommitTrial rejected preexisting predicate item={:08X} predicate={} exception=0x{:08X}",
				a_item->formID,
				beforePredicate,
				beforeException);
			return false;
		}

		std::uint32_t commitException = 0;
		const bool commitCalled = CallWorkshopCommit(target, context, a_item, commitException);
		bool afterPredicate = false;
		std::uint32_t afterException = 0;
		const bool afterCalled = CallClaimTimeWorkshopPredicate(
			gameModule.GetBaseAddress(), a_item, afterPredicate, afterException);
		REX::INFO(
			"WorkshopCommitTrial completed item={:08X} base={:08X} beacon={:08X} context=0x{:X} flags=01/01 commitCalled={} commitException=0x{:08X} predicateCalled={} predicate={} predicateException=0x{:08X}",
			a_item->formID,
			itemBase->formID,
			a_beacon->formID,
			reinterpret_cast<std::uintptr_t>(context),
			commitCalled,
			commitException,
			afterCalled,
			afterPredicate,
			afterException);
		return commitCalled && afterCalled && afterPredicate;
	}

	bool ReattachWorkshopItem(
		std::monostate,
		RE::TESObjectREFR* a_item,
		RE::TESObjectREFR* a_beacon,
		RE::BGSLocation* a_claimLocation,
		float a_radius,
		RE::BGSListForm* a_excludedOriginals,
		RE::BGSListForm* a_excludedReplacements)
	{
		if (!a_item || !a_beacon || !a_claimLocation || a_item->IsDeleted() || a_item->IsDisabled() ||
			a_beacon->IsDeleted() || a_beacon->IsDisabled() || !a_item->parentCell || !a_beacon->parentCell ||
			!a_item->parentCell->IsAttached() || !a_beacon->parentCell->IsAttached() ||
			!std::isfinite(a_radius) || a_radius <= 0.0F || a_radius > 10000.0F) {
			REX::ERROR("ReattachWorkshopItem rejected invalid lifecycle, cells, or radius");
			return false;
		}
		const auto beaconBase = a_beacon->GetBaseObject();
		if (!beaconBase || beaconBase->formID != kClaimTimeVanillaBeaconBaseID ||
			a_beacon->GetCurrentLocation() != a_claimLocation) {
			REX::ERROR("ReattachWorkshopItem rejected beacon context mismatch");
			return false;
		}
		const auto itemBase = a_item->GetBaseObject();
		if (!itemBase || itemBase->formID != kWorkshopReplacementChairBaseID) {
			REX::WARN("ReattachWorkshopItem rejected non-chair base item={:08X} base={:08X}", a_item->formID, itemBase ? itemBase->formID : 0);
			return false;
		}
		const auto gameModule = REX::FModule::GetExecutingModule();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) {
			REX::ERROR("ReattachWorkshopItem blocked: unsupported runtime");
			return false;
		}
		const auto moduleBase = gameModule.GetBaseAddress();
		if (!VerifyClaimTimeWorkshopAttachTarget(moduleBase) ||
			!VerifyClaimTimeWorkshopPredicateTarget(moduleBase) ||
			!VerifyActiveWorkshopEnsureTarget(moduleBase)) {
			REX::ERROR("ReattachWorkshopItem blocked: attach/predicate/ensure prefix mismatch");
			return false;
		}
		bool originalsListValid = true;
		bool replacementsListValid = true;
		const bool excludedByOriginals = SafeFormListContainsReference(a_excludedOriginals, a_item, originalsListValid);
		const bool excludedByReplacements = SafeFormListContainsReference(a_excludedReplacements, a_item, replacementsListValid);
		if (!originalsListValid || !replacementsListValid) {
			REX::ERROR("ReattachWorkshopItem rejected malformed exclusion list");
			return false;
		}
		if (excludedByOriginals || excludedByReplacements) {
			REX::INFO(
				"ReattachWorkshopItem excluded item={:08X} original={} replacement={}",
				SafeGetClaimTimeCandidateID(a_item),
				excludedByOriginals,
				excludedByReplacements);
			return false;
		}
		ClaimTimeCandidateValidation validation;
		if (!SafeValidateClaimTimeWorkshopCandidate(
				a_item,
				a_beacon,
				a_beacon,
				RE::PlayerCharacter::GetSingleton(),
				a_beacon->GetParentWorldSpace(),
				a_beacon->GetCurrentLocation(),
				a_radius,
				validation)) {
			REX::WARN(
				"ReattachWorkshopItem rejected item={:08X} base={:08X} reason={} flags=0x{:08X} candidateLocation={:08X} expectedLocation={:08X} locationMatched={}",
				SafeGetClaimTimeCandidateID(a_item),
				validation.baseID,
				validation.reason,
				validation.safetyFlags,
				validation.candidateLocationID,
				validation.expectedLocationID,
				validation.locationMatched);
			return false;
		}
		const bool preexistingExtra = HasClaimTimeWorkshopState(a_item);
		if (preexistingExtra) {
			REX::WARN("ReattachWorkshopItem rejected opaque preexisting Workshop extra item={:08X}", a_item->formID);
			return false;
		}
		// This is intentionally before F3CF40.  The active-session ensure owns
		// any Workshop extra it creates; this transaction never unlinks or frees
		// that engine-owned node on a later failure.
		RE::BSExtraData* preEnsuredExtra = nullptr;
		std::uint32_t preEnsureException = 0;
		const bool preEnsureCalled = CallActiveWorkshopEnsure(
			moduleBase,
			a_item,
			preEnsuredExtra,
			preEnsureException);
		ActiveWorkshopExtraTelemetry preEnsureTelemetry;
		const bool preEnsureInspectionCompleted = preEnsureCalled &&
			ValidateActiveWorkshopExtra(a_item, preEnsuredExtra, preEnsureTelemetry);
		const bool preEnsureMeaningful = preEnsureInspectionCompleted &&
			IsMeaningfulActiveWorkshopExtra(preEnsureTelemetry);

		std::uint32_t attachException = 0;
		const bool attachCalled = CallClaimTimeWorkshopAttach(moduleBase, a_item, true, a_beacon, attachException);
		const bool attachAttempted = attachCalled || attachException != 0;
		bool predicateState = false;
		std::uint32_t predicateException = 0;
		const bool predicateCalled = CallClaimTimeWorkshopPredicate(moduleBase, a_item, predicateState, predicateException);
		const bool attachVerified = attachCalled && predicateCalled && predicateState;

		// A non-meaningful pre-attach result gets exactly one post-attach
		// fallback.  This bounded second call is the only other ensure attempt.
		RE::BSExtraData* postEnsuredExtra = nullptr;
		std::uint32_t postEnsureException = 0;
		const bool postEnsureCalled = attachVerified && !preEnsureMeaningful &&
			CallActiveWorkshopEnsure(moduleBase, a_item, postEnsuredExtra, postEnsureException);
		ActiveWorkshopExtraTelemetry postEnsureTelemetry;
		const bool postEnsureInspectionCompleted = postEnsureCalled &&
			ValidateActiveWorkshopExtra(a_item, postEnsuredExtra, postEnsureTelemetry);
		const bool postEnsureMeaningful = postEnsureInspectionCompleted &&
			IsMeaningfulActiveWorkshopExtra(postEnsureTelemetry);
		const bool ensureReady = preEnsureMeaningful || postEnsureMeaningful;
		bool requeueCalled = false;
		bool requeuePredicateCalled = false;
		bool requeuePredicateState = false;
		std::uint32_t requeueException = 0;
		std::uint32_t requeuePredicateException = 0;
		if (attachVerified && ensureReady) {
			requeueCalled = CallClaimTimeWorkshopAttach(moduleBase, a_item, true, a_beacon, requeueException);
			if (requeueCalled) {
				requeuePredicateCalled = CallClaimTimeWorkshopPredicate(
					moduleBase,
					a_item,
					requeuePredicateState,
					requeuePredicateException);
			}
		}
		const bool requeueVerified = requeueCalled && requeuePredicateCalled && requeuePredicateState;
		REX::INFO(
			"ReattachWorkshopItem chair item={:08X} beacon={:08X} candidateLocation={:08X} expectedLocation={:08X} locationMatched={} preexistingExtra={} preEnsureCalled={} preEnsureException=0x{:08X} preEnsureInspectionCompleted={} preEnsureMeaningful={} preReturnedNonNull={} preActualType=0x{:02X} preExpectedType=0x{:02X} preActualVtable=0x{:X} preExpectedVtable=0x{:X} preItemList=0x{:X} preListedWorkshop=0x{:X} preMembershipEqual={} preInspectionException=0x{:08X} attachCalled={} attachAttempted={} attachException=0x{:08X} predicateCalled={} predicate={} postEnsureCalled={} postEnsureException=0x{:08X} postEnsureInspectionCompleted={} postEnsureMeaningful={} postReturnedNonNull={} postActualType=0x{:02X} postExpectedType=0x{:02X} postActualVtable=0x{:X} postExpectedVtable=0x{:X} postItemList=0x{:X} postListedWorkshop=0x{:X} postMembershipEqual={} postInspectionException=0x{:08X} ensureReady={} ensureAttempts={} requeueCalled={} requeuePredicateCalled={} requeuePredicate={} requeueException=0x{:08X} requeuePredicateException=0x{:08X}",
			a_item->formID,
			a_beacon->formID,
			validation.candidateLocationID,
			validation.expectedLocationID,
			validation.locationMatched,
			preexistingExtra,
			preEnsureCalled,
			preEnsureException,
			preEnsureInspectionCompleted,
			preEnsureMeaningful,
			preEnsureTelemetry.returnedNonNull,
			preEnsureTelemetry.actualType,
			preEnsureTelemetry.expectedType,
			preEnsureTelemetry.actualVtable,
			preEnsureTelemetry.expectedVtable,
			preEnsureTelemetry.itemList,
			preEnsureTelemetry.listedWorkshop,
			preEnsureTelemetry.membershipEqual,
			preEnsureTelemetry.inspectionException,
			attachCalled,
			attachAttempted,
			attachException,
			predicateCalled,
			predicateState,
			postEnsureCalled,
			postEnsureException,
			postEnsureInspectionCompleted,
			postEnsureMeaningful,
			postEnsureTelemetry.returnedNonNull,
			postEnsureTelemetry.actualType,
			postEnsureTelemetry.expectedType,
			postEnsureTelemetry.actualVtable,
			postEnsureTelemetry.expectedVtable,
			postEnsureTelemetry.itemList,
			postEnsureTelemetry.listedWorkshop,
			postEnsureTelemetry.membershipEqual,
			postEnsureTelemetry.inspectionException,
			ensureReady,
			static_cast<std::uint32_t>(preEnsureCalled) + static_cast<std::uint32_t>(postEnsureCalled),
			requeueCalled,
			requeuePredicateCalled,
			requeuePredicateState,
			requeueException,
			requeuePredicateException);
		if (!attachVerified || !ensureReady || !requeueVerified) {
			if (preEnsureTelemetry.returnedNonNull) {
				REX::WARN(
					"ReattachWorkshopItem preserving engine-owned pre-ensure node item={:08X} returnedExtra=0x{:X}; no manual removal/free on failed transaction",
					a_item->formID,
					preEnsuredExtra ? reinterpret_cast<std::uintptr_t>(preEnsuredExtra) : 0);
			}
			if (attachAttempted) {
				std::uint32_t rollbackException = 0;
				const bool rollbackCalled = CallClaimTimeWorkshopAttach(moduleBase, a_item, false, a_beacon, rollbackException);
				bool rollbackPredicate = false;
				std::uint32_t rollbackPredicateException = 0;
				const bool rollbackPredicateCalled = CallClaimTimeWorkshopPredicate(
					moduleBase, a_item, rollbackPredicate, rollbackPredicateException);
				if (!rollbackCalled || !rollbackPredicateCalled || rollbackPredicate) {
					REX::ERROR(
						"ReattachWorkshopItem rollback failed item={:08X} beacon={:08X} called={} predicateCalled={} predicate={} exception=0x{:08X} predicateException=0x{:08X}",
						a_item->formID,
						a_beacon->formID,
						rollbackCalled,
						rollbackPredicateCalled,
						rollbackPredicate,
						rollbackException,
						rollbackPredicateException);
				}
			}
			return false;
		}
		return true;
	}

	void RawScanPopulationDiagnosticCell(
		RE::TESObjectCELL* a_cell,
		const RE::NiPoint3A& a_origin,
		float a_squaredRadius,
		RE::TESWorldSpace* a_worldspace,
		RE::TESObjectREFR* a_center,
		PopulationDiagnosticScan& a_scan) noexcept
	{
		if (!a_cell || !a_worldspace || !a_center) {
			a_scan.layoutRejected = true;
			return;
		}

		const auto base = reinterpret_cast<std::uintptr_t>(a_cell);
		RE::BSReadWriteLock* lock = reinterpret_cast<RE::BSReadWriteLock*>(base + kClaimTimeCellReferencesLockOffset);
		RE::BSAutoReadLock lockGuard(*lock);

		std::uint32_t size = 0;
		std::uint32_t capacity = 0;
		const RE::NiPointer<RE::TESObjectREFR>* data = nullptr;
		if (!SafeReadClaimTimeCellReferenceHeader(a_cell, size, capacity, data) ||
			size > kClaimTimeCellReferenceRawSlotCap || capacity > kClaimTimeCellReferenceRawSlotCap) {
			a_scan.layoutRejected = true;
			return;
		}

		for (std::uint32_t index = 0; index < size; ++index) {
			RE::NiPointer<RE::TESObjectREFR> reference;
			if (!SafeReadClaimTimeCellReferenceSlot(data, index, reference)) {
				a_scan.readRejected = true;
				return;
			}
			if (!reference || reference.get() == a_center ||
				reference->formID == 0x00000014 ||
				!SafeIsClaimTimeReferenceInRange(reference.get(), a_origin, a_squaredRadius)) {
				continue;
			}
			if (a_scan.scanned >= kClaimTimeReferenceScanCap) {
				a_scan.truncated = true;
				return;
			}
			++a_scan.scanned;

			ReferenceDiagnosticState state{};
			if (!SafeCollectReferenceDiagnosticState(reference.get(), state) || state.worldspaceID != a_worldspace->formID) {
				++a_scan.stateRejected;
				continue;
			}
			if (state.baseID == kOverlayActivatorID || state.baseID == kClaimTimeVanillaBeaconBaseID) {
				continue;
			}
			if (a_scan.entries.size() >= kPopulationDiagnosticReferenceCap) {
				a_scan.truncated = true;
				return;
			}
			auto entry = MakePopulationDiagnosticEntry(state);
			if (a_scan.proceduralKeys) {
				if (a_scan.proceduralKeyReads >= ClaimablePOIs::ProceduralKeyDiagnostics::kReadCap) {
					a_scan.readRejected = true;
					return;
				}
				++a_scan.proceduralKeyReads;
				entry.proceduralKey = ClaimablePOIs::ProceduralKeyDiagnostics::Observe(reference.get(), state.refID);
				if (!entry.proceduralKey.readable || entry.proceduralKey.present != state.hasProceduralInstanceData) {
					a_scan.readRejected = true;
					return;
				}
			}
			a_scan.entries.push_back(entry);
		}
	}

	void ScanPopulationDiagnosticCell(
		RE::TESObjectCELL* a_cell,
		const RE::NiPoint3A& a_origin,
		float a_squaredRadius,
		RE::TESWorldSpace* a_worldspace,
		RE::TESObjectREFR* a_center,
		PopulationDiagnosticScan& a_scan) noexcept
	{
		// Keep the SEH frame outside the lock-owning scanner so an invalid runtime
		// layout fails the diagnostic without crossing a C++ unwind boundary.
		__try {
			RawScanPopulationDiagnosticCell(a_cell, a_origin, a_squaredRadius, a_worldspace, a_center, a_scan);
		} __except (a_scan.readRejected = true, EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	[[nodiscard]] bool CollectPopulationDiagnostic(
		RE::TESObjectREFR* a_center,
		RE::TESObjectCELL* a_claimCell,
		const std::vector<RE::TESObjectCELL*>& a_loadedCells,
		RE::BGSLocation* a_location,
		float a_radius,
		const RE::NiPoint3A& a_origin,
		PopulationDiagnosticScan& a_scan) noexcept
	{
		if (!a_center || !a_claimCell || !a_location || a_loadedCells.empty() ||
			a_loadedCells.size() > kClaimTimeLoadedCellCap || !std::isfinite(a_radius) ||
			a_radius <= 0.0F || a_radius > 10000.0F) {
			return false;
		}
		const auto worldspace = a_center->GetParentWorldSpace();
		if (!worldspace) {
			return false;
		}
		a_scan.candidateCells = static_cast<std::uint32_t>(a_loadedCells.size());

		std::array<RE::NiPointer<RE::TESObjectCELL>, kClaimTimeLoadedCellCap> cells{};
		std::uint32_t cellCount = 0;
		bool claimCellPresent = false;
		for (auto* candidateCell : a_loadedCells) {
			if (!candidateCell) {
				return false;
			}
			bool duplicate = false;
			for (std::uint32_t index = 0; index < cellCount; ++index) {
				if (cells[index].get() == candidateCell) {
					duplicate = true;
					break;
				}
			}
			if (duplicate) {
				continue;
			}
			bool attached = false;
			bool exterior = false;
			if (!SafeGetClaimTimeCellState(candidateCell, attached, exterior)) {
				return false;
			}
			if (!attached || !exterior) {
				continue;
			}
			++a_scan.attachedExteriorCells;
			if (candidateCell == a_claimCell) {
				claimCellPresent = true;
			}
			cells[cellCount++].reset(candidateCell);
		}
		if (!claimCellPresent || cellCount == 0) {
			return false;
		}

		try {
			a_scan.entries.reserve(kPopulationDiagnosticReferenceCap);
			a_scan.cellIDs.reserve(kClaimTimeLoadedCellCap);
		} catch (...) {
			return false;
		}
		const float squaredRadius = a_radius * a_radius;
		for (std::uint32_t index = 0; index < cellCount; ++index) {
			// The scanner performs only bounded reads under the verified cell lock.
			// SafeCollectReferenceDiagnosticState contains the per-reference SEH
			// boundary; no reference or engine-owned state is retained afterward.
			RE::TESWorldSpace* matchedWorldspace = nullptr;
			if (!SafeGetClaimTimeCellWorldspace(cells[index].get(), worldspace, matchedWorldspace)) {
				a_scan.layoutRejected = true;
				return false;
			}
			if (!matchedWorldspace) {
				++a_scan.worldspaceMismatchedCells;
				continue;
			}
			++a_scan.worldspaceMatchedCells;
			if (cells[index].get() == a_claimCell) {
				a_scan.claimCellWorldspaceMatched = true;
			}
			if (a_scan.packInCensus) {
				if (a_scan.packInCellCount < a_scan.packInCellOverlays.size()) {
					auto& value = a_scan.packInCellOverlays[a_scan.packInCellCount++];
					value.cell = cells[index]->formID;
					bool neverResets = false;
					value.resolved = a_scan.packInLayoutVerified && RawReadOverlayDiagnostic(
						REX::FModule::GetExecutingModule().GetBaseAddress(), cells[index].get(), a_location,
						value.overlay, value.flag, neverResets, value.exception);
				} else {
					a_scan.packInCellsTruncated = true;
				}
			}
			ScanPopulationDiagnosticCell(cells[index].get(), a_origin, squaredRadius, worldspace, a_center, a_scan);
			a_scan.cellIDs.push_back(cells[index]->formID);
			if (a_scan.truncated || a_scan.layoutRejected || a_scan.readRejected) {
				return false;
			}
		}
		if (a_scan.worldspaceMatchedCells == 0 || !a_scan.claimCellWorldspaceMatched) {
			a_scan.layoutRejected = true;
			return false;
		}

		std::ranges::sort(a_scan.cellIDs);
		std::ranges::sort(a_scan.entries, {}, &PopulationDiagnosticEntry::refID);
		a_scan.entries.erase(
			std::ranges::unique(a_scan.entries, {}, &PopulationDiagnosticEntry::refID).begin(),
			a_scan.entries.end());
		return true;
	}

	[[nodiscard]] std::string_view GetPopulationDiagnosticStage(std::int32_t a_stage) noexcept
	{
		switch (a_stage) {
		case 0: return "pre-overlay-baseline";
		case 1: return "post-claim-commit";
		case 2: return "workshop-enter-pre-overlay";
		case 3: return "workshop-enter-post-reattach";
		case 4: return "workshop-exit-pre-restore";
		case 5: return "workshop-exit-post-restore";
		case 6: return "claimed-location-return";
		case 7: return "player-load-return";
		default: return "unknown";
		}
	}

	void LogPopulationDiagnosticEntry(
		std::string_view a_kind,
		std::int32_t a_stage,
		const PopulationDiagnosticEntry& a_entry) noexcept
	{
		REX::INFO(
			"PopulationDelta kind={} stage={}({}) ref={:08X} base={:08X} type={} cell={:08X} location={:08X} pos=({:.2f},{:.2f},{:.2f}) flags=0x{:06X}",
			a_kind,
			a_stage,
			GetPopulationDiagnosticStage(a_stage),
			a_entry.refID,
			a_entry.baseID,
			a_entry.baseType,
			a_entry.cellID,
			a_entry.locationID,
			a_entry.x,
			a_entry.y,
			a_entry.z,
			a_entry.stateFlags);
	}

	[[nodiscard]] bool VerifyEnsurePromotedReferenceTarget(std::uintptr_t a_target) noexcept
	{
		__try {
			return std::memcmp(
				reinterpret_cast<const void*>(a_target),
				kEnsurePromotedReferenceFunctionPrefix.data(),
				kEnsurePromotedReferenceFunctionPrefix.size()) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool CallEnsurePromotedReference(
		std::uintptr_t a_target,
		RE::TESObjectREFR* a_item,
		bool& a_created,
		std::uint32_t& a_exceptionCode) noexcept
	{
		a_created = false;
		a_exceptionCode = 0;
		if (!a_item || !a_item->extraDataList || !a_item->parentCell ||
			!a_item->parentCell->IsAttached()) {
			return false;
		}
		__try {
			if (a_item->extraDataList->HasType(RE::ExtraDataType::kPromotedRef)) {
				return true;
			}
			reinterpret_cast<EnsurePromotedReferenceFunction>(a_target)(
				a_item->extraDataList.get(),
				a_item->parentCell,
				false);
			a_created = a_item->extraDataList->HasType(RE::ExtraDataType::kPromotedRef);
			return a_created;
		} __except (a_exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	std::int32_t DetachSessionWorkshopFurniture(
		std::monostate,
		RE::TESObjectREFR* a_beacon)
	{
		if (g_sessionFurnitureCount == 0) {
			ClearSessionFurnitureTracking();
			return 0;
		}
		if (!a_beacon || g_sessionFurnitureBeacon.get() != a_beacon) {
			REX::ERROR(
				"DetachSessionWorkshopFurniture rejected beacon mismatch active={:08X} incoming={:08X}",
				g_sessionFurnitureBeacon ? g_sessionFurnitureBeacon->formID : 0,
				a_beacon ? a_beacon->formID : 0);
			return -1;
		}
		std::uint32_t detachedCount = 0;
		if (!DetachTrackedSessionFurniture(detachedCount)) {
			return -1;
		}
		return static_cast<std::int32_t>(detachedCount);
	}

	std::int32_t ReattachAllWorkshopItems(
		std::monostate,
		RE::TESObjectREFR* a_beacon,
		RE::TESObjectCELL* a_claimCell,
		std::vector<RE::TESObjectCELL*> a_loadedCells,
		RE::BGSLocation* a_claimLocation,
		float a_radius,
		RE::BGSListForm* a_excludedOriginals,
		RE::BGSListForm* a_excludedReplacements,
		bool a_commitFurniture)
	{
		std::lock_guard furnitureLock(g_furnitureEnrollmentMutex);
        if (!ClaimablePOIs::TeardownProtection::Ready() || ClaimablePOIs::TeardownProtection::Busy() ||
            ClassifyProtectedBeacon(a_beacon)!=2) return -1;
		if (g_furnitureEnrollmentFailed.load(std::memory_order_acquire)) {
			REX::ERROR("FurnitureRollback-v1: enrollment blocked by a prior failure; no mutation attempted");
			return -1;
		}
		if (!a_beacon || !a_claimCell || a_beacon->parentCell != a_claimCell ||
			a_beacon->GetCurrentLocation() != a_claimLocation) {
			return -1;
		}
		const auto gameModule = REX::FModule::GetExecutingModule();
		const auto moduleBase = gameModule.GetBaseAddress();
		if (gameModule.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244 ||
			!VerifyClaimTimeWorkshopAttachTarget(moduleBase) ||
			!VerifyClaimTimeWorkshopPredicateTarget(moduleBase) ||
			!VerifyActiveWorkshopEligibilityTarget(moduleBase)) {
			return -1;
		}
		// Both type-separated paths require a live, verified workshop session.
		// Both modes may use the verified generic reference-promotion primitive.
		// No root-commit call or Workshop::ExtraData construction is involved.
		{
			const auto base = a_beacon->GetBaseObject();
			RE::TESObjectREFR* contextBeacon = nullptr;
			std::uint32_t contextException = 0;
			if (!base || base->formID != kClaimTimeVanillaBeaconBaseID ||
				a_beacon->IsDeleted() || a_beacon->IsDisabled() ||
				!std::isfinite(a_radius) || a_radius <= 0.0F || a_radius > 500.0F ||
				!g_trackedUnlockActive.load(std::memory_order_acquire) ||
				g_trackedRecoveryOnly.load(std::memory_order_acquire) ||
				g_trackedPurpose.load(std::memory_order_acquire) != UnlockPurpose::ClaimedWorkshop ||
				g_sessionFurnitureCount != 0 || g_sessionFurnitureBeacon ||
				!VerifyWorkshopCommitTarget(kWorkshopCommitFunctionID.address()) ||
				!ReadWorkshopCommitContextBeacon(reinterpret_cast<void*>(kWorkshopCommitContextID.address()), contextBeacon, contextException) ||
				contextBeacon != a_beacon ||
				!VerifyEnsurePromotedReferenceTarget(kEnsurePromotedReferenceFunctionID.address())) {
				REX::ERROR("StandaloneEnrollment-v1: rejected inactive/mismatched workshop, retained session, or invalid promotion target; no enrollment attempted");
				return -1;
			}
		}
		ClaimTimeWorkshopScan scan;
		if (!CollectClaimTimeWorkshopCandidates(a_beacon, a_claimCell, a_loadedCells, a_radius, scan)) {
			REX::ERROR("ReattachAllWorkshopItems discovery failed closed beacon={:08X}", a_beacon->formID);
			return -1;
		}
		std::array<RE::NiPointer<RE::TESObjectREFR>, kBatchEnrollmentMaxItems> validated{};
		std::uint32_t validatedCount = 0;
		std::uint32_t sessionOnlySkipped = 0;
		std::uint32_t incompatibleFurnitureCount = 0;
		RE::TESFormID firstExcludedRef = 0;
		RE::TESFormID firstExcludedBase = 0;
		std::uint32_t firstCompatibilityFlags = 0;
		std::uint32_t furnitureStateSkipped = 0;
		std::uint32_t furnitureRejectedFlags = 0;
		const auto hasLoaded3D = [](RE::TESObjectREFR* a_item) {
			auto guard = a_item->loadedData.LockRead();
			auto* loaded = *guard;
			return loaded && loaded->data3D.get();
		};
		const auto isStandaloneFurnitureSafe = [&](RE::TESObjectREFR* a_item, std::uint32_t a_flags) noexcept {
			if (!a_item) return false;
			bool alreadyLinked = false;
			std::uint32_t predicateException = 0;
			const bool predicateChecked = CallClaimTimeWorkshopPredicate(moduleBase, a_item, alreadyLinked, predicateException);
			if (!predicateChecked) return false;

			const std::uint32_t allowedFlags = kWorkshopReplacementPromotedRefMissing |
			                                   kWorkshopReplacementLinkedRef |
			                                   kWorkshopReplacementBaseNotAllowlisted;
			return (a_flags & ~allowedFlags) == 0;
		};
		const auto isCompoundContainerSafe = [&](RE::TESObjectREFR* a_item, std::uint32_t a_flags) noexcept {
			// The shared validator proves the live beacon/player context, exact
			// exterior world and radius. Like inherited FURN, a CONT can retain its
			// source Location. Preserve incoming children; never adopt opaque links.
			if (!a_item || !ClaimablePOIs::IsCompoundContainerMaskAllowed(a_flags) ||
				!SafeHasUnaliasedContainerExtras(a_item)) {
				return false;
			}
			bool linked = false;
			std::uint32_t exception = 0;
			return CallClaimTimeWorkshopPredicate(moduleBase, a_item, linked, exception) && !linked;
		};
		std::uint32_t containerRejectedSamples = 0;
		for (std::uint32_t index = 0; index < scan.candidateCount; ++index) {
			auto* item = scan.candidates[index].get();
			const auto itemBase = item ? item->GetBaseObject() : nullptr;
			const bool isContainer = itemBase && itemBase->Is(RE::FormType::kCONT);
			const bool isFurniture = itemBase && itemBase->Is(RE::FormType::kFURN);
			if (a_commitFurniture ? !isFurniture : !isContainer) {
				++sessionOnlySkipped;
				continue;
			}
			RE::TESFormID baseID = 0;
			const auto compatibilityFlags = GetWorkshopFurnitureCompatibilityFlags(item, baseID);
			if (compatibilityFlags != 0) {
				++incompatibleFurnitureCount;
				if (firstExcludedRef == 0) {
					firstExcludedRef = item ? item->formID : 0;
					firstExcludedBase = baseID;
					firstCompatibilityFlags = compatibilityFlags;
				}
				continue;
			}
			bool originalsValid = true;
			bool replacementsValid = true;
			const bool excluded = SafeFormListContainsReference(a_excludedOriginals, item, originalsValid) ||
				SafeFormListContainsReference(a_excludedReplacements, item, replacementsValid);
			if (!originalsValid || !replacementsValid) {
				return -1;
			}
			if (excluded) {
				continue;
			}
			ClaimTimeCandidateValidation validation;
			const bool accepted = SafeValidateClaimTimeWorkshopCandidate(item, a_beacon, a_beacon, scan.player,
				scan.expectedWorldspace, scan.expectedLocation, a_radius, validation);
			if (a_commitFurniture) {
				// Strict standalone subset: furniture entry data present, loaded 3D,
				// and zero unauthorized safety flags. Pack-in/procedural/group/instance
				// carriers, keywords, ownership, and enable-parents remain strictly excluded.
				// Unedited exterior furniture lacks ExtraPromotedRef, which will be promoted
				// via verified engine ID 45048. Residual or active ExtraLinkedRef is permitted
				// when CallClaimTimeWorkshopPredicate succeeds.
				const auto flags = static_cast<std::uint32_t>(SafeGetWorkshopReplacementSafetyFlags(item));
				if (!accepted || !isStandaloneFurnitureSafe(item, flags) || !hasLoaded3D(item)) {
					++furnitureStateSkipped;
					furnitureRejectedFlags |= flags;
					continue;
				}
			} else {
				const auto flags = static_cast<std::uint32_t>(SafeGetWorkshopReplacementSafetyFlags(item));
				const bool containerSafe = accepted && isCompoundContainerSafe(item, flags);
				const bool loaded = containerSafe && hasLoaded3D(item);
				if (!loaded) {
					++furnitureStateSkipped;
					furnitureRejectedFlags |= flags;
					if (ClaimablePOIs::Diagnostics::Enabled() && containerRejectedSamples++ < 8) {
						REX::INFO("CompoundCONT-v2 rejected ref={:08X} base={:08X} flags={:08X} sharedAccepted={} sharedReason={} containerSafe={} loaded3D={} loaded3DChecked={} sourceLocation={:08X} claimLocation={:08X}",
							item->formID, baseID, flags, accepted, validation.reason, containerSafe, loaded,
							containerSafe, validation.candidateLocationID, validation.expectedLocationID);
					}
					continue;
				}
			}
			if (!accepted && validation.safetyFlags != kWorkshopReplacementWorkshop) {
				continue;
			}
			if (validatedCount >= kBatchEnrollmentMaxItems) {
				return -1;
			}
			validated[validatedCount++].reset(item);
		}

		if (incompatibleFurnitureCount != 0) {
			REX::INFO("StandaloneEnrollment-v1 compatibilitySkipped={} firstRef={:08X} firstBase={:08X} firstFlags={:08X}",
				incompatibleFurnitureCount, firstExcludedRef, firstExcludedBase, firstCompatibilityFlags);
		}
		FurnitureEnrollmentBatch furnitureBatch;
		const auto rollbackNewBatch = [&]() {
			// Set the latch before cleanup, including when all link removals later
			// verify: promotion and queued engine work have no verified full undo.
			g_furnitureEnrollmentFailed.store(true, std::memory_order_release);
			const bool targetsValid = VerifyClaimTimeWorkshopAttachTarget(moduleBase) &&
				VerifyClaimTimeWorkshopPredicateTarget(moduleBase);
			const auto resolveAttempt = [&](const ClaimablePOIs::FurnitureEnrollmentAttempt& attempt) {
				auto* item = attempt.candidateIndex < validatedCount ? validated[attempt.candidateIndex].get() : nullptr;
				return targetsValid && item && item->formID == attempt.referenceID ? item : nullptr;
			};
			std::uint32_t detachExceptions = 0;
			std::uint32_t predicateExceptions = 0;
			const auto summary = furnitureBatch.Rollback(
				[&](const auto& attempt) {
					std::uint32_t exception = 0;
					const bool returned = CallClaimTimeWorkshopAttach(moduleBase, resolveAttempt(attempt), false, a_beacon, exception);
					detachExceptions |= exception;
					return returned;
				},
				[&](const auto& attempt, bool& linked) {
					std::uint32_t exception = 0;
					const bool read = CallClaimTimeWorkshopPredicate(moduleBase, resolveAttempt(attempt), linked, exception);
					predicateExceptions |= exception;
					return read;
				});
			g_failedFurnitureBatch = furnitureBatch;
			g_failedFurnitureBeaconID = a_beacon->formID;
			std::uint32_t logged = 0;
			// Spend the bounded sample budget on unresolved/promotion-only entries
			// first, so a late cleanup failure is not hidden by earlier successes.
			for (bool needsAttention : { true, false }) {
				for (const auto& attempt : g_failedFurnitureBatch.Attempts()) {
					if (logged == 8) break;
					const bool uncertain = !attempt.attachAttempted || !attempt.detachReturned || !attempt.predicateRead || attempt.linkedAfter;
					if (uncertain != needsAttention) continue;
					++logged;
					REX::ERROR("FurnitureRollback-v1 sample ref={:08X} promotionAttempted={} attachAttempted={} detachReturned={} predicateRead={} linkedAfter={}",
						attempt.referenceID, attempt.promotionAttempted, attempt.attachAttempted,
						attempt.detachReturned, attempt.predicateRead, attempt.linkedAfter);
				}
			}
			REX::ERROR("FurnitureRollback-v1: batch aborted beacon={:08X} tracked={} promotionAttempts={} attachAttempts={} verifiedDetached={} unresolved={} targetsValid={} detachExceptions={:08X} predicateExceptions={:08X}; enrollment blocked until restart from pre-claim save; promotion not undone",
				g_failedFurnitureBeaconID, g_failedFurnitureBatch.Attempts().size(), summary.promotionAttempts,
				summary.attachAttempts, summary.verifiedDetached, summary.unresolved, targetsValid, detachExceptions, predicateExceptions);
			return -1;
		};

		std::uint32_t reattachedCount = 0;
		std::uint32_t alreadyEnrolledCount = 0;
		std::uint32_t directCount = 0;
		std::uint32_t promotedCount = 0;
		for (std::uint32_t index = 0; index < validatedCount; ++index) {
			auto* item = validated[index].get();
			{
				// Recheck before each mutation, and retain the item before calling
				// the engine.
				ClaimTimeCandidateValidation current{};
				const auto flags = static_cast<std::uint32_t>(SafeGetWorkshopReplacementSafetyFlags(item));
				if (!SafeValidateClaimTimeWorkshopCandidate(item, a_beacon, a_beacon, scan.player,
					scan.expectedWorldspace, scan.expectedLocation, a_radius, current) ||
					!(a_commitFurniture ? isStandaloneFurnitureSafe(item, flags) : isCompoundContainerSafe(item, flags)) ||
					!hasLoaded3D(item)) {
					return rollbackNewBatch();
				}
				bool alreadyLinked = false;
				std::uint32_t exception = 0;
				if (!CallClaimTimeWorkshopPredicate(moduleBase, item, alreadyLinked, exception)) return rollbackNewBatch();
				if (alreadyLinked) {
					++alreadyEnrolledCount;
					continue;
				}

                // Durable save-local ownership is recorded before the first
                // promotion/link mutation. Unchanged successful links stay tracked.
                if (!ClaimablePOIs::TeardownProtection::Record(
                    TeardownList(ClaimablePOIs::TeardownProtection::kInheritedItems),item)) return rollbackNewBatch();
				const bool newlyPromoted = !item->extraDataList ||
					!item->extraDataList->HasType(RE::ExtraDataType::kPromotedRef);
				bool promoCreated = false;
				std::uint32_t promoException = 0;
				std::uint32_t attachException = 0;
				bool predicateState = false;
				std::uint32_t predicateException = 0;
				const auto result = furnitureBatch.Enroll(index, item->formID, newlyPromoted,
					[&] { return CallEnsurePromotedReference(kEnsurePromotedReferenceFunctionID.address(), item, promoCreated, promoException) && promoCreated; },
					[&] { return CallClaimTimeWorkshopAttach(moduleBase, item, true, a_beacon, attachException); },
					[&] { return CallClaimTimeWorkshopPredicate(moduleBase, item, predicateState, predicateException) && predicateState; });
				if (result != ClaimablePOIs::FurnitureEnrollmentResult::Linked) {
					REX::ERROR("FurnitureRollback-v1: enrollment failed ref={:08X} result={} promotionException={:08X} attachException={:08X} predicateException={:08X}",
						item->formID, ClaimablePOIs::ToString(result), promoException, attachException, predicateException);
					return rollbackNewBatch();
				}

				++reattachedCount;
				if (newlyPromoted) {
					++promotedCount;
				} else {
					++directCount;
				}
				if (ClaimablePOIs::Diagnostics::Enabled() && reattachedCount <= 8) {
					REX::INFO("{}: linked ref={:08X} base={:08X} beacon={:08X} state={}",
						a_commitFurniture ? "StandaloneFURN-v1" : "CompoundCONT-v2",
						item->formID, item->GetBaseObject()->formID, a_beacon->formID,
						newlyPromoted ? "newly_promoted" : "already_promoted");
				}
			}
		}
		if (a_commitFurniture) {
			const std::uint32_t totalEnrolled = reattachedCount + alreadyEnrolledCount;
			REX::INFO("StandaloneFURN-v1: totalEnrolled={}/{} (newly_linked={} already_linked={} newly_promoted={} already_promoted={}) nonFurnitureSkipped={} unsafeOrPackInSkipped={} rejectedFlags={:08X}; persistent links retained",
				totalEnrolled, validatedCount, reattachedCount, alreadyEnrolledCount, promotedCount, directCount, sessionOnlySkipped, furnitureStateSkipped, furnitureRejectedFlags);
			return static_cast<std::int32_t>(totalEnrolled);
		}
		REX::INFO("CompoundCONT-v2: newlyLinked={}/{} newlyPromoted={} alreadyPromoted={} nonContainerSkipped={} unsafeOrPrelinkedSkipped={} rejectedFlags={:08X}; no explicit inventory/lock/owner edits",
			reattachedCount, validatedCount, promotedCount, directCount, sessionOnlySkipped, furnitureStateSkipped, furnitureRejectedFlags);
		return static_cast<std::int32_t>(reattachedCount);
	}

	void PopulateConstructibleBaseIDs() noexcept
	{
		g_constructibleBaseIDs.clear();
		static REL::Relocation<std::uintptr_t> allFormsGlobal{
			RE::ID::TESForm::AllFormsMap
		};
		auto* registry = *reinterpret_cast<RuntimeAllFormsRegistry**>(allFormsGlobal.address());
		if (!registry) {
			REX::WARN("PopulateConstructibleBaseIDs: runtime all-forms registry unavailable");
			return;
		}

		registry->lock.LockRead();
		const auto loadedFormCount = registry->forms.size();
		if (loadedFormCount == 0 || loadedFormCount > kRuntimeAllFormsCap) {
			registry->lock.UnlockRead();
			REX::WARN(
				"PopulateConstructibleBaseIDs: rejected runtime all-forms count={}",
				loadedFormCount);
			return;
		}

		std::size_t cobjCount = 0;
		std::size_t outpostRecipeCount = 0;
		std::size_t internalCategoryCount = 0;
		std::size_t missingCreatedObjectCount = 0;
		for (const auto& entry : registry->forms) {
			auto* form = entry.value;
			if (!form || !form->Is(RE::FormType::kCOBJ)) {
				continue;
			}
			++cobjCount;
			auto* cobj = static_cast<RE::BGSConstructibleObject*>(form);
			if (!IsRuntimeOutpostCategory(cobj->benchKeyword)) {
				if (IsRuntimeOutpostCategoryPrefix(cobj->benchKeyword)) {
					++internalCategoryCount;
				}
				continue;
			}
			++outpostRecipeCount;
			auto* created = static_cast<RE::TESForm*>(cobj->createdObject);
			if (!created) {
				++missingCreatedObjectCount;
				continue;
			}
			g_constructibleBaseIDs.insert(created->formID);
		}
		registry->lock.UnlockRead();

		REX::INFO(
			"PopulateConstructibleBaseIDs: loadedForms={} COBJ={} outpostRecipes={} outpostBases={} internalCategories={} missingCreated={}",
			loadedFormCount,
			cobjCount,
			outpostRecipeCount,
			g_constructibleBaseIDs.size(),
			internalCategoryCount,
			missingCreatedObjectCount);
	}

	[[nodiscard]] bool EnsureEngineWorkshopExtraForBuildableFurniture(
		std::uintptr_t a_moduleBase,
		RE::TESObjectREFR* a_item,
		bool& a_created,
		std::uint32_t& a_exceptionCode) noexcept
	{
		a_created = false;
		a_exceptionCode = 0;
		if (!a_item || !a_item->extraDataList) {
			return false;
		}
		const auto base = a_item->GetBaseObject();
		if (!base || !base->Is(RE::FormType::kFURN) || !IsConstructibleBase(base.get())) {
			return false;
		}

		WorkshopComparisonTelemetry eligibility{};
		if (!CallActiveWorkshopEligibility(a_moduleBase, a_item, eligibility) ||
			!eligibility.eligibilityCalled || !eligibility.eligibilityResult) {
			a_exceptionCode = eligibility.eligibilityException;
			return false;
		}

		// A persisted engine node is owned by the save/runtime. Reattachment must
		// not unlink, recreate, or reinterpret it. The fresh-node path below is the
		// only creation performed by this plugin.
		if (HasClaimTimeWorkshopState(a_item)) {
			return true;
		}

		RE::BSExtraData* ensured = nullptr;
		if (!CallActiveWorkshopEnsure(a_moduleBase, a_item, ensured, a_exceptionCode)) {
			return false;
		}
		ActiveWorkshopExtraTelemetry telemetry{};
		if (!ValidateActiveWorkshopExtra(a_item, ensured, telemetry) ||
			!IsMeaningfulActiveWorkshopExtra(telemetry)) {
			return false;
		}
		a_created = true;
		return true;
	}

	[[nodiscard]] bool VerifyWorkshopCommitTarget(std::uintptr_t a_target) noexcept
	{
		__try {
			return std::memcmp(
				reinterpret_cast<const void*>(a_target),
				kWorkshopCommitFunctionPrefix.data(),
				kWorkshopCommitFunctionPrefix.size()) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool ReadWorkshopCommitContextBeacon(
		void* a_context,
		RE::TESObjectREFR*& a_beacon,
		std::uint32_t& a_exceptionCode) noexcept
	{
		a_beacon = nullptr;
		a_exceptionCode = 0;
		__try {
			a_beacon = a_context ?
				*reinterpret_cast<RE::TESObjectREFR**>(reinterpret_cast<std::uintptr_t>(a_context) + 0x608) : nullptr;
			return true;
		} __except (a_exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	[[nodiscard]] bool CallWorkshopCommit(
		std::uintptr_t a_target,
		void* a_context,
		RE::TESObjectREFR* a_item,
		std::uint32_t& a_exceptionCode) noexcept
	{
		a_exceptionCode = 0;
		std::uint16_t flags = 0x0101;
		__try {
			reinterpret_cast<WorkshopCommitFunction>(a_target)(
				a_context,
				a_item,
				reinterpret_cast<const std::uint8_t*>(&flags));
			return true;
		} __except (a_exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

}

namespace ClaimablePOIs::PapyrusNatives
{
    int ClassifyTeardown(RE::TESObjectREFR* a_beacon) noexcept { return ::ClassifyProtectedBeacon(a_beacon); }
    bool InheritedObjectsDetached(RE::TESObjectREFR* a_beacon) noexcept { return ::AllInheritedDetached(a_beacon); }
    bool SnapshotInheritedIDs(std::vector<std::uint32_t>& a_ids) noexcept
    { return TeardownProtection::Snapshot(::TeardownList(TeardownProtection::kInheritedItems),a_ids); }
	void PopulateConstructibleBaseIDs() noexcept
	{
		::PopulateConstructibleBaseIDs();
	}

	void SetPluginFileIndex(const RE::TESFile& a_file) noexcept
	{
		SetPluginIndex(a_file);
	}

	bool Register(RE::BSScript::IVirtualMachine* a_vm) noexcept
	{
		if (!a_vm) {
			REX::ERROR("PapyrusNatives: null VM");
			return false;
		}

		constexpr std::string_view kScriptName{ "CPOI_Native" };

		a_vm->BindNativeMethod(kScriptName, "IsLocationExplored", IsLocationExplored, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "IsLocationClaimable", IsLocationClaimable, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "IsLocationNeverReset", IsLocationNeverReset, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "IsSiteClaimed", IsSiteClaimed, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "GetClaimedCount", GetClaimedCount, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "GetClaimRadius", GetClaimRadius, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "IsDiagnosticsEnabled", IsDiagnosticsEnabled, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "ClaimSite", ClaimSite, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "UnclaimSite", UnclaimSite, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "BeginPOIOverlayUnlock", BeginPOIOverlayUnlockNative, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "BeginClaimedPOIWorkshopUnlock", BeginClaimedPOIWorkshopUnlockNative, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "ResumePOIOverlayUnlock", ResumePOIOverlayUnlockNative, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "ResumePOIOverlayRecovery", ResumePOIOverlayRecoveryNative, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "NotifyPlayerLoaded", NotifyPlayerLoaded, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "RestorePOIOverlay", RestorePOIOverlayNative, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "IsInitialClaimBound", IsInitialClaimBoundNative, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "RecoverClaimedPOIWorkshopUnlock", RecoverClaimedPOIWorkshopUnlockNative, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "LogReferenceState", LogReferenceState, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "LogReferencePopulation", LogReferencePopulation, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "LogMiniSiteCensus", LogMiniSiteCensus, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "GetDecorationSafetyFlags", GetDecorationSafetyFlags, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "GetWorkshopReplacementSafetyFlags", GetWorkshopReplacementSafetyFlags, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "IsRuntimeOutpostBuildable", IsRuntimeOutpostBuildable, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "BeginClaimTimeWorkshopEnrollment", BeginClaimTimeWorkshopEnrollment, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "GetClaimTimeWorkshopEnrollmentStatus", GetClaimTimeWorkshopEnrollmentStatus, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "CommitClaimTimeWorkshopEnrollment", CommitClaimTimeWorkshopEnrollment, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "RollbackClaimTimeWorkshopEnrollment", RollbackClaimTimeWorkshopEnrollment, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "CommitSelectedInheritedWorkshopItem", CommitSelectedInheritedWorkshopItem, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "ReattachWorkshopItem", ReattachWorkshopItem, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "ReattachAllWorkshopItems", ReattachAllWorkshopItems, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "DetachSessionWorkshopFurniture", DetachSessionWorkshopFurniture, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "GetLocationName", GetLocationName, std::nullopt, false);
		a_vm->BindNativeMethod(kScriptName, "SanitizeClaimedBeacon", SanitizeClaimedBeacon, std::nullopt, false);
        a_vm->BindNativeMethod(kScriptName, "InitializeInheritedProtection", InitializeInheritedProtection, std::nullopt, false);
        a_vm->BindNativeMethod(kScriptName, "HasInheritedProtection", HasInheritedProtection, std::nullopt, false);
        a_vm->BindNativeMethod(kScriptName, "IsInheritedTeardownReady", IsInheritedTeardownReady, std::nullopt, false);
        a_vm->BindNativeMethod(kScriptName, "GetPendingInheritedTeardown", GetPendingInheritedTeardown, std::nullopt, false);
        a_vm->BindNativeMethod(kScriptName, "GetInheritedTeardownBeacon", GetInheritedTeardownBeacon, std::nullopt, false);
        a_vm->BindNativeMethod(kScriptName, "CancelInheritedTeardown", CancelInheritedTeardown, std::nullopt, false);
        a_vm->BindNativeMethod(kScriptName, "PrepareInheritedTeardown", PrepareInheritedTeardown, std::nullopt, false);
        a_vm->BindNativeMethod(kScriptName, "GetInheritedTeardownCompletion", GetInheritedTeardownCompletion, std::nullopt, false);
		REX::INFO("PapyrusNatives: registered {} natives on \"{}\"", 41, kScriptName);
		return true;
	}
}
