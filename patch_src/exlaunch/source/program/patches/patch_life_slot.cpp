#include "pointer_guard.hpp"
#include "patch_life_slot.hpp"
#include "patch_hud.hpp"
#include "patch_core.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>
#include <cstring>

// PATCH-0232 - Player 2 native life slot.
//
//   * the status-component ctor 0x01A87AC8 stores player index 1 at comp+0x48;
//   * course start 0x0138E508 sizes CourseGameStateBase+0x490 to one element;
//
// This patch adds the second element with the title's own TArray growth path
// and points P2's component at it. Everything that READS or WRITES health stays
// native: GetPlayerHealth, SetPlayerHealth, the 0..5 clamp, the life-decreased
// broadcast and every status-component method that passes comp+0x48 (16 sites,
// ghidra_out/route_a_proof_utf8.txt). Title code that addresses player 1 by a
// literal (8 sites, incl. the HUD region 0x009A5960/0x009A6800) keeps reading
// P1's slot.

namespace pshowcoop::patches {

namespace {

// Kill switch. false = previous behaviour: one native slot, P2 served by the
// comp+0x58 shadow in patch_damage.cpp.
constexpr bool kPatch0232P2LifeSlot = true;
// PATCH-0238 kill switch. false = HUD pointers reset only on the first travel.
constexpr bool kPatch0238ResetHudOnCourseInit = true;
// PATCH-0239 kill switch. false = pawns and P2 controller survive a restart.
constexpr bool kPatch0239ResetCoreOnCourseInit = true;
// Course-start life-array initialiser. Entry: sub sp,sp,#0xb0 ; stp x29,x30,[sp,#0x50].
constexpr ptrdiff_t CourseLifeInitOffset = 0x0138E508;
// comp -> GetWorld()->GameState if it IsA CourseGameStateBase, else null.
constexpr ptrdiff_t GetCourseGameStateOffset = 0x01377800;
// (gs, uint8 idx) -> life of slot idx-1; 0 when idx == 0 or idx > Num.
constexpr ptrdiff_t GetPlayerLifeOffset = 0x0138F1EC;
// TArray<16-byte element>::ResizeGrow(OldNum). Sizes from the already
// incremented Num; 0x0138E508 calls it at 0x0138E84C.
constexpr ptrdiff_t LifeArrayResizeGrowOffset = 0x01398D14;

// ACourseGameStateBase (FIELD-COURSEGAMESTATEBASE-0490 / -0498).
constexpr std::uintptr_t kLifeArrayData = 0x490;
constexpr std::uintptr_t kLifeArrayNum = 0x498;
constexpr std::uintptr_t kLifeArrayMax = 0x49C;
constexpr std::uintptr_t kLifeSlotStride = 0x10;
// Status component 1-based player index byte (FIELD-UPLAYERSTATUSCOMPONENT-0048).
constexpr std::uintptr_t kStatusPlayerIndex = 0x48;

constexpr std::int32_t kSinglePlayerSlots = 1;
constexpr std::uint8_t kP1Index = 1;
constexpr std::uint8_t kP2Index = 2;

using GetCourseGameStateFn = void* (*)(void* comp);
using GetPlayerLifeFn = std::int32_t (*)(void* gs, std::uint32_t idx);
using ResizeGrowFn = void (*)(void* array, std::int32_t oldNum);

inline bool IsHeapish(const void* value) {
    return pshowcoop::pointerguard::Readable(value);
}

template <typename Fn>
Fn Native(ptrdiff_t offset) {
    return reinterpret_cast<Fn>(exl::util::GetMainModuleInfo().m_Total.m_Start + offset);
}

inline std::int32_t* Field32(void* object, std::uintptr_t disp) {
    return reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(object) + disp);
}

inline std::uint8_t** LifeData(void* gs) {
    return reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uintptr_t>(gs) + kLifeArrayData);
}

