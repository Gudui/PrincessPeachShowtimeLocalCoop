#include "pointer_guard.hpp"
#include "patch_gauge_visibility.hpp"
#include "patch_core.hpp"
#include "patch_hud.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>

// PATCH-0237 - Player 2 life-gauge show/hide driver.
//
// P1's gauge is shown and hidden every frame by the CourseUI driver 0x009A6800
// reads player 0's pawn (0x044FB668(ui, 0)), life slot 1 and the one gauge at
// CourseUI+0x448, and keeps its state in single CourseUI fields. The title has
// no second instance, so P2's gauge was pinned shown (PATCH-0233 ShowAtSpawn).
//
// Route B (operator-approved 2026-09-23, an explicit exception to the no-mod-
// timer rule): after the native pass for P1, run the same rules for P2. Every
// query is the native function the driver itself calls, asked of P2's pawn,
// P2's PlayerInterface and P2's life slot; every constant (show duration
// +0x88C, idle delay +0x888) and every shared gate (+0x341/+0x718/+0x8B0/
// +0x970/+0x980) is read from the live CourseUI. Only the per-player driver
// state (+0x558 timer, +0x55C shown, +0x560 idle accumulator, +0x564 idle shown,
// +0x6DD, +0x8B1) is held here, one copy for P2, updated by the same rules.
// Each step below cites the native instruction range it mirrors.

