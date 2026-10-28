#pragma once

#include "Forms.h"
#include "SFSE/Interfaces.h"

#include <atomic>
#include <cstdint>

namespace ClaimablePOIs::Plugin
{
	enum class ActivationState : std::uint8_t
	{
		Uninitialized,
		WaitingForData,
		Ready,
		Disabled
	};

	[[nodiscard]] bool Load(const SFSE::LoadInterface* a_sfse) noexcept;
	void OnMessage(SFSE::MessagingInterface::Message* a_message) noexcept;
	[[nodiscard]] ActivationState GetActivationState() noexcept;
}
