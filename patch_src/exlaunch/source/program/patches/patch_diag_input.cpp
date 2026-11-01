#include "pointer_guard.hpp"
#include "patch_diag_input.hpp"
#include "diag_controls.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>
#include <cstdio>

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// bit then, so this only has to name P1's mechanism. Four read-only
// trampolines, the original always runs first:
//  - gate: PlayerCore input-gate recompute 0x1A747B4. 0x1A74788(core,bit,set)
//    and the pawn +0x988 thunk tail-branch into it, so LR is the game code that
//    changed the gate. Logs when a player's mask differs from its last line.
//  - cine5/cine3: SetCinematicMode 0x4800514 (5 args) and 0x47FE92C (3 args,
//    the path into SetIgnoreMoveInput). Logs when args or caller change.
//  - watch: the per-player input dispatcher 0x1A8ED14, every tick. Logs a
//    player's input fields when they change, and a dispatch stall when one
//    caller stops dispatching a player while the other player keeps ticking.
//    This covers a freeze that bypasses the three hooks above (DisableInput
//    0x47877BC or SetIgnoreMoveInput 0x442E304 called directly, or a state
//    that stops dispatching P1).
// Every line carries both players' fields: the subject read fresh, the other
// player as of its last dispatch. Remove once the freeze is named.
// ---------------------------------------------------------------------------
constexpr bool kDiag0242InputFreeze = !PSHOW_DIAG0257_PERF_CONTROL;
// Gate recompute: stp x29,x30,[sp,#-0x30]! ; x0 core; w1 bit and w2 set are
// still 0x1A74788's arguments when entered from it.
constexpr ptrdiff_t GateRecomputeFn = 0x1A747B4;
// SetCinematicMode(ctrl, bInCinematic, bHidePlayer, bAffectsHUD,
// bAffectsMovement, bAffectsTurning): stp x29,x30,[sp,#-0x30]!.
constexpr ptrdiff_t SetCinematic5Fn = 0x4800514;
// SetCinematicMode(ctrl, bInCinematic, bAffectsMovement, bAffectsTurning):
// stp x29,x30,[sp,#-0x30]!.
constexpr ptrdiff_t SetCinematic3Fn = 0x47FE92C;
// Input dispatcher(actionInput x0, float frame time v0): sub sp,sp,#0xb0 ...
// stp x29,x30,[sp,#0x50]; the float is used as s8, so a float argument keeps it.
constexpr ptrdiff_t InputDispatcherFn = 0x1A8ED14;
constexpr std::uintptr_t DispatchFromState = 0x1B604A8;  // player state 0x1B5E708
constexpr std::uintptr_t DispatchFromPawn = 0x1D26528;   // pawn method, x19 = pawn

// Natives the recompute itself calls (0x1A747E0, 0x1A74838) and PATCH-0010 uses.
constexpr ptrdiff_t WeakObjectGet = 0x2A7312C;
constexpr ptrdiff_t PlayerControllerClassFn = 0x4C9CEE4;
constexpr ptrdiff_t ResolveLocalPlayerFn = 0x47FD218;  // FUNC-PLAYERCONTROLLER-RESOLVE-LOCALPLAYER
constexpr ptrdiff_t PawnClassFn = 0x2514FB8;
// input-allowed byte, set to (mask == -1) by the recompute via 0x1A917A8
// (strb w1&1,[x0,#0x30]; getter 0x1A917B4).
constexpr std::uintptr_t ActionInputWeakPawn = 0x28;
constexpr std::uintptr_t ActionInputAllowed = 0x30;
// PlayerCore: +0x3E0 u32 reason mask, input allowed when -1 (FIELD-PLAYERCORE-03E0);
// +0x78 actionInput (0x1A634F4: ldr x0,[x0,#0x78]); +0x88 weak pawn (0x1A747DC).
constexpr std::uintptr_t CoreMask = 0x3E0;
constexpr std::uintptr_t CoreActionInput = 0x78;
constexpr std::uintptr_t CoreWeakPawn = 0x88;
constexpr std::uintptr_t PawnInputByte = 0x228;
constexpr std::uint8_t PawnInputBit = 0x10;
constexpr std::uintptr_t PawnController = 0x258;
constexpr std::uintptr_t PawnCore = 0x7D0;
// PlayerController: +0x250 pawn, +0x3E0 cinematic flags (FUNC-PC-SETCINEMATICMODE);
// +0x295/+0x296 ignore move/look counters; +0x4E8 cinema input-disable flags
// (FIELD-PLAYERCONTROLLER-0295/0296/04E8).
constexpr std::uintptr_t ControllerPawn = 0x250;
constexpr std::uintptr_t ControllerIgnoreMove = 0x295;
constexpr std::uintptr_t ControllerIgnoreLook = 0x296;
constexpr std::uintptr_t ControllerCinematic = 0x3E0;
constexpr std::uintptr_t ControllerCinemaDisable = 0x4E8;
// ULocalPlayer +0xB8 player id, the label PATCH-0010 uses.
constexpr std::uintptr_t LocalPlayerId = 0xB8;
// UStruct class-tree fields the native IsA reads (0x1A7483C..0x1A7484C).
constexpr std::uintptr_t ObjectClassOffset = 0x10;
constexpr std::uintptr_t ClassTreeArrayOffset = 0x30;
constexpr std::uintptr_t ClassTreeDepthOffset = 0x38;

