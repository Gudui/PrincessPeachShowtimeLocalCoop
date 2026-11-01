#include "pointer_guard.hpp"
#include "patch_input_parity.hpp"
#include "patch_event_parity.hpp"
#include "patch_dev_teleport.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// The PlayerCore gate allows input only while its reason mask is 0xFFFFFFFF
// (FIELD-PLAYERCORE-03E0). Scene flows change two reasons on "the player"
// The theater's WMEventManagerBase caches GetPlayerPawn(this, 0) and closes
// flows loop i < 0x1E481D8() (= 1) over GetPlayerCharacter. So P2's gate is
// never opened natively (mask 0xFFFFFFF7) and never closed for a talk.
//  - Site parity: after the unchanged helper 0x2041C2C, the same two pawn
//    calls for GetPlayerPawn(this, 1), in the same order, under the helper's
//    own guard (its weak pawn +0x3B8 is valid).
//  - Drain parity: on each P2 input drain, when P2's bit 3 or bit 1 differs
//    from P1's, P2's own native setter (+0xA98 for bit 3, then +0x988 for
//    bit 1, the course-start lock's order) with P1's value. This replaces
//    PATCH-0010's pawn-byte forcing, which undid every native disable
//    lacks site parity.
// No field writes: every change goes through P2's own pawn virtuals and the
// native recompute 0x1A747B4 that they reach.
// ---------------------------------------------------------------------------
constexpr bool kPatch0251InputGateParity = true;

// ---------------------------------------------------------------------------
// The manager's state-5 handler brackets the helper with PlayerCore calls on
// player 0: talk start 0x1D3F04C(&weak core) at 0x203F430, just before
// 0x2041C2C(this, 0) (core 0x1A7A8B0: +0x332 = 1 and a named state event);
// talk end 0x1D3EF58(&weak core, 1) at 0x203F89C, just after
// 0x2041C2C(this, 1) (core flags +0x3E8 bit 0x4). P2 gets the same two calls
// on its own PlayerCore, after the gate calls of PATCH-0251.
// ---------------------------------------------------------------------------
constexpr bool kPatch0252TalkStateParity = true;
// 0x1D30E78(pawn): weak pointer to pawn +0x7D0 (PlayerCore), returned by value.
constexpr ptrdiff_t MakeCoreWeakFn = 0x1D30E78;
constexpr ptrdiff_t CoreTalkStartFn = 0x1D3F04C;
constexpr ptrdiff_t CoreTalkEndFlagFn = 0x1D3EF58;

// WMEventManagerBase input helper (this, bool on): IsValid(this+0x3B8), then
// pawn->vslot+0x988(on) and pawn->vslot+0xA38(!on); returns 1. Entry
// stp x29,x30,[sp,#-0x20]! / stp x20,x19,[sp,#0x10] / mov x29,sp /
// add x20,x0,#0x3b8 / mov w19,w1; body 0x64 bytes; callers 0x203F43C (off)
constexpr ptrdiff_t ManagerInputHelper = 0x2041C2C;
constexpr std::uintptr_t ManagerWeakPawn = 0x3B8;
// FWeakObjectPtr IsValid(ptr), as at 0x2041C44 (reads only x0).
constexpr ptrdiff_t WeakIsValidFn = 0x2A73028;
// UGameplayStatics natives (ctx, int32 index): GetPlayerPawn is the manager's
// getter (0x203DA74, index 0), GetPlayerCharacter the course loops' getter.
constexpr ptrdiff_t GetPlayerPawnFn = 0x44FB668;
constexpr ptrdiff_t GetPlayerCharacterFn = 0x44FB574;
// Player pawn class getter, the cast in the manager tick (0x203DA80) and the
// course-start lock (FUNC-PLAYERS-GATE-LOOP).
constexpr ptrdiff_t PawnClassFn = 0x2514FB8;
// Pawn vslots (vtable 0x95175C0): +0x988 -> 0x1D2F48C gate bit 1;
// +0xA38 -> 0x1D29998 PlayerCore +0x24F flag (0x1A7D56C);
// +0xA98 -> 0x1D32168 pawn +0x7A0 and gate bit 3 (FUNC-PEACHPRINCESS-VSLOT-A98).
constexpr std::uintptr_t PawnGateBit1Slot = 0x988;
constexpr std::uintptr_t PawnTalkFlagSlot = 0xA38;
constexpr std::uintptr_t PawnGateBit3Slot = 0xA98;
constexpr std::uintptr_t PawnCore = 0x7D0;
constexpr std::uintptr_t CoreMask = 0x3E0;
// Player label, as PATCH-0010: pawn +0x258 controller -> ResolveLocalPlayer
// 0x47FD218 -> ULocalPlayer +0xB8.
constexpr std::uintptr_t PawnController = 0x258;
constexpr ptrdiff_t PlayerControllerClassFn = 0x4C9CEE4;
constexpr ptrdiff_t ResolveLocalPlayerFn = 0x47FD218;
constexpr std::uintptr_t LocalPlayerId = 0xB8;
// UStruct class-tree fields the native IsA reads (0x1A7483C..0x1A7484C).
constexpr std::uintptr_t ObjectClassOffset = 0x10;
constexpr std::uintptr_t ClassTreeArrayOffset = 0x30;
constexpr std::uintptr_t ClassTreeDepthOffset = 0x38;

