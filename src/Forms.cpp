#include "Forms.h"

#include "RE/A/AlchemyItem.h"
#include "RE/B/BGSMessage.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESFile.h"
#include "RE/T/TESForm.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace
{
	constexpr std::string_view kPluginName{ "ClaimablePOIs.esm" };

	// Local (in-file) FormIDs emitted by tools/ClaimablePOIsGen. These are stable
	// generator output; scripts/validate_esm.ps1 guards the record set.
	constexpr std::uint32_t kLocalTransponder{ 0x800 };
	constexpr std::uint32_t kLocalMsgClaimUnavailable{ 0x801 };
	constexpr std::uint32_t kLocalMsgAssistedInstructions{ 0x802 };

	[[nodiscard]] bool EqualsIgnoreCase(std::string_view a_left, std::string_view a_right) noexcept
	{
		return a_left.size() == a_right.size() &&
		       std::ranges::equal(a_left, a_right, [](char a_lhs, char a_rhs) {
			       return std::tolower(static_cast<unsigned char>(a_lhs)) ==
			              std::tolower(static_cast<unsigned char>(a_rhs));
		       });
	}

	[[nodiscard]] std::string_view FileName(const RE::TESFile& a_file) noexcept
	{
		const auto length = strnlen_s(a_file.fileName, std::size(a_file.fileName));
		return std::string_view{ a_file.fileName, length };
	}

	template <class T>
	[[nodiscard]] const RE::TESFile* FindPlugin(const T& a_files) noexcept
	{
		for (const auto* file : a_files) {
			if (file && EqualsIgnoreCase(FileName(*file), kPluginName)) {
				return file;
			}
		}
		return nullptr;
	}

	[[nodiscard]] const RE::TESFile* FindPluginFileImpl() noexcept
	{
		const auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			return nullptr;
		}

		const auto& files = handler->compiledFileCollection;
		if (const auto* file = FindPlugin(files.files)) {
			return file;
		}
		if (const auto* file = FindPlugin(files.smallFiles)) {
			return file;
		}
		return FindPlugin(files.mediumFiles);
	}

	// Composes a runtime FormID from a plugin-local FormID. Starfield partitions the
	// FormID space by plugin tier: full plugins own the high byte, medium plugins live
	// under 0xFD, and small (light) plugins under 0xFE.
	[[nodiscard]] RE::TESFormID ComposeFormID(const RE::TESFile& a_file, std::uint32_t a_localID) noexcept
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

	// Resolves a plugin-local FormID and verifies the concrete type. The composed
	// FormID is already unique (plugin index + known local ID), so a type match is
	// sufficient for correctness. EditorID is logged as a diagnostic only because
	// GetFormEditorID() returns "" for some form types in Starfield even when the
	// EDID subrecord is present.
	template <class T>
	[[nodiscard]] T* ResolveVerified(
		const RE::TESFile& a_file,
		std::uint32_t      a_localID,
		std::string_view   a_expectedEditorID) noexcept
	{
		const auto formID = ComposeFormID(a_file, a_localID);
		auto*      form = RE::TESForm::LookupByID(formID);
		if (!form) {
			REX::ERROR("Forms: no form at 0x{:08X} (local 0x{:03X}, expected \"{}\")",
				formID, a_localID, a_expectedEditorID);
			return nullptr;
		}

		auto* typed = form->As<T>();
		if (!typed) {
			REX::ERROR("Forms: form 0x{:08X} has unexpected type {} (expected \"{}\")",
				formID, static_cast<unsigned>(form->GetFormType()), a_expectedEditorID);
			return nullptr;
		}

		const auto* editorID = form->GetFormEditorID();
		if (editorID && *editorID != '\0') {
			if (!EqualsIgnoreCase(editorID, a_expectedEditorID)) {
				REX::WARN("Forms: form 0x{:08X} has EditorID \"{}\" (expected \"{}\")",
					formID, editorID, a_expectedEditorID);
			} else {
				REX::INFO("Forms: resolved \"{}\" at 0x{:08X}", a_expectedEditorID, formID);
			}
		} else {
			REX::INFO("Forms: resolved 0x{:08X} as \"{}\" (type {}, editorID unavailable)",
				formID, a_expectedEditorID, static_cast<unsigned>(form->GetFormType()));
		}

		return typed;
	}
}

namespace ClaimablePOIs::Forms
{
	bool IsPluginLoaded() noexcept
	{
		return FindPluginFileImpl() != nullptr;
	}

	const RE::TESFile* FindPluginFile() noexcept
	{
		return FindPluginFileImpl();
	}

	Resolved Resolve() noexcept
	{
		const auto* file = FindPluginFileImpl();
		if (!file) {
			REX::ERROR("Forms::Resolve: {} is not present in the compiled file collection", kPluginName);
			return {};
		}

		REX::INFO("Forms::Resolve: {} compileIndex=0x{:02X} full=0x{:02X} medium=0x{:02X} small=0x{:04X} flags=0x{:08X}",
			kPluginName, file->compileIndex, file->fileIndex.fullIndex,
			file->fileIndex.mediumIndex, file->fileIndex.smallIndex,
			file->fileFlags.underlying());

		Resolved result{};
		result.transponder =
			ResolveVerified<RE::AlchemyItem>(*file, kLocalTransponder, "CPOI_ClaimTransponder");
		result.claimUnavailableMessage =
			ResolveVerified<RE::BGSMessage>(*file, kLocalMsgClaimUnavailable, "CPOI_Msg_ClaimUnavailable");
		result.assistedInstructionsMessage =
			ResolveVerified<RE::BGSMessage>(*file, kLocalMsgAssistedInstructions, "CPOI_Msg_AssistedInstructions");
		return result;
	}
}
