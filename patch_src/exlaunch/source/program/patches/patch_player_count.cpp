#include "patch_player_count.hpp"
#include "lib.hpp"
#include "lib/patch/code_patcher.hpp"
#include "program/loggers.hpp"
#include <cstdint>

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// PATCH-0250: native player count follows the live player characters
// The game's own per-player loops (enemy target finders, event-flow commands,
// the input-gate loop 0x138A0B4; 103 call sites) run i < 0x1E481D8() over
// GetPlayerCharacter(ctx, i), and 0x1E481D8 is `mov w0,#1 / ret`. This
// rewrites that one instruction in place between `mov w0,#1` and `mov w0,#2`
// so the loops reach index 1 exactly while both indices have a character.
// 20 of the 113 index-getter calls dereference the character unchecked
// characters: it is recomputed from the native GetPlayerCharacter after every
// controller SetPawn (possess, unpossess, destroy, costume swap).
// The function is 8 bytes and exlaunch sits beyond B range, so it cannot be
// hooked (a hook writes 16-20 bytes over 0x1E481E0): the instruction itself
// is the only safe edit point. The patcher writes through exlaunch's RW alias
// and invalidates the instruction cache; each switch calls the function back
// and logs what it returns, so a stale JIT translation shows in the log.
// ---------------------------------------------------------------------------
// freeze is not count-bounded, and Eden's IC IVAU refreshes only the patching
constexpr bool kPatch0250PlayerCount = false;

// FUNC-PLAYER-COUNT-CONST, body 0x1E481D8-0x1E481DF (mov w0,#0x1 / ret).
constexpr ptrdiff_t PlayerCountFn = 0x1E481D8;
// APlayerController::SetPawn (FUNC-PC-SETPAWN), vtable slot 0x6C0, entry
// prologue sub sp,#0x30 / stp x29,x30,[sp,#0x10] / add x29 / stp x20,x19;
// this in x0, pawn in x1; body 0x4803B5C-0x4803C17.
constexpr ptrdiff_t SetPawnFn = 0x4803B5C;
// UGameplayStatics::GetPlayerCharacter(ctx, int32 index), the call the loops
constexpr ptrdiff_t GetPlayerCharacterFn = 0x44FB574;
constexpr std::uint32_t MaxLines = 32;

using CountFnT = int (*)();
using GetPlayerCharacterFnT = void* (*)(void*, int);

static std::uint32_t sLines = 0;

static std::uintptr_t MainStart() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

static std::uint32_t Encoding(std::uint16_t count) {
    return exl::armv8::inst::Movz(exl::armv8::reg::W0, count).Value();
}

static std::uint32_t CurrentWord() {
    return *reinterpret_cast<const volatile std::uint32_t*>(MainStart() + PlayerCountFn);
}

// Both indices the loops will visit must resolve to a character.
static std::uint16_t LiveCount(void* ctx) {
    auto get = reinterpret_cast<GetPlayerCharacterFnT>(MainStart() + GetPlayerCharacterFn);
    if (get(ctx, 0) == nullptr) return 1;
    return get(ctx, 1) != nullptr ? 2 : 1;
}

static void ApplyCount(std::uint16_t count, void* controller, void* pawn) {
    const std::uint32_t word = CurrentWord();
    const std::uint32_t want = Encoding(count);
    if (word == want) return;
    if (word != Encoding(1) && word != Encoding(2)) {
        if (sLines++ < MaxLines)
            Logging.Log("[PShowCoop] PATCH-0250 unexpected instruction 0x%08x at 0x%lx; not patched", word,
                        static_cast<unsigned long>(PlayerCountFn));
        return;
    }
    {
        exl::patch::CodePatcher patcher(PlayerCountFn);
        patcher.WriteInst(exl::armv8::inst::Movz(exl::armv8::reg::W0, count));
    }
    const int seen = reinterpret_cast<CountFnT>(MainStart() + PlayerCountFn)();
    if (sLines++ < MaxLines)
        Logging.Log("[PShowCoop] PATCH-0250 count %d -> %d (pc=%p pawn=%p) word=0x%08x call=%d", word == Encoding(2) ? 2 : 1,
                    count, controller, pawn, CurrentWord(), seen);
}

HOOK_DEFINE_TRAMPOLINE(PlayerCountOnSetPawn) {
    static void Callback(void* controller, void* pawn) {
        Orig(controller, pawn);
        if (controller == nullptr) return;
        ApplyCount(LiveCount(controller), controller, pawn);
    }
};

} // namespace

void InstallPlayerCount() {
    if (!kPatch0250PlayerCount) return;
    const std::uint32_t word = CurrentWord();
    if (word != Encoding(1)) {
        Logging.Log("[PShowCoop] PATCH-0250 not installed: 0x%lx holds 0x%08x, expected 0x%08x",
                    static_cast<unsigned long>(PlayerCountFn), word, Encoding(1));
        return;
    }
    PlayerCountOnSetPawn::InstallAtOffset(SetPawnFn);
    Logging.Log("[PShowCoop] PATCH-0250 installed SetPawn=0x%lx count=0x%lx word=0x%08x",
                static_cast<unsigned long>(SetPawnFn), static_cast<unsigned long>(PlayerCountFn), word);
}

} // namespace pshowcoop::patches
