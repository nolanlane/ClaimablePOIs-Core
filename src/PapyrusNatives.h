#pragma once
#include <cstdint>
#include <vector>

namespace RE
{
	class TESFile;
	class TESObjectREFR;

	namespace BSScript
	{
		class IVirtualMachine;
	}
}

namespace ClaimablePOIs::PapyrusNatives
{
	void SetPluginFileIndex(const RE::TESFile& a_file) noexcept;
	void PopulateConstructibleBaseIDs() noexcept;
	[[nodiscard]] bool Register(RE::BSScript::IVirtualMachine* a_vm) noexcept;
	// Hook-side queries perform bounded reads only, never engine mutation.
	int ClassifyTeardown(RE::TESObjectREFR*) noexcept;
	bool InheritedObjectsDetached(RE::TESObjectREFR*) noexcept;
    bool SnapshotInheritedIDs(std::vector<std::uint32_t>&) noexcept;
}
