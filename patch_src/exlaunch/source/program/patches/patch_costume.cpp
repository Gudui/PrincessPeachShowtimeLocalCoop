#include "patch_costume.hpp"
#include "patch_core.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// The henshin stand (AHenshinAreaLinearEventBase) swaps only
// GetPlayerCharacter(0) to its costume when its princess sequencer ends. The
// swap routine itself is per pawn: it spawns the costume class and possesses it
// with the given pawn's own controller. After the unchanged original, this
// makes the same native swap call for player index 1. No field writes.
// ---------------------------------------------------------------------------
constexpr bool kPatch0248HenshinCostumeParity = true;

// Sequencer-end handler, entry prologue sub sp,#0x30 / stp x29,x30,[sp,#0x10] /
// add x29 / stp x20,x19; delegate-bound at 0x15EAAF0
// (FUNC-HENSHINAREALINEAREVENT-ONPRINCESSSEQUENCEREND).
constexpr ptrdiff_t OnPrincessSequencerEnd = 0x15EB0B0;
// UGameplayStatics::GetPlayerCharacter(ctx, int32 index); the original reaches
constexpr ptrdiff_t GetPlayerCharacterFn = 0x44FB574;
// Pawn class gate the original applies before swapping (0x15EB0F4).
constexpr ptrdiff_t PawnClassFn = 0x2514FB8;
// ChangePawnToCostume(pawn, costumeID, bool) (FUNC-PAWN-CHANGETOCOSTUME).
constexpr ptrdiff_t ChangePawnToCostumeFn = 0x137D7F0;
// World-context virtual the original calls on `this` (0x15EB0DC).
constexpr std::uintptr_t ContextSlot = 0x168;
// henshinTarget_ UPROPERTY, EPeachCostumeID (int32), read at 0x15EB120.
constexpr std::uintptr_t HenshinTargetOffset = 0x4F0;
// UStruct class-tree fields the native IsA reads (0x15EB0F8..0x15EB118).
constexpr std::uintptr_t ObjectClassOffset = 0x10;
constexpr std::uintptr_t ClassTreeArrayOffset = 0x30;
constexpr std::uintptr_t ClassTreeDepthOffset = 0x38;

// ---------------------------------------------------------------------------
// The stand hides the new player-0 pawn at sequencer end with
// 0x1E89558(pawn+0x4B8, 1, 0) (call 0x15EB1B8) and shows it again from the
// timeline tick 0x15EC960 with 0x1E89558(pawn+0x4B8, 1, 1) (call 0x15ECED8)
// when the tick sets its once-latch this+0x727 (strb 0x15ECE8C). The gate
// recompute then calls SetActorHiddenInGame(false), which also clears the
// possess-time cinematic hide (SetPawn 0x4803B5C). The tick only reaches the
// pawn in its this+0x6D8 handle (player 0), so this makes the same two native
// calls for P2 at the same two moments. The stencil call 0x1EDE99C stays
// ---------------------------------------------------------------------------
constexpr bool kPatch0249HenshinGateParity = true;

// Timeline tick, entry prologue sub sp,#0xa0 / stp d11,d10 / stp d9,d8 /
// stp x29,x30,[sp,#0x60]; this in x0 (mov x19,x0 at 0x15EC984); single
// caller 0x15EBBD0 (FUNC-HENSHINAREALINEAREVENT-TIMELINETICK).
constexpr ptrdiff_t TimelineTick = 0x15EC960;
// Gate update (FUNC-PAWN-IFACE-GATE-UPDATE): (pawn+0x4B8, bit, set).
constexpr ptrdiff_t GateUpdateFn = 0x1E89558;
constexpr std::uintptr_t GateInterfaceOffset = 0x4B8;
constexpr int GateBit = 1;
// Once-latch byte the tick sets right before its unhide call.
constexpr std::uintptr_t UnhideLatchOffset = 0x727;
// Gate target: 0x1E89558 resolves it as iface->vtable[+0x1C8](iface) and
// skips a null result (listing 0x1E89564..0x1E89578).
constexpr std::uintptr_t GateTargetSlot = 0x1C8;
// Gate target fields read by the recompute 0x1F12E50 (FUNC-NATIVE-GATE-RECOMPUTE):
// +0x3C1 active byte, +0x1F0 reason mask (int), +0x1E9 all-set byte.
constexpr std::uintptr_t GateActiveOffset = 0x3C1;
constexpr std::uintptr_t GateMaskOffset = 0x1F0;
constexpr std::uintptr_t GateAllSetOffset = 0x1E9;
constexpr std::uintptr_t ActorHiddenOffset = 0x58;
constexpr std::uint8_t ActorHiddenBit = 0x20;
constexpr std::uint32_t MaxGateLines = 24;

