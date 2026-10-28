#include "pch.h"
#include "Diagnostics.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <filesystem>

namespace ClaimablePOIs::Diagnostics
{
	namespace { std::atomic_bool g_enabled{ false }; }

	void Initialize() noexcept
	{
		try {
			std::array<wchar_t, 32768> executable{};
			const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
			if (!length || length >= executable.size()) return;
			const auto path = std::filesystem::path(executable.data()).parent_path() /
				L"Data/SFSE/Plugins/ClaimablePOIs.ini";
			g_enabled.store(GetPrivateProfileIntW(L"Diagnostics", L"Verbose", 0, path.c_str()) == 1,
				std::memory_order_relaxed);
		} catch (...) {
			g_enabled.store(false, std::memory_order_relaxed);
		}
	}

	bool Enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }
}
