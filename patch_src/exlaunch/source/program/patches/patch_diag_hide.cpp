#include "patch_diag_hide.hpp"
#include "patch_core.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// After the swap-only PATCH-0248, P2's new pawn ends up actor-hidden (+0x58 bit
// 0x20) with the stand's ActorCtrl gate clean. The hide setter 0x41F3560 (pawn
// vslot +0x310) writes the bit and tail-branches (b, not bl) into 0x41F35CC, so
// on entry to 0x41F35CC the link register is still the hide setter's caller.
// Logs, for player-class actors only: self, the hidden bit, P1/P2 identity, the
// return address and one frame up. Read-only; the original always runs first.
// Remove after the writer is identified.
// ---------------------------------------------------------------------------
constexpr bool kDiag0241HidePawnWriter = true;

// Hide-state apply, entry prologue stp x29,x30,[sp,#-0x60]! ; str x27,[sp,#0x10] ;
// mov x29,sp (listing 0x41F35CC..0x41F35EC, this session).
constexpr ptrdiff_t HideApplyFn = 0x41F35CC;
// Player pawn class getter, the gate PATCH-0248's original applies (0x15EB0F4).
constexpr ptrdiff_t PawnClassFn = 0x2514FB8;
// AActor bHidden byte/bit written by 0x41F3560.
constexpr std::uintptr_t HiddenOffset = 0x58;
constexpr std::uint32_t HiddenBit = 0x20;
// UStruct class-tree fields the native IsA reads (0x15EB0F8..0x15EB118).
constexpr std::uintptr_t ObjectClassOffset = 0x10;
constexpr std::uintptr_t ClassTreeArrayOffset = 0x30;
constexpr std::uintptr_t ClassTreeDepthOffset = 0x38;
constexpr std::uint32_t MaxLines = 48;

using ClassGetterFn = void* (*)();

static std::uint32_t sLines = 0;

static std::uintptr_t MainStart() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

static bool InMain(std::uintptr_t p) { return exl::util::GetMainModuleInfo().m_Total.InRange(p); }

static std::uintptr_t ToNso(std::uintptr_t p) { return InMain(p) ? p - MainStart() : 0; }

template <typename T>
static T Read(const void* base, std::uintptr_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(base) + offset);
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

HOOK_DEFINE_TRAMPOLINE(HideApply) {
    static void Callback(void* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        // Our saved frame record holds the caller's x29; its +8 is the caller's return.
        const auto* ourFrame = static_cast<const std::uintptr_t*>(__builtin_frame_address(0));
        const std::uintptr_t callerFp = ourFrame != nullptr ? ourFrame[0] : 0;
        std::uintptr_t ra2 = 0;
        if (callerFp != 0 && (callerFp & 0xF) == 0) ra2 = reinterpret_cast<const std::uintptr_t*>(callerFp)[1];

        Orig(self, a1, a2, a3, a4, a5, a6, a7);

        if (sLines >= MaxLines || self == nullptr) return;
        auto getter = reinterpret_cast<ClassGetterFn>(MainStart() + PawnClassFn);
        if (!IsA(self, getter())) return;
        const bool hidden = (Read<std::uint32_t>(self, HiddenOffset) & HiddenBit) != 0;
        const char* who = self == GetP1Pawn() ? "P1" : (self == GetP2Pawn() ? "P2" : "other");
        sLines++;
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC hide-apply pawn=%p who=%s hidden=%d raNSO=0x%lx ra2NSO=0x%lx",
                    self, who, hidden ? 1 : 0, static_cast<unsigned long>(ToNso(ra)),
                    static_cast<unsigned long>(ToNso(ra2)));
    }
};

} // namespace

void InstallDiagHide() {
    if (!kDiag0241HidePawnWriter) return;
    HideApply::InstallAtOffset(HideApplyFn);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC installed HideApply=0x%lx", static_cast<unsigned long>(HideApplyFn));
}

} // namespace pshowcoop::patches