namespace pshowcoop::patches {

namespace {

// Kill switch. false = previous behaviour (P2's gauge shown by RevealP2DamageHUD
// on each hit and never hidden).
constexpr bool kPatch0237P2GaugeDriver = true;
constexpr bool kDiag0237DriverTrace = false;
// Driver entry: str d8,[sp,#-0x60]! ; stp x29,x30,[sp,#0x10] ; add x29,sp,#0x10.
constexpr ptrdiff_t GaugeDriverOffset = 0x009A6800;
constexpr ptrdiff_t ResolveWeakPtrOffset = 0x02A7312C;          // FWeakObjectPtr -> UObject*
constexpr ptrdiff_t GetCourseGameStateOffset = 0x01377800;      // (ctx) -> CourseGameState or null
constexpr ptrdiff_t GetPlayerLifeOffset = 0x0138F1EC;           // (gs, idx) -> slot life
constexpr ptrdiff_t GetPlayerLifeExtraOffset = 0x0138F224;      // (gs, idx) -> slot word +4
constexpr ptrdiff_t GaugeCountOffset = 0x009D6314;              // (gauge) -> +0x508
constexpr ptrdiff_t PlayerInterfaceClassOffset = 0x0252124C;    // PlayerInterface StaticClass
constexpr ptrdiff_t GetInterfaceAddressOffset = 0x02A376CC;     // (obj, iface class) -> subobject
constexpr ptrdiff_t LockOnTargetQueryOffset = 0x00D400A0;       // (actor) -> LockReceiver target
constexpr ptrdiff_t GameModePhaseGt1Offset = 0x0137E9EC;        // (world) -> phase > 1
constexpr ptrdiff_t GameModePhaseEq1Offset = 0x0137E984;        // (world) -> phase == 1
constexpr ptrdiff_t RemoteControlReceiverClassOffset = 0x02558820;
constexpr ptrdiff_t RemoteControlActiveOffset = 0x00D5D470;     // (comp) -> comp+0xB8 > 0
constexpr ptrdiff_t IdleGateOwnerOffset = 0x01323E30;           // (ui) -> subsystem
constexpr ptrdiff_t IdleGateFlagOffset = 0x01323F78;            // (subsystem) -> *(+0x2B0)+0xC0
constexpr ptrdiff_t SightControlClassOffset = 0x025A453C;
constexpr ptrdiff_t SightControlHasTargetOffset = 0x01E0376C;   // (comp) -> bit0
constexpr ptrdiff_t GetPlayerPawnOffset = 0x044FB668;           // (ctx, index) -> pawn (DIAG only)

// CourseUI fields (FUNC-APSHOWHUD-LIFE-GAUGE-DRIVER detail.layout).
constexpr std::uintptr_t kUiGaugeWeak = 0x448;
constexpr std::uintptr_t kUi6DD = 0x6DD;
constexpr std::uintptr_t kUiIdleDelay = 0x888;
constexpr std::uintptr_t kUiShowDuration = 0x88C;
constexpr std::uintptr_t kUi8B0 = 0x8B0;
constexpr std::uintptr_t kUi8B1 = 0x8B1;
constexpr std::uintptr_t kUiHideBits = 0x970;
constexpr std::uintptr_t kUiOverride = 0x980;
constexpr std::uint32_t kHideBitPawn = 0x8;   // 0x009A68A4 / 0x009A68BC

// Virtual slots used by the driver.
constexpr std::uintptr_t kVtGetWorld = 0x168;          // ui->vtbl+0x168 (0x009A6A88)
constexpr std::uintptr_t kVtFindComponent = 0x628;     // actor->vtbl+0x628(class)
constexpr std::uintptr_t kVtSetDisplayMode = 0x518;    // gauge (0x009A69F0)
constexpr std::uintptr_t kVtSetShown = 0x4C0;          // gauge (0x009A6AB8), 0x01F5C174
constexpr std::uintptr_t kPiHold = 0x40;               // 0x01D3071C
constexpr std::uintptr_t kPiIdleNotify = 0x128;        // 0x01A96410 (ret for Peach)
constexpr std::uintptr_t kPiHide = 0x130;              // 0x01C5486C
constexpr std::uintptr_t kPiIdleVeto = 0x138;          // 0x01D320A8

constexpr std::uint32_t kP2LifeIndex = 2;  // PATCH-0232 slot

using DriverFn = void (*)(void*, float);
using PtrFn = void* (*)(void*);
using PtrPtrFn = void* (*)(void*, void*);
using ClassFn = void* (*)();
using Bit0Fn = std::uint64_t (*)(void*);
using LifeFn = std::int32_t (*)(void*, std::uint32_t);
using CountFn = std::int32_t (*)(void*);
using SetDisplayModeFn = void (*)(void*, std::int32_t, std::uint32_t, std::uint32_t);
using SetShownFn = void (*)(void*, std::uint32_t);
using NotifyFn = void (*)(void*, std::uint32_t);
using GetPlayerPawnFn = void* (*)(void*, std::int32_t);

inline bool IsHeapish(const void* value) {
    return pshowcoop::pointerguard::Readable(value);
}

template <typename Fn>
Fn Native(ptrdiff_t offset) {
    return reinterpret_cast<Fn>(exl::util::GetMainModuleInfo().m_Total.m_Start + offset);
}

template <typename Fn>
Fn Virtual(void* object, std::uintptr_t slot) {
    auto** vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<Fn>(vtable[slot / 8]);
}

inline std::uint8_t U8(void* object, std::uintptr_t disp) {
    return *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(object) + disp);
}
inline std::uint32_t U32(void* object, std::uintptr_t disp) {
    return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(object) + disp);
}
inline float F32(void* object, std::uintptr_t disp) {
    return *reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(object) + disp);
}

// P2's copy of the driver's per-player CourseUI state.
struct P2DriverState {
    void* gauge = nullptr;
    float timer = 0.0f;         // +0x558
    bool shown = false;         // +0x55C
    float idleAcc = 0.0f;       // +0x560
    bool idleShown = false;     // +0x564
    std::uint8_t flag6DD = 0;   // +0x6DD
    std::uint8_t flag8B1 = 0;   // +0x8B1
    // Last CourseUI values after the native pass, to tell writes made between
    // frames by other native code (0x009AC064 writes +0x6DD; the course event
    // handler 0x009A5728 sets +0x8B1) from the P1 pass's own writes.
    std::uint8_t lastUi6DD = 0;
    std::uint8_t lastUi8B1 = 0;
    bool havePost = false;
    int lastCount = -1;
    int lastTotal = -1;
    bool lastShownLogged = false;
    bool lastIdleLogged = false;
    unsigned long frames = 0;
};

