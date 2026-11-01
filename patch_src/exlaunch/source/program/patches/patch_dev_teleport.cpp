#include "patch_dev_teleport.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cstdint>
// Layout per the SDK: sampling number, button set, two sticks, attributes.
namespace nn::hid {
struct AnalogStickState {
    std::int32_t x;
    std::int32_t y;
};
struct NpadFullKeyState {
    std::int64_t samplingNumber;
    std::uint64_t buttons;
    AnalogStickState analogStickL;
    AnalogStickState analogStickR;
    std::uint32_t attributes;
    std::uint32_t reserved;
};
struct NpadJoyDualState : NpadFullKeyState {};
struct NpadHandheldState : NpadFullKeyState {};
void GetNpadState(NpadFullKeyState* out, const std::uint32_t& id);
void GetNpadState(NpadJoyDualState* out, const std::uint32_t& id);
void GetNpadState(NpadHandheldState* out, const std::uint32_t& id);
} // namespace nn::hid

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// ramp for the paused binding census. Pressing Minus on player 1's pad moves
// P1, then P2 beside it, to the place the
// same native SetActorLocation the leash uses (PATCH-0015, teleport=true).
// It skips that scripted move and the arena, so the section may not be in its
// ---------------------------------------------------------------------------
// Off since 2026-10-22: it skips the section move PATCH-0268 acts on.
constexpr bool kDiag0250RampTeleport = false;

struct FVector3f {
    float X;
    float Y;
    float Z;
};
constexpr ptrdiff_t SetActorLocationFnOffset = 0x41F5578;  // PATCH-0015
using SetActorLocationFn = bool (*)(void* actor, FVector3f newLoc, bool bSweep, void* hitResult, bool bTeleport);
constexpr FVector3f P1Target{145794.0f, -538.0f, 324.0f};
constexpr FVector3f P2Target{145914.0f, -538.0f, 324.0f};

// nn::hid::NpadButton bit 11 = Minus. Npad id 0 = No1, 0x20 = Handheld.
constexpr std::uint64_t TriggerButton = 1ULL << 11;
constexpr std::uint32_t PadNo1 = 0;
constexpr std::uint32_t PadHandheld = 0x20;

static bool sWasDown = false;  // one teleport per press

std::uint64_t P1Buttons() {
    nn::hid::NpadFullKeyState full{};
    nn::hid::NpadJoyDualState dual{};
    nn::hid::NpadHandheldState handheld{};
    nn::hid::GetNpadState(&full, PadNo1);
    nn::hid::GetNpadState(&dual, PadNo1);
    nn::hid::GetNpadState(&handheld, PadHandheld);
    return full.buttons | dual.buttons | handheld.buttons;
}

bool Move(void* pawn, const FVector3f& to) {
    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    auto setLoc = reinterpret_cast<SetActorLocationFn>(mainRange.m_Start + SetActorLocationFnOffset);
    return setLoc(pawn, to, false, nullptr, true);
}

} // namespace

void DevTeleportFrame(void* p1Pawn, void* p2Pawn) {
    if (!kDiag0250RampTeleport) return;
    const bool down = (P1Buttons() & TriggerButton) != 0;
    const bool pressed = down && !sWasDown;
    sWasDown = down;
    if (!pressed) return;
    const bool ok1 = Move(p1Pawn, P1Target);
    const bool ok2 = Move(p2Pawn, P2Target);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC ramp teleport P1=%p ok=%d to (%.0f,%.0f,%.0f) P2=%p ok=%d to (%.0f,%.0f,%.0f)",
                p1Pawn, (int)ok1, P1Target.X, P1Target.Y, P1Target.Z, p2Pawn, (int)ok2, P2Target.X, P2Target.Y,
                P2Target.Z);
}

} // namespace pshowcoop::patches