constexpr int P2PlayerIndex = 1;
constexpr std::uint32_t MaxLines = 16;

using ContextFn = void* (*)(void*);
using GetPlayerCharacterFnT = void* (*)(void*, int);
using ClassGetterFn = void* (*)();
using ChangePawnToCostumeFnT = void* (*)(void*, std::uint32_t, bool);
using GateUpdateFnT = void (*)(void*, int, bool);
using GateTargetFnT = void* (*)(void*);

static std::uint32_t sLines = 0;
// PATCH-0249's own throttle, so PATCH-0248 lines cannot crowd out the gate
// lines of a later stand.
static std::uint32_t sGateLines = 0;

static bool InMain(const void* p) {
    return exl::util::GetMainModuleInfo().m_Total.InRange(reinterpret_cast<std::uintptr_t>(p));
}

template <typename Fn>
static Fn At(ptrdiff_t offset) {
    return reinterpret_cast<Fn>(exl::util::GetMainModuleInfo().m_Total.m_Start + offset);
}

template <typename T>
static T Read(const void* base, std::uintptr_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(base) + offset);
}

// The same class-tree test the original performs inline.
static bool IsA(void* object, void* cls) {
    void* objClass = Read<void*>(object, ObjectClassOffset);
    if (objClass == nullptr || cls == nullptr) return false;
    const std::int32_t depth = Read<std::int32_t>(cls, ClassTreeDepthOffset);
    if (depth > Read<std::int32_t>(objClass, ClassTreeDepthOffset)) return false;
    void** tree = Read<void**>(objClass, ClassTreeArrayOffset);
    if (tree == nullptr) return false;
    return tree[depth] == static_cast<std::uint8_t*>(cls) + ClassTreeArrayOffset;
}

// PATCH-0248: P2's native player character, accepted only when it is the
// pawn patch_core tracks as P2 and passes the stand's class test.
static void* ResolveP2Pawn(void* self, const char* tag, void* expected = nullptr) {
    void* vtable = *reinterpret_cast<void**>(self);
    if (!InMain(vtable)) return nullptr;
    auto context = Read<ContextFn>(vtable, ContextSlot);
    if (!InMain(reinterpret_cast<void*>(context))) return nullptr;
    void* ctx = context(self);
    if (ctx == nullptr) return nullptr;

    void* pawn = At<GetPlayerCharacterFnT>(GetPlayerCharacterFn)(ctx, P2PlayerIndex);
    if (expected == nullptr) expected = GetP2Pawn();
    if (pawn == nullptr || pawn != expected) {
        if (sLines++ < MaxLines)
            Logging.Log("[PShowCoop] %s skip pawn native=%p expected=%p", tag, pawn, expected);
        return nullptr;
    }
    if (!IsA(pawn, At<ClassGetterFn>(PawnClassFn)())) {
        if (sLines++ < MaxLines) Logging.Log("[PShowCoop] %s skip pawn=%p class gate", tag, pawn);
        return nullptr;
    }
    return pawn;
}

// P2's native player character with the class test the stand applies to
// player 0. The native code re-resolves player 0 after the swap (0x15EB144)
// rather than trusting the swap's result; P2 is re-resolved the same way at
// both steps and never matched against patch_core's P2 pointer, which is
// player 0 through its +0x6D8 handle (0x15ECE94), which P2 has no slot in.
static void* ResolveNativeP2(void* self, const char* when) {
    void* vtable = *reinterpret_cast<void**>(self);
    if (!InMain(vtable)) return nullptr;
    auto context = Read<ContextFn>(vtable, ContextSlot);
    if (!InMain(reinterpret_cast<void*>(context))) return nullptr;
    void* ctx = context(self);
    if (ctx == nullptr) return nullptr;

    void* pawn = At<GetPlayerCharacterFnT>(GetPlayerCharacterFn)(ctx, P2PlayerIndex);
    if (pawn == nullptr || !IsA(pawn, At<ClassGetterFn>(PawnClassFn)())) {
        if (sGateLines++ < MaxGateLines)
            Logging.Log("[PShowCoop] PATCH-0249 %s skip native P2=%p (null or class gate)", when, pawn);
        return nullptr;
    }
    return pawn;
}

struct GateState {
    void* target;
    unsigned active;
    std::uint32_t mask;
    unsigned allSet;
    unsigned hidden;
};