P2DriverState sState;

inline void SetShown(void* gauge, bool shown) {
    Virtual<SetShownFn>(gauge, kVtSetShown)(gauge, shown ? 1u : 0u);
}

// P1's gauge by the CourseUI's own reference (0x009A6928 resolve of +0x448).
// Used only to confirm this driver call belongs to the gameplay CourseUI.
bool IsGameplayCourseUi(void* ui) {
    void* p1Gauge = GetP1LifeGauge();
    if (!IsHeapish(p1Gauge)) return false;
    void* weak = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(ui) + kUiGaugeWeak);
    return Native<PtrFn>(ResolveWeakPtrOffset)(weak) == p1Gauge;
}

void TraceTransition(const char* why, int count, int total, bool hidden) {
    if (!kDiag0237DriverTrace) return;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC P2 driver %s: frame=%lu shown=%d timer=%.2f idleAcc=%.2f idleShown=%d "
                "count=%d total=%d hidden=%d 6dd=%u 8b1=%u",
                why, sState.frames, sState.shown ? 1 : 0, static_cast<double>(sState.timer),
                static_cast<double>(sState.idleAcc), sState.idleShown ? 1 : 0, count, total,
                hidden ? 1 : 0, sState.flag6DD, sState.flag8B1);
}

void InitState(void* ui, void* gauge, void* p2Pawn) {
    sState = P2DriverState{};
    sState.gauge = gauge;
    sState.flag6DD = U8(ui, kUi6DD);
    sState.flag8B1 = U8(ui, kUi8B1);
    void* nativeIdx1 = Native<GetPlayerPawnFn>(GetPlayerPawnOffset)(ui, 1);
    Logging.Log("[PShowCoop] PATCH-0237 P2 gauge driver attached: ui=%p gauge=%p p2Pawn=%p "
                "GetPlayerPawn(ui,1)=%p (same=%d) 6dd=%u 8b1=%u showDur=%.2f idleDelay=%.2f",
                ui, gauge, p2Pawn, nativeIdx1, nativeIdx1 == p2Pawn ? 1 : 0, sState.flag6DD,
                sState.flag8B1, static_cast<double>(F32(ui, kUiShowDuration)),
                static_cast<double>(F32(ui, kUiIdleDelay)));
}

