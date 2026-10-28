#include "pch.h"
#include "TeardownProtection.h"
#include "TeardownPolicy.h"
#include "PapyrusNatives.h"
#include "GuardedCallValidation.h"
#include "REL/Trampoline.h"
#include "REL/ASM.h"
#include "REX/FModule.h"
#include "SFSE/Version.h"
#include <Windows.h>
#undef ERROR
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace
{
    using namespace ClaimablePOIs;
    using Dispatch = void (*)(void*, void*, void*, void*);
    std::atomic<bool> s_installed{}, s_faulted{};
    std::mutex s_requestMutex;
    std::recursive_mutex s_listMutex;
    TeardownRequest s_request;
    std::uint64_t s_authorizedAt{};
    int s_completion{};
    std::atomic<std::uint64_t> s_loadEpoch{};
    struct Gate { std::uintptr_t rva; std::size_t length; std::uint64_t hash; };
    // Complete bodies split only around the five owned calls, checked separately.
    constexpr std::array kGates{
        Gate{0x021BFAF0, 0x4C, 0xF7AF38D3782D8F48ull},
        Gate{0x021BFB42, 0xB, 0x569BECB729774BBBull},
        Gate{0x021BFA90, 0x4C, 0xF7AF38D3782D8F48ull},
        Gate{0x021BFAE2, 0xB, 0x569BECB729774BBBull},
        Gate{0x021B7200, 0x294, 0x008404476AF8F201ull},
        Gate{0x021B7499, 0x3, 0x4ABBF319CECF538Dull},
        Gate{0x021B74A1, 0x438, 0x46711FC81C04197Bull},
        Gate{0x021B78DE, 0xDA, 0x5DCA4BF638FFCFD8ull},
        Gate{0x021B79C0, 0x209, 0x4A558B957EA5C44Bull},
        Gate{0x00BD6550, 0x148, 0x22F90B127EA9C829ull},
        Gate{0x00F3CF40, 0x2EE, 0xBC05D0FBC3BA046Full},
        Gate{0x00F3CC90, 0x96, 0xAA34374225CAE042ull},
        Gate{0x020296A0, 0x5B, 0x7986154FA9FC30D5ull},
        Gate{0x00517D60, 0x34A, 0x94D929F1F8231E38ull},
        Gate{0x021BB570, 0x130, 0x8A82BA03922D7EBAull},
        Gate{0x002B65E0, 0x73, 0x98C18B8C088A1FB7ull},
        Gate{0x022CC350, 0x7E, 0xFA64C30B72CD38E8ull},
        Gate{0x002C5C60, 0x180, 0xE38120D2DC190D89ull},
        Gate{0x00F3B500, 0x17A, 0x297DDA3487C98FDEull},
        Gate{0x00F3A6E0, 0x8E6, 0xC00F55F52E1F10EBull},
        Gate{0x002CEF20, 0x5A, 0x8604EDEB0CE67777ull},
        Gate{0x002CF060, 0x11, 0x7D687FBBF225E19Cull}
    };
    struct Site { std::uintptr_t rva, slot, eventVtable, hook, pointer{}; };
    void RemoveOutpost(void*, void*, void*, void*);
    void RemoveItems(void*, void*, void*, void*);
    std::array<Site, 2> s_sites{{
        {0x021BFB3C, 0x90, 0x04D4B818, reinterpret_cast<std::uintptr_t>(&RemoveOutpost)},
        {0x021BFADC, 0x88, 0x04D4B650, reinterpret_cast<std::uintptr_t>(&RemoveItems)}
    }};
    void RemoveItem(RE::TESObjectREFR*);
    void RetireBeacon(RE::TESObjectREFR*);
    void NotifyItem(void*, RE::TESObjectREFR*, void*, std::uint32_t, bool);
    struct Call { std::uintptr_t rva, target, hook, stub{}; };
    std::array<Call,3> s_calls{{
        {0x021B7494,0x0061CDA0,reinterpret_cast<std::uintptr_t>(&NotifyItem)},
        {0x021B749C,0x00F3A6E0,reinterpret_cast<std::uintptr_t>(&RemoveItem)},
        {0x021B78D9,0x00F3A6E0,reinterpret_cast<std::uintptr_t>(&RetireBeacon)}
    }};

    // Pins exist only on this synchronous stock-handler stack. No object pointer
    // survives a deferred queue, load, Papyrus poll, or thread boundary.
    struct Survivor {
        RE::NiPointer<RE::TESObjectREFR> ref;
        std::uint32_t id{};
        bool disabled{};
        RE::NiPoint3 position{};
    };
    struct Removal {
        RE::TESObjectREFR* beacon{};
        std::uint64_t epoch{};
        std::uint32_t registryKey{};
        std::vector<Survivor> survivors;
        std::uint32_t notifications{}, preserved{}, built{};
        bool retired{}, fault{};
    };
    thread_local Removal* s_removal{};

    // Shared workshop-beacon registry, populated during ordinary reference
    // initialization too. Copy under its actual read lock; no engine storage is
    // resized/written and no pointer escapes the synchronous snapshot.
    bool CopyRegistry(std::span<std::uint32_t> a_handles, std::uint32_t& a_count,std::uint32_t& a_capacity) noexcept
    {
        __try {
            const auto base=REX::FModule::GetExecutingModule().GetBaseAddress();
            const auto size=*reinterpret_cast<const std::uint32_t*>(base+0x05F0BA78);
            const auto capacity=*reinterpret_cast<const std::uint32_t*>(base+0x05F0BA7C);
            const auto* data=*reinterpret_cast<const std::uint32_t* const*>(base+0x05F0BA80);
            a_count=size; a_capacity=capacity;
            return CopyTeardownRegistry(data,size,capacity,a_handles);
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ReadCanonicalKey(RE::TESObjectREFR* a_beacon,std::uint32_t& a_key) noexcept
    {
        __try {
            // Same out-parameter ABI and exact callback as the stock registry's
            // registration/erase paths. Do not substitute a FormID/index.
            using Fn=std::uint32_t* (*)(std::uint32_t*,RE::TESObjectREFR*);
            return reinterpret_cast<Fn>(REX::FModule::GetExecutingModule().GetBaseAddress()+0x002CEF20)(&a_key,a_beacon)==&a_key;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ResolveRegistryKey(RE::TESObjectREFR* a_beacon,std::uint32_t& a_key)
    {
        if (!a_beacon || !ReadCanonicalKey(a_beacon,a_key)) {
            REX::ERROR("InheritedTeardown-v4: canonical target-key read failed"); return false;
        }
        RE::BSPointerHandle<RE::TESObjectREFR> handle;
        static_assert(sizeof(handle)==sizeof(a_key));
        std::memcpy(&handle,&a_key,sizeof(handle));
        const auto resolved=handle.get();
        if (resolved.get()!=a_beacon) {
            REX::ERROR("InheritedTeardown-v4: target key did not resolve exact beacon={:08X} key={:08X}",a_beacon->formID,a_key);
            return false;
        }
        return true;
    }
    bool RegistryContains(std::uint32_t a_key,bool& a_present)
    {
        a_present=false;
        std::vector<std::uint32_t> handles;
        try { handles.resize(kTeardownRegistryLimit); }
        catch (...) { REX::ERROR("InheritedTeardown-v4: registry snapshot allocation failed before acquiring engine lock"); return false; }
        std::uint32_t count{},capacity{};
        bool copied{};
        const auto base=REX::FModule::GetExecutingModule().GetBaseAddress();
        {
            RE::BSAutoReadLock lock(*reinterpret_cast<RE::BSReadWriteLock*>(base+0x05F0BA88));
            copied=CopyRegistry(handles,count,capacity);
        }
        if (!copied) {
            REX::ERROR("InheritedTeardown-v4: registry snapshot rejected size={} capacity={} maxEntries={} maxCapacity={} bufferEntries={}",count,capacity,kTeardownRegistryLimit,kTeardownRegistryCapacityLimit,handles.size());
            return false;
        }
        a_present=ContainsTeardownRegistryKey(std::span<const std::uint32_t>(handles.data(),count),a_key);
        REX::INFO("InheritedTeardown-v4: exact shared-registry membership key={:08X} count={} capacity={} present={}",a_key,count,capacity,a_present);
        return true;
    }
    bool Capture(Removal& a_removal)
    {
        std::vector<std::uint32_t> ids;
        if (!PapyrusNatives::SnapshotInheritedIDs(ids)) {
            REX::ERROR("InheritedTeardown-v4: survivor ledger snapshot rejected"); return false;
        }
        a_removal.survivors.reserve(ids.size());
        for (const auto id:ids) {
            if (!id) continue;
            auto* ref=RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
            if (!ref) { REX::ERROR("InheritedTeardown-v4: survivor unresolved ref={:08X}",id); return false; }
            if (ref->IsDeleted()) continue; // Intentional scrapping stays deleted.
            const auto base=ref->GetBaseObject();
            if (!base || (!base->Is(RE::FormType::kFURN) && !base->Is(RE::FormType::kCONT))) {
                REX::ERROR("InheritedTeardown-v4: survivor unsupported base ref={:08X}",id); return false;
            }
            a_removal.survivors.push_back({RE::NiPointer<RE::TESObjectREFR>(ref),id,ref->IsDisabled(),ref->GetPosition()});
        }
        return true;
    }
    bool Preflight(Removal& a_removal)
    {
        if (!ResolveRegistryKey(a_removal.beacon,a_removal.registryKey)) return false;
        bool present{};
        if (!RegistryContains(a_removal.registryKey,present)) return false;
        if (!present) {
            REX::ERROR("InheritedTeardown-v4: exact target absent before removal beacon={:08X} key={:08X}",a_removal.beacon->formID,a_removal.registryKey);
            return false;
        }
        return Capture(a_removal);
    }
    bool Preserve(RE::TESObjectREFR* a_item)
    {
        if (!s_removal || !a_item) return false;
        if (s_removal->epoch!=s_loadEpoch.load()) { s_removal->fault=true; return true; }
        for (const auto& item:s_removal->survivors) {
            if (item.ref.get()!=a_item) continue;
            const auto action=ClassifyTeardownItem(reinterpret_cast<std::uintptr_t>(item.ref.get()),item.id,
                reinterpret_cast<std::uintptr_t>(a_item),a_item->formID,
                reinterpret_cast<std::uintptr_t>(RE::TESForm::LookupByID<RE::TESObjectREFR>(item.id)));
            if (action==TeardownItemAction::Fault) s_removal->fault=true;
            return true; // Identity drift blocks this destructive operation too.
        }
        return false;
    }
    void NotifyItem(void* a_manager,RE::TESObjectREFR* a_item,void* a_key,std::uint32_t a_unknown,bool a_final)
    {
        if (Preserve(a_item)) { ++s_removal->notifications; return; }
        using Fn=void (*)(void*,RE::TESObjectREFR*,void*,std::uint32_t,bool);
        reinterpret_cast<Fn>(REX::FModule::GetExecutingModule().GetBaseAddress()+0x0061CDA0)(a_manager,a_item,a_key,a_unknown,a_final);
    }
    void RemoveItem(RE::TESObjectREFR* a_item)
    {
        if (Preserve(a_item)) {
            ++s_removal->preserved;
            REX::INFO("InheritedTeardown-v4: skipped vanilla item retirement ref={:08X}",a_item->formID);
            return;
        }
        if (s_removal) ++s_removal->built;
        using Fn=void (*)(RE::TESObjectREFR*);
        reinterpret_cast<Fn>(REX::FModule::GetExecutingModule().GetBaseAddress()+0x00F3A6E0)(a_item);
    }
    void RetireBeacon(RE::TESObjectREFR* a_beacon)
    {
        if (s_removal && (s_removal->beacon!=a_beacon || s_removal->epoch!=s_loadEpoch.load())) { s_removal->fault=true; return; }
        using Fn=void (*)(RE::TESObjectREFR*);
        reinterpret_cast<Fn>(REX::FModule::GetExecutingModule().GetBaseAddress()+0x00F3A6E0)(a_beacon);
        if (s_removal) s_removal->retired=true;
    }
    bool Survived(const Removal& a_removal)
    {
        if (a_removal.fault) return false;
        for (const auto& item:a_removal.survivors) {
            auto* ref=item.ref.get();
            const auto pos=ref->GetPosition();
            if (RE::TESForm::LookupByID<RE::TESObjectREFR>(item.id)!=ref || ref->IsDeleted() ||
                ref->IsDisabled()!=item.disabled || pos.x!=item.position.x || pos.y!=item.position.y || pos.z!=item.position.z) {
                REX::ERROR("InheritedTeardown-v4: survivor state changed ref={:08X}; never restoring/re-enabling",item.id);
                return false;
            }
        }
        return true;
    }
    bool Verify(std::uintptr_t a_base, bool a_owned) noexcept
    {
        __try {
            for (const auto& gate : kGates) {
                std::uint64_t h = 14695981039346656037ull;
                const auto* p = reinterpret_cast<const std::uint8_t*>(a_base + gate.rva);
                for (std::size_t i=0; i<gate.length; ++i) h=(h^p[i])*1099511628211ull;
                if (h != gate.hash) return false;
            }
            for (const auto& site : s_sites) {
                const auto* p = reinterpret_cast<const std::uint8_t*>(a_base+site.rva);
                std::int32_t disp{};
                std::memcpy(&disp,p+2,4);
                if (a_owned) {
                    if (!MatchesOwnedIndirectGuardCall(std::span<const std::uint8_t,6>(p,6),a_base+site.rva,
                        std::span<const std::uint8_t,8>(reinterpret_cast<const std::uint8_t*>(site.pointer),8),site.pointer,site.hook)) return false;
                } else if (p[0]!=0xFF || p[1]!=0x90 || disp!=static_cast<std::int32_t>(site.slot)) return false;
            }
            for (const auto& call:s_calls) {
                const auto* p=reinterpret_cast<const std::uint8_t*>(a_base+call.rva);
                if (a_owned) {
                    if (!MatchesOwnedGuardCall(std::span<const std::uint8_t,5>(p,5),a_base+call.rva,
                        std::span<const std::uint8_t,14>(reinterpret_cast<const std::uint8_t*>(call.stub),14),call.stub,call.hook)) return false;
                } else {
                    std::int32_t disp{}; std::memcpy(&disp,p+1,4);
                    if (p[0]!=0xE8 || a_base+call.rva+5+static_cast<std::uintptr_t>(disp)!=a_base+call.target) return false;
                }
            }
            // RTTI-pinned idle state is where the accepted removal occurs.
            return *reinterpret_cast<const std::uintptr_t*>(a_base+0x04D49DC8+0x90)==a_base+0x021B7200 &&
                *reinterpret_cast<const std::uintptr_t*>(a_base+0x04D49DC8+0x88)==a_base+0x021B79C0 &&
                *reinterpret_cast<const std::uintptr_t*>(a_base+0x04AF8BE0+8)==a_base+0x002CF060;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Command(void* a_state, void* a_event, const Site& a_site,
        RE::TESObjectREFR*& a_beacon, Dispatch& a_original) noexcept
    {
        __try {
            const auto base=REX::FModule::GetExecutingModule().GetBaseAddress();
            if (!a_state || !a_event || *reinterpret_cast<std::uintptr_t*>(a_event)!=base+a_site.eventVtable) return false;
            const auto vtable=*reinterpret_cast<std::uintptr_t*>(a_state);
            a_original=reinterpret_cast<Dispatch>(*reinterpret_cast<std::uintptr_t*>(vtable+a_site.slot));
            a_beacon=*reinterpret_cast<RE::TESObjectREFR**>(static_cast<std::byte*>(a_event)+8);
            return a_original != nullptr;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void DispatchRemoval(void* a_state, void* a_event, void* a_context, void* a_resources, std::size_t a_site)
    {
        RE::TESObjectREFR* beacon{};
        Dispatch original{};
        if (!Command(a_state,a_event,s_sites[a_site],beacon,original)) {
            REX::ERROR("InheritedTeardown-v4: malformed removal command blocked");
            return;
        }
        const auto kind=PapyrusNatives::ClassifyTeardown(beacon);
        if (kind==0) { original(a_state,a_event,a_context,a_resources); return; }
        // Unknown state/layout/ledger never authorizes a destructive call.
        const auto base=REX::FModule::GetExecutingModule().GetBaseAddress();
        if (kind<0 || !TeardownProtection::Ready() ||
            reinterpret_cast<std::uintptr_t>(original)!=base+(a_site==0 ? 0x021B7200 : 0x021B79C0)) {
            REX::ERROR("InheritedTeardown-v4: removal blocked; unreadable ledger/runtime/state");
            return;
        }
        if (a_site!=0) {
            REX::WARN("InheritedTeardown-v4: bulk item removal blocked for claimed beacon {:08X}",beacon->formID);
            return; // Never convert a bulk-item request into outpost removal.
        }
        bool authorized=false;
        std::uint32_t executingToken{};
        {
            std::scoped_lock lock(s_requestMutex);
            if (s_request.authorized && GetTickCount64()-s_authorizedAt<=10000 &&
                PapyrusNatives::InheritedObjectsDetached(beacon)) authorized=s_request.Consume(beacon->formID);
            if (authorized) executingToken=s_request.token;
            if (!authorized) {
                const auto token=s_request.Queue(beacon->formID);
                REX::INFO("InheritedTeardown-v4: removal deferred beacon={:08X} token={} protected={}",beacon->formID,token,kind==2);
            }
        }
        if (!authorized) return;
        Removal removal;
        removal.beacon=beacon;
        removal.epoch=s_loadEpoch.load();
        // Preflight before entering the destructive handler. Scope pins release
        // at normal return and never escape to another queue/load/thread.
        bool captured=false;
        try { captured=!s_removal && Preflight(removal); }
        catch (...) { captured=false; REX::ERROR("InheritedTeardown-v4: destructive-handler preflight exception"); }
        const bool sameEpoch=removal.epoch==s_loadEpoch.load();
        const bool sameRequest=TeardownProtection::Matches(executingToken,beacon->formID);
        const bool runtimeReady=TeardownProtection::Ready();
        if (!captured || !sameEpoch || !sameRequest || !runtimeReady) {
            TeardownProtection::Fault();
            std::scoped_lock lock(s_requestMutex); s_completion=-1;
            REX::ERROR("InheritedTeardown-v4: destructive-handler preflight blocked captured={} sameEpoch={} sameRequest={} runtimeReady={}; no vanilla removal forwarded",captured,sameEpoch,sameRequest,runtimeReady);
            return;
        }
        const RE::NiPointer<RE::TESObjectREFR> retainedBeacon(beacon);
        struct Scope { explicit Scope(Removal& r) { s_removal=&r; } ~Scope() { s_removal=nullptr; } };
        {
            Scope scope(removal);
            REX::INFO("InheritedTeardown-v4: forwarding scoped stock removal beacon={:08X} survivors={}",beacon->formID,removal.survivors.size());
            original(a_state,a_event,a_context,a_resources);
        }
        bool stillRegistered=true;
        // Use the before-removal key; the retired beacon need not resolve now.
        const bool readable=RegistryContains(removal.registryKey,stillRegistered);
        const bool survived=removal.epoch==s_loadEpoch.load() && Survived(removal);
        const bool completed=ConfirmTeardown(true,removal.retired,true,readable,stillRegistered,survived);
        {
            std::scoped_lock lock(s_requestMutex);
            if (s_request.Matches(executingToken,beacon->formID) && s_request.consumed) s_completion=completed ? 1 : -1;
        }
        if (!completed) TeardownProtection::Fault();
        REX::INFO("InheritedTeardown-v4: completion beacon={:08X} retired={} registryReadable={} stillRegistered={} survivorsUnchanged={} skippedRetirements={} skippedNotifications={} vanillaOtherItems={} confirmed={}",
            beacon->formID,removal.retired,readable,stillRegistered,survived,removal.preserved,removal.notifications,removal.built,completed);
    }
    void RemoveOutpost(void* a,void* b,void* c,void* d) { DispatchRemoval(a,b,c,d,0); }
    void RemoveItems(void* a,void* b,void* c,void* d) { DispatchRemoval(a,b,c,d,1); }

    struct Array { std::uint32_t size{}, capacity{}; const void* data{}; bool operator==(const Array&) const = default; };
    static_assert(sizeof(Array)==16 && offsetof(Array,data)==8);
    struct List { Array authored{}, dynamic{}; std::uint32_t added{}; bool operator==(const List&) const = default; };
    bool ReadLayout(const RE::BGSListForm* a_list,List& a_out) noexcept
    {
        __try {
            if (!a_list || !a_list->Is(RE::FormType::kFLST)) return false;
            const auto* p=reinterpret_cast<const std::byte*>(a_list);
            a_out.authored=*reinterpret_cast<const Array*>(p+0x38);
            const auto* dynamic=*reinterpret_cast<const Array* const*>(p+0x88);
            a_out.dynamic=dynamic ? *dynamic : Array{};
            a_out.added=*reinterpret_cast<const std::uint32_t*>(p+0x90);
            return IsBoundedTeardownList(a_out.authored.size,a_out.authored.capacity,a_out.authored.data!=nullptr,
                a_out.dynamic.size,a_out.dynamic.capacity,a_out.dynamic.data!=nullptr,a_out.added);
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ReadEntries(const List& a_list,std::uint32_t* a_out) noexcept
    {
        __try {
            std::size_t n=0;
            const auto* forms=static_cast<RE::TESForm* const*>(a_list.authored.data);
            for (std::uint32_t i=0;i<a_list.authored.size;++i) a_out[n++]=forms[i] ? forms[i]->formID : 0;
            const auto* ids=static_cast<const std::uint32_t*>(a_list.dynamic.data);
            for (std::uint32_t i=0;i<a_list.dynamic.size;++i) a_out[n++]=ids[i];
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Add(RE::BGSListForm* a_list,RE::TESObjectREFR* a_ref) noexcept
    {
        __try {
            using Fn=void (*)(RE::BGSListForm*,RE::TESForm*);
            reinterpret_cast<Fn>(REX::FModule::GetExecutingModule().GetBaseAddress()+0xBD6550)(a_list,a_ref);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool QueueVanilla(RE::TESObjectREFR* a_beacon) noexcept
    {
        __try {
            using Fn=bool (*)(RE::TESObjectREFR*);
            return reinterpret_cast<Fn>(REX::FModule::GetExecutingModule().GetBaseAddress()+0x021BB570)(a_beacon);
        } __except(EXCEPTION_EXECUTE_HANDLER) { s_faulted.store(true); return false; }
    }
}

namespace ClaimablePOIs::TeardownProtection
{
    bool Ready() noexcept
    {
        const auto m=REX::FModule::GetExecutingModule();
        return s_installed.load() && !s_faulted.load() && m.GetFileVersion()==SFSE::RUNTIME_SF_1_16_244 && Verify(m.GetBaseAddress(),true);
    }
    bool PreflightRemoval(RE::TESObjectREFR* a_beacon) noexcept
    {
        if (!Ready() || !a_beacon) return false;
        try {
            Removal removal;
            removal.beacon=a_beacon;
            removal.epoch=s_loadEpoch.load();
            return !s_removal && Preflight(removal) && removal.epoch==s_loadEpoch.load();
        } catch (...) { REX::ERROR("InheritedTeardown-v4: before-detach preflight exception"); return false; }
    }
    void Fault() noexcept { s_faulted.store(true); }
    bool Install() noexcept
    {
        const auto m=REX::FModule::GetExecutingModule(); const auto base=m.GetBaseAddress();
        if (s_installed.load()) return Ready();
        if (m.GetFileVersion()!=SFSE::RUNTIME_SF_1_16_244 || !Verify(base,false) ||
            REL::ID(466548).address()!=base+0x04D49A10 || REL::ID(466550).address()!=base+0x04D499F0 ||
            REL::ID(467347).address()!=base+0x04D4B818 || REL::ID(467350).address()!=base+0x04D4B650 ||
            REL::ID(123862).address()!=base+0x022CC350 || REL::ID(36021).address()!=base+0x002C5C60 ||
            REL::ID(35638).address()!=base+0x002B65E0) return false;
        auto& trampoline=REL::GetTrampoline();
        for (auto& site:s_sites) site.pointer=trampoline.allocate_branch6(site.hook);
        for (auto& call:s_calls) call.stub=trampoline.allocate_branch5(call.hook);
        for (const auto& site:s_sites) REL::WriteSafeData(base+site.rva,REL::ASM::CALL6(base+site.rva,site.pointer));
        for (const auto& call:s_calls) trampoline.write_call<5>(base+call.rva,call.hook);
        if (!Verify(base,true)) {
            for (const auto& call:s_calls) {
                const auto* p=reinterpret_cast<const std::uint8_t*>(base+call.rva);
                if (!MatchesOwnedGuardCall(std::span<const std::uint8_t,5>(p,5),base+call.rva,
                    std::span<const std::uint8_t,14>(reinterpret_cast<const std::uint8_t*>(call.stub),14),call.stub,call.hook)) continue;
                std::array<std::uint8_t,5> bytes{0xE8};
                const auto disp=static_cast<std::int32_t>(call.target-call.rva-5);
                std::memcpy(bytes.data()+1,&disp,4);
                REL::WriteSafeData(base+call.rva,bytes);
            }
            for (const auto& site:s_sites) {
                const auto* call=reinterpret_cast<const std::uint8_t*>(base+site.rva);
                if (!MatchesOwnedIndirectGuardCall(std::span<const std::uint8_t,6>(call,6),base+site.rva,
                    std::span<const std::uint8_t,8>(reinterpret_cast<const std::uint8_t*>(site.pointer),8),site.pointer,site.hook)) continue;
                std::array<std::uint8_t,6> bytes{0xFF,0x90};
                const auto slot=static_cast<std::int32_t>(site.slot); std::memcpy(bytes.data()+2,&slot,4);
                REL::WriteSafeData(base+site.rva,bytes);
            }
            return false;
        }
        s_installed.store(true);
        REX::INFO("InheritedTeardown-v4: dispatch, item-retirement and beacon-completion guards installed; no deferred engine object retained");
        return true;
    }
    void ResetLoad() noexcept { std::scoped_lock lock(s_requestMutex); ++s_loadEpoch; s_request.Reset(); s_authorizedAt=0; s_completion=0; }
    bool Snapshot(const RE::BGSListForm* a_list,std::vector<std::uint32_t>& a_out) noexcept
    {
        std::scoped_lock lock(s_listMutex); a_out.clear(); List before{},after{};
        if (!ReadLayout(a_list,before)) return false;
        try { a_out.resize(before.authored.size+before.dynamic.size); } catch (...) { return false; }
        if (!ReadEntries(before,a_out.data()) || !ReadLayout(a_list,after) || before!=after) { a_out.clear(); return false; }
        return true;
    }
    bool Contains(const RE::BGSListForm* a_list,std::uint32_t a_id,bool& a_valid) noexcept
    {
        std::vector<std::uint32_t> ids; a_valid=Snapshot(a_list,ids);
        return a_valid && std::ranges::find(ids,a_id)!=ids.end();
    }
    bool Record(RE::BGSListForm* a_list,RE::TESObjectREFR* a_ref) noexcept
    {
        if (!Ready() || !a_ref) return false;
        std::scoped_lock lock(s_listMutex); std::vector<std::uint32_t> ids;
        if (!Snapshot(a_list,ids)) return false;
        if (std::ranges::find(ids,a_ref->formID)!=ids.end()) return true;
        if (ids.size()>=kListLimit || !Add(a_list,a_ref)) return false;
        bool valid{}; return Contains(a_list,a_ref->formID,valid) && valid;
    }
    bool WorkshopOwner(RE::TESObjectREFR* a_ref,RE::TESObjectREFR*& a_owner) noexcept
    {
        a_owner=nullptr;
        if (!Ready() || !a_ref) return false;
        __try {
            const auto base=REX::FModule::GetExecutingModule().GetBaseAddress();
            auto* key=*reinterpret_cast<RE::BGSKeyword**>(base+0x05F0B3D8);
            if (!key || !key->Is(RE::FormType::kKYWD)) return false;
            if (!a_ref->extraDataList) return true;
            using Fn=RE::TESObjectREFR* (*)(RE::ExtraDataList*,RE::BGSKeyword*,bool);
            a_owner=reinterpret_cast<Fn>(base+0x00517D60)(a_ref->extraDataList.get(),key,true);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool EnqueueRemoval(RE::TESObjectREFR* a_beacon,std::uint32_t a_token) noexcept
    {
        if (!Ready() || !a_beacon) return false;
        std::scoped_lock lock(s_requestMutex);
        if (!s_request.Matches(a_token,a_beacon->formID) || !s_request.authorized || s_request.consumed) return false;
        return QueueVanilla(a_beacon);
    }
    std::uint32_t PendingToken() noexcept { std::scoped_lock lock(s_requestMutex); return s_request.token; }
    std::uint32_t PendingBeacon(std::uint32_t a_token) noexcept { std::scoped_lock lock(s_requestMutex); return s_request.token==a_token ? s_request.beacon : 0; }
    bool Matches(std::uint32_t a_token,std::uint32_t a_beacon) noexcept { std::scoped_lock lock(s_requestMutex); return s_request.Matches(a_token,a_beacon); }
    bool Authorize(std::uint32_t a_token,std::uint32_t a_beacon) noexcept
    { std::scoped_lock lock(s_requestMutex); if(!s_request.Authorize(a_token,a_beacon)) return false; s_authorizedAt=GetTickCount64(); return true; }
    void Cancel(std::uint32_t a_token) noexcept { std::scoped_lock lock(s_requestMutex); if(s_request.token==a_token) { s_request.Reset(); s_completion=0; } }
    bool Busy() noexcept { std::scoped_lock lock(s_requestMutex); return s_request.token!=0; }
    int Completion(std::uint32_t a_token) noexcept
    { std::scoped_lock lock(s_requestMutex); return a_token && s_request.token==a_token && s_request.consumed ? s_completion : 0; }
}
