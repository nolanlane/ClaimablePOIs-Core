#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ClaimablePOIs
{
	enum class FurnitureEnrollmentResult
	{
		Linked,
		LedgerRejected,
		PromotionFailed,
		AttachFailed,
		PredicateFailed
	};

	[[nodiscard]] constexpr const char* ToString(FurnitureEnrollmentResult a_result) noexcept
	{
		switch (a_result) {
		case FurnitureEnrollmentResult::Linked: return "linked";
		case FurnitureEnrollmentResult::LedgerRejected: return "ledger-rejected";
		case FurnitureEnrollmentResult::PromotionFailed: return "promotion-failed";
		case FurnitureEnrollmentResult::AttachFailed: return "attach-failed";
		case FurnitureEnrollmentResult::PredicateFailed: return "predicate-failed";
		}
		return "unknown";
	}

	struct FurnitureEnrollmentAttempt
	{
		std::uint32_t candidateIndex{};
		std::uint32_t referenceID{};
		bool promotionAttempted{};
		bool attachAttempted{};
		bool detachReturned{};
		bool predicateRead{};
		bool linkedAfter{ true };
	};

	struct FurnitureRollbackSummary
	{
		std::size_t promotionAttempts{};
		std::size_t attachAttempts{};
		std::size_t verifiedDetached{};
		std::size_t unresolved{};
	};

	// Plain values only. The caller retains the candidate references for the
	// synchronous batch and cleanup; a failed ledger is evidence, never a recipe
	// for resolving/replaying dynamic IDs after a load. Promotion is not undone.
	template <std::size_t Capacity>
	class FurnitureEnrollmentBatch
	{
	public:
		template <class Promote, class Attach, class Verify>
		[[nodiscard]] FurnitureEnrollmentResult Enroll(
			std::uint32_t a_candidateIndex, std::uint32_t a_referenceID,
			bool a_needsPromotion, Promote a_promote, Attach a_attach, Verify a_verify)
		{
			if (_failed || _count == Capacity || a_candidateIndex >= Capacity || a_referenceID == 0) {
				_failed = true;
				return FurnitureEnrollmentResult::LedgerRejected;
			}
			for (const auto& attempt : Attempts()) {
				if (attempt.referenceID == a_referenceID || attempt.candidateIndex == a_candidateIndex) {
					_failed = true;
					return FurnitureEnrollmentResult::LedgerRejected;
				}
			}
			// Record BEFORE either engine call: failure can occur after mutation.
			auto& attempt = _attempts[_count++];
			attempt.candidateIndex = a_candidateIndex;
			attempt.referenceID = a_referenceID;
			attempt.promotionAttempted = a_needsPromotion;
			if (a_needsPromotion && !a_promote()) {
				_failed = true;
				return FurnitureEnrollmentResult::PromotionFailed;
			}
			attempt.attachAttempted = true;
			if (!a_attach()) {
				_failed = true;
				return FurnitureEnrollmentResult::AttachFailed;
			}
			if (!a_verify()) {
				_failed = true;
				return FurnitureEnrollmentResult::PredicateFailed;
			}
			return FurnitureEnrollmentResult::Linked;
		}

		template <class Detach, class Verify>
		[[nodiscard]] FurnitureRollbackSummary Rollback(Detach a_detach, Verify a_verify)
		{
			_failed = true;
			FurnitureRollbackSummary summary{};
			for (std::size_t i = _count; i > 0; --i) {
				auto& attempt = _attempts[i - 1];
				if (attempt.promotionAttempted) ++summary.promotionAttempts;
				if (!attempt.attachAttempted) continue;
				++summary.attachAttempts;
				// Do not trust a failed attach's pre-cleanup predicate: queued/partial
				// work may exist. Attempt cleanup for every attempted new link, and
				// read the postcondition even when detach itself reports an exception.
				attempt.detachReturned = a_detach(attempt);
				attempt.linkedAfter = true;
				attempt.predicateRead = a_verify(attempt, attempt.linkedAfter);
				if (attempt.detachReturned && attempt.predicateRead && !attempt.linkedAfter) {
					++summary.verifiedDetached;
				} else {
					++summary.unresolved;
				}
			}
			return summary;
		}

		[[nodiscard]] std::span<const FurnitureEnrollmentAttempt> Attempts() const noexcept
		{
			return { _attempts.data(), _count };
		}

	private:
		std::array<FurnitureEnrollmentAttempt, Capacity> _attempts{};
		std::size_t _count{};
		bool _failed{};
	};
}