// A stream stalls after this many dispatches of the other player (~0.5 s).
constexpr std::uint32_t StallCalls = 30;
constexpr std::uint32_t MaxGateLines = 48;
constexpr std::uint32_t MaxCine5Lines = 24;
constexpr std::uint32_t MaxCine3Lines = 24;
constexpr std::uint32_t MaxWatchLines = 96;
constexpr std::uint32_t MaxStallLines = 40;
constexpr std::uint32_t MaxOtherLines = 8;

using WeakGetFn = void* (*)(void*);
using ClassGetterFn = void* (*)();
using ResolveLpFn = void* (*)(void*);

// Input fields of one player. Pointers are identity tokens for the log.
struct Snap {
    const void* pawn;
    const void* actionInput;
    std::uint32_t mask;
    std::uint8_t input;
    std::uint8_t allowed;
    std::uint8_t move;
    std::uint8_t look;
    std::uint8_t cine;
    std::uint8_t cineDisable;
    std::uint8_t valid;  // 1 pawn, 2 core, 4 actionInput, 8 controller
};

static Snap sLatest[2] = {};       // from the watch, as of each player's last dispatch
static Snap sWatchLogged[2] = {};
static bool sWatchHave[2] = {};
static std::uint32_t sGateMask[2] = {};
static bool sGateHave[2] = {};
static std::uint64_t sCine5Key[3] = {};
static std::uint64_t sCine3Key[3] = {};
// Dispatch streams: player * 3 + caller (state, pawn, other).
static std::uint32_t sCalls[6] = {};
static std::uint32_t sMark[6] = {};
static std::uint32_t sStill[6] = {};
static bool sStalled[6] = {};

static std::uint32_t sGateLines = 0;
static std::uint32_t sCine5Lines = 0;
static std::uint32_t sCine3Lines = 0;
static std::uint32_t sWatchLines = 0;
static std::uint32_t sStallLines = 0;
static std::uint32_t sOtherLines = 0;

inline bool IsHeapish(const void* value) {
    return pshowcoop::pointerguard::Readable(value);
}

static std::uintptr_t MainStart() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

static bool InMain(std::uintptr_t p) { return exl::util::GetMainModuleInfo().m_Total.InRange(p); }

static std::uintptr_t ToNso(std::uintptr_t p) { return InMain(p) ? p - MainStart() : 0; }

template <typename Fn>
static Fn At(ptrdiff_t offset) {
    return reinterpret_cast<Fn>(MainStart() + offset);
}

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

// One frame up: our saved frame record holds the caller's x29, whose +8 is the
// caller's own return address.
static std::uintptr_t CallerReturn(const void* frame) {
    const auto* ourFrame = static_cast<const std::uintptr_t*>(frame);
    const std::uintptr_t callerFp = ourFrame != nullptr ? ourFrame[0] : 0;
    if (callerFp == 0 || (callerFp & 0xF) != 0) return 0;
    return reinterpret_cast<const std::uintptr_t*>(callerFp)[1];
}

