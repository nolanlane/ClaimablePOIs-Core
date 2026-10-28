#include "pch.h"
#include "FormReferenceRuntime.h"
#include "REX/FModule.h"
#include "SFSE/Version.h"
#include <Windows.h>
#include <array>

static_assert(RE::TESFormRefCount::kRuntimeRefCountABI == 116244);
static_assert(RE::TESFormRefCount::kRefCountMask == 0x1FFFFF);
static_assert(sizeof(RE::TESFormRefCount) == 0x10);

namespace
{
    struct CodeGate { std::uintptr_t rva; std::size_t size; std::uint64_t hash; };
    // Complete engine release routines and a paired strong-acquire call site.
    // These validate the counter protocol, not a guessed private-object layout.
    constexpr std::array<CodeGate, 3> kGates{{
        { 0x00384600, 0xE3, 0x66E1240BAADCD5BBull },
        { 0x003846F0, 0x9C, 0x00C6698C438B578Bull },
        { 0x021B3CE1, 0x1D, 0xB2BD46954E667809ull }
    }};

    bool CheckCode(std::uintptr_t a_base) noexcept
    {
        __try {
            for (const auto& gate : kGates) {
                auto* bytes = reinterpret_cast<const std::uint8_t*>(a_base + gate.rva);
                std::uint64_t hash = 14695981039346656037ull;
                for (std::size_t i = 0; i < gate.size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
                if (hash != gate.hash) return false;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

bool ClaimablePOIs::VerifyFormReferenceRuntime() noexcept
{
    const auto& module = REX::FModule::GetExecutingModule();
    if (module.GetFileVersion() != SFSE::RUNTIME_SF_1_16_244) return false;
    const auto base = module.GetBaseAddress();
    REL::Relocation<std::uintptr_t> strong{ RE::ID::TESForm::DecRefCount };
    REL::Relocation<std::uintptr_t> external{ RE::ID::TESForm::DecExternalRefCount };
    return strong.address() == base + 0x00384600 && external.address() == base + 0x003846F0 && CheckCode(base);
}
