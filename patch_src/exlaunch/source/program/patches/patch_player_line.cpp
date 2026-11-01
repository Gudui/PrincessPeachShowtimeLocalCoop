#include "patch_player_line.hpp"
#include "patch_core.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// decision 2026-10-22: "none of them" walk in a fixed lane, it causes
// blocking). Fencer_A's walk-round puts every pawn its section move carries on
// MapPath_Spawned5: 0x1A52DB0 calls MapPathActorComponent's set-path
// 0x1DCF70C through 0x1DCF900, and each tick 0x1A50314 snaps a pawn whose
// component rides a path (0x1DCE088: weak path +0xC8 valid and the path's
// isActivePath_ +0x280) back onto it through 0x1A52DB0 (census ring-both,
// isPlayerLine_ (+0x282, getter 0x1DD8C3C) set, isCircleMovePath_ and
// isMovingConstrainPath_ clear.
// After the unchanged set-path attaches a player pawn (component owner +0xA0
// is P1's or P2's pawn) to a path whose isPlayerLine_ getter is true, the same
// native set-path runs again with no path: the game's own detach, which clears
// the ride and tells the component's listeners. Paths without isPlayerLine_
// (rails, ladders, gimmicks) and non-player owners are left alone.
// Entry 0x1DCF70C: str d8,[sp,#-0x40]! / stp x29,x30,[sp,#8] / add x29 /
// str x23 / stp x22,x21 (no PC-relative instruction in the moved 20 bytes);
// arguments x0 component, x1 path, w2 check-active, s0 distance along path.
// ---------------------------------------------------------------------------
constexpr bool kPatch0268PlayerLineRelease = true;

constexpr ptrdiff_t SetPathFn = 0x1DCF70C;
constexpr ptrdiff_t IsPlayerLineFn = 0x1DD8C3C;
constexpr std::uintptr_t ComponentOwner = 0xA0;
constexpr std::uint32_t MaxReleaseLines = 24;

using IsPlayerLineFnT = bool (*)(void* path);

static std::uint32_t sReleases = 0;

template <typename T>
T At(ptrdiff_t offset) {
    return reinterpret_cast<T>(exl::util::GetMainModuleInfo().m_Total.m_Start + offset);
}

int PlayerOf(void* component) {
    void* owner = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(component) + ComponentOwner);
    if (owner == nullptr) return 0;
    if (owner == GetP1Pawn()) return 1;
    if (owner == GetP2Pawn()) return 2;
    return 0;
}

HOOK_DEFINE_TRAMPOLINE(SetPath) {
    static void Callback(void* component, void* path, bool checkActive, float distance) {
        Orig(component, path, checkActive, distance);
        if (!kPatch0268PlayerLineRelease || component == nullptr || path == nullptr) return;
        const int who = PlayerOf(component);
        if (who == 0 || !At<IsPlayerLineFnT>(IsPlayerLineFn)(path)) return;
        Orig(component, nullptr, false, distance);
        sReleases++;
        if (sReleases <= MaxReleaseLines) {
            const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) -
                            exl::util::GetMainModuleInfo().m_Total.m_Start;
            Logging.Log("[PShowCoop] PATCH-0268 released P%d from player line path=%p dist=%.0f raNSO=0x%lx "
                        "releases=%u",
                        who, path, distance, static_cast<unsigned long>(ra), sReleases);
        }
    }
};

} // namespace

void InstallPlayerLine() {
    if (!kPatch0268PlayerLineRelease) return;
    SetPath::InstallAtOffset(SetPathFn);
    Logging.Log("[PShowCoop] PATCH-0268 installed set_path=0x%lx is_player_line=0x%lx owner=+0x%lx",
                static_cast<unsigned long>(SetPathFn), static_cast<unsigned long>(IsPlayerLineFn),
                static_cast<unsigned long>(ComponentOwner));
}

} // namespace pshowcoop::patches