static bool IsPlayerController(void* controller) {
    return IsHeapish(controller) && IsA(controller, At<ClassGetterFn>(PlayerControllerClassFn)());
}

// 0 = P1, 1 = P2, -1 = not a local player's controller. Same label as PATCH-0010.
static int WhoOfController(void* controller) {
    if (!IsPlayerController(controller)) return -1;
    void* lp = At<ResolveLpFn>(ResolveLocalPlayerFn)(controller);
    if (!IsHeapish(lp)) return -1;
    const std::int32_t id = Read<std::int32_t>(lp, LocalPlayerId);
    return (id == 0 || id == 1) ? id : -1;
}

static int WhoOfPawn(void* pawn) {
    if (!IsHeapish(pawn)) return -1;
    return WhoOfController(Read<void*>(pawn, PawnController));
}

static const char* Name(int who) { return who == 0 ? "P1" : (who == 1 ? "P2" : "other"); }

static Snap ReadSnap(void* pawn) {
    Snap s = {};
    if (!IsHeapish(pawn)) return s;
    s.pawn = pawn;
    s.valid |= 1;
    s.input = (Read<std::uint8_t>(pawn, PawnInputByte) & PawnInputBit) != 0 ? 1 : 0;
    if (IsA(pawn, At<ClassGetterFn>(PawnClassFn)())) {
        void* core = Read<void*>(pawn, PawnCore);
        if (IsHeapish(core)) {
            s.valid |= 2;
            s.mask = Read<std::uint32_t>(core, CoreMask);
            void* actionInput = Read<void*>(core, CoreActionInput);
            if (IsHeapish(actionInput)) {
                s.valid |= 4;
                s.actionInput = actionInput;
                s.allowed = Read<std::uint8_t>(actionInput, ActionInputAllowed);
            }
        }
    }
    void* controller = Read<void*>(pawn, PawnController);
    if (IsPlayerController(controller)) {
        s.valid |= 8;
        s.move = Read<std::uint8_t>(controller, ControllerIgnoreMove);
        s.look = Read<std::uint8_t>(controller, ControllerIgnoreLook);
        s.cine = Read<std::uint8_t>(controller, ControllerCinematic);
        s.cineDisable = Read<std::uint8_t>(controller, ControllerCinemaDisable);
    }
    return s;
}

static bool SameSnap(const Snap& a, const Snap& b) {
    return a.pawn == b.pawn && a.mask == b.mask && a.input == b.input && a.allowed == b.allowed &&
           a.move == b.move && a.look == b.look && a.cine == b.cine &&
           a.cineDisable == b.cineDisable && a.valid == b.valid;
}

static void FormatOne(char* out, std::size_t size, int who, const Snap& s) {
    const std::uint32_t* n = &sCalls[who * 3];
    std::snprintf(out, size,
                  "%s{pawn=%p in=%u ok=%u mask=0x%x mv=%u lk=%u cine=0x%x cd=0x%x v=0x%x n=%u/%u/%u}",
                  Name(who), s.pawn, s.input, s.allowed, s.mask, s.move, s.look, s.cine,
                  s.cineDisable, s.valid, n[0], n[1], n[2]);
}

// Both players: `fresh` replaces the watch's copy for `freshWho`.
static void FormatPlayers(char* out, std::size_t size, int freshWho, const Snap& fresh) {
    char p1[160];
    char p2[160];
    FormatOne(p1, sizeof(p1), 0, freshWho == 0 ? fresh : sLatest[0]);
    FormatOne(p2, sizeof(p2), 1, freshWho == 1 ? fresh : sLatest[1]);
    std::snprintf(out, size, "%s %s", p1, p2);
}

static int CallerSlot(std::uintptr_t raNso) {
    if (raNso == DispatchFromState) return 0;
    if (raNso == DispatchFromPawn) return 1;
    return 2;
}

static const char* SlotName(int slot) { return slot == 0 ? "state" : (slot == 1 ? "pawn" : "other"); }

