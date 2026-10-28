#include "FurnitureEnrollmentBatch.h"
#include "ContainerEnrollmentPolicy.h"

#include <array>
#include <iostream>
#include <type_traits>
#include <vector>

namespace
{
	using ClaimablePOIs::FurnitureEnrollmentAttempt;
	using ClaimablePOIs::FurnitureEnrollmentBatch;
	using ClaimablePOIs::FurnitureEnrollmentResult;
	using Result = FurnitureEnrollmentResult;

	bool Check(bool a_condition, const char* a_message)
	{
		if (!a_condition) std::cerr << "Furniture rollback: " << a_message << '\n';
		return a_condition;
	}
}

bool RunFurnitureEnrollmentTests()
{
	static_assert(std::is_trivially_copyable_v<FurnitureEnrollmentBatch<4>>);
	bool passed = true;
	{
		// Replay the first failed compound scan: 28 ordinary non-COBJ bases,
		// three incoming-link storage cases, an active buildable placement preview,
		// and one grouped/placed-content outpost object. Never adopt the preview.
		std::array<std::uint32_t, 33> observed{};
		observed.fill(0x8);
		observed[28] = 0x1000;  // Active vanilla CONT placement preview, not inherited loot.
		observed[29] = observed[30] = observed[31] = 0x1008;
		observed[32] = 0x01080008;
		std::size_t eligible = 0;
		for (auto flags : observed) eligible += ClaimablePOIs::IsCompoundContainerMaskAllowed(flags);
		passed &= Check(eligible == 31 && !ClaimablePOIs::IsCompoundContainerMaskAllowed(observed[28]) &&
			!ClaimablePOIs::IsCompoundContainerMaskAllowed(observed[32]),
			"compound storage was rejected or preview/procedural outpost object was adopted");
		for (std::uint32_t protectedFlag : { 0x80u, 0x100u, 0x200u, 0x800u, 0x10000u, 0x800000u, 0x80000000u }) {
			passed &= Check(!ClaimablePOIs::IsCompoundContainerMaskAllowed(0x1008u | protectedFlag),
				"compound policy admitted protected or unknown state");
		}
	}
	{
		// The second engine call creates its link, then reports a fault. Both
		// attempted new links must be cleaned; the preexisting one is not ours.
		std::array<bool, 4> linked{ true, false, false, false };
		FurnitureEnrollmentBatch<4> batch;
		const auto enroll = [&](std::uint32_t index, bool failAttach) {
			return batch.Enroll(index, 100 + index, false, [] { return true; },
				[&] { linked[index] = true; return !failAttach; }, [&] { return linked[index]; });
		};
		passed &= Check(enroll(1, false) == Result::Linked, "first attachment failed");
		passed &= Check(enroll(2, true) == Result::AttachFailed, "partial attachment was not reported");
		passed &= Check(enroll(3, false) == Result::LedgerRejected && !linked[3], "batch continued after a failed attach");
		std::vector<std::uint32_t> cleanupOrder;
		const auto summary = batch.Rollback(
			[&](const auto& attempt) { cleanupOrder.push_back(attempt.referenceID); linked[attempt.candidateIndex] = false; return true; },
			[&](const auto& attempt, bool& state) { state = linked[attempt.candidateIndex]; return true; });
		passed &= Check(cleanupOrder == std::vector<std::uint32_t>{ 102, 101 }, "partial attach omitted or cleanup order changed");
		passed &= Check(summary.attachAttempts == 2 && summary.verifiedDetached == 2 && summary.unresolved == 0,
			"verified rollback totals incorrect");
		passed &= Check(linked[0] && !linked[1] && !linked[2], "rollback changed a preexisting link or left a new link");
		passed &= Check(enroll(3, false) == Result::LedgerRejected && !linked[3], "verified cleanup reopened failed batch");
	}
	{
		// Detach returning normally is not proof of removal. A detach exception
		// with an unlinked postcondition is still uncertain. Probe failure is too.
		for (int failure = 0; failure < 3; ++failure) {
			FurnitureEnrollmentBatch<3> batch;
			for (std::uint32_t index = 0; index < 3; ++index) {
				passed &= Check(batch.Enroll(index, 100 + index, false, [] { return true; }, [] { return true; }, [] { return true; }) == Result::Linked,
					"setup attachment failed");
			}
			int detached = 0;
			int probed = 0;
			const auto summary = batch.Rollback(
				[&](const auto& attempt) { ++detached; return attempt.candidateIndex != 1 || failure != 1; },
				[&](const auto& attempt, bool& state) {
					++probed;
					state = attempt.candidateIndex == 1 && failure == 0;
					return attempt.candidateIndex != 1 || failure != 2;
				});
			passed &= Check(detached == 3 && probed == 3, "cleanup stopped early or skipped probe after detach failure");
			passed &= Check(summary.verifiedDetached == 2 && summary.unresolved == 1, "uncertain cleanup counted as success");
			const auto retained = batch;
			passed &= Check(retained.Attempts().size() == 3 && retained.Attempts()[1].referenceID == 101,
				"failed ledger lost reference identity");
			passed &= Check(retained.Attempts()[1].detachReturned == (failure != 1) &&
				retained.Attempts()[1].predicateRead == (failure != 2) && retained.Attempts()[1].linkedAfter == (failure == 0),
				"failed ledger lost cleanup outcome");
		}
	}
	{
		// Promotion can mutate before reporting failure. Preserve the attempt,
		// but do not call detach for a link that was never attempted or demote it.
		FurnitureEnrollmentBatch<2> batch;
		bool promoted = false;
		bool attached = false;
		const auto result = batch.Enroll(0, 200, true,
			[&] { promoted = true; return false; }, [&] { attached = true; return true; }, [] { return true; });
		int cleanupCalls = 0;
		const auto summary = batch.Rollback(
			[&](const auto&) { ++cleanupCalls; return true; }, [&](const auto&, bool&) { ++cleanupCalls; return true; });
		passed &= Check(result == Result::PromotionFailed && promoted && !attached && cleanupCalls == 0,
			"failed promotion attempted attach or speculative undo");
		passed &= Check(summary.promotionAttempts == 1 && summary.attachAttempts == 0 && batch.Attempts().size() == 1,
			"promotion-only failure was lost");
	}
	{
		FurnitureEnrollmentBatch<2> batch;
		bool linked = false;
		const auto result = batch.Enroll(0, 200, false, [] { return true; },
			[&] { linked = true; return true; }, [] { return false; });
		const auto summary = batch.Rollback([&](const auto&) { linked = false; return true; },
			[&](const auto&, bool& state) { state = linked; return true; });
		passed &= Check(result == Result::PredicateFailed && summary.verifiedDetached == 1 && !linked,
			"post-attach verification failure escaped cleanup");
	}
	{
		// Full and duplicate ledgers reject before invoking any mutation callback.
		for (bool duplicate : { false, true }) {
			FurnitureEnrollmentBatch<2> batch;
			int mutations = 0;
			const auto enroll = [&](std::uint32_t index, std::uint32_t id) {
				return batch.Enroll(index, id, true, [&] { ++mutations; return true; }, [&] { ++mutations; return true; }, [] { return true; });
			};
			passed &= Check(enroll(0, 200) == Result::Linked, "ledger setup failed");
			if (duplicate) {
				passed &= Check(enroll(1, 200) == Result::LedgerRejected && mutations == 2, "duplicate ref mutated");
			} else {
				passed &= Check(enroll(1, 201) == Result::Linked, "last ledger slot rejected");
				passed &= Check(enroll(2, 202) == Result::LedgerRejected && mutations == 4, "overflow mutated");
			}
		}
	}
	{
		bool linked = false;
		int promotions = 0;
		{
			FurnitureEnrollmentBatch<1> successful;
			passed &= Check(successful.Enroll(0, 300, false, [&] { ++promotions; return true; },
				[&] { linked = true; return true; }, [&] { return linked; }) == Result::Linked, "success path changed");
		}
		passed &= Check(linked && promotions == 0, "successful batch undid persistent link or promoted unnecessarily");
	}
	{
		// Workshop entry now runs independent CONT and FURN transactions. A
		// later furniture failure must not detach a committed container, and
		// the already-promoted container must never invoke promotion.
		std::array<bool, 2> linked{};
		int containerPromotions = 0;
		{
			FurnitureEnrollmentBatch<2> containers;
			passed &= Check(containers.Enroll(0, 400, false,
				[&] { ++containerPromotions; return false; },
				[&] { linked[0] = true; return true; }, [&] { return linked[0]; }) == Result::Linked,
				"container transaction failed or attempted promotion");
		}
		FurnitureEnrollmentBatch<2> furniture;
		passed &= Check(furniture.Enroll(1, 401, true, [] { return true; },
			[&] { linked[1] = true; return false; }, [&] { return linked[1]; }) == Result::AttachFailed,
			"later partial furniture attachment was not rejected");
		const auto summary = furniture.Rollback(
			[&](const auto& attempt) { linked[attempt.candidateIndex] = false; return true; },
			[&](const auto& attempt, bool& state) { state = linked[attempt.candidateIndex]; return true; });
		passed &= Check(linked[0] && !linked[1] && containerPromotions == 0 && summary.verifiedDetached == 1,
			"later rollback changed committed container state or missed its own failed link");
	}
	if (passed) std::cout << "Furniture/container enrollment rollback fault-injection checks passed\n";
	return passed;
}
