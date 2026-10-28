#include "pch.h"
#include "ProceduralKeyDiagnostics.h"
#include "REX/FModule.h"
#include "SFSE/Version.h"
#include <Windows.h>
#include <cstring>

namespace
{
	constexpr std::uintptr_t kGetterRva = 0x00B21590;
	constexpr std::uintptr_t kExtraVtableRva = 0x04B21D78;
	static_assert(sizeof(RE::BSExtraData) == 0x18);
	static_assert(sizeof(RE::ExtraDataList) == 0x28);
	static_assert(sizeof(RE::BSReadWriteLock) == 8);
	static_assert(std::to_underlying(RE::ExtraDataType::kProceduralInstanceData) == 0xEF);

	bool CheckGetter(std::uintptr_t a_base) noexcept
	{
		// Complete leaf getter: C8 list, 20 lock, EF tag, 18/1C/20 payload.
		// Fingerprint provenance is in research; no engine getter is invoked.
		__try {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_base + kGetterRva);
			std::uint64_t hash = 14695981039346656037ull;
			for (std::size_t i = 0; i < 0xB4; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
			return hash == 0x1472D851C448F359ull;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool ReadList(RE::ExtraDataList* a_list, std::uintptr_t& a_head, bool& a_present,
		std::uint32_t& a_exception) noexcept
	{
		__try {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_list);
			std::memcpy(&a_head, bytes + 8, sizeof(a_head));
			const std::uint8_t* flags = nullptr;
			std::memcpy(&flags, bytes + 0x18, sizeof(flags));
			a_present = flags && (flags[0xEF >> 3] & (1u << (0xEF & 7))) != 0;
			return true;
		} __except (a_exception = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool ReadNode(std::uintptr_t a_address, std::uintptr_t a_vtable, ClaimablePOIs::ProceduralKeyDiagnostics::Node& a_node,
		std::uint32_t& a_exception) noexcept
	{
		__try {
			const auto* extra = reinterpret_cast<const RE::BSExtraData*>(a_address);
			a_node.next = reinterpret_cast<std::uintptr_t>(extra->next);
			a_node.type = extra->type.underlying();
			if (a_node.type == 0xEF) {
				std::memcpy(&a_node.vtable, extra, sizeof(a_node.vtable));
				if (a_node.vtable != a_vtable) return false;
				std::memcpy(a_node.key.parts.data(), reinterpret_cast<const std::uint8_t*>(extra) + 0x18, 12);
			}
			return true;
		} __except (a_exception = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	ClaimablePOIs::ProceduralKeyDiagnostics::Snapshot ObserveLocked(RE::TESObjectREFR* a_reference,
		std::uint32_t a_expectedID)
	{
		namespace PK = ClaimablePOIs::ProceduralKeyDiagnostics;
		if (!a_reference || !a_expectedID || a_reference->formID != a_expectedID) return {};
		auto* list = a_reference->extraDataList.get();
		if (!list) return PK::Snapshot{ .readable = true };
		// Existing cell scan lock -> extra-list read lock ordering; no form calls
		// or lookup/task dispatch while locked. Raw read guards return normally so
		// the RAII lock is released even when a node cannot be read.
		RE::BSAutoReadLock lock(*reinterpret_cast<RE::BSReadWriteLock*>(
			reinterpret_cast<std::uintptr_t>(list) + 0x20));
		std::uintptr_t head = 0;
		bool present = false;
		std::uint32_t exception = 0;
		if (!ReadList(list, head, present, exception)) return PK::Snapshot{ .exception = exception };
		const auto vtable = REX::FModule::GetExecutingModule().GetBaseAddress() + kExtraVtableRva;
		auto result = PK::CopyBounded(head, vtable,
			present, [&](std::uintptr_t address, PK::Node& node) { return ReadNode(address, vtable, node, exception); });
		result.exception = exception;
		return result;
	}
}

bool ClaimablePOIs::ProceduralKeyDiagnostics::VerifyRuntime() noexcept
{
	const auto module = REX::FModule::GetExecutingModule();
	if (module.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) return false;
	REL::Relocation<std::uintptr_t> getter{ REL::ID(63113) };
	REL::Relocation<std::uintptr_t> vtable{ REL::ID(398658) };
	return getter.address() == module.GetBaseAddress() + kGetterRva &&
		vtable.address() == module.GetBaseAddress() + kExtraVtableRva && CheckGetter(module.GetBaseAddress());
}

ClaimablePOIs::ProceduralKeyDiagnostics::Snapshot ClaimablePOIs::ProceduralKeyDiagnostics::Observe(
	RE::TESObjectREFR* a_reference, std::uint32_t a_expectedID) noexcept
{
	// Gate once in the operator entry point, outside any cell/extra-list locks.
	__try { return ObserveLocked(a_reference, a_expectedID); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return Snapshot{ .exception = GetExceptionCode() }; }
}