// Called on each dispatch of `observer`: a stream of the other player that has
// not advanced for StallCalls of these dispatches is stalled.
static void CheckStalls(int observer) {
    const int q = observer ^ 1;
    for (int c = 0; c < 3; ++c) {
        const int s = q * 3 + c;
        if (sCalls[s] == 0) continue;
        if (sCalls[s] != sMark[s]) {
            if (sStalled[s]) {
                sStalled[s] = false;
                if (sStallLines < MaxStallLines) {
                    sStallLines++;
                    char players[340];
                    FormatPlayers(players, sizeof(players), -1, Snap{});
                    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC resume who=%s caller=%s gap=%u %s-dispatches %s",
                                Name(q), SlotName(c), sStill[s], Name(observer), players);
                }
            }
            sMark[s] = sCalls[s];
            sStill[s] = 0;
            continue;
        }
        ++sStill[s];
        if (!sStalled[s] && sStill[s] == StallCalls) {
            sStalled[s] = true;
            if (sStallLines < MaxStallLines) {
                sStallLines++;
                char players[340];
                FormatPlayers(players, sizeof(players), -1, Snap{});
                Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC stall who=%s caller=%s after %u %s-dispatches %s",
                            Name(q), SlotName(c), StallCalls, Name(observer), players);
            }
        }
    }
}

HOOK_DEFINE_TRAMPOLINE(GateRecompute) {
    static void Callback(void* core, std::uintptr_t bit, std::uintptr_t set) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));

        Orig(core, bit, set);

        if (!IsHeapish(core)) return;
        const std::uint32_t mask = Read<std::uint32_t>(core, CoreMask);
        void* pawn = At<WeakGetFn>(WeakObjectGet)(static_cast<std::uint8_t*>(core) + CoreWeakPawn);
        const int who = WhoOfPawn(pawn);
        if (who < 0) {
            if (sOtherLines >= MaxOtherLines) return;
            sOtherLines++;
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC gate who=other core=%p pawn=%p bit=%u set=%u mask=0x%x raNSO=0x%lx ra2NSO=0x%lx",
                        core, pawn, static_cast<unsigned>(bit & 0xFFFFFFFF), static_cast<unsigned>(set & 1),
                        mask, static_cast<unsigned long>(ToNso(ra)), static_cast<unsigned long>(ToNso(ra2)));
            return;
        }
        if (sGateHave[who] && sGateMask[who] == mask) return;
        const std::uint32_t before = sGateHave[who] ? sGateMask[who] : sLatest[who].mask;
        sGateHave[who] = true;
        sGateMask[who] = mask;
        if (sGateLines >= MaxGateLines) return;
        sGateLines++;
        char players[340];
        FormatPlayers(players, sizeof(players), who, ReadSnap(pawn));
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC gate who=%s core=%p bit=%u set=%u mask 0x%x->0x%x raNSO=0x%lx ra2NSO=0x%lx %s",
                    Name(who), core, static_cast<unsigned>(bit & 0xFFFFFFFF), static_cast<unsigned>(set & 1),
                    before, mask, static_cast<unsigned long>(ToNso(ra)), static_cast<unsigned long>(ToNso(ra2)),
                    players);
    }
};

HOOK_DEFINE_TRAMPOLINE(Cinematic5) {
    static void Callback(void* controller, std::uintptr_t cinematic, std::uintptr_t hidePlayer,
                         std::uintptr_t affectsHud, std::uintptr_t affectsMove, std::uintptr_t affectsTurn) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));

        Orig(controller, cinematic, hidePlayer, affectsHud, affectsMove, affectsTurn);

        const int who = WhoOfController(controller);
        const std::uint64_t key = (static_cast<std::uint64_t>(ToNso(ra)) << 8) | ((cinematic & 1) << 4) |
                                  ((hidePlayer & 1) << 3) | ((affectsHud & 1) << 2) |
                                  ((affectsMove & 1) << 1) | (affectsTurn & 1);
        if (sCine5Key[who + 1] == key) return;
        sCine5Key[who + 1] = key;
        if (sCine5Lines >= MaxCine5Lines) return;
        sCine5Lines++;
        void* pawn = IsPlayerController(controller) ? Read<void*>(controller, ControllerPawn) : nullptr;
        char players[340];
        FormatPlayers(players, sizeof(players), who, ReadSnap(pawn));
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC cine5 who=%s ctrl=%p cin=%u hide=%u hud=%u move=%u turn=%u raNSO=0x%lx ra2NSO=0x%lx %s",
                    Name(who), controller, static_cast<unsigned>(cinematic & 1),
                    static_cast<unsigned>(hidePlayer & 1), static_cast<unsigned>(affectsHud & 1),
                    static_cast<unsigned>(affectsMove & 1), static_cast<unsigned>(affectsTurn & 1),
                    static_cast<unsigned long>(ToNso(ra)), static_cast<unsigned long>(ToNso(ra2)), players);
    }
};

