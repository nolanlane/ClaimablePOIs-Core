#pragma once

namespace RE
{
	class AlchemyItem;
	class BGSMessage;
	class TESFile;
}
namespace ClaimablePOIs::Forms
{
	struct Resolved
	{
		RE::AlchemyItem* transponder{ nullptr };
		RE::BGSMessage* claimUnavailableMessage{ nullptr };
		RE::BGSMessage* assistedInstructionsMessage{ nullptr };
	};

	[[nodiscard]] bool IsPluginLoaded() noexcept;
	[[nodiscard]] Resolved Resolve() noexcept;
	[[nodiscard]] const RE::TESFile* FindPluginFile() noexcept;
}
