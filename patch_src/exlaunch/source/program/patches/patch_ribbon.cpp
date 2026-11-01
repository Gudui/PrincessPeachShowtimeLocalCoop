#include "patch_ribbon.hpp"
#include "patch_core.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// SetRibbonGot sets the course ribbon flag and, when `apply` is set, gives only
// GetPlayerCharacter(0) the ponytail and SetNoRibbonMode(!got). P2 keeps
// pawn+0xBB5 = 1 and stays in the WaitAttack 'NoRibbon' path (no spark) until a
// respawn re-reads the flag. After the unchanged original, this repeats the
// original's own index-0 sequence for player index 1 through the same native
// routines in the same order. No field writes.
// ---------------------------------------------------------------------------
constexpr bool kPatch0247RibbonGrantParity = true;

// SetRibbonGot(ctx, bool got, bool apply); entry prologue
// stp x29,x30,[sp,#-0x30]! / stp x22,x21 / mov x29,sp / stp x20,x19; only
// caller 0x130B694 (BL) (FUNC-COURSEGAMESTATE-SETRIBBONGOT).
constexpr ptrdiff_t SetRibbonGot = 0x137EB80;
// UGameplayStatics::GetPlayerCharacter(ctx, int32 index); the original reaches
constexpr ptrdiff_t GetPlayerCharacterFn = 0x44FB574;
// Interface class getter and native interface cast, as at 0x137EC0C/0x137EC18.
constexpr ptrdiff_t InterfaceClassFn = 0x247B24C;
constexpr ptrdiff_t InterfaceCastFn = 0x2A376CC;
// Hair-style bracket, as at 0x137EC28..0x137ECA4: subsystem of the game
// instance, save its current value, select the style FName, restore after.
constexpr ptrdiff_t GameInstanceFn = 0x1E3D6FC;
constexpr ptrdiff_t HairSubsystemFn = 0x95F328;
constexpr ptrdiff_t HairSaveFn = 0x966378;
constexpr ptrdiff_t HairSelectFn = 0x9662DC;
constexpr ptrdiff_t HairRestoreFn = 0x9662D4;
constexpr ptrdiff_t HairPonyNameFn = 0x1C57040;      // H_Default_Pony
constexpr ptrdiff_t HairDownNameFn = 0x1C570AC;      // H_DownHair_00
// Interface slots called at 0x137EC74 and 0x137EC88 (VTBL-PEACHPRINCESSINTERFACE-IN-PEACHPRINCESS).
constexpr std::uintptr_t IfaceHairRefreshSlot = 0x88;
constexpr std::uintptr_t IfaceSetNoRibbonModeSlot = 0xB0;
// Pawn primary vtable IsNoRibbon (0x1C54AA4, ldrb +0xBB5) (FUNC-PEACHPRINCESSBASE-ISNORIBBON).
constexpr std::uintptr_t PawnIsNoRibbonSlot = 0xFD0;

constexpr int P2PlayerIndex = 1;
constexpr std::uint32_t MaxLines = 16;

using SetRibbonGotFn = void (*)(void*, bool, bool);
using GetPlayerCharacterFnT = void* (*)(void*, int);
using ClassGetterFn = void* (*)();
using InterfaceCastFnT = void* (*)(void*, void*);
using PtrToPtrFn = void* (*)(void*);
using HairSaveFnT = std::uint32_t (*)(void*);
using HairSelectFnT = void (*)(void*, std::uint64_t);
using HairRestoreFnT = void (*)(void*, std::uint32_t);
using NameGetterFn = std::uint64_t (*)();
using IfaceVoidFn = void (*)(void*);
using IfaceSetBoolFn = void (*)(void*, bool);
using PawnBoolFn = bool (*)(void*);

static std::uint32_t sLines = 0;

static bool InMain(const void* p) {
    return exl::util::GetMainModuleInfo().m_Total.InRange(reinterpret_cast<std::uintptr_t>(p));
}

template <typename Fn>
static Fn At(ptrdiff_t offset) {
    return reinterpret_cast<Fn>(exl::util::GetMainModuleInfo().m_Total.m_Start + offset);
}