HOOK_DEFINE_TRAMPOLINE(Cinematic3) {
    static void Callback(void* controller, std::uintptr_t cinematic, std::uintptr_t affectsMove,
                         std::uintptr_t affectsTurn) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));

        Orig(controller, cinematic, affectsMove, affectsTurn);

        const int who = WhoOfController(controller);
        const std::uint64_t key = (static_cast<std::uint64_t>(ToNso(ra)) << 8) | ((cinematic & 1) << 2) |
                                  ((affectsMove & 1) << 1) | (affectsTurn & 1);
        if (sCine3Key[who + 1] == key) return;
        sCine3Key[who + 1] = key;
        if (sCine3Lines >= MaxCine3Lines) return;
        sCine3Lines++;
        void* pawn = IsPlayerController(controller) ? Read<void*>(controller, ControllerPawn) : nullptr;
        char players[340];
        FormatPlayers(players, sizeof(players), who, ReadSnap(pawn));
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC cine3 who=%s ctrl=%p cin=%u move=%u turn=%u raNSO=0x%lx ra2NSO=0x%lx %s",
                    Name(who), controller, static_cast<unsigned>(cinematic & 1),
                    static_cast<unsigned>(affectsMove & 1), static_cast<unsigned>(affectsTurn & 1),
                    static_cast<unsigned long>(ToNso(ra)), static_cast<unsigned long>(ToNso(ra2)), players);
    }
};

HOOK_DEFINE_TRAMPOLINE(InputDispatch) {
    static void Callback(void* actionInput, float frameTime) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));

        Orig(actionInput, frameTime);

        if (!IsHeapish(actionInput)) return;
        void* pawn = At<WeakGetFn>(WeakObjectGet)(static_cast<std::uint8_t*>(actionInput) + ActionInputWeakPawn);
        const int who = WhoOfPawn(pawn);
        if (who < 0) return;
        const std::uintptr_t raNso = ToNso(ra);
        sCalls[who * 3 + CallerSlot(raNso)]++;
        const Snap s = ReadSnap(pawn);
        if (s.valid == 15 && s.actionInput == actionInput)
            diagcontrols::Observe(actionInput, pawn, who, raNso);
        sLatest[who] = s;
        CheckStalls(who);
        if (sWatchHave[who] && SameSnap(sWatchLogged[who], s)) return;
        sWatchHave[who] = true;
        sWatchLogged[who] = s;
        if (sWatchLines >= MaxWatchLines) return;
        sWatchLines++;
        char players[340];
        FormatPlayers(players, sizeof(players), who, s);
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC watch who=%s raNSO=0x%lx obj=%s %s",
                    Name(who), static_cast<unsigned long>(raNso),
                    s.actionInput == actionInput ? "core" : "mismatch", players);
    }
};

} // namespace

void InstallDiagInput() {
    if (!kDiag0242InputFreeze) return;
    GateRecompute::InstallAtOffset(GateRecomputeFn);
    Cinematic5::InstallAtOffset(SetCinematic5Fn);
    Cinematic3::InstallAtOffset(SetCinematic3Fn);
    InputDispatch::InstallAtOffset(InputDispatcherFn);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC installed gate=0x%lx cine5=0x%lx cine3=0x%lx dispatch=0x%lx",
                static_cast<unsigned long>(GateRecomputeFn), static_cast<unsigned long>(SetCinematic5Fn),
                static_cast<unsigned long>(SetCinematic3Fn), static_cast<unsigned long>(InputDispatcherFn));
}

} // namespace pshowcoop::patches