// The idle section, 0x009A6AC0-0x009A6C70, for P2.
void RunIdleRules(void* ui, void* world, void* pawn, void* pi, void* gauge, float dt,
                  bool hidden, bool hold) {
    // 0x009A6AC0-0x009A6ADC: reset when hidden, not course phase 1, or held.
    const bool phase1 = (Native<Bit0Fn>(GameModePhaseEq1Offset)(world) & 1) != 0;
    if (hidden || !phase1 || hold) {
        // 0x009A6B54-0x009A6B74
        sState.flag8B1 = 0;
        sState.idleShown = false;
        SetShown(gauge, sState.shown);
        sState.idleAcc = 0.0f;
        return;
    }

    // 0x009A6AE0-0x009A6B20: block = idle veto || +0x8B0 || remote control active.
    bool block = true;
    const bool veto = (Virtual<Bit0Fn>(pi, kPiIdleVeto)(pi) & 1) != 0;
    if (!veto && U8(ui, kUi8B0) == 0) {
        void* rcClass = Native<ClassFn>(RemoteControlReceiverClassOffset)();
        void* rc = Virtual<PtrPtrFn>(pawn, kVtFindComponent)(pawn, rcClass);
        block = rc != nullptr && (Native<Bit0Fn>(RemoteControlActiveOffset)(rc) & 1) != 0;
    }

    // 0x009A6B24-0x009A6B30 / 0x009A6B98-0x009A6BA4: subsystem idle gate.
    bool gate = false;
    void* owner = Native<PtrFn>(IdleGateOwnerOffset)(ui);
    if (IsHeapish(owner) &&
        IsHeapish(*reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(owner) + 0x2B0))) {
        gate = (Native<Bit0Fn>(IdleGateFlagOffset)(owner) & 1) != 0;
    }

    const float idleDelay = F32(ui, kUiIdleDelay);
    if (gate && !block) {
        // 0x009A6B38-0x009A6B50
        sState.idleAcc += dt;
        if (!(sState.idleAcc >= idleDelay)) return;
    } else {
        // 0x009A6BA8-0x009A6BF4 / 0x009A6BFC-0x009A6C08
        sState.idleAcc = 0.0f;
        if (sState.idleShown) {
            sState.idleShown = false;
            sState.flag8B1 = 0;
            SetShown(gauge, sState.shown);
            Virtual<NotifyFn>(pi, kPiIdleNotify)(pi, 0);
        }
        if (!(sState.idleAcc >= idleDelay)) return;
    }

    // 0x009A6C0C-0x009A6C70: show when the event flag or a sight target allows it.
    void* scClass = Native<ClassFn>(SightControlClassOffset)();
    void* sc = Virtual<PtrPtrFn>(pawn, kVtFindComponent)(pawn, scClass);
    const bool hasTarget = sc != nullptr && (Native<Bit0Fn>(SightControlHasTargetOffset)(sc) & 1) != 0;
    if (sState.flag8B1 == 0 && !hasTarget) return;
    sState.idleShown = true;
    sState.idleAcc = idleDelay;
    SetShown(gauge, true);
    Virtual<NotifyFn>(pi, kPiIdleNotify)(pi, 1);
}

// The driver body, 0x009A681C-0x009A6ABC, for P2.
void RunP2Driver(void* ui, float dt) {
    void* gauge = GetP2LifeGauge();
    void* pawn = GetP2Pawn();
    if (!IsHeapish(gauge) || !GaugeVtableIntact(gauge) || !IsHeapish(pawn)) return;
    if (sState.gauge != gauge) InitState(ui, gauge, pawn);
    ++sState.frames;

    // 0x009A6870-0x009A6884: pawn as PlayerInterface; none -> no update.
    void* ifaceClass = Native<ClassFn>(PlayerInterfaceClassOffset)();
    void* pi = Native<PtrPtrFn>(GetInterfaceAddressOffset)(pawn, ifaceClass);
    if (pi == nullptr) return;
    void* world = Virtual<PtrFn>(ui, kVtGetWorld)(ui);

    // 0x009A6888-0x009A68F8: hide reasons. The shared bits come from the native
    // pass just run; the pawn bit 0x8 is P2's own PlayerInterface +0x130.
    const bool hidden = (Virtual<Bit0Fn>(pi, kPiHide)(pi) & 1) != 0;
    const std::uint32_t bits = (U32(ui, kUiHideBits) & ~kHideBitPawn) | (hidden ? kHideBitPawn : 0u);
    if (U8(ui, kUiOverride) == 0 && bits != 0) {
        SetShown(gauge, false);
        return;
    }

    bool hold = false;  // w24
    int count = -1;
    int total = -1;
    void* gs = Native<PtrFn>(GetCourseGameStateOffset)(ui);
    if (gs != nullptr) {
        // 0x009A6908-0x009A6938: total from P2's slot, count from P2's gauge.
        total = Native<LifeFn>(GetPlayerLifeOffset)(gs, kP2LifeIndex) +
                Native<LifeFn>(GetPlayerLifeExtraOffset)(gs, kP2LifeIndex);
        count = Native<CountFn>(GaugeCountOffset)(gauge);
        if (count != total) {
            // 0x009A69E0-0x009A6A20: set the count, show for +0x88C (0 while held);
            // no SetShown this frame.
            Virtual<SetDisplayModeFn>(gauge, kVtSetDisplayMode)(gauge, total, sState.flag6DD, 1);
            sState.shown = true;
            sState.timer = F32(ui, kUiShowDuration);
            if ((Virtual<Bit0Fn>(pi, kPiHold)(pi) & 1) != 0) sState.timer = 0.0f;
            TraceTransition("life-change", count, total, hidden);
        } else if (count == 1) {
            // 0x009A695C-0x009A69D4 / 0x009A6A64-0x009A6AA4: last heart.
            if (Native<PtrFn>(LockOnTargetQueryOffset)(pawn) != nullptr) {
                sState.shown = false;
                SetShown(gauge, false);
                return;
            }
            if ((Native<Bit0Fn>(GameModePhaseGt1Offset)(world) & 1) == 0) {
                sState.shown = !hidden;
            }
            SetShown(gauge, sState.shown);
        } else {
            // 0x009A6A24-0x009A6AA4: count the timer down; expire unless held.
            hold = (Virtual<Bit0Fn>(pi, kPiHold)(pi) & 1) != 0;
            sState.timer -= dt;
            if (sState.timer < 0.0f && !hold) {
                sState.shown = false;
                sState.flag6DD = 1;
            }
            SetShown(gauge, sState.shown);
        }
    }

    RunIdleRules(ui, world, pawn, pi, gauge, dt, hidden, hold);

    if (kDiag0237DriverTrace &&
        (sState.shown != sState.lastShownLogged || sState.idleShown != sState.lastIdleLogged ||
         count != sState.lastCount || total != sState.lastTotal)) {
        TraceTransition("state", count, total, hidden);
        sState.lastShownLogged = sState.shown;
        sState.lastIdleLogged = sState.idleShown;
        sState.lastCount = count;
        sState.lastTotal = total;
    }
}

