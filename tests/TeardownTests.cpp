#include "TeardownPolicy.h"
#include "GuardedCallValidation.h"
#include <array>
#include <iostream>

bool RunTeardownTests()
{
    using namespace ClaimablePOIs;
    bool passed=true;
    const auto check=[&](bool value,const char* name) {
        if (!value) { std::cerr<<"Teardown: "<<name<<'\n'; passed=false; }
    };
    TeardownRequest request;
    {
        std::array<std::uint32_t,419> captured{},snapshot{};
        for (std::uint32_t i=0;i<captured.size();++i) captured[i]=0x00100000+i;
        constexpr std::uint32_t beaconKey=0x12345678;
        captured.back()=beaconKey;
        check(IsBoundedTeardownRegistry(419,512,true),"captured 419-entry/capacity512 engine registry is valid");
        check(CopyTeardownRegistry(captured.data(),419,512,snapshot) && snapshot==captured &&
            ContainsTeardownRegistryKey(snapshot,beaconKey),"captured registry fully copies and finds target beyond old 256-entry cutoff");
        std::array<std::uint32_t,256> oldBuffer{};
        oldBuffer.fill(0xDEADBEEF);
        check(!CopyTeardownRegistry(captured.data(),419,512,oldBuffer) && oldBuffer.front()==0xDEADBEEF && oldBuffer.back()==0xDEADBEEF,
            "undersized destination rejects before any write rather than truncating registry");
        check(!CopyTeardownRegistry(captured.data(),419,418,snapshot),"size beyond capacity rejected");
        check(!CopyTeardownRegistry(nullptr,419,512,snapshot),"missing engine storage rejected");
        check(IsBoundedTeardownRegistry(8192,16384,true),"shared registry not limited by inherited ledger capacity");
        check(IsBoundedTeardownRegistry(kTeardownRegistryLimit,kTeardownRegistryCapacityLimit,true),"resource bounds include exact upper limit");
        check(!IsBoundedTeardownRegistry(kTeardownRegistryLimit+1,kTeardownRegistryCapacityLimit,true),"oversized registry fails closed");
        check(!IsBoundedTeardownRegistry(419,kTeardownRegistryCapacityLimit+1,true),"corrupt capacity fails closed");
        check(IsBoundedTeardownRegistry(0,0,false),"empty unallocated registry is readable");
    }
    const std::array<std::uint32_t,4> registry{0xFFFFFFFF,0x00123456,0,0x00ABCDEF};
    check(ContainsTeardownRegistryKey(registry,0x00123456),"unrelated stale handles do not reject exact target membership");
    check(!ContainsTeardownRegistryKey(registry,0x01123456),"different generation with same lower index cannot match");
    check(!ContainsTeardownRegistryKey(registry,0xFF026AA3),"beacon FormID cannot substitute for canonical handle");
    check(!ContainsTeardownRegistryKey({},0x00123456),"empty registry proves absence of known canonical target key");
    check(ConfirmTeardown(true,true,true,true,false,true),"completion uses executed retirement and registry absence, no delete bit");
    check(!ConfirmTeardown(false,true,true,true,false,true),"queue acceptance alone cannot confirm completion");
    check(!ConfirmTeardown(true,false,true,true,false,true),"early handler return is not completion");
    check(!ConfirmTeardown(true,true,false,true,false,true),"beacon must have been registered before removal");
    check(!ConfirmTeardown(true,true,true,false,false,true),"unreadable registry cannot prove absence");
    check(!ConfirmTeardown(true,true,true,true,true,true),"beacon still registered blocks completion");
    check(!ConfirmTeardown(true,true,true,true,false,false),"lost inherited state blocks completion");
    check(ClassifyTeardownItem(0x1000,42,0x1000,42,0x1000)==TeardownItemAction::Preserve,"exact inherited object bypasses retirement");
    check(ClassifyTeardownItem(0x1000,42,0x2000,42,0x2000)==TeardownItemAction::Forward,"equal ID on another object is not authority");
    check(ClassifyTeardownItem(0x1000,42,0x2000,43,0x2000)==TeardownItemAction::Forward,"player-built object follows vanilla");
    check(ClassifyTeardownItem(0x1000,42,0x1000,43,0x1000)==TeardownItemAction::Fault,"changed identity blocks retirement and completion");
    check(ClassifyTeardownItem(0x1000,42,0x1000,42,0)==TeardownItemAction::Fault,"missing registry lookup cannot authorize destruction");
    check(ClassifyTeardownItem(0,0,0x2000,43,0x2000)==TeardownItemAction::Forward,"no captured authority preserves vanilla behavior");
    check(IsBoundedTeardownList(0,0,false,0,0,false,0),"empty save-local list accepted");
    check(IsBoundedTeardownList(2,4,true,3,4,true,4),"added count can include authored-array additions, not just temporary IDs");
    check(!IsBoundedTeardownList(3,2,true,0,0,false,0),"size beyond capacity rejected");
    check(!IsBoundedTeardownList(1,2,false,0,0,false,0),"missing array storage rejected");
    check(!IsBoundedTeardownList(0,0,false,0,0,false,1),"impossible added count rejected");
    check(!IsBoundedTeardownList(2049,4096,true,2049,4096,true,0),"combined ledger cap enforced");
    check(!IsBoundedTeardownList(0,0xFFFFFFFF,true,0,0,false,0),"overflow-sized metadata rejected");
    {
        std::array<std::uint8_t,6> call{0xFF,0x15};
        std::array<std::uint8_t,8> pointer{};
        const std::int32_t displacement=-0x106;
        const std::uint64_t hook=0x12345678;
        std::memcpy(call.data()+2,&displacement,4);
        std::memcpy(pointer.data(),&hook,8);
        check(MatchesOwnedIndirectGuardCall(call,0x2000,pointer,0x1F00,hook),"owned six-byte call accepts signed displacement");
        auto foreign=pointer;foreign[0]^=1;
        check(!MatchesOwnedIndirectGuardCall(call,0x2000,foreign,0x1F00,hook),"foreign hook target rejected");
        call[1]=0x90;
        check(!MatchesOwnedIndirectGuardCall(call,0x2000,pointer,0x1F00,hook),"original virtual call is never treated as an owned patch");
    }
    check(request.Queue(0)==0,"null beacon rejected");
    const auto first=request.Queue(0xFF000123);
    check(first && request.Queue(0xFF000123)==first,"duplicate removal is idempotent");
    check(!request.Queue(0xFF000124),"second beacon cannot replace pending request");
    check(!request.Consume(0xFF000123),"unprepared request cannot reach vanilla");
    check(!request.Authorize(first,0xFF000124),"foreign beacon cannot use token");
    request.Reset();
    const auto second=request.Queue(0xFF000123);
    check(second!=first && !request.Authorize(first,0xFF000123),"load reset invalidates stale token");
    check(request.Authorize(second,0xFF000123),"matching prepared request accepted");
    check(!request.Consume(0xFF000124) && request.Consume(0xFF000123) && !request.Consume(0xFF000123),"permit is beacon-bound and single-use");
    const std::array<int,3> targets{0,1,2};
    for (int failure=-1;failure<3;++failure) {
        std::array<bool,3> linked{true,true,true};
        int calls=0;
        const auto result=DetachForTeardown(targets,[]{return true;},[&](int i){return linked[i];},
            [&](int i){++calls;if(i==failure)return false;linked[i]=false;return true;},
            [&](int i){return !linked[i];});
        if (failure<0) check(result.success && result.detached==3 && calls==3,"complete batch verifies every detach");
        else check(!result.success && result.attempted && result.detached==static_cast<unsigned>(failure) && calls==failure+1,
            "failure stops remaining mutations and cannot authorize vanilla");
        // No reattach/enable/recreate path exists in this controller.
        if(failure>0)check(!linked[0],"failed batch does not silently undo successful detach");
    }
    int mutations=0;
    const auto preflight=DetachForTeardown(targets,[]{return true;},[](int i){return i!=2;},
        [&](int){++mutations;return true;},[](int){return true;});
    check(!preflight.success && !preflight.attempted && mutations==0,"late invalid ownership preflights before first mutation");
    const auto stale=DetachForTeardown(targets,[]{return false;},[](int){return true;},
        [&](int){++mutations;return true;},[](int){return true;});
    check(!stale.success && !stale.attempted && mutations==0,"load/epoch change blocks batch");
    std::array<bool,3> linked{true,true,true};
    const auto badPostcondition=DetachForTeardown(targets,[]{return true;},[&](int i){return linked[i];},
        [&](int i){linked[i]=false;return true;},[](int){return false;});
    check(!badPostcondition.success && badPostcondition.detached==0 && linked[1],"unverified engine result blocks subsequent detach");
    int epoch=0;
    const auto changed=DetachForTeardown(targets,[&]{return epoch==0;},[](int){return true;},
        [&](int){epoch=1;return true;},[](int){return true;});
    check(!changed.success && changed.detached==1,"epoch change after mutation never approves vanilla");
    if(passed)std::cout<<"Inherited teardown request/preflight/failure tests passed\n";
    return passed;
}