// Read-only view of what the recompute will see: the same target lookup
// 0x1E89558 performs, then the fields 0x1F12E50 reads.
static GateState ReadGate(void* pawn) {
    GateState g{nullptr, 0, 0, 0, 0};
    g.hidden = (Read<std::uint8_t>(pawn, ActorHiddenOffset) & ActorHiddenBit) != 0 ? 1 : 0;
    void* iface = static_cast<std::uint8_t*>(pawn) + GateInterfaceOffset;
    void* vtable = *reinterpret_cast<void**>(iface);
    if (!InMain(vtable)) return g;
    auto getTarget = Read<GateTargetFnT>(vtable, GateTargetSlot);
    if (!InMain(reinterpret_cast<void*>(getTarget))) return g;
    g.target = getTarget(iface);
    if (g.target == nullptr) return g;
    g.active = Read<std::uint8_t>(g.target, GateActiveOffset);
    g.mask = Read<std::uint32_t>(g.target, GateMaskOffset);
    g.allSet = Read<std::uint8_t>(g.target, GateAllSetOffset);
    return g;
}

static void SetP2Gate(void* pawn, bool set, const char* when) {
    const GateState before = ReadGate(pawn);
    void* iface = static_cast<std::uint8_t*>(pawn) + GateInterfaceOffset;
    At<GateUpdateFnT>(GateUpdateFn)(iface, GateBit, set);
    const GateState after = ReadGate(pawn);
    if (sGateLines++ < MaxGateLines)
        Logging.Log("[PShowCoop] PATCH-0249 %s gate(pawn=%p, bit=%d, set=%d) target=%p active=%u "
                    "allset %u->%u mask 0x%08x->0x%08x hidden %u->%u",
                    when, pawn, GateBit, set ? 1 : 0, after.target, after.active, before.allSet,
                    after.allSet, before.mask, after.mask, before.hidden, after.hidden);
}

static void SwapP2(void* self) {
    void* pawn = ResolveP2Pawn(self, "PATCH-0248");
    if (pawn == nullptr) return;

    const std::uint32_t costume = Read<std::uint32_t>(self, HenshinTargetOffset);
    void* newPawn = At<ChangePawnToCostumeFnT>(ChangePawnToCostumeFn)(pawn, costume, false);
    if (sLines++ < MaxLines)
        Logging.Log("[PShowCoop] PATCH-0248 applied stand=%p costume=%u old=%p new=%p",
                    self, costume, pawn, newPawn);

    // Same hide the original applies to new player 0 at 0x15EB1B8. It is the
    // half that makes the timeline's set a transition: the recompute 0x1F12E50
    // notifies the owner only when +0x1E9 changes, and the possess-time
    if (kPatch0249HenshinGateParity && newPawn != nullptr) {
        void* p2 = ResolveNativeP2(self, "sequencer-end hide");
        if (p2 == nullptr) return;
        if (p2 != newPawn && sGateLines++ < MaxGateLines)
            Logging.Log("[PShowCoop] PATCH-0249 sequencer-end native P2=%p differs from swap result %p", p2,
                        newPawn);
        SetP2Gate(p2, false, "sequencer-end hide");
    }
}

HOOK_DEFINE_TRAMPOLINE(HenshinCostumeParity) {
    static void Callback(void* self) {
        Orig(self);
        if (sLines < MaxLines) {
            sLines++;
            Logging.Log("[PShowCoop] PATCH-0248 sequencer end stand=%p", self);
        }
        if (self == nullptr) return;
        SwapP2(self);
    }
};

HOOK_DEFINE_TRAMPOLINE(HenshinGateParity) {
    static void Callback(void* self) {
        const bool latchedBefore = self != nullptr && Read<std::uint8_t>(self, UnhideLatchOffset) != 0;
        Orig(self);
        if (self == nullptr || latchedBefore || Read<std::uint8_t>(self, UnhideLatchOffset) == 0) return;
        // The tick set its latch this call: it has just unhidden player 0.
        if (sGateLines++ < MaxGateLines)
            Logging.Log("[PShowCoop] PATCH-0249 timeline unhide latch stand=%p core P2=%p", self, GetP2Pawn());
        void* p2 = ResolveNativeP2(self, "timeline unhide");
        if (p2 != nullptr) SetP2Gate(p2, true, "timeline unhide");
    }
};

} // namespace

void InstallCostume() {
    if (!kPatch0248HenshinCostumeParity) return;
    HenshinCostumeParity::InstallAtOffset(OnPrincessSequencerEnd);
    Logging.Log("[PShowCoop] PATCH-0248 installed OnPrincessSequencerEnd=0x%lx p2_index=%d",
                (unsigned long)OnPrincessSequencerEnd, P2PlayerIndex);
    if (!kPatch0249HenshinGateParity) return;
    HenshinGateParity::InstallAtOffset(TimelineTick);
    Logging.Log("[PShowCoop] PATCH-0249 installed TimelineTick=0x%lx GateUpdate=0x%lx",
                (unsigned long)TimelineTick, (unsigned long)GateUpdateFn);
}

} // namespace pshowcoop::patches
