#include "GuardedCallValidation.h"
#include "PopulationDiagnostics.h"
#include "MiniSiteDiagnostics.h"
#include "ProceduralKeyDiagnostics.h"
#include "VanillaOverlayPolicy.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

bool RunFurnitureEnrollmentTests();
bool RunProceduralLifecycleTests();
bool RunTeardownTests();

int main()
{
	bool passed = RunFurnitureEnrollmentTests();
	passed = RunTeardownTests() && passed;
	passed = RunProceduralLifecycleTests() && passed;
	{
		namespace PK = ClaimablePOIs::ProceduralKeyDiagnostics;
		const PK::Key key{ { 1, 0xFF123456, 3 } };
		PK::Node first{ 0x2000, 0, 0x84, {} }, second{ 0, 0x4000, 0xEF, key };
		auto read = [&](std::uintptr_t address, PK::Node& node) {
			if (address == 0x1000) node = first;
			else if (address == 0x2000) node = second;
			else return false;
			return true;
		};
		const auto valid = PK::CopyBounded(0x1000, 0x4000, true, read);
		const auto absent = PK::CopyBounded(0, 0x4000, false, read);
		if (!valid.readable || !valid.present || valid.key.parts != key.parts ||
			!absent.readable || absent.present || PK::CopyBounded(0, 0x4000, true, read).readable ||
			PK::CopyBounded(0x1000, 0x4000, false, read).readable ||
			PK::CopyBounded(0x1000, 0x5000, true, read).readable ||
			PK::CopyBounded(0x1001, 0x4000, true, read).readable) {
			std::cerr << "Procedural key read accepted inconsistent presence/vtable/alignment\n";
			passed = false;
		}
		first = PK::Node{ 0x2000, 0x4000, 0xEF, key };
		if (PK::CopyBounded(0x1000, 0x4000, true, read).readable) {
			std::cerr << "Duplicate procedural extras accepted\n";
			passed = false;
		}
		second.next = 0x3000;  // failure after a copied key must discard it
		const auto failed = PK::CopyBounded(0x2000, 0x4000, true, read);
		if (failed.readable || failed.present || failed.key.parts != PK::Key{}.parts) {
			std::cerr << "Partial procedural key leaked through failed read\n";
			passed = false;
		}
		std::uint32_t reads = 0;
		const auto cycle = PK::CopyBounded(0x1000, 0x4000, false, [&](std::uintptr_t, PK::Node& node) {
			++reads;
			node = PK::Node{ 0x1000, 0, 0x84, {} };
			return true;
		});
		if (cycle.readable || reads != PK::kNodeCap) {
			std::cerr << "Procedural extra traversal exceeded its bounded read budget\n";
			passed = false;
		}
	}
	{
		namespace MD = ClaimablePOIs::MiniSiteDiagnostics;
		MD::Session session;
		session.Reset(1);
		const MD::Origin position{ -2214, -96, 0.13F };
		if (session.Begin(1, 1, position, 1) || session.Begin(0, 0, position, 1) ||
			session.Begin(0, 1, MD::Origin{ NAN, 0, 0 }, 1) || !session.Begin(0, 1, position, 1)) {
			std::cerr << "Mini-site anchor admitted invalid context or skipped its baseline\n";
			passed = false;
		}
		session.Complete(1, position);
		const MD::Origin nearby{ -2213, -96, 0.13F };
		if (session.Begin(0, 1, position, 1) || session.Begin(1, 2, nearby, 1) ||
			session.Begin(1, 1, nearby, 2) || session.Begin(1, 1, MD::Origin{ -2100, -96, 0.13F }, 1) ||
			session.Begin(1, 1, nearby, 1) != std::optional<MD::Origin>{ position }) {
			std::cerr << "Mini-site anchor moved or accepted a different world/load/range\n";
			passed = false;
		}
		if (!session.Begin(1, 1, nearby, 1) || session.Begin(1, 1, nearby, 1)) {
			std::cerr << "Mini-site failed-scan retry budget was not bounded\n";
			passed = false;
		}
		session.Reset(2);
		if (session.Begin(1, 1, nearby, 2) || !session.Begin(0, 2, nearby, 2)) {
			std::cerr << "Mini-site anchor survived a load boundary\n";
			passed = false;
		}
	}

	{
		using ClaimablePOIs::InitialOverlayClaim;
		using ClaimablePOIs::MatchesInitialOverlayClaim;
		using ClaimablePOIs::SelectSeparateVanillaOverlay;
		const InitialOverlayClaim claim{ 7, 0xFF000001, 0xFF000002, 0xFF000003, -200, 100, 5, 200 };
		const auto eligible = [&](std::uint32_t base, std::uint32_t world, float x, float y, float z) {
			return MatchesInitialOverlayClaim(claim, base, world, x, y, z);
		};
		if (!eligible(0x002C60E9, claim.cellWorld, -200, 100, 5) ||
			!eligible(0x002C60E9, claim.cellWorld, 0, 100, 5) ||
			eligible(0x002C60E9, claim.cellWorld, 0.01F, 100, 5) ||
			eligible(0x00260ED4, claim.cellWorld, -200, 100, 5) ||
			eligible(0x002C60E9, claim.cellWorld + 1, -200, 100, 5) ||
			eligible(0x002C60E9, claim.cellWorld, std::numeric_limits<float>::quiet_NaN(), 100, 5) ||
			eligible(0x002C60E9, claim.cellWorld, -200, 100, std::numeric_limits<float>::infinity()) ||
			SelectSeparateVanillaOverlay(claim, 7, claim.overlay, claim.cellWorld) ||
			!SelectSeparateVanillaOverlay(claim, 7, claim.overlay, claim.cellWorld, true) ||
			SelectSeparateVanillaOverlay(claim, 0, claim.overlay, claim.cellWorld, true) ||
			SelectSeparateVanillaOverlay(claim, 8, claim.overlay, claim.cellWorld, true) ||
			SelectSeparateVanillaOverlay(claim, 7, claim.overlay + 1, claim.cellWorld, true) ||
			SelectSeparateVanillaOverlay(claim, 7, claim.overlay, claim.overlay, true) ||
			SelectSeparateVanillaOverlay(claim, 7, claim.overlay, 0, true)) {
			std::cerr << "Initial claim overlay isolation admitted an unrelated/stale registration or rejected a bounded beacon\n";
			passed = false;
		}
		for (const float radius : { 0.0F, -1.0F, 501.0F, std::numeric_limits<float>::infinity() }) {
			auto invalid = claim;
			invalid.radius = radius;
			if (MatchesInitialOverlayClaim(invalid, 0x002C60E9, claim.cellWorld, -200, 100, 5)) {
				passed = false;
			}
		}
		if (MatchesInitialOverlayClaim({}, 0x002C60E9, claim.cellWorld, -200, 100, 5)) {
			passed = false;
		}
	}

	{
		constexpr std::uintptr_t callSite = 0x00F40F1B;
		constexpr std::uintptr_t stubAddress = 0x01000000;
		constexpr std::uintptr_t hookAddress = 0x00007FF800001234;
		const std::array<std::uint8_t, 5> vanilla{ 0xE8, 0xF0, 0x26, 0xB9, 0xFF };
		std::array<std::uint8_t, 5> call{ 0xE8 };
		std::array<std::uint8_t, 14> stub{ 0xFF, 0x25 };
		const auto displacement = static_cast<std::int32_t>(stubAddress - (callSite + 5));
		const auto target = static_cast<std::uint64_t>(hookAddress);
		std::memcpy(call.data() + 1, &displacement, sizeof(displacement));
		std::memcpy(stub.data() + 6, &target, sizeof(target));
		const auto valid = [&] { return ClaimablePOIs::MatchesOwnedGuardCall(call, callSite, stub, stubAddress, hookAddress); };
		if (call == vanilla || !valid() ||
			ClaimablePOIs::MatchesOwnedGuardCall(vanilla, callSite, stub, stubAddress, hookAddress) ||
			ClaimablePOIs::MatchesOwnedGuardCall(call, callSite, stub, stubAddress + 16, hookAddress) ||
			ClaimablePOIs::MatchesOwnedGuardCall(call, callSite, stub, stubAddress, 0)) {
			std::cerr << "Owned guard recognition failed\n";
			passed = false;
		}
		for (auto& byte : call) {
			byte ^= 1;
			if (valid()) {
				passed = false;
			}
			byte ^= 1;
		}
		for (auto& byte : stub) {
			byte ^= 1;
			if (valid()) {
				passed = false;
			}
			byte ^= 1;
		}
		constexpr std::uintptr_t lowerStub = 0x00800000;
		const auto negative = static_cast<std::int32_t>(static_cast<std::int64_t>(lowerStub) - (callSite + 5));
		std::memcpy(call.data() + 1, &negative, sizeof(negative));
		if (!ClaimablePOIs::MatchesOwnedGuardCall(call, callSite, stub, lowerStub, hookAddress)) {
			std::cerr << "Negative guard displacement rejected\n";
			passed = false;
		}
	}

	{
		namespace PD = ClaimablePOIs::PopulationDiagnostics;
		const auto foundation = PD::MakeKey(0x123, 1, 10.01F, -20.01F, 3.01F);
		const auto regenerated = PD::MakeKey(0x123, 1, 10.02F, -20.02F, 3.02F);
		const auto otherBase = PD::MakeKey(0x456, 1, 10.01F, -20.01F, 3.01F);
		const auto moved = PD::MakeKey(0x123, 1, 11.0F, -20.01F, 3.01F);
		const auto otherType = PD::MakeKey(0x123, 2, 10.01F, -20.01F, 3.01F);
		if (!foundation || !regenerated || !otherBase || !moved || !otherType) {
			std::cerr << "Valid footprint metadata rejected\n";
			return EXIT_FAILURE;
		}
		const std::vector<PD::Key> before{ *foundation, *foundation, *otherBase };
		const std::vector<PD::Key> after{ *otherBase, *regenerated, *moved };
		const auto result = PD::Compare(before, after);
		const auto reordered = PD::Compare(before, std::vector<PD::Key>{ *otherBase, *foundation, *foundation });
		const auto changedType = PD::Compare(std::vector<PD::Key>{ *foundation }, std::vector<PD::Key>{ *otherType });
		const auto empty = PD::Compare({}, {});
		if (!result || result->matched != 2 || result->unmatchedBefore.size() != 1 || result->unmatchedAfter.size() != 1 ||
			!reordered || reordered->matched != 3 || !reordered->unmatchedBefore.empty() ||
			!changedType || changedType->matched != 0 || !empty || empty->matched != 0 ||
			PD::MakeKey(0, 1, 0, 0, 0) || PD::MakeKey(1, 1, INFINITY, 0, 0) ||
			PD::MakeKey(1, 1, NAN, 0, 0) || PD::MakeKey(1, 1, 1.0e20F, 0, 0) ||
			PD::Compare(std::vector<PD::Key>(PD::kSampleCap + 1, *foundation), {})) {
			std::cerr << "Footprint comparison lost multiplicity, identity, or bounds\n";
			passed = false;
		}
		if (PD::MakeKey(1, 1, 0.049F, 0, 0) == PD::MakeKey(1, 1, 0.051F, 0, 0)) {
			std::cerr << "Footprint bin boundary changed\n";
			passed = false;
		}
	}

	{
		using ClaimablePOIs::InitialOverlayBinding;
		using ClaimablePOIs::HasVerifiedInitialBinding;
		const InitialOverlayBinding binding{ 7, 11, 12, 13, 13, 0 };
		if (!HasVerifiedInitialBinding(binding, 7, 11, 12) ||
			HasVerifiedInitialBinding(binding, 0, 11, 12) ||
			HasVerifiedInitialBinding(binding, 8, 11, 12) ||
			HasVerifiedInitialBinding(binding, 7, 14, 12) ||
			HasVerifiedInitialBinding(binding, 7, 11, 14) ||
			HasVerifiedInitialBinding({}, 7, 11, 12) ||
			HasVerifiedInitialBinding({ 7, 11, 12, 13, 14, 0 }, 7, 11, 12) ||
			HasVerifiedInitialBinding({ 7, 11, 12, 13, 13, 14 }, 7, 11, 12) ||
			HasVerifiedInitialBinding({ 7, 11, 12, 0, 0, 0 }, 7, 11, 12)) {
			std::cerr << "Initial binding accepted missing, stale, foreign or newly created overlay evidence\n";
			passed = false;
		}
	}
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