template <typename Fn>
static Fn Slot(void* object, std::uintptr_t slot) {
    void* vtable = *reinterpret_cast<void**>(object);
    if (!InMain(vtable)) return nullptr;
    void* target = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(vtable) + slot);
    if (!InMain(target)) return nullptr;
    return reinterpret_cast<Fn>(target);
}

static void GrantToP2(void* ctx, bool got) {
    void* pawn = At<GetPlayerCharacterFnT>(GetPlayerCharacterFn)(ctx, P2PlayerIndex);
    void* coreP2 = GetP2Pawn();
    if (pawn == nullptr || pawn != coreP2) {
        if (sLines++ < MaxLines)
            Logging.Log("[PShowCoop] PATCH-0247 skip pawn native=%p core=%p got=%d", pawn, coreP2, got ? 1 : 0);
        return;
    }

    auto isNoRibbon = Slot<PawnBoolFn>(pawn, PawnIsNoRibbonSlot);
    if (isNoRibbon == nullptr) {
        if (sLines++ < MaxLines) Logging.Log("[PShowCoop] PATCH-0247 skip pawn=%p no IsNoRibbon slot", pawn);
        return;
    }
    const bool before = isNoRibbon(pawn);
    if (before == !got) {
        if (sLines++ < MaxLines)
            Logging.Log("[PShowCoop] PATCH-0247 already pawn=%p no_ribbon=%d got=%d", pawn, before ? 1 : 0, got ? 1 : 0);
        return;
    }

    void* iface = At<InterfaceCastFnT>(InterfaceCastFn)(pawn, At<ClassGetterFn>(InterfaceClassFn)());
    if (iface == nullptr) {
        if (sLines++ < MaxLines) Logging.Log("[PShowCoop] PATCH-0247 skip pawn=%p no interface", pawn);
        return;
    }
    auto hairRefresh = Slot<IfaceVoidFn>(iface, IfaceHairRefreshSlot);
    auto setNoRibbonMode = Slot<IfaceSetBoolFn>(iface, IfaceSetNoRibbonModeSlot);
    void* subsystem = At<PtrToPtrFn>(HairSubsystemFn)(At<PtrToPtrFn>(GameInstanceFn)(ctx));
    if (hairRefresh == nullptr || setNoRibbonMode == nullptr || subsystem == nullptr) {
        if (sLines++ < MaxLines)
            Logging.Log("[PShowCoop] PATCH-0247 skip pawn=%p iface=%p refresh=%p mode=%p subsystem=%p",
                        pawn, iface, hairRefresh, setNoRibbonMode, subsystem);
        return;
    }

    const std::uint32_t saved = At<HairSaveFnT>(HairSaveFn)(subsystem);
    const std::uint64_t style = At<NameGetterFn>(got ? HairPonyNameFn : HairDownNameFn)();
    At<HairSelectFnT>(HairSelectFn)(subsystem, style);
    hairRefresh(iface);
    setNoRibbonMode(iface, !got);
    At<HairRestoreFnT>(HairRestoreFn)(subsystem, saved);

    const bool after = isNoRibbon(pawn);
    if (sLines++ < MaxLines)
        Logging.Log("[PShowCoop] PATCH-0247 applied pawn=%p iface=%p got=%d no_ribbon %d->%d",
                    pawn, iface, got ? 1 : 0, before ? 1 : 0, after ? 1 : 0);
}

HOOK_DEFINE_TRAMPOLINE(RibbonGrantParity) {
    static void Callback(void* ctx, bool got, bool apply) {
        Orig(ctx, got, apply);
        if (sLines < MaxLines) {
            sLines++;
            Logging.Log("[PShowCoop] PATCH-0247 SetRibbonGot ctx=%p got=%d apply=%d", ctx, got ? 1 : 0, apply ? 1 : 0);
        }
        if (!apply || ctx == nullptr) return;
        GrantToP2(ctx, got);
    }
};

} // namespace

void InstallRibbon() {
    if (!kPatch0247RibbonGrantParity) return;
    RibbonGrantParity::InstallAtOffset(SetRibbonGot);
    Logging.Log("[PShowCoop] PATCH-0247 installed SetRibbonGot=0x%lx p2_index=%d",
                (unsigned long)SetRibbonGot, P2PlayerIndex);
}

} // namespace pshowcoop::patches
