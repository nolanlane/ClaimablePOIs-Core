#include "Plugin.h"

#include "Forms.h"
#include "Diagnostics.h"
#include "FormReferenceRuntime.h"
#include "PapyrusNatives.h"
#include "PCMOverlayHook.h"
#include "VanillaOverlayHook.h"
#include "ProceduralLifecycleDiagnostics.h"
#include "TeardownProtection.h"

#include "SFSE/API.h"
#include "SFSE/Interfaces.h"
#include "SFSE/Version.h"

#include <string>

namespace
{
	std::atomic<ClaimablePOIs::Plugin::ActivationState> g_state{
		ClaimablePOIs::Plugin::ActivationState::Uninitialized
	};
	ClaimablePOIs::Forms::Resolved g_forms{};

	void Disable(std::string_view a_reason) noexcept
	{
		g_state.store(ClaimablePOIs::Plugin::ActivationState::Disabled, std::memory_order_release);
		REX::ERROR("ClaimablePOIs disabled: {}", a_reason);
	}

	void ResolveData() noexcept
	{
		g_forms = ClaimablePOIs::Forms::Resolve();
		if (!g_forms.transponder || !g_forms.claimUnavailableMessage || !g_forms.assistedInstructionsMessage) {
			Disable("ClaimablePOIs.esm or one or more required typed EditorIDs are missing");
			return;
		}

		// Set plugin file index for FormID composition in Papyrus natives.
		if (const auto* file = ClaimablePOIs::Forms::FindPluginFile()) {
			ClaimablePOIs::PapyrusNatives::SetPluginFileIndex(*file);
		}

		// Collect all workshop-buildable base form IDs from COBJ recipes.
		ClaimablePOIs::PapyrusNatives::PopulateConstructibleBaseIDs();
		if (!ClaimablePOIs::TeardownProtection::Install()) {
			REX::ERROR("InheritedTeardown-v4: pre-removal guard gates failed; CPOI mutation will fail closed");
		}

		// Observe the two worldspace queries without overriding their outputs.
		if (!ClaimablePOIs::PCMOverlayHook::Install()) {
			REX::ERROR("PCMQueryAudit-v1: hook installation failed; stop this trial");
		}
		ClaimablePOIs::PCMOverlayHook::SyncFromSave();
		if (!ClaimablePOIs::VanillaOverlayHook::Install()) {
			REX::ERROR("SeparateVanillaOverlay-v1: registration hook gates failed; initial claims will fail closed");
		}
		if (ClaimablePOIs::Diagnostics::Enabled() && !ClaimablePOIs::ProceduralLifecycleDiagnostics::Install()) {
			REX::ERROR("MiniSelection-v1: exact runtime hook gates failed; stop this trial");
		}

		REX::INFO("PCMQueryAudit-v1 active: vanilla query return/output preserved; unverified PCM exemption removed; claim preflight and null-cell guard retained");

		// Register Papyrus native functions with the VM.
		const auto* gameVM = RE::GameVM::GetSingleton();
		auto* vm = gameVM ? gameVM->GetVM() : nullptr;
		if (vm) {
			if (!ClaimablePOIs::PapyrusNatives::Register(vm)) {
				REX::WARN("Papyrus native registration failed");
			}
		} else {
			REX::WARN("Papyrus VM not available; native functions will not be registered");
		}

		g_state.store(ClaimablePOIs::Plugin::ActivationState::Ready, std::memory_order_release);
		REX::INFO("ClaimablePOIs forms resolved; Papyrus natives registered");
	}
}

namespace ClaimablePOIs::Plugin
{
	bool Load(const SFSE::LoadInterface* a_sfse) noexcept
	{
		SFSE::Init(a_sfse, {
			.logLevel = REX::ELogLevel::Info,
			.logName = "ClaimablePOIs",
			.trampoline = true,
			.trampolineSize = 512
		});
		ClaimablePOIs::Diagnostics::Initialize();
		REX::INFO("ClaimablePOIs alpha.5: verbose diagnostics={}", ClaimablePOIs::Diagnostics::Enabled());
		if (SFSE::GetSFSEVersion() != SFSE::SFSE_PACK_LATEST.pack()) {
			Disable("unsupported SFSE version");
			return false;
		}
		if (!VerifyFormReferenceRuntime()) {
			Disable("FormRefCountABI-v1: runtime ownership protocol verification failed");
			return false;
		}
		REX::INFO("FormRefCountABI-v1: strong/external acquisition and engine release protocol verified for 1.16.244");
		const auto* messaging = SFSE::GetMessagingInterface();
		if (!messaging) {
			Disable("required SFSE messaging interface is unavailable");
			return false;
		}

		if (!messaging->RegisterListener(OnMessage)) {
			Disable("SFSE lifecycle listener registration failed");
			return false;
		}

		g_state.store(ActivationState::WaitingForData, std::memory_order_release);
		REX::INFO("ClaimablePOIs initialized; waiting for post-data-load form resolution");
		return true;
	}

	void OnMessage(SFSE::MessagingInterface::Message* a_message) noexcept
	{
		if (!a_message) {
			return;
		}
		if (GetActivationState() != ActivationState::WaitingForData) {
			return;
		}
		if (a_message->type == SFSE::MessagingInterface::kPostDataLoad) {
			ResolveData();
		}
	}

	ActivationState GetActivationState() noexcept
	{
		return g_state.load(std::memory_order_acquire);
	}
}