HOOK_DEFINE_TRAMPOLINE(P2GaugeDriver) {
    static void Callback(void* ui, float dt) {
        if (!kPatch0237P2GaugeDriver || !IsHeapish(ui)) {
            Orig(ui, dt);
            return;
        }
        const std::uint8_t pre6DD = U8(ui, kUi6DD);
        const std::uint8_t pre8B1 = U8(ui, kUi8B1);
        Orig(ui, dt);  // native pass for P1, unchanged
        if (!IsGameplayCourseUi(ui)) return;

        // Mirror writes other native code made between frames into P2's copy.
        if (sState.havePost && sState.gauge == GetP2LifeGauge()) {
            if (pre6DD != sState.lastUi6DD) sState.flag6DD = pre6DD;
            if (pre8B1 != 0 && sState.lastUi8B1 == 0) sState.flag8B1 = 1;
        }
        RunP2Driver(ui, dt);
        sState.lastUi6DD = U8(ui, kUi6DD);
        sState.lastUi8B1 = U8(ui, kUi8B1);
        sState.havePost = sState.gauge != nullptr;
    }
};

} // namespace

bool P2GaugeDriverEnabled() {
    return kPatch0237P2GaugeDriver;
}

void ResetP2GaugeDriver() {
    sState = P2DriverState{};
}

void InstallGaugeVisibility() {
    if (!kPatch0237P2GaugeDriver) {
        Logging.Log("[PShowCoop] PATCH-0237 P2 gauge driver disabled (kill switch)");
        return;
    }
    P2GaugeDriver::InstallAtOffset(GaugeDriverOffset);
    Logging.Log("[PShowCoop] PATCH-0237 installed: P2 gauge driver after CourseUI driver 0x%lx (trace=%d)",
                static_cast<unsigned long>(GaugeDriverOffset), kDiag0237DriverTrace ? 1 : 0);
}

} // namespace pshowcoop::patches