constexpr int P1PlayerIndex = 0;
constexpr int P2PlayerIndex = 1;
constexpr std::uint32_t MaxSiteLines = 32;
constexpr std::uint32_t MaxDrainLines = 64;

using WeakIsValidFnT = bool (*)(const void*);
using GetPlayerFnT = void* (*)(void*, int);
using ClassGetterFn = void* (*)();
using ResolveLpFn = void* (*)(void*);
using PawnSetBoolFn = void (*)(void*, bool);
using MakeCoreWeakFnT = std::uint64_t (*)(void*);
using CoreWeakFn = void (*)(std::uint64_t*);
using CoreWeakBoolFn = void (*)(std::uint64_t*, bool);

static std::uint32_t sSiteLines = 0;
static std::uint32_t sDrainLines = 0;
static std::uint32_t sDrainCorrections = 0;

inline bool IsHeapish(const void* value) {
    return pshowcoop::pointerguard::Readable(value);
}

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

template <typename Fn>
static Fn Slot(void* object, std::uintptr_t slot) {
    void* vtable = *reinterpret_cast<void**>(object);
    if (!InMain(vtable)) return nullptr;
    void* target = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(vtable) + slot);
    if (!InMain(target)) return nullptr;
    return reinterpret_cast<Fn>(target);
}

static bool IsA(void* object, void* cls) {
    void* objClass = Read<void*>(object, ObjectClassOffset);
    if (objClass == nullptr || cls == nullptr) return false;
    const std::int32_t depth = Read<std::int32_t>(cls, ClassTreeDepthOffset);
    if (depth > Read<std::int32_t>(objClass, ClassTreeDepthOffset)) return false;
    void** tree = Read<void**>(objClass, ClassTreeArrayOffset);
    if (tree == nullptr) return false;
    return tree[depth] == static_cast<std::uint8_t*>(cls) + ClassTreeArrayOffset;
}

static bool IsPlayerPawn(void* pawn) {
    return IsHeapish(pawn) && IsA(pawn, At<ClassGetterFn>(PawnClassFn)());
}

// 0 = P1, 1 = P2, -1 = no local player's controller. Same label as PATCH-0010.
static int WhoOfPawn(void* pawn) {
    void* controller = Read<void*>(pawn, PawnController);
    if (!IsHeapish(controller) || !IsA(controller, At<ClassGetterFn>(PlayerControllerClassFn)())) return -1;
    void* lp = At<ResolveLpFn>(ResolveLocalPlayerFn)(controller);
    if (!IsHeapish(lp)) return -1;
    const std::int32_t id = Read<std::int32_t>(lp, LocalPlayerId);
    return (id == 0 || id == 1) ? id : -1;
}

// PlayerCore reason mask of a player pawn; false when the core is missing.
static bool CoreMaskOf(void* pawn, std::uint32_t* mask) {
    void* core = Read<void*>(pawn, PawnCore);
    if (!IsHeapish(core)) return false;
    *mask = Read<std::uint32_t>(core, CoreMask);
    return true;
}

