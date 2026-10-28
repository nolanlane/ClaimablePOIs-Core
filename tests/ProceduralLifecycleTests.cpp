#include "ProceduralLifecycleDiagnostics.h"
#include <iostream>
#include <limits>
#include <memory>

bool RunProceduralLifecycleTests()
{
	namespace PD = ClaimablePOIs::ProceduralLifecycleDiagnostics;
	auto session = std::make_unique<PD::Session>();
	session->Reset(7);
	session->origin = { 10, 20, 30 };
	const PD::Key original{ 1, 2, 3 }, replacement{ 4, 5, 6 };
	session->Add({ 99, original, true });
	session->Observe({ .kind = PD::Kind::Associate, .key = replacement, .reference = 100 });
	session->Observe({ .kind = PD::Kind::CreateBegin, .key = replacement, .position = { 10, 20, 30 } });
	session->Observe({ .kind = PD::Kind::Lookup, .key = replacement, .reference = 101, .result = true });
	session->Observe({ .kind = PD::Kind::CreateEnd, .key = replacement, .position = { 10, 20, 30 }, .result = true });
	session->Observe({ .kind = PD::Kind::RetireBegin, .reference = 99 });
	if (session->eventCount != 4 || session->events[0].sequence != 1 || session->events[3].sequence != 4 ||
		!session->HasKey(replacement) || !session->HasReference(101) || session->HasReference(100) ||
		session->InRange({ std::numeric_limits<float>::quiet_NaN(), 20, 30 }) || session->InRange({ 100, 20, 30 })) {
		std::cerr << "Lifecycle correlation admitted unrelated events or lost order/range validation\n";
		return false;
	}
	// A buffer boundary must mark the evidence incomplete, not silently evict
	// the first causal events. Load reset must invalidate every old identity.
	while (session->eventCount < PD::kEventCap) session->Observe({ .kind = PD::Kind::RetireEnd, .reference = 99 });
	session->Observe({ .kind = PD::Kind::RetireEnd, .reference = 99 });
	if (!session->fault || session->eventCount != PD::kEventCap || session->events[0].sequence != 1) return false;
	session->Reset(8);
	if (session->fault || session->eventCount || session->keyCount || session->referenceCount ||
		session->HasKey(original) || session->HasReference(99) || session->epoch != 8) return false;
	for (std::uint32_t i = 1; i <= PD::kIdentityCap; ++i) if (!session->Add({ i, {}, false })) return false;
	if (session->Add({ 5000, {}, false }) || !session->fault) return false;

	// Selection/inventory inputs have only verified XY here. Admit the local
	// request despite an unrelated height, preserve a false evaluator result,
	// and do not seed a reference/key from a content/base ID. Out-of-range or
	// nonfinite XY must not become evidence of a local template selection.
	session->Reset(9);
	session->origin = { 10, 20, 30 };
	session->Observe({ .kind = PD::Kind::ContentBegin, .position = { 10, 20, 9000 }, .invocation = 40, .node = 99 });
	session->Observe({ .kind = PD::Kind::ContentEnd, .position = { 10, 20, 9000 }, .invocation = 40, .node = 99, .result = false });
	session->Observe({ .kind = PD::Kind::InventoryEntry, .position = { 10, 20, 0 }, .invocation = 41, .parentInvocation = 40, .base = 100, .category = 2, .mask = 1, .result = false });
	session->Observe({ .kind = PD::Kind::ContentBegin, .position = { 100, 20, 30 } });
	session->Observe({ .kind = PD::Kind::InventoryEntry, .position = { std::numeric_limits<float>::quiet_NaN(), 20, 0 } });
	if (session->eventCount != 3 || session->events[1].result || session->events[2].result ||
		session->events[2].parentInvocation != 40 || session->HasReference(99) || session->HasReference(100) ||
		session->keyCount || session->referenceCount || session->InRange({ 10, 20, 9000 })) return false;
	if (!PD::ValidPlacementBlock(0, 0, 85) || !PD::ValidPlacementBlock(84, 0, 85) ||
		PD::ValidPlacementBlock(85, 0, 85) || PD::ValidPlacementBlock(0, 1, 85) ||
		PD::ValidPlacementBlock(84, 0, 86) || PD::ValidPlacementBlock(0, 0, 0)) return false;
	while (session->eventCount < PD::kEventCap) session->Observe({ .kind = PD::Kind::InventoryEntry, .position = { 10, 20, 0 } });
	session->Observe({ .kind = PD::Kind::ContentEnd, .position = { 10, 20, 0 } });
	if (!session->fault || session->eventCount != PD::kEventCap || session->events[0].invocation != 40) return false;
	session->Reset(10);
	if (session->eventCount || session->fault || session->epoch != 10) return false;
	return true;
}