HOOK_DEFINE_TRAMPOLINE(CaptureCourseLifeInit) {
    static void Callback(void* gs, void* params, void* a2, void* a3,
                         void* a4, void* a5, void* a6, void* a7) {
        // PATCH-0238: every course start (first load AND restart after a death)
        // drops the mod's HUD pointers from the previous course. The travel
        // reset in patch_warp.cpp fires only on the first travel, so a restart
        // gauge spawn, so the new course's gauge is spawned afresh.
        if (kPatch0238ResetHudOnCourseInit) {
            Logging.Log("[PShowCoop] PATCH-0238 course init gs=%p: resetting HUD state", gs);
            ResetHudForLevelTransition();
        }
        if (kPatch0239ResetCoreOnCourseInit) {
            ResetCoreForLevelTransition();
        }
        Orig(gs, params, a2, a3, a4, a5, a6, a7);

        if (!IsHeapish(gs)) {
            Logging.Log("[PShowCoop] PATCH-0232 course init: gs=%p not readable; no second life slot", gs);
            return;
        }
        std::int32_t* num = Field32(gs, kLifeArrayNum);
        std::int32_t* max = Field32(gs, kLifeArrayMax);
        std::uint8_t** data = LifeData(gs);
        if (*num != kSinglePlayerSlots || !IsHeapish(*data)) {
            Logging.Log("[PShowCoop] PATCH-0232 course init: gs=%p unexpected num=%d max=%d data=%p; no second life slot",
                        gs, *num, *max, *data);
            return;
        }

        // TArray::AddUninitialized(1), the same sequence 0x0138E508 inlines at
        // 0x0138E82C-0x0138E84C: bump Num, then ResizeGrow(OldNum) if it no
        // longer fits.
        const std::int32_t oldNum = *num;
        *num = oldNum + 1;
        if (oldNum >= *max) {
            Native<ResizeGrowFn>(LifeArrayResizeGrowOffset)(data, oldNum);
        }
        if (!IsHeapish(*data) || *max <= oldNum) {
            *num = oldNum;
            Logging.Log("[PShowCoop] PATCH-0232 course init: gs=%p grow failed (data=%p max=%d); stays single-slot",
                        gs, *data, *max);
            return;
        }

        // The new element takes the initialiser's output for element 0. The
        // native fill loop body is index-invariant: each word comes from the
        // course params, a constant, or a per-GameState lookup, so this is what
        // the loop itself writes for a second element.
        std::memcpy(*data + oldNum * kLifeSlotStride, *data, kLifeSlotStride);

        auto getLife = Native<GetPlayerLifeFn>(GetPlayerLifeOffset);
        Logging.Log("[PShowCoop] PATCH-0232 course init: gs=%p life slots %d -> %d (max=%d) P1=%d P2=%d",
                    gs, oldNum, *num, *max, getLife(gs, kP1Index), getLife(gs, kP2Index));
    }
};

} // namespace

bool LifeSlotPatchEnabled() {
    return kPatch0232P2LifeSlot;
}

int SyncP2LifeIndex(void* comp) {
    if (!kPatch0232P2LifeSlot || !IsHeapish(comp)) return 0;

    void* gs = Native<GetCourseGameStateFn>(GetCourseGameStateOffset)(comp);
    const std::int32_t slots = IsHeapish(gs) ? *Field32(gs, kLifeArrayNum) : 0;
    const std::uint8_t want = slots >= kP2Index ? kP2Index : kP1Index;

    auto* index = reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(comp) + kStatusPlayerIndex);
    if (*index != want) {
        Logging.Log("[PShowCoop] PATCH-0232 P2 comp=%p life index %u -> %u (gs=%p slots=%d)",
                    comp, static_cast<unsigned>(*index), static_cast<unsigned>(want), gs, slots);
        *index = want;
    }
    return want;
}

void LogLifeSlots(const char* tag, void* comp) {
    if (!kPatch0232P2LifeSlot || !IsHeapish(comp)) return;
    void* gs = Native<GetCourseGameStateFn>(GetCourseGameStateOffset)(comp);
    if (!IsHeapish(gs)) {
        Logging.Log("[PShowCoop] PATCH-0232 %s: comp=%p no CourseGameStateBase", tag, comp);
        return;
    }
    auto getLife = Native<GetPlayerLifeFn>(GetPlayerLifeOffset);
    const auto index = *reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(comp) + kStatusPlayerIndex);
    Logging.Log("[PShowCoop] PATCH-0232 %s: comp=%p index=%u slots=%d P1=%d P2=%d",
                tag, comp, static_cast<unsigned>(index), *Field32(gs, kLifeArrayNum),
                getLife(gs, kP1Index), getLife(gs, kP2Index));
}

void InstallLifeSlot() {
    if (!kPatch0232P2LifeSlot) {
        Logging.Log("[PShowCoop] PATCH-0232 disabled (kill switch)");
        return;
    }
    CaptureCourseLifeInit::InstallAtOffset(CourseLifeInitOffset);
    Logging.Log("[PShowCoop] PATCH-0232 installed: CourseLifeInit=0x%lx", static_cast<unsigned long>(CourseLifeInitOffset));
}

} // namespace pshowcoop::patches