HOOK_DEFINE_TRAMPOLINE(ManagerInputParity) {
    static std::uint32_t Callback(void* manager, std::uint32_t on) {
        const std::uint32_t result = Orig(manager, on);
        if (!IsHeapish(manager)) return result;
        // The helper's own guard: it acts only while its cached pawn is valid.
        if (!At<WeakIsValidFnT>(WeakIsValidFn)(static_cast<std::uint8_t*>(manager) + ManagerWeakPawn)) return result;

        void* pawn = At<GetPlayerFnT>(GetPlayerPawnFn)(manager, P2PlayerIndex);
        if (!IsPlayerPawn(pawn) || WhoOfPawn(pawn) != P2PlayerIndex) {
            if (sSiteLines < MaxSiteLines) {
                sSiteLines++;
                Logging.Log("[PShowCoop] PATCH-0251 site skip manager=%p on=%u pawn=%p", manager, on & 1, pawn);
            }
            return result;
        }
        auto setBit1 = Slot<PawnSetBoolFn>(pawn, PawnGateBit1Slot);
        auto setTalk = Slot<PawnSetBoolFn>(pawn, PawnTalkFlagSlot);
        if (setBit1 == nullptr || setTalk == nullptr) {
            if (sSiteLines < MaxSiteLines) {
                sSiteLines++;
                Logging.Log("[PShowCoop] PATCH-0251 site skip pawn=%p slots %p/%p", pawn, setBit1, setTalk);
            }
            return result;
        }
        std::uint32_t before = 0;
        const bool haveBefore = CoreMaskOf(pawn, &before);
        setBit1(pawn, (on & 1) != 0);
        setTalk(pawn, (on & 1) == 0);
        // PATCH-0252: the talk-state call that brackets the helper for player 0.
        // The wrappers dereference the core unchecked, so require it first.
        const bool talkState = kPatch0252TalkStateParity && IsHeapish(Read<void*>(pawn, PawnCore));
        if (talkState) {
            std::uint64_t weakCore = At<MakeCoreWeakFnT>(MakeCoreWeakFn)(pawn);
            if ((on & 1) == 0) {
                At<CoreWeakFn>(CoreTalkStartFn)(&weakCore);
            } else {
                At<CoreWeakBoolFn>(CoreTalkEndFlagFn)(&weakCore, true);
            }
        }
        std::uint32_t after = 0;
        const bool haveAfter = CoreMaskOf(pawn, &after);
        if (sSiteLines < MaxSiteLines) {
            sSiteLines++;
            Logging.Log("[PShowCoop] PATCH-0251 site manager=%p on=%u P2 pawn=%p mask 0x%x->0x%x PATCH-0252 talk=%s",
                        manager, on & 1, pawn, haveBefore ? before : 0, haveAfter ? after : 0,
                        !talkState ? "skip" : ((on & 1) == 0 ? "start" : "end"));
        }
        return result;
    }
};

} // namespace

void InputGateParityOnP2Drain(void* p2Pawn) {
    if (!kPatch0251InputGateParity) return;
    if (!IsPlayerPawn(p2Pawn)) return;
    void* p1Pawn = At<GetPlayerFnT>(GetPlayerCharacterFn)(p2Pawn, P1PlayerIndex);
    if (p1Pawn == p2Pawn || !IsPlayerPawn(p1Pawn) || WhoOfPawn(p1Pawn) != P1PlayerIndex) return;
    EventParityFrame(p1Pawn, p2Pawn);
    DevTeleportFrame(p1Pawn, p2Pawn);

    // Bit 3 first, then bit 1: the course-start lock's order (+0xA98, +0x988).
    static constexpr std::uint32_t kBits[2] = {3, 1};
    for (std::uint32_t bit : kBits) {
        std::uint32_t m1 = 0;
        std::uint32_t m2 = 0;
        if (!CoreMaskOf(p1Pawn, &m1) || !CoreMaskOf(p2Pawn, &m2)) return;
        if ((((m1 ^ m2) >> bit) & 1) == 0) continue;
        const bool value = ((m1 >> bit) & 1) != 0;
        auto set = Slot<PawnSetBoolFn>(p2Pawn, bit == 3 ? PawnGateBit3Slot : PawnGateBit1Slot);
        if (set == nullptr) return;
        set(p2Pawn, value);
        sDrainCorrections++;
        if (sDrainLines < MaxDrainLines) {
            sDrainLines++;
            std::uint32_t after = 0;
            CoreMaskOf(p2Pawn, &after);
            Logging.Log("[PShowCoop] PATCH-0251 drain #%u bit=%u v=%d P1 mask=0x%x P2 mask 0x%x->0x%x p1=%p p2=%p",
                        sDrainCorrections, bit, value ? 1 : 0, m1, m2, after, p1Pawn, p2Pawn);
        }
    }
}

void InstallInputParity() {
    if (!kPatch0251InputGateParity) return;
    ManagerInputParity::InstallAtOffset(ManagerInputHelper);
    Logging.Log("[PShowCoop] PATCH-0251 installed ManagerInputHelper=0x%lx p2_index=%d",
                static_cast<unsigned long>(ManagerInputHelper), P2PlayerIndex);
}

} // namespace pshowcoop::patches
