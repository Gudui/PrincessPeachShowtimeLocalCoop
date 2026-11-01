#include "pointer_guard.hpp"
#include "patch_hud.hpp"
#include "patch_core.hpp"
#include "patch_damage.hpp"
#include "patch_gauge_visibility.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>
#include <cstring>

namespace pshowcoop::patches {

namespace {

 // PATCH-0188 one-line compile-time kill switch: set false to restore the legacy
 // Outer rewrite to the P2 PlayerController.
 constexpr bool kPatch0188KeepCreationOuter = true;

 // PATCH-0190 one-line kill switch: the P2 gauge Y offset below P1's translation.
 constexpr float kPatch0190YOffset = 40.0f;

 // PATCH-0193 one-line kill switch: set false to restore the PlayerContext (+0x28)
 // writes. P1's live gauge keeps +0x28 null and renders; the field is unproven, so
 // the writes stay off until Ghidra proves the slot.
 constexpr bool kPatch0193KeepNullPlayerContext = true;

 // PATCH-0196 one-line kill switch: set false to skip the native APShowHUD setup
 // tail. Replicates exactly the four calls native 0x009A40B0 performs after
 // InitHeartMeshes(1,1) that our spawn omits: vtable+0x510(gauge,3),
 // vtable+0x518(gauge,3,1,1), byte +0x5DC=0, vtable+0x4E0(gauge,0xE).
 constexpr bool kPatch0196CompleteNativeSetup = true;

 // PATCH-0206 one-line kill switch: set false to restore routing P2 comps to
 // sP2HudContext. Null is the decomp-proven absence path (early return, no
 // virtual dispatch); no native function is skipped and P1 is untouched.
 constexpr bool kPatch0206NullP2HudLookup = true;
 // vtable-liveness probe.
 constexpr bool kDiag0203LivenessEnabled = true;

 // PATCH-0204 one-line compile-time kill switch: set false to restore legacy
 // unguarded dispatch into sP2LifeGauge (all indirect calls execute regardless
 // of liveness, as before).
 constexpr bool kPatch0204GuardDeadWidget = true;
 constexpr bool kDiag0204bCtxLinkageEnabled = true;
  // queue snapshot.
  constexpr bool kDiag0205SnapshotEnabled = true;
  // registry snapshot.
  constexpr bool kDiag0207PreRestoreEnabled = true;
  // conversion snapshot.
  constexpr bool kDiag0208PreRevealEnabled = true;
  // its outer chain (P1's CourseUI tree) alive across courses and a restart ran
  // through AddToScreen's viewport holder, like any on-screen UUserWidget.

  // PATCH-0245 one-line kill switch: true = do not OR 0x400 (RF_NeedLoad) into
  // the spawned P2 gauge's ObjectFlags. false restores the old write.
  constexpr bool kPatch0245DropNeedLoadWrite = true;
  // per-slot AnimSetup('heartcharm') replication with WIDGET-LOCAL slot
  // indices 0..count-1 (v1 used HUD-context slots 5-7 and was inert,
  constexpr bool kDiag0210AnimSetupEnabled = true;
  // GetSlotBase()+i for i < GetSlotCount(), as the native factory loop does
  // (0x009A40CC-0x009A4124: AnimSetup(gauge, 0x0138F10C()+i, 'heartcharm')), with
  // no widget-slot bound (AnimSetup itself ignores slot >= +0x4B8). false = the
  // v4 widget-local indices 0..n-1, which turned P2's hearts 0-2 into collapsed
  // charm slots (live read: P2 hearts 0-2 Visibility=1, P1's all 3).
  constexpr bool kPatch0234NativeCharmSlotIndex = true;
  // factory's heart sequence once with the native count: vtbl+0x510 = SetCurrentHearts
  // (0x009D56A0) then vtbl+0x518 = SetDisplayMode (0x009D5F68)(n, 1, 1), n =
  // 0x0138F10C() (0x009A4024..0x009A4080), and skip the later duplicate
  // SetCurrentHearts(5)/SetDisplayMode(5,1,1). false = the old literal 3 followed by a
  // rebuild to 5, which left only hearts 3-4 with a queued appear step (P2 showed 2).
  constexpr bool kPatch0236NativeHeartSequence = true;
  constexpr bool kPatch0211Param3Enable = true;
  // templateObj=nullptr in the createWidget call (no archetype).
  constexpr bool kPatch0212ArchetypeTemplate = true;
  constexpr bool kPatch0213SpawnActive = true;
  // erase-adoption behavior (Slot 1 left empty; tick driver never runs).
  constexpr bool kPatch0214SlotAdoption = true;
  // per-frame fade-tick mirror (0x9d6598 driven only for P1's gauge natively).
  // same caller as P1's), so the mirror ticked P2's gauge twice while P1's was shown.
  constexpr bool kPatch0216FrameTickMirror = false;
  // the PATCH-0220 shared-tree copy. Multiple patches were cancelling each
  // other. DISABLED for a clean baseline.
  constexpr bool kPatch0217ContentRef = false;

  // PATCH-0219 KILLED: ObjectFlags copy did not change the SWidget class.
  // DISABLED for a clean baseline.
  constexpr bool kPatch0219ObjectFlags = false;

  // PATCH-0220 KILLED: the shared-tree copy is OVERWRITTEN by PATCH-0217's
  // re-attach. DISABLED for a clean baseline.
  constexpr bool kPatch0220SharedTree = false;
  // caused near-null unmapped writes. The Blueprint VM requires frame state
  // that cannot be provided from native code. KILLED and DISABLED.
  // fix the crash. The near-null writes are from INSIDE the Blueprint VM —
  // the construction script tries to access child widgets on a null
  // WidgetTree. The WidgetTree should have been cloned by UUserWidget::Initialize
  // but the gauge vtable's Initialize slot (vtbl+0x188) is a stub returning 0.
  // The Blueprint VM functions expect a WidgetTree that doesn't exist.
  // This is the FUNDAMENTAL blocker: the Blueprint VM requires the WidgetTree
  // which requires Initialize which is a stub in the Blueprint class vtable.
  constexpr bool kPatch0221ConstructionScript = false;
  // make the gauge render - the SWidget needs CONTENT pointers too.
  // DISABLED (superseded by PATCH-0222 shared content).
  constexpr bool kPatch0221VtableSwap = false;

  // PATCH-0223 DISCOVERED DEAD CODE (Step 66): the 0223 copy sits inside the
  // disabled kPatch0221ConstructionScript block and NEVER executed (no log
  // line in build 76889). The shared +0xE8 content instance arises NATURALLY
  // DISABLED and left in place as retired history.
  constexpr bool kPatch0223WidgetTreePtr = false;
  // fresh-rebuild cache invalidation (gauge + root UWidget cached widget
  // pairs zeroed before AddToViewport so TakeWidget rebuilds a fresh SWidget
  // tree through the engine's own rebuild path).
  // NOT rebind display state (fresh SWidgets still read the shared tree's
  // P1-owned UWidget instances). DISABLED for a clean baseline; also a fault
  constexpr bool kPatch0224FreshRebuild = false;
  // CreateWidgetOfClass archetype instead of P1's live gauge.
  //
  // NULL, so CDO-as-archetype gave our gauge its OWN but EMPTY tree; nothing
  // rendered and the reveal path faulted (Unmapped Read64 @ 0x28).
  //
  // Why that reason no longer applies: PATCH-0226/0227 arrived AFTER, at Step
  // 68, and exist precisely to create and POPULATE the gauge's own tree
  // through UUserWidget::Initialize. 0225 and 0227 have never been enabled
  // together; 0225's sole failure mode is the precondition 0227 fixes.
  //
  // holds TEN slots, five of whose contents are P1's LifeIcon_C widgets - and
  // those widgets name P1's slots as their owner (+0x28), so P2's panel holds
  // enabled path handing P1's identity to P2's gauge: every native gauge-setup
  // tree duplication.
  //
  // PRE-REGISTERED PREDICTION: P2's LifeIconPanel comes back with 5 slots, not
  // 10, and none of its slot contents are P1's icons. The gauge becoming
  // visible is NOT sufficient - the panel must be clean
  // (tools/gdb_course_widget_tree.py --live measures both directly).
  // ROLLBACK: set this flag back to false; nothing else changes.
  constexpr bool kPatch0225CdoArchetype = true;

  // PATCH-0226 one-line kill switch (Step 68): set false to disable calling the
  // engine's own UUserWidget::Initialize (0x3ea1404, prologue-verified) on our
  // gauge after creation. Initialize creates our gauge's OWN WidgetTree
  // construction through the gauge's own vtable (vtbl+0x338) - the exact native
  // flow P1's gauge went through. Our prior construction-script crash (0221)
  // was caused by the tree being NULL; Initialize fixes that precondition.
  constexpr bool kPatch0226CallInitialize = true;

  // PATCH-0227 one-line kill switch (Step 68 composite): set false to disable
  // dispatch the BP construction natively without crashing (the 0221-era
  // crash was purely the null-tree precondition) - but the archetype copy had
  // already installed P1's tree pointer, so Initialize took the existing-tree
  // path on P1's tree. Nulling +0x1D8 first makes Initialize NewObject OUR OWN
  // tree (outer=our gauge) and dispatch BP construction to populate it with
  // OUR UWidget instances - the exact native flow P1 experienced. The gauge's
  // cached SWidget pairs (+0xD8..+0xF0, copied from P1 by PostConstructInit)
  // are zeroed after Initialize so TakeWidget rebuilds from OUR tree.
  //
  // HALF true as long as P1 is the archetype. 0227 does get its own tree and
  // its own panels, but it does NOT get its own instances - P2's LifeIconPanel
  // ends up with TEN slots, five of which hold P1's LifeIcon_C widgets, and
  // those widgets name P1's slots as their owner. The BP construction reads
  // archetype-copied state, so the instances follow the archetype. Enabling
  // PATCH-0225 (CDO archetype) is the change that makes this comment true.
  constexpr bool kPatch0227OwnTreeInit = true;

  // PATCH-0228 one-line kill switch (Step 69): set false to disable the explicit
  // content-SWidget rebuild from our OWN populated tree. Ghidra (this session,
  // PShowDecompileContaining 0x3ea3fbc + gauge-vtable read): gauge vtbl +0x2C0 =
  // 0x3ea3fbc(undefined8 *outPair, long *self). With a populated tree root at
  // (self+0x1D8)+0x28 it calls 0x3e89580(outPair, root, thunk), which resolves or
  // rebuilds the ROOT UWidget's own Slate widget and returns the refcounted pair.
  // AddToViewport (0x3ea4930) only attaches the real widget when the gauge's own
  // +0x238/+0x240 pair is valid; when it is invalid it runs its placeholder
  // 0227 owns a populated tree yet never rebuilt +0x238 from it. Install the
  // returned pair into +0x238/+0x240 BEFORE AddToViewport.
  // Read64 @0x2E0 (vtable slot deref on a near-null self) and then the
  // on the loading screen. Root-cause before any retry. DISABLED.
  constexpr bool kPatch0228ContentRebuild = false;
  // UFunction-invocation capture. Trampolines the proven UFunction dispatcher
  // the live invocation stack, and logs that stack when P1's LifeGauge first
  // reaches AddToViewport. Goal: identify the exact HUD Blueprint UFunction that
  // creates the gauge so ProcessEvent can be driven with it on P2's context.
  // it is not built via CreateWidgetOfClass, so Route A-as-Blueprint has no
  // target. Hooks removed for a clean build. Re-enable to re-instrument.
  constexpr bool kDiag0216UfCapture = false;
  // disable the read-only object-allocator capture. Trampolines the internal
  // StaticConstructObject (0x2A4BFF0) and public StaticConstructObject
  // object's vtable is the LifeGauge vtable - identifying the native function
  // that creates P1's gauge. Both prologues verified this session.
  // game will not launch). DISABLED - do not re-enable as-is.
  constexpr bool kDiag0217AllocCapture = false;
  // to disable. Runs the engine's own per-widget fetcher 0x9A88F4 ONCE per
  // level against the fresh P2 HUD context with fully-sourced native args:
  //   container       = P1 identity chain (p1ctx+0x20 -> +0x30 -> +0x280,
  //                     the HUD SWidget the engine feeds its own fetchers,
  //   classRef        = *(p1ctx+0x370) raw class ref (the same field
  // Read-only on engine structures plus this single native dispatch. Logs the
  // returned gauge pointer/vtable and the P2 ctx +0x428/+0x448/+0x450 state.
  // KILLED (fault session 2026-09-19): the factory dispatched and CREATED a
  // gauge (0x14C0BD2000), but the native registry-append/alert flow then
  // consumed the FREED p2ctx registry region (30 unmapped faults at
  // p2ctx+0x428..+0x460, near-null entry writes). The P2 ctx lifetime must be
  // FIXED before this probe can run clean. DISABLED.
  // (native0x80, vt 0x095E1AF8), NOT a UICourseBase owner. The +0x428 registry
  // and +0x448/+0x450 widget fields belong to UICourseBase (native0x988, vt
  // 0x09060538). Applying UICourseBase fields to the sound object is a
  // re-enable without a graph-proven live UICourseBase receiver.
  constexpr bool kDiag0218FactoryProbe = false;
  // to restore the raw-pawn outer for the P2 InitPlayerHud call. The native P1
  // call passes the GameInstance identity object (0x01F4C0C8(pawn,1)) as arg0;
  constexpr bool kPatch0230IdentityOuter = true;
  constexpr ptrdiff_t GetLifeGaugeOffset = 0x009A88F4;
  using GetLifeGaugeFn = void* (*)(void* registryHeader, void* container,
                                   void* classRef, std::uint32_t player);
  constexpr bool kDiag0212FadeStateEnabled = true;
  // file long called "SetVisibility", is UUserWidget::RemoveFromViewport(): it tail-
  // calls vtbl+0x298 = 0x03EA5CCC UUserWidget::RemoveFromParent, and CourseUI uses it
  // as `if (IsInViewport(w)) RemoveFromViewport(w)` (0x009A3E90/0x009A3EA0). Calling
  // it right after AddToScreen detached P2's gauge in the same frame: dead canvas,
  // true = never call it on P2's gauge, and attach through AddToScreen's null-player
  // branch (AddViewportWidgetContent -> ViewportOverlayWidget, where P1's content is).
  constexpr bool kPatch0233NoRemoveFromViewport = true;
  // true = end P2's spawn with SetShown(true) (vtbl+0x4C0), as the native CourseUI
  // factory ends gauge #0's setup (0x009A4130); P2's gauge has no HUD driver to fade it.
  constexpr bool kPatch0233ShowAtSpawn = true;
  // true = after the native fade tick 0x009D6598 runs for P2's gauge (PATCH-0216),
  // re-run the gauge's own native screen-position routine 0x009D61C4(gauge, &loc)
  // with P2's pawn location. The native tick always follows GetPlayerPawn(.., 0)
  // (0x044FB668 with literal 0) = P1's pawn, so P2's gauge otherwise hovers over P1.
  // loc is read exactly as the native tick reads it: pawn+0x130 root component,
  // then +0x1D0/+0x1D4/+0x1D8; only when the native follow flag +0x5DC is set.
  constexpr bool kPatch0233FollowP2Pawn = true;
  // true = do not add kPatch0190YOffset to +0x4A4: +0x4A0/+0x4A4 are display-state
  // timing thresholds compared with timer +0x4D8 in 0x009D4040, not a render translation.
  constexpr bool kPatch0233NativeTimings = true;
  // gauge is P2's, pass P2's pawn location (pawn+0x130 root, +0x1D0..+0x1D8, read as
  // the native tick reads P1's) instead of the caller's. The live read showed P2's
  // +0x5E4 and "Parent" translation equal to P1's although PATCH-0233's anchor runs
  // after the mirrored tick, so some other positioning of P2's gauge uses player 0.
  // This covers every caller (fade tick, 0x0136E7A0, 0x0191C74C) at the one routine.
  constexpr bool kPatch0235P2PositionSource = true;
  constexpr ptrdiff_t GaugeUpdateScreenPosOffset = 0x009D61C4; // (gauge, const FVector*) -> +0x5E4
  using GaugeUpdateScreenPosFn = void (*)(void*, const float*);

constexpr ptrdiff_t AddToViewportOffset = 0x03EA4930;     // UUserWidget::AddToScreen(Player, ZOrder)
constexpr ptrdiff_t CreateWidgetOfClassOffset = 0x045EC770;
constexpr ptrdiff_t SetVisibilityOffset = 0x03EA5CC0;     // MISLABELED: UUserWidget::RemoveFromViewport (PATCH-0233)
constexpr ptrdiff_t IsInViewportOffset = 0x03EA6210;      // UUserWidget::IsInViewport: FullScreenWidget(+0x238/+0x240) shared > 0
using IsInViewportFn = bool (*)(void*);
constexpr ptrdiff_t InitHeartMeshesOffset = 0x009D7438;
constexpr ptrdiff_t GaugeFadeTickOffset = 0x009D6598; // (gauge, float dt) per-frame fade driver
constexpr ptrdiff_t ProcessEventOffset = 0x02A2F548;
constexpr ptrdiff_t UFunctionDispatchOffset = 0x0292EA0C;
constexpr ptrdiff_t CreateWidgetWrapperOffset = 0x045EA1E0;
constexpr ptrdiff_t StaticConstructInternalOffset = 0x02A4BFF0;
constexpr ptrdiff_t StaticConstructPublicOffset = 0x02A4EBC4;
constexpr std::uintptr_t LifeGaugeVtableNso = 0x0906D800;
// (0x9a40cc-0x9a4114) and the 'heartcharm' anim-set name at 0x874ee50.
constexpr ptrdiff_t GetSlotCountOffset = 0x0138EC34;   // int(void) heart slot count
constexpr ptrdiff_t GetSlotBaseOffset = 0x0138F10C;    // int(void) slot base
constexpr ptrdiff_t FNameConstructOffset = 0x02882898; // (void* out, const char* str, int number)
constexpr ptrdiff_t AnimSetupOffset = 0x009D7570;      // (gauge, slotIndex, const FName*)
constexpr std::uintptr_t HeartCharmStringNso = 0x0874EE50;

static void* sP1LifeGauge = nullptr;
static void* sP2LifeGauge = nullptr;
static bool sIsSpawningP2Widget = false;

inline bool IsHeapish(const void* value) {
    return pshowcoop::pointerguard::Readable(value);
}

using CreateWidgetOfClassFn = void* (*)(void* outer, void* widgetClass, std::uint64_t name,
                                       std::uint32_t flags, void* templateObj, std::uint8_t unk6,
                                       void* unk7, void* unk8);
using SetVisibilityFn = void (*)(void*, std::uint8_t);
using AddToViewportFn = void (*)(void*, void*, std::int32_t);
using InitHeartMeshesFn = void (*)(void*, std::int32_t, std::int32_t);
constexpr ptrdiff_t SetHasInAnimationOffset = 0x009D2E94;
using SetHasInAnimationFn = void (*)(void*);
constexpr ptrdiff_t InitializeVisualsOffset = 0x009D2404;
constexpr ptrdiff_t InitializeWidgetOffset = 0x003EA1404;
using InitializeVisualsFn = bool (*)(void*);

constexpr ptrdiff_t ResolveWeakPtrOffset = 0x02A7312C;
using ResolveWeakPtrFn = void* (*)(void* weakPtr);

// IdentityResolver_01F4C0C8(pawn, 1): GameInstance pawn-identity lookup used by
// decides whether P2 fails registration at the identity gate.
constexpr ptrdiff_t IdentityResolverOffset = 0x01F4C0C8;
using IdentityResolverFn = void* (*)(void*, std::int32_t);

constexpr ptrdiff_t HudRegistryArrayOffset = 0x0A1BAA50;
constexpr ptrdiff_t InitPlayerHudOffset = 0x01EF79F8;
constexpr ptrdiff_t HudLookupOffset = 0x01EF7EB0;
using InitPlayerHudFn = void* (*)(void* pawn, void* hudClass);

static void* sP1HudClass = nullptr;
static void* sP2HudContext = nullptr;

static void SpawnDedicatedP2LifeGauge(void* p1Gauge) {
    if (sP2LifeGauge != nullptr) return; // STRICTLY ONCE
    if (SinglePlayerControl()) {
        static bool sLogged0240 = false;
        if (!sLogged0240) {
            sLogged0240 = true;
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC single-player control: P2 LifeGauge NOT spawned");
        }
        return;
    }
    sIsSpawningP2Widget = true;
    sP2HudContext = nullptr; // Reset on fresh gauge creation for level load

    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    auto createWidget = reinterpret_cast<CreateWidgetOfClassFn>(mainRange.m_Start + CreateWidgetOfClassOffset);
    auto addToViewport = reinterpret_cast<AddToViewportFn>(mainRange.m_Start + AddToViewportOffset);
    auto setVisibility = reinterpret_cast<SetVisibilityFn>(mainRange.m_Start + SetVisibilityOffset);
    auto initHeartMeshes = reinterpret_cast<InitHeartMeshesFn>(mainRange.m_Start + InitHeartMeshesOffset);
    auto setHasInAnimation = reinterpret_cast<SetHasInAnimationFn>(mainRange.m_Start + SetHasInAnimationOffset);
    auto initVisuals = reinterpret_cast<InitializeVisualsFn>(mainRange.m_Start + InitializeVisualsOffset);

    void* widgetClass = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p1Gauge) + 0x10);
    void* outer = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p1Gauge) + 0x20);

    void* p2Ctrl = GetP2Controller();
    if (IsHeapish(p2Ctrl)) {
        outer = p2Ctrl;
        Logging.Log("[PShowCoop] Using P2 PlayerController outer: %p", outer);
    } else {
        Logging.Log("[PShowCoop] P2 PlayerController not yet available (p2Ctrl=%p), using default outer: %p",
                    p2Ctrl, outer);
    }

    if (IsHeapish(widgetClass) && IsHeapish(outer)) {
        Logging.Log("[PShowCoop] Spawning dedicated P2 LifeGauge_C (outer=%p, class=%p)...",
                    outer, widgetClass);
        // template=archetype from the options struct). Our previous call passed
        // templateObj=nullptr; use the P1 gauge as the same-class archetype.
        // archetype instead so the property copy comes from the CDO, not P1.
        void* templateObj = nullptr;
        if (kPatch0225CdoArchetype && IsHeapish(widgetClass)) {
            void* cdo = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(widgetClass) + 0x118);
            if (IsHeapish(cdo)) {
                templateObj = cdo;
                void* cdoTree = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(cdo) + 0x1D8);
                void* cdoTreeRoot = (IsHeapish(cdoTree)) ?
                    *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(cdoTree) + 0x28) : nullptr;
                Logging.Log("[PShowCoop] PATCH-0225 DIAG: CDO=%p CDOtree=%p CDOtreeRoot=%p",
                            cdo, cdoTree, cdoTreeRoot);
            } else {
                Logging.Log("[PShowCoop] PATCH-0225 DIAG: CDO at class+0x118 is null/non-heap (%p)", cdo);
            }
        }
        if (templateObj == nullptr && kPatch0212ArchetypeTemplate && IsHeapish(p1Gauge)) {
            templateObj = p1Gauge;
        }
        Logging.Log("[PShowCoop] Step 39: createWidget archetype/template=%p (flags=0)",
                    templateObj);
        void* newGauge = createWidget(outer, widgetClass, 0, 0, templateObj, 0, nullptr, nullptr);
        if (IsHeapish(newGauge)) {
            sP2LifeGauge = newGauge;
            ResetP2HealthTracking();
            LogGaugeLiveness("spawn");

            // PATCH-0245: this used to OR 0x400 into ObjectFlags (+0x08) as a
            // Set kPatch0245DropNeedLoadWrite=false to restore the old write.
            if (!kPatch0245DropNeedLoadWrite) {
                *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x08) |= 0x400;
            }

            // PATCH-0226 (Step 68): call the engine's own Initialize on our gauge.
            // Our native creation path (CreateWidgetOfClass) never calls Initialize
            // never got its own WidgetTree - the construction script then crashed on
            // populated tree we ever had (identity collision). Initialize creates the
            // tree (NewObject, outer=our gauge) and dispatches BP construction through
            // the gauge's own vtable - the native flow P1 experienced.
            if (kPatch0226CallInitialize || kPatch0227OwnTreeInit) {
                using InitializeFn = int (*)(void*);
                auto initWidget = reinterpret_cast<InitializeFn>(mainRange.m_Start + InitializeWidgetOffset);
                void* preTree = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x1D8);
                void* savedTree = preTree;
                if (kPatch0227OwnTreeInit) {
                    // Force Initialize's null-tree branch: it will NewObject our own
                    // WidgetTree (outer=our gauge) and dispatch BP construction to
                    // populate it with our own UWidget instances.
                    *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x1D8) = nullptr;
                }
                int initResult = initWidget(sP2LifeGauge);
                void* tree = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x1D8);
                void* treeRoot = (IsHeapish(tree)) ?
                    *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(tree) + 0x28) : nullptr;
                void* treeOuter = (IsHeapish(tree)) ?
                    *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(tree) + 0x20) : nullptr;
                Logging.Log("[PShowCoop] PATCH-0227: Initialize(gauge) result=%d preTree=%p tree=%p (new: %s) treeOuter=%p (sP1=%p, outer==sP1: %s) root=%p",
                            initResult, preTree, tree,
                            (kPatch0227OwnTreeInit && tree != nullptr && tree != savedTree) ? "YES" : "NO",
                            treeOuter, sP1LifeGauge,
                            (treeOuter == sP1LifeGauge) ? "YES" : "NO", treeRoot);
                if (kPatch0227OwnTreeInit && IsHeapish(tree) && tree != savedTree) {
                    // Invalidate the gauge's cached SWidget pairs (copied from P1 by
                    // PostConstructInit) so TakeWidget rebuilds fresh SWidgets bound
                    // to OUR tree's UWidget instances.
                    std::memset(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0xD8), 0, 32);
                    Logging.Log("[PShowCoop] PATCH-0227: own tree adopted; gauge +0xD8..+0xF0 cache invalidated");
                }
            }

            // PATCH-0228 (Step 69): rebuild the gauge's content SWidget from OUR
            // OWN populated tree. Call convention is statically proven from the
            // real engine call site inside 0x3e89580:
            //   (**(code **)(*param_2 + 0x2c0))(&lStack_68, param_2);
            // i.e. x0 = out pair buffer, x1 = self. Guarded: only invoked when our
            // gauge, its WidgetTree, the tree root UWidget, and the vtable all sit
            // in the main image. The returned refcounted pair is installed into
            // +0x238/+0x240 (the pair AddToViewport validates) so it attaches the
            // rebuilt widget instead of its placeholder. Kill switch
            // kPatch0228ContentRebuild.
            if (kPatch0228ContentRebuild && IsHeapish(sP2LifeGauge)) {
                void* treeObj = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x1D8);
                void* treeRoot = IsHeapish(treeObj)
                    ? *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(treeObj) + 0x28) : nullptr;
                void** gaugeVtable = *reinterpret_cast<void***>(sP2LifeGauge);
                const bool vtableInImage =
                    mainRange.InRange(reinterpret_cast<std::uintptr_t>(gaugeVtable));
                if (IsHeapish(treeObj) && IsHeapish(treeRoot) && vtableInImage) {
                    void* slot = gaugeVtable[0x2C0 / 8];
                    const auto slotAddr = reinterpret_cast<std::uintptr_t>(slot);
                    if (mainRange.InRange(slotAddr)) {
                        using RebuildContentFn = void (*)(void* outPair, void* self);
                        auto rebuildContent = reinterpret_cast<RebuildContentFn>(slot);
                        void* outPair[2] = { nullptr, nullptr };
                        rebuildContent(outPair, sP2LifeGauge);
                        void* outVt = IsHeapish(outPair[0]) ? *reinterpret_cast<void**>(outPair[0]) : nullptr;
                        std::uintptr_t outVtNso = 0;
                        if (IsHeapish(outVt) && mainRange.InRange(reinterpret_cast<std::uintptr_t>(outVt))) {
                            outVtNso = reinterpret_cast<std::uintptr_t>(outVt) - mainRange.m_Start;
                        }
                        const auto swPairAddr = reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x238;
                        const std::uint64_t preSw = *reinterpret_cast<std::uint64_t*>(swPairAddr);
                        const std::uint64_t preSwCtrl = *reinterpret_cast<std::uint64_t*>(swPairAddr + 8);
                        if (IsHeapish(outPair[0])) {
                            *reinterpret_cast<void**>(swPairAddr) = outPair[0];
                            *reinterpret_cast<void**>(swPairAddr + 8) = outPair[1];
                        }
                        Logging.Log("[PShowCoop] PATCH-0228: vf[0x2C0] rebuild -> sw=%p ctrl=%p swVt=NSO 0x%llx (pre +0x238=0x%llx ctrl=0x%llx) treeRoot=%p",
                                    outPair[0], outPair[1], (unsigned long long)outVtNso,
                                    (unsigned long long)preSw, (unsigned long long)preSwCtrl, treeRoot);
                    } else {
                        Logging.Log("[PShowCoop] PATCH-0228: vf[0x2C0] slot not in main range (%p), skipped", slot);
                    }
                } else {
                    Logging.Log("[PShowCoop] PATCH-0228: guards failed (tree=%p root=%p vt=%p), skipped",
                                treeObj, treeRoot, reinterpret_cast<void*>(gaugeVtable));
                }
            }

            // Phase-2 Step 0: Native Visual Hierarchy & Animation Player Initialization (0x009D2404)
            // Statically verified AAPCS entrypoint UICourseLifeGaugeBase::InitializeVisuals from APShowHUD (0x009A9A28).
            // Binds UWidgetAnimation tracks, instantiates Slate child widgets, initializes state machine at +0x4C4, and sets +0x4BC = 1.
            if (initVisuals != nullptr) {
                bool initResult = initVisuals(sP2LifeGauge);
                Logging.Log("[PShowCoop] >> Phase2 Step 0: Native InitializeVisuals(sP2LifeGauge) executed at 0x009D2404 (result=%d)",
                            initResult ? 1 : 0);
            }

            // Phase-2 Native Setup Sequence from APShowHUD (0x009A40B0)
            void** vtable = *reinterpret_cast<void***>(sP2LifeGauge);

            // 1. Allocate 5 FHeartSlot structures at +0x580
            using SetMaxHeartsFn = void (*)(void*, std::int32_t, std::int32_t);
            auto setMaxHearts = reinterpret_cast<SetMaxHeartsFn>(vtable[0x4D8 / 8]);
            if (setMaxHearts != nullptr) {
                setMaxHearts(sP2LifeGauge, 5, 1);
                Logging.Log("[PShowCoop] >> Phase2 Step 1/6: SetMaxHearts(5, 1) executed at vtable[0x4D8]");
            }
            // (factory 0x9a40cc-0x9a4114), which our spawn omitted entirely
            // InitHeartMeshes. Same receivers, same args, same relative order.
            // 2. Set widget active/enabled via native vtable[0x4C0] (0x02A4CC5C)
            using SetActiveFn = void (*)(void*, bool);
            auto setActive = reinterpret_cast<SetActiveFn>(vtable[0x4C0 / 8]);
            // (0x9a4128) - our spawn omitted it, leaving the widget inactive so
            if (kPatch0213SpawnActive && setActive != nullptr) {
                setActive(sP2LifeGauge, true);
                Logging.Log("[PShowCoop] >> Phase2 Step 2/6: SetActive(true) executed at vtable[0x4C0] (native parity, pre-InitHeartMeshes)");
            }

            // 2. Spawn and bind 3D visual heart mesh instances
            if (initHeartMeshes != nullptr) {
                initHeartMeshes(sP2LifeGauge, 1, 1);
                Logging.Log("[PShowCoop] >> Phase2 Step 2/6: InitHeartMeshes(1, 1) executed at 0x009D7438");
            }
            // our spawn omits. Native drives animation virtuals on gauges during enemy
            // Same receivers, same args, same relative order as native. All native calls.
            if (kPatch0196CompleteNativeSetup) {
                using AnimInitFn = void (*)(void*, std::int32_t);
                using AnimConfigFn = void (*)(void*, std::int32_t, std::int32_t, std::int32_t);
                auto animInit = reinterpret_cast<AnimInitFn>(vtable[0x510 / 8]);
                auto animConfig = reinterpret_cast<AnimConfigFn>(vtable[0x518 / 8]);
                auto gaugeEnable = reinterpret_cast<AnimInitFn>(vtable[0x4E0 / 8]);
                std::int32_t heartCount = 3;
                if (kPatch0236NativeHeartSequence) {
                    using GetSlotBaseFn = std::int32_t (*)(void);
                    heartCount = reinterpret_cast<GetSlotBaseFn>(mainRange.m_Start + GetSlotBaseOffset)();
                    const std::int32_t p1Entries = IsHeapish(sP1LifeGauge)
                        ? *reinterpret_cast<const std::int32_t*>(reinterpret_cast<std::uintptr_t>(sP1LifeGauge) + 0x588) : -1;
                    Logging.Log("[PShowCoop] PATCH-0236: native heart count n=0x0138F10C()=%d (P1 gauge +0x588 entries=%d)",
                                heartCount, p1Entries);
                }
                if (animInit != nullptr) {
                    animInit(sP2LifeGauge, heartCount);
                    Logging.Log("[PShowCoop] >> Phase2 Step 2b/6: Native vtable[0x510] SetCurrentHearts(%d) executed (APShowHUD parity)",
                                heartCount);
                }
                if (animConfig != nullptr) {
                    animConfig(sP2LifeGauge, heartCount, 1, 1);
                    Logging.Log("[PShowCoop] >> Phase2 Step 2b/6: Native vtable[0x518] SetDisplayMode(%d, 1, 1) executed (APShowHUD parity)",
                                heartCount);
                }
                // +0x5DC=0 write targets a DIFFERENT auxiliary widget (not the
                // P1 gauge); the gauge CONSTRUCTOR (0x9d5158) sets +0x5DC=1 and
                // P1's live value is 1; our tail replication was zeroing our
                // widget's ctor-initialized flag.
                if (gaugeEnable != nullptr) {
                    gaugeEnable(sP2LifeGauge, 0xE);
                    Logging.Log("[PShowCoop] >> Phase2 Step 2b/6: Native vtable[0x4E0](0xE) executed (APShowHUD parity)");
                }
            }

            // 3. Set current hearts to 5 and initialize display mode via direct function pointers
            using SetCurrentHeartsFn = void (*)(void*, std::int32_t);
            auto setCurrentHearts = reinterpret_cast<SetCurrentHeartsFn>(mainRange.m_Start + 0x009D569C);
            if (setCurrentHearts != nullptr && !(kPatch0236NativeHeartSequence && kPatch0196CompleteNativeSetup)) {
                setCurrentHearts(sP2LifeGauge, 5);
                Logging.Log("[PShowCoop] >> Phase2 Step 3/6: SetCurrentHearts(5) executed at 0x009D569C (+0x4B8=%u now)",
                            *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x4B8));
            }
            // placed after SetCurrentHearts because +0x4B8 (anim-slot count) is
            // populated somewhere between InitHeartMeshes and here (v2/v3
            // tripped with widgetSlots=0 at the earlier positions).
            if (kDiag0210AnimSetupEnabled) {
                using GetSlotCountFn = std::int32_t (*)(void);
                using FNameConstructFn = void (*)(void*, const char*, std::int32_t);
                using AnimSetupFn = void (*)(void*, std::int32_t, const void*);
                auto getSlotCount = reinterpret_cast<GetSlotCountFn>(mainRange.m_Start + GetSlotCountOffset);
                auto getSlotBase = reinterpret_cast<GetSlotCountFn>(mainRange.m_Start + GetSlotBaseOffset);
                auto makeFName = reinterpret_cast<FNameConstructFn>(mainRange.m_Start + FNameConstructOffset);
                auto animSetup = reinterpret_cast<AnimSetupFn>(mainRange.m_Start + AnimSetupOffset);
                const auto widgetSlotCount =
                    *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x4B8);
                if (kPatch0234NativeCharmSlotIndex) {
                    // Native factory order and indices (0x009A40CC-0x009A4124).
                    const std::int32_t charmCount = getSlotCount();
                    alignas(8) unsigned char fnameBuf[16];
                    makeFName(fnameBuf,
                              reinterpret_cast<const char*>(mainRange.m_Start + HeartCharmStringNso), 1);
                    for (std::int32_t i = 0; i < charmCount; ++i) {
                        const std::int32_t slot = getSlotBase() + i;
                        animSetup(sP2LifeGauge, slot, fnameBuf);
                        Logging.Log("[PShowCoop] PATCH-0234: AnimSetup(gauge, slot=%d = base+%d, 'heartcharm') (charmCount=%d, widgetSlots=%u; slot>=widgetSlots is a native no-op)",
                                    slot, i, charmCount, widgetSlotCount);
                    }
                } else if (IsHeapish(sP2LifeGauge) && widgetSlotCount >= 1 && widgetSlotCount <= 24 &&
                    getSlotCount != nullptr && makeFName != nullptr && animSetup != nullptr) {
                    const std::int32_t bindCount = getSlotCount();
                    if (bindCount >= 1) {
                        alignas(8) unsigned char fnameBuf[16];
                        makeFName(fnameBuf,
                                  reinterpret_cast<const char*>(mainRange.m_Start + HeartCharmStringNso), 1);
                        const std::int32_t bounded =
                            (bindCount <= static_cast<std::int32_t>(widgetSlotCount))
                                ? bindCount
                                : static_cast<std::int32_t>(widgetSlotCount);
                        for (std::int32_t i = 0; i < bounded; ++i) {
                            animSetup(sP2LifeGauge, i, fnameBuf);
                            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTICv4: AnimSetup_9D7570(gauge, slot=%d, 'heartcharm') executed (i=%d of %d, widgetSlots=%u)",
                                        i, i, bounded, widgetSlotCount);
                        }
                    } else {
                        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTICv4: bind count %d < 1, AnimSetup loop skipped", bindCount);
                    }
                } else {
                    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTICv4: guards failed (widgetSlots=%u), AnimSetup loop skipped",
                                widgetSlotCount);
                }
            }

            using SetDisplayModeFn = void (*)(void*, std::int32_t, std::uint32_t, std::uint8_t);
            auto setDisplayMode = reinterpret_cast<SetDisplayModeFn>(mainRange.m_Start + 0x009D5F68);
            if (setDisplayMode != nullptr && !(kPatch0236NativeHeartSequence && kPatch0196CompleteNativeSetup)) {
                // block (weak-resolve +0x4C0 -> Func_01F5DCAC, null-safe) which
                // 1 on P1's visible gauge and 0 on ours.
                setDisplayMode(sP2LifeGauge, 5, kPatch0211Param3Enable ? 1u : 0u, 1);
                Logging.Log("[PShowCoop] >> Phase2 Step 4/6: SetDisplayMode(5, %u, 1) executed at 0x009D5F68",
                            kPatch0211Param3Enable ? 1u : 0u);
            }

            // 4. Native entrance animation activation (0x009D2E94)
            // Sets native UICourseLifeGaugeBase::bHasInAnimation (+0x678) = 1 via engine method
            std::uint8_t preInAnim = *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x678);
            if (setHasInAnimation != nullptr) {
                setHasInAnimation(sP2LifeGauge);
            }
            std::uint8_t postInAnim = *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x678);
            Logging.Log("[PShowCoop] >> Phase2 Step 5/6: Native SetHasInAnimation(sP2LifeGauge) executed at 0x009D2E94: +0x678 before=%u, after=%u",
                        (unsigned)preInAnim, (unsigned)postInAnim);

            // 5. Setup 2D Render Translation (+0x4A0)
            // Statically verified in Ghidra (0x009D5214/0x009D5220), +0x4A0 is FVector2D (X, Y) Translation.
            // proven-invisible +70.0f. Copies P1's live X; Y offset is one constant.
            struct FVector2D { float X; float Y; };
            const auto* p1Bytes = reinterpret_cast<const std::uint8_t*>(p1Gauge);
            auto* gaugeBytes = reinterpret_cast<std::uint8_t*>(sP2LifeGauge);
            const auto* p1Trans = reinterpret_cast<const FVector2D*>(p1Bytes + 0x4A0);
            auto* p2Trans = reinterpret_cast<FVector2D*>(gaugeBytes + 0x4A0);
            p2Trans->X = p1Trans->X;
            p2Trans->Y = p1Trans->Y + (kPatch0233NativeTimings ? 0.0f : kPatch0190YOffset);
            Logging.Log("[PShowCoop] >> Phase2 Step 6/6: RenderTranslation set: X=%.2f, Y=%.2f (P1 was X=%.2f, Y=%.2f)",
                        p2Trans->X, p2Trans->Y, p1Trans->X, p1Trans->Y);
            // The slot is unproven and the writes are the only remaining speculative
            // pointer writes on the gauge; a corrupted +0x28 is the prime suspect for
            // the enemy-touch guest fatal. The AddToViewport context argument below is
            // untouched (native P1 pattern passes its own arg independently).
            void* p2Lp = GetP2LocalPlayer();
            void* p1PlayerCtx = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p1Gauge) + 0x28);
            void* initCtx = IsHeapish(p2Lp) ? p2Lp : (IsHeapish(p1PlayerCtx) ? p1PlayerCtx : nullptr);
            if (!kPatch0193KeepNullPlayerContext && initCtx != nullptr) {
                *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x28) = initCtx;
            } else {
                Logging.Log("[PShowCoop] PATCH-0193: sP2LifeGauge (%p) +0x28 left null (write of %p skipped)",
                            sP2LifeGauge, initCtx);
            }

            // Add P2 LifeGauge to Viewport ONCE (matching native P1 zOrder 14)
            // between P1 (0x08) and ours (0x400) is a creation-context
            // difference that AddToViewport's internal tree construction may
            // read. Copy P1's ObjectFlags dword to our gauge.
            if (kPatch0219ObjectFlags && IsHeapish(p1Gauge) && IsHeapish(sP2LifeGauge)) {
                const std::uint32_t p1Flags = *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<std::uintptr_t>(p1Gauge) + 0x08);
                // Preserve our root-set bits if any, apply P1's creation flags
                *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x08) =
                    p1Flags;
                Logging.Log("[PShowCoop] PATCH-0219: ObjectFlags copied (P1=0x%08X)", p1Flags);
            }
            // P1's tree and therefore P1's built SWidget instances, which is
            // the identity collision. Invalidate the cached widget pairs on
            // our gauge AND the tree root UWidget so AddToViewport's
            // TakeWidget rebuilds a FRESH SWidget tree for our gauge through
            // the engine's own rebuild path (vtbl+0x2C0 -> 0x3e89580).
            // Offsets proven: gauge+0xD8/+0xE0 MyWidget and +0xE8/+0xF0
            // CachedWidget (0x3e89580 decompile); tree+0x28 root (0x3ea3fbc);
            // the root UWidget shares the UWidget base layout.
            if (kPatch0224FreshRebuild && IsHeapish(p1Gauge) && IsHeapish(sP2LifeGauge)) {
                void* treeObj = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x1D8);
                if (IsHeapish(treeObj)) {
                    void* treeOuter = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(treeObj) + 0x20);
                    void* treeRoot = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(treeObj) + 0x28);
                    Logging.Log("[PShowCoop] PATCH-0224 DIAG: tree=%p outer=%p (sP1=%p, outer==sP1: %s) root=%p",
                                treeObj, treeOuter, sP1LifeGauge,
                                (treeOuter == sP1LifeGauge) ? "YES" : "NO", treeRoot);
                    if (IsHeapish(treeRoot)) {
                        void* rootVt = *reinterpret_cast<void**>(treeRoot);
                        Logging.Log("[PShowCoop] PATCH-0224 DIAG: root=%p rootVt=NSO 0x%llx",
                                    treeRoot,
                                    (unsigned long long)(IsHeapish(rootVt) ?
                                        (reinterpret_cast<std::uintptr_t>(rootVt) - mainRange.m_Start) : 0));
                        std::memset(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(treeRoot) + 0xD8), 0, 32);
                    }
                    std::memset(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0xD8), 0, 32);
                    Logging.Log("[PShowCoop] PATCH-0224: cached widget pairs invalidated (gauge +0xD8..+0xF0, root +0xD8..+0xF0); AddToViewport will rebuild fresh");
                }
            }

            // PATCH-0233: AddToScreen's Player arg selects the branch at 0x03EA5790.
            // Non-null -> AddViewportWidgetForPlayer (0x04514F04, GameLayerManager
            // player layer); null -> AddViewportWidgetContent (0x04514BF4) into
            // ViewportOverlayWidget (vpc+0x100), where P1's content lives.
            addToViewport(sP2LifeGauge, kPatch0233NoRemoveFromViewport ? nullptr : initCtx, 14);
            // construction builds a generic empty SWidget for our gauge (class
            // After AddToViewport, overwrite our +0x238/+0x240 SWidget pair with
            // P1's SWidget pair so both gauges render P1's painted tree. This
            // is a shared-tree diagnostic to prove the rendering pipeline works.
            if (kPatch0220SharedTree && IsHeapish(p1Gauge) && IsHeapish(sP2LifeGauge)) {
                const auto p1Sw = *reinterpret_cast<std::uint64_t*>(
                    reinterpret_cast<std::uintptr_t>(p1Gauge) + 0x238);
                const auto p1SwCtrl = *reinterpret_cast<const std::uint64_t*>(
                    reinterpret_cast<std::uintptr_t>(p1Gauge) + 0x240);
                auto gaugeBytes = reinterpret_cast<std::uint8_t*>(sP2LifeGauge);
                *reinterpret_cast<std::uint64_t*>(gaugeBytes + 0x238) = p1Sw;
                *reinterpret_cast<std::uint64_t*>(gaugeBytes + 0x240) = p1SwCtrl;
                Logging.Log("[PShowCoop] PATCH-0220: shared-tree diagnostic - SWidget pair copied from P1 (sw=0x%llx ctrl=0x%llx)",
                            (unsigned long long)p1Sw, (unsigned long long)p1SwCtrl);
            }
            // clear + re-attach. TakeWidget (0x3ea2f04) returns the cached
            // SWidget at +0x258, else the CONTENT ref at +0x248, else the
            // outer-chain fallback (which gave us the generic empty tree -
            // the archetype's (P1's) content tree: copy P1's +0x248/+0x24C
            // content ref, clear our stale +0x258 cache, and re-run
            // AddToViewport so TakeWidget rebuilds from the real content.
            if (kPatch0217ContentRef && IsHeapish(p1Gauge) && IsHeapish(sP2LifeGauge)) {
                const std::uint32_t p1Idx = *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<std::uintptr_t>(p1Gauge) + 0x248);
                const std::uint32_t p1Ser = *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<std::uintptr_t>(p1Gauge) + 0x24C);
                auto gaugeBytes = reinterpret_cast<std::uint8_t*>(sP2LifeGauge);
                if (p1Idx != 0xFFFFFFFFu) {
                    // clear the TakeWidget cache (weak-null pair at +0x258)
                    *reinterpret_cast<std::uint32_t*>(gaugeBytes + 0x258) = 0xFFFFFFFFu;
                    *reinterpret_cast<std::uint32_t*>(gaugeBytes + 0x25C) = 0xFFFFFFFFu;
                    // assign the archetype content ref (+0x248 index / +0x24C serial)
                    *reinterpret_cast<std::uint32_t*>(gaugeBytes + 0x248) = p1Idx;
                    *reinterpret_cast<std::uint32_t*>(gaugeBytes + 0x24C) = p1Ser;
            // between P1 (0x08) and ours (0x400) is a creation-context
            // difference that AddToViewport's internal tree construction may
            // read. Copy P1's ObjectFlags dword to our gauge.
            if (kPatch0219ObjectFlags && IsHeapish(p1Gauge) && IsHeapish(sP2LifeGauge)) {
                const std::uint32_t p1Flags = *reinterpret_cast<const std::uint32_t*>(
                    reinterpret_cast<std::uintptr_t>(p1Gauge) + 0x08);
                // Preserve our root-set bits if any, apply P1's creation flags
                *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x08) =
                    p1Flags;
                Logging.Log("[PShowCoop] PATCH-0219: ObjectFlags copied (P1=0x%08X)", p1Flags);
            }

            addToViewport(sP2LifeGauge, initCtx, 14);
                    Logging.Log("[PShowCoop] PATCH-0217: content ref assigned (idx=0x%x ser=0x%x), cache cleared, re-attached",
                                p1Idx, p1Ser);
                } else {
                    Logging.Log("[PShowCoop] PATCH-0214x: P1 content ref invalid (0x%x), content assignment skipped", p1Idx);
                }
            }

            // PATCH-0233: end like native gauge #0 (SetShown(true), 0x009A4130) and
            // never call 0x03EA5CC0 (RemoveFromViewport) on our gauge.
            if (setActive != nullptr) {
                setActive(sP2LifeGauge, kPatch0233ShowAtSpawn);
            }
            if (!kPatch0233NoRemoveFromViewport && setVisibility != nullptr) {
                setVisibility(sP2LifeGauge, 0); // pre-0233 behaviour: actually RemoveFromViewport
            }
            {
                auto isInViewport = reinterpret_cast<IsInViewportFn>(mainRange.m_Start + IsInViewportOffset);
                const auto gaugeAddr = reinterpret_cast<std::uintptr_t>(sP2LifeGauge);
                Logging.Log("[PShowCoop] PATCH-0233 spawn: IsInViewport=%d FullScreenWidget=%p ctrl=%p shownAtSpawn=%d noRemove=%d",
                            isInViewport(sP2LifeGauge) ? 1 : 0,
                            *reinterpret_cast<void**>(gaugeAddr + 0x238),
                            *reinterpret_cast<void**>(gaugeAddr + 0x240),
                            kPatch0233ShowAtSpawn ? 1 : 0, kPatch0233NoRemoveFromViewport ? 1 : 0);
            }
            // by passing nullptr as params — the UFunction has a params struct
            // size > 0 and ProcessEvent memcpy's from the caller's buffer into
            // the VM frame. nullptr as the source = crash.
            // Fix: allocate a zeroed params buffer of the UFunction's params
            // size (read from UFunction+0x58) and pass that.
            if (kPatch0221ConstructionScript) {
                void* bpClass = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x10);
                if (IsHeapish(bpClass)) {
                    using ProcessEventFn = void (*)(void*, void*, void*);
                    auto processEvent = reinterpret_cast<ProcessEventFn>(mainRange.m_Start + ProcessEventOffset);
                    // Read the chain head at bpClass+0x48
                    void* ufunc = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(bpClass) + 0x48);
                    for (int i = 0; i < 3 && ufunc != nullptr; ++i) {
                        // Read the UFunction's params size from +0x58
                        const std::int32_t paramsSize = *reinterpret_cast<const std::int32_t*>(
                            reinterpret_cast<std::uintptr_t>(ufunc) + 0x58);
                        Logging.Log("[PShowCoop] PATCH-0221v3: ProcessEvent call %d: gauge=%p ufunc=%p paramsSize=%d",
                                    i, sP2LifeGauge, ufunc, paramsSize);
                        if (paramsSize > 0 && paramsSize < 0x1000) {
                            // Allocate a zeroed params buffer on the stack
                            alignas(16) std::uint8_t paramsBuf[0x1000];
                            std::memset(paramsBuf, 0, sizeof(paramsBuf));
                            processEvent(sP2LifeGauge, ufunc, paramsBuf);
                        } else {
                            // No params or oversized — pass a small zeroed buffer
                            alignas(16) std::uint8_t paramsBuf[64] = {};
                            processEvent(sP2LifeGauge, ufunc, paramsBuf);
                        }
                        // Walk to the next UFunction via UField::Next (+0x28)
                        ufunc = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(ufunc) + 0x28);
                        if (!IsHeapish(ufunc)) break;
                    }
                    // After calling the Blueprint functions, re-attach so AddToViewport
                    // constructs the SWidget from the (potentially newly-built) tree
                    Logging.Log("[PShowCoop] PATCH-0221v3: re-attaching after Blueprint function calls");
            // (gdb_gauge_diff_20260912-155231) found that the gauge widget's
            // +0xE8 field is the ONLY field with a DIFFERENT CLASS between
            // P1 (vt 0x9c61d88 = the real gauge content widget) and ours
            // (vt 0x9b40190 = the generic SWidget). This field is read by
            // func_0x3e89580 (AddToViewport tree construction) to build the
            // SWidget tree. The fix: copy P1's +0xE8 pointer to ours.
            if (kPatch0223WidgetTreePtr && IsHeapish(p1Gauge) && IsHeapish(sP2LifeGauge)) {
                const std::uint64_t p1TreePtr = *reinterpret_cast<const std::uint64_t*>(
                    reinterpret_cast<std::uintptr_t>(p1Gauge) + 0xE8);
                *reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0xE8) =
                    p1TreePtr;
                // Also copy +0xF0 (the paired sub-object)
                *reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0xF0) =
                    *reinterpret_cast<const std::uint64_t*>(
                        reinterpret_cast<std::uintptr_t>(p1Gauge) + 0xF0);
                Logging.Log("[PShowCoop] PATCH-0223: WidgetTree ptr copied: +0xE8=0x%llx +0xF0=0x%llx",
                            (unsigned long long)p1TreePtr,
                            (unsigned long long)*reinterpret_cast<const std::uint64_t*>(
                                reinterpret_cast<std::uintptr_t>(p1Gauge) + 0xF0));
            }

            addToViewport(sP2LifeGauge, initCtx, 14);
                    // Log the SWidget class to check if it changed
                    void* sw = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x238);
                    void* swVt = (IsHeapish(sw)) ? *reinterpret_cast<void**>(sw) : nullptr;
                    std::uintptr_t swVtNso = (IsHeapish(swVt)) ?
                        reinterpret_cast<std::uintptr_t>(swVt) - mainRange.m_Start : 0;
                    Logging.Log("[PShowCoop] PATCH-0221v3: after Blueprint calls: SWidget=%p class=0x%llx",
                                sw, (unsigned long long)(reinterpret_cast<std::uintptr_t>(swVtNso)));
                }
            }
            // diff (gdb_gauge_diff_20260911-222313) proved the two SWidgets are
            // 98.2% identical (504/512 bytes) — ONLY the vtable pointer and
            // self-referencing pointers differ. The structural data from +0x28
            // onward is byte-identical. This means the AddToViewport construction
            // produces the SAME SWidget structure for both gauges but with the
            // WRONG VTABLE CLASS for ours (0x9b40190 vs P1's 0x9b58738).
            // The fix: after AddToViewport, read our SWidget (from +0x238) and
            // overwrite its VTABLE POINTER with P1's SWidget vtable. This changes
            // the virtual dispatch to the gauge's specialized class — OnPaint,
            // OnArrangeChildren, etc. — using our widget's own data.
            if (kPatch0221VtableSwap && IsHeapish(p1Gauge) && IsHeapish(sP2LifeGauge)) {
                void* ourSW = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x238);
                void* p1SW = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p1Gauge) + 0x238);
                if (IsHeapish(ourSW) && IsHeapish(p1SW)) {
                    void* p1Vt = *reinterpret_cast<void**>(p1SW);
                    void* ourVt = *reinterpret_cast<void**>(ourSW);
                    if (p1Vt != ourVt) {
                        *reinterpret_cast<void**>(ourSW) = p1Vt;
                        Logging.Log("[PShowCoop] PATCH-0221v2: SWIDGET VTABLE SWAPPED: ourSW=%p old_vt=%p -> new_vt=%p (P1's class)",
                                    ourSW, ourVt, p1Vt);
                    } else {
                        Logging.Log("[PShowCoop] PATCH-0221v2: SWidget vtables already match, no swap needed");
                    }
                }
            }

            Logging.Log("[PShowCoop] SUCCESS: Dedicated P2 LifeGauge_C instantiated & attached to viewport: %p (ctx=%p, zOrder=14)",
                        sP2LifeGauge, initCtx);
            if (kDiag0212FadeStateEnabled) {
                const auto* fb = reinterpret_cast<const std::uint8_t*>(sP2LifeGauge);
                float fade = 0.0f;
                std::memcpy(&fade, fb + 0x520, sizeof(float));
                Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC spawn-complete fade-state: +0x550=%u +0x520=%.3f +0x5DC=%u +0x678=%u states(A=%d,B=%d) dmgQ=%u",
                            fb[0x550], static_cast<double>(fade), fb[0x5DC], fb[0x678],
                            *reinterpret_cast<const std::int32_t*>(fb + 0x524),
                            *reinterpret_cast<const std::int32_t*>(fb + 0x528),
                            *reinterpret_cast<const std::uint32_t*>(fb + 0x560));
            }
            const auto* p2Bytes = reinterpret_cast<const std::uint8_t*>(sP2LifeGauge);
            std::uint8_t p2InAnim = p2Bytes[0x678];
            std::uint32_t p2Count = *reinterpret_cast<const std::uint32_t*>(p2Bytes + 0x4B8);
            std::uint32_t p2Cap = *reinterpret_cast<const std::uint32_t*>(p2Bytes + 0x4BC);
            std::uint64_t p2T0 = *reinterpret_cast<const std::uint64_t*>(p2Bytes + 0x4D8);
            std::uint64_t p2T1 = *reinterpret_cast<const std::uint64_t*>(p2Bytes + 0x4E0);
            std::uint64_t p2T2 = *reinterpret_cast<const std::uint64_t*>(p2Bytes + 0x4E8);
            std::uint64_t p2T3 = *reinterpret_cast<const std::uint64_t*>(p2Bytes + 0x4F0);
            std::uint64_t p2T4 = *reinterpret_cast<const std::uint64_t*>(p2Bytes + 0x4F8);
            std::uint64_t p2T5 = *reinterpret_cast<const std::uint64_t*>(p2Bytes + 0x500);
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC P2 LifeGauge Initial State: gauge=%p, +0x678(bHasInAnim)=%u, +0x4B8(count)=%u, +0x4BC(cap)=%u",
                        sP2LifeGauge, (unsigned)p2InAnim, (unsigned)p2Count, (unsigned)p2Cap);
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC P2 Tracks: [Heal=0x%llx, Dmg=0x%llx, Max=0x%llx, Alrt=0x%llx, Dest=0x%llx, Neut=0x%llx]",
                        (unsigned long long)p2T0, (unsigned long long)p2T1, (unsigned long long)p2T2,
                        (unsigned long long)p2T3, (unsigned long long)p2T4, (unsigned long long)p2T5);
        } else {
            Logging.Log("[PShowCoop] ERROR: CreateWidgetOfClass returned invalid pointer %p", newGauge);
        }
    } else {
        Logging.Log("[PShowCoop] ERROR: Invalid widgetClass (%p) or outer (%p)", widgetClass, outer);
    }

    sIsSpawningP2Widget = false;
}
// dt) is the gauge's per-frame fade driver (prologue-verified: +0x530=dt,
// +0x520+=dt, state-machine continuation via 0x9f673c, mesh-slot iteration).
// The HUD drives it for P1's gauge every frame; our gauge was never in that
// native call to P2's gauge whenever it arrives for P1's.
// x1=object, x2=frame, x3=result; a real entry with a PC-relative-free stolen
// prologue. Mirror the stack depth so that, when a LifeGauge reaches
// AddToViewport, the enclosing Blueprint UFunction is known for a later
// ProcessEvent attempt. Pure reads; no behavior change.
static void* sUfInvokeStack[16];
static void* sUfInvokeObjStack[16];
static int sUfInvokeDepth = 0;
static unsigned long sUfInvokeTotal = 0;
static unsigned long sWidgetFunnelCalls = 0;
static unsigned long sWidgetInnerCalls = 0;
static bool sDiag0216CaptureDone = false;

HOOK_DEFINE_TRAMPOLINE(CaptureUfInvoke) {
    static void Callback(void* ufunc, void* object, void* frame, void* result) {
        const int depth = sUfInvokeDepth;
        sUfInvokeTotal++;
        if (kDiag0216UfCapture && sUfInvokeTotal == 1) {
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC first UFunction invoke seen: uf=%p obj=%p", ufunc, object);
        }
        if (depth < 16) {
            sUfInvokeStack[depth] = ufunc;
            sUfInvokeObjStack[depth] = object;
            sUfInvokeDepth = depth + 1;
        }
        Orig(ufunc, object, frame, result);
        sUfInvokeDepth = depth;
    }
};

static void LogUfInvokeStack(const char* tag) {
    if (!kDiag0216UfCapture) return;
    const int depth = (sUfInvokeDepth < 16) ? sUfInvokeDepth : 16;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC stack (%s): depth=%d totalUfInvokes=%lu funnelCalls=%lu innerCalls=%lu",
                tag, sUfInvokeDepth, sUfInvokeTotal, sWidgetFunnelCalls, sWidgetInnerCalls);
    for (int i = 0; i < depth; ++i) {
        void* uf = sUfInvokeStack[i];
        void* ob = sUfInvokeObjStack[i];
        const std::uint32_t nameIdx = IsHeapish(uf)
            ? *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(uf) + 0x18) : 0;
        const std::int32_t parmsSize = IsHeapish(uf)
            ? *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(uf) + 0x58) : 0;
        void* obClass = IsHeapish(ob)
            ? *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(ob) + 0x10) : nullptr;
        const std::uint32_t obNameIdx = IsHeapish(ob)
            ? *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(ob) + 0x18) : 0;
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC   [%d] uf=%p ufNameIdx=%u parmsSize=%d | obj=%p objNameIdx=%u objClass=%p",
                    i, uf, nameIdx, parmsSize, ob, obNameIdx, obClass);
    }
}
// session (sub sp,sp,#0x180; ...; stp x29,x30,[sp,#0x130]; add x29,sp,#0x130),
// with a relocatable first instruction. Because the VM calls it inside the
// creating UFunction, this reveals the HUD Blueprint function that builds P1's
// gauge. Read-only; filters on the produced widget's vtable == LifeGauge.
using CreateWidgetWrapperFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*);
HOOK_DEFINE_TRAMPOLINE(CaptureWidgetCreate) {
    static void* Callback(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
        sWidgetFunnelCalls++;
        void* result = Orig(a0, a1, a2, a3, a4, a5, a6, a7);
        if (kDiag0216UfCapture && IsHeapish(result)) {
            const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
            const auto vt = *reinterpret_cast<std::uintptr_t*>(result);
            if (mainRange.InRange(vt) && (vt - mainRange.m_Start) == LifeGaugeVtableNso) {
                Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC gauge created via native funnel: new=%p", result);
                LogUfInvokeStack("gauge-create-funnel");
            }
        }
        return result;
    }
};
// prologue verified this session: sub sp,sp,#0x90; stp x29,x30,[sp,#0x40]; ...).
// Catches any creation path that bypasses the funnel wrapper. Our own P2 spawn
// uses this leaf directly and is excluded via sIsSpawningP2Widget.
using CreateWidgetOfClassRawFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*);
HOOK_DEFINE_TRAMPOLINE(CaptureWidgetCreateInner) {
    static void* Callback(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
        sWidgetInnerCalls++;
        void* result = Orig(a0, a1, a2, a3, a4, a5, a6, a7);
        if (kDiag0216UfCapture && !sIsSpawningP2Widget && IsHeapish(result)) {
            const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
            const auto vt = *reinterpret_cast<std::uintptr_t*>(result);
            if (mainRange.InRange(vt) && (vt - mainRange.m_Start) == LifeGaugeVtableNso) {
                Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC gauge created via inner leaf: new=%p", result);
                LogUfInvokeStack("gauge-create-inner");
            }
        }
        return result;
    }
};
// constructed object carries the LifeGauge vtable, log the calling NSO offset.
// so the direct allocator caller is the native creator. Pass-through only.
using StaticConstructFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*);

static void NoteGaugeAlloc(const char* site, void* obj, std::uintptr_t allocCaller) {
    if (!kDiag0217AllocCapture || sIsSpawningP2Widget || !IsHeapish(obj)) return;
    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    const auto vt = *reinterpret_cast<std::uintptr_t*>(obj);
    if (!(mainRange.InRange(vt) && (vt - mainRange.m_Start) == LifeGaugeVtableNso)) return;
    const auto raNso = mainRange.InRange(allocCaller) ? (allocCaller - mainRange.m_Start) : 0;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC LifeGauge alloc via %s: obj=%p allocCallerNSO=0x%lx",
                site, obj, static_cast<unsigned long>(raNso));
}

HOOK_DEFINE_TRAMPOLINE(CaptureAllocInternal) {
    static void* Callback(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        void* result = Orig(a0, a1, a2, a3, a4, a5, a6, a7);
        NoteGaugeAlloc("StaticConstructObject-internal", result, ra);
        return result;
    }
};

HOOK_DEFINE_TRAMPOLINE(CaptureAllocPublic) {
    static void* Callback(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        void* result = Orig(a0, a1, a2, a3, a4, a5, a6, a7);
        NoteGaugeAlloc("StaticConstructObject-public", result, ra);
        return result;
    }
};

// P2's pawn location read exactly as the native fade tick reads player 0's
// (0x009D665C..0x009D66A4: pawn+0x130 root component, +0x1D0/+0x1D4/+0x1D8).
static bool ReadP2PawnLocation(float out[3]) {
    void* p2Pawn = GetP2Pawn();
    if (!IsHeapish(p2Pawn)) return false;
    void* root = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p2Pawn) + 0x130);
    if (!IsHeapish(root)) return false;
    const auto rootAddr = reinterpret_cast<std::uintptr_t>(root);
    out[0] = *reinterpret_cast<const float*>(rootAddr + 0x1D0);
    out[1] = *reinterpret_cast<const float*>(rootAddr + 0x1D4);
    out[2] = *reinterpret_cast<const float*>(rootAddr + 0x1D8);
    return true;
}

// PATCH-0235: every native positioning of P2's gauge uses P2's pawn.
HOOK_DEFINE_TRAMPOLINE(P2GaugePositionSource) {
    static void Callback(void* gauge, const float* loc) {
        float p2Loc[3];
        if (kPatch0235P2PositionSource && gauge != nullptr && gauge == sP2LifeGauge &&
            ReadP2PawnLocation(p2Loc)) {
            Orig(gauge, p2Loc);
            return;
        }
        Orig(gauge, loc);
    }
};

// PATCH-0233: anchor P2's gauge to P2's pawn through the gauge's own native
// screen-position routine (see kPatch0233FollowP2Pawn).
static void AnchorP2GaugeToP2Pawn() {
    static unsigned long sAnchorCalls = 0;
    void* p2Pawn = GetP2Pawn();
    if (!IsHeapish(p2Pawn) || !IsHeapish(sP2LifeGauge)) return;
    const auto* gaugeBytes = reinterpret_cast<const std::uint8_t*>(sP2LifeGauge);
    if (gaugeBytes[0x5DC] == 0) return; // native follow flag (0x009D663C)
    float loc[3];
    if (!ReadP2PawnLocation(loc)) return;
    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    auto updateScreenPos = reinterpret_cast<GaugeUpdateScreenPosFn>(mainRange.m_Start + GaugeUpdateScreenPosOffset);
    updateScreenPos(sP2LifeGauge, loc);
    ++sAnchorCalls;
    if (sAnchorCalls <= 3 || sAnchorCalls % 1800 == 0) {
        Logging.Log("[PShowCoop] PATCH-0233 anchor #%lu: P2 pawn=%p loc=(%.1f, %.1f, %.1f) screen(+0x5E4)=(%.1f, %.1f)",
                    sAnchorCalls, p2Pawn, loc[0], loc[1], loc[2],
                    *reinterpret_cast<const float*>(gaugeBytes + 0x5E4),
                    *reinterpret_cast<const float*>(gaugeBytes + 0x5E8));
    }
}

HOOK_DEFINE_TRAMPOLINE(MirrorFadeTick) {
    static void Callback(void* gauge, float delta) {
        if (kPatch0216FrameTickMirror && gauge == sP1LifeGauge && IsHeapish(sP2LifeGauge)) {
            Orig(sP2LifeGauge, delta);
            if (kPatch0233FollowP2Pawn) {
                AnchorP2GaugeToP2Pawn();
            }
        }
        Orig(gauge, delta);
    }
};

HOOK_DEFINE_TRAMPOLINE(CaptureAddToViewport) {
    static void Callback(void* widget, void* context, std::int32_t zOrder) {
        const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
        const std::uintptr_t objVt =
            IsHeapish(widget) ? *reinterpret_cast<std::uintptr_t*>(widget) : 0;

        if (mainRange.InRange(objVt)) {
            const std::uintptr_t vtOffset = objVt - mainRange.m_Start;
            if (vtOffset == LifeGaugeVtableNso) {
                // If this is our own spawned P2 widget or we are currently spawning, pass through immediately!
                if (widget == sP2LifeGauge || sIsSpawningP2Widget) {
                    Orig(widget, context, zOrder);
                    return;
                }
                // time P1's gauge is added to the viewport (sP1LifeGauge may
                // already be set by DiagOnLifeChanged, so do not gate on null).
                if (kDiag0216UfCapture && !sDiag0216CaptureDone &&
                    (widget == sP1LifeGauge || sP1LifeGauge == nullptr)) {
                    sDiag0216CaptureDone = true;
                    LogUfInvokeStack("p1-addtoviewport");
                    // Caller of AddToViewport -> the native code that attached P1's
                    // gauge (NSO offset where in-image). Only frame 0 is portable.
                    const auto raRaw0 = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
                    const auto raNso = mainRange.InRange(raRaw0) ? (raRaw0 - mainRange.m_Start) : 0;
                    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC p1-add caller NSO: 0x%lx",
                                (unsigned long)raNso);
                }

                if (sP1LifeGauge == nullptr) {
                    sP1LifeGauge = widget;
                    Logging.Log("[PShowCoop] Level P1 LifeGauge_C active in viewport: %p (outer(+0x20)=%p, ctx(+0x28)=%p, translation(+0x4a0)=0x%llx, context=%p, zOrder=%d)",
                                widget,
                                *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(widget) + 0x20),
                                *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(widget) + 0x28),
                                *reinterpret_cast<unsigned long long*>(reinterpret_cast<std::uintptr_t>(widget) + 0x4A0),
                                context,
                                zOrder);
                    const auto* p1Bytes = reinterpret_cast<const std::uint8_t*>(widget);
                    std::uint8_t p1InAnim = p1Bytes[0x678];
                    std::uint32_t p1Count = *reinterpret_cast<const std::uint32_t*>(p1Bytes + 0x4B8);
                    std::uint32_t p1Cap = *reinterpret_cast<const std::uint32_t*>(p1Bytes + 0x4BC);
                    std::uint64_t p1T0 = *reinterpret_cast<const std::uint64_t*>(p1Bytes + 0x4D8);
                    std::uint64_t p1T1 = *reinterpret_cast<const std::uint64_t*>(p1Bytes + 0x4E0);
                    std::uint64_t p1T2 = *reinterpret_cast<const std::uint64_t*>(p1Bytes + 0x4E8);
                    std::uint64_t p1T3 = *reinterpret_cast<const std::uint64_t*>(p1Bytes + 0x4F0);
                    std::uint64_t p1T4 = *reinterpret_cast<const std::uint64_t*>(p1Bytes + 0x4F8);
                    std::uint64_t p1T5 = *reinterpret_cast<const std::uint64_t*>(p1Bytes + 0x500);
                    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC P1 LifeGauge Initial State: gauge=%p, +0x678(bHasInAnim)=%u, +0x4B8(count)=%u, +0x4BC(cap)=%u",
                                widget, (unsigned)p1InAnim, (unsigned)p1Count, (unsigned)p1Cap);
                    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC P1 Tracks: [Heal=0x%llx, Dmg=0x%llx, Max=0x%llx, Alrt=0x%llx, Dest=0x%llx, Neut=0x%llx]",
                                (unsigned long long)p1T0, (unsigned long long)p1T1, (unsigned long long)p1T2,
                                (unsigned long long)p1T3, (unsigned long long)p1T4, (unsigned long long)p1T5);
                }

                Orig(widget, context, zOrder);

                // Spawn dedicated P2 widget strictly ONCE per level load
                if (sP2LifeGauge == nullptr) {
                    SpawnDedicatedP2LifeGauge(widget);
                }
                return;
            }
        }

        Orig(widget, context, zOrder);
    }
};

constexpr ptrdiff_t OnLifeChangedOffset = 0x009D5300;
constexpr ptrdiff_t MulticastDelegateAddOffset = 0x01D208F0;

using OnLifeChangedFn = void (*)(void*);
using MulticastDelegateAddFn = void (*)(void*, void*, void*, void*);

static void* sBoundP2DamageComp = nullptr;
// PATCH-0239: file scope so a course restart can clear it (a new controller at
// the freed one's address would otherwise skip the P2 HUD binding).
static void* sLastBoundP2Controller = nullptr;

static void NativeP2DamageHUDCallback(void* listenerLifeGauge, void* damageParams, void* result) {
    if (listenerLifeGauge != nullptr && listenerLifeGauge == sP2LifeGauge) {
        Logging.Log("[PShowCoop] NativeP2DamageHUDCallback: Delegate fired on sP2LifeGauge (%p)", listenerLifeGauge);
        TriggerP2DamageHUD();
    }
}

HOOK_DEFINE_TRAMPOLINE(DiagOnLifeChanged) {
    static void Callback(void* gauge) {
        const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
        auto caller0 = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        std::uintptr_t off0 = mainRange.InRange(caller0) ? (caller0 - mainRange.m_Start) : 0;

        if (sP1LifeGauge == nullptr && gauge != sP2LifeGauge && IsHeapish(gauge)) {
            sP1LifeGauge = gauge;
        }

        const char* name = (gauge == sP1LifeGauge) ? "sP1LifeGauge" :
                           (gauge == sP2LifeGauge) ? "sP2LifeGauge" : "UNKNOWN_GAUGE";

        static void* sLastDiagGauge = nullptr;
        static std::uintptr_t sLastDiagCaller = 0;
        static int sDiagCount = 0;
        if (gauge != sLastDiagGauge || off0 != sLastDiagCaller || sDiagCount < 10) {
            sLastDiagGauge = gauge;
            sLastDiagCaller = off0;
            if (sDiagCount < 25) {
                sDiagCount++;
                // could shatter without a fed queue (closes the stale-queue loophole).
                const auto* bytes = IsHeapish(gauge) ? reinterpret_cast<const std::uint8_t*>(gauge) : nullptr;
                std::uint32_t dmgDepth = bytes ? *reinterpret_cast<const std::uint32_t*>(bytes + 0x560) : 0;
                std::uint32_t healDepth = bytes ? *reinterpret_cast<const std::uint32_t*>(bytes + 0x570) : 0;
                Logging.Log("[PShowCoop] DIAG OnLifeChanged: gauge=%p (%s) caller=NSO 0x%lx (count=%d) dmgQ=%u healQ=%u",
                            gauge, name, (unsigned long)off0, sDiagCount,
                            (unsigned)dmgDepth, (unsigned)healDepth);
            }
        }

        // PATCH-0204: never execute native code on a provably dead receiver. P1 and
        // unknown gauges always pass through untouched; only torn sP2 is skipped,
        // and the skip is logged explicitly (never silent). Rationale recorded on
        if (kPatch0204GuardDeadWidget && gauge == sP2LifeGauge && !GaugeVtableIntact(gauge)) {
            Logging.Log("[PShowCoop] PATCH-0204: OnLifeChanged Orig skipped, sP2LifeGauge (%p) torn", gauge);
            return;
        }

        Orig(gauge);
    }
};

HOOK_DEFINE_TRAMPOLINE(CaptureInitPlayerHud) {
    static void* Callback(void* pawn, void* hudClass) {
        Logging.Log("[PShowCoop] Native InitPlayerHud (0x01EF79F8) called: pawn=%p, hudClass=%p",
                    pawn, hudClass);
        if (sP1HudClass == nullptr && hudClass != nullptr) {
            sP1HudClass = hudClass;
            Logging.Log("[PShowCoop] Captured authentic native HudClass: %p", sP1HudClass);
        }
        void* result = Orig(pawn, hudClass);
        Logging.Log("[PShowCoop] Native InitPlayerHud completed: hudContext=%p for pawn=%p",
                    result, pawn);
        return result;
    }
};

HOOK_DEFINE_TRAMPOLINE(CaptureHudLookup) {
    static void* Callback(void* comp) {
        if (IsPlayer2StatusComponent(comp)) {
            // handles a null HudLookup result by early return (decomp-proven),
            // which removes the virtual dispatch on the decayed +0x38 linkage.
            // P1 comps always pass through untouched below.
            if (kPatch0206NullP2HudLookup) {
                Logging.Log("[PShowCoop] PATCH-0206: HudLookup P2 comp (%p) routed to null (absence path)", comp);
                return nullptr;
            }
            if (IsHeapish(sP2HudContext)) {
                Logging.Log("[PShowCoop] HudLookup (0x01EF7EB0): Routed P2 UPlayerStatusComponent (%p) to dedicated P2 HUD context (%p)",
                            comp, sP2HudContext);
                return sP2HudContext;
            }
        }
        return Orig(comp);
    }
};

} // namespace

void BindP2ControllerHUD(void* p2Controller, void* p2LocalPlayer) {
    if (!IsHeapish(p2Controller) || !IsHeapish(sP2LifeGauge)) return;

    if (p2Controller == sLastBoundP2Controller) return;
    sLastBoundP2Controller = p2Controller;

    if (p2LocalPlayer == nullptr) {
        p2LocalPlayer = GetP2LocalPlayer();
    }
    // it to the P2 PlayerController. P1's live outer never changes and P1 renders;
    // the rewrite may detach sP2LifeGauge from the Slate tree AddToViewport put it in.
    void** outerSlot = reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x20);
    if (kPatch0188KeepCreationOuter) {
        Logging.Log("[PShowCoop] PATCH-0188: sP2LifeGauge (%p) Outer preserved as %p (rewrite to %p skipped)",
                    sP2LifeGauge, *outerSlot, p2Controller);
    } else if (*outerSlot != p2Controller) {
        void* oldOuter = *outerSlot;
        *outerSlot = p2Controller;
        Logging.Log("[PShowCoop] sP2LifeGauge (%p) Outer updated from %p to %p",
                    sP2LifeGauge, oldOuter, p2Controller);
    }
    // Same unproven slot as the spawn-time write above; P1 renders with null.
    if (!kPatch0193KeepNullPlayerContext && IsHeapish(p2LocalPlayer)) {
        void** playerCtxSlot = reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x28);
        *playerCtxSlot = p2LocalPlayer;
    } else {
        Logging.Log("[PShowCoop] PATCH-0193: sP2LifeGauge (%p) +0x28 left null at bind (write of %p skipped)",
                    sP2LifeGauge, p2LocalPlayer);
    }

    Logging.Log("[PShowCoop] BindP2ControllerHUD: Bound sP2LifeGauge=%p outer(+0x20)=%p playerCtx(+0x28)=%p",
                sP2LifeGauge,
                *reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x20),
                IsHeapish(p2LocalPlayer) ? p2LocalPlayer
                    : *reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x28));
    // Nonzero-vs-zero decides whether Slot-1 refusal happens at the identity gate.
    // Runs once per controller bind (guarded above); pure observation.
    {
        const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
        auto identityOf = reinterpret_cast<IdentityResolverFn>(mainRange.m_Start + IdentityResolverOffset);
        void* p1Pawn = GetP1Pawn();
        void* p2Pawn = GetP2Pawn();
        void* p1Ident = (identityOf != nullptr && IsHeapish(p1Pawn)) ? identityOf(p1Pawn, 1) : nullptr;
        void* p2Ident = (identityOf != nullptr && IsHeapish(p2Pawn)) ? identityOf(p2Pawn, 1) : nullptr;
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC Identity: P1Pawn=%p ident=%p | P2Pawn=%p ident=%p",
                    p1Pawn, p1Ident, p2Pawn, p2Ident);
    }

    RegisterP2HudPlayer(p2Controller, p2LocalPlayer);
}

void InspectHudRegistry() {
    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    auto resolveWeakPtr = reinterpret_cast<ResolveWeakPtrFn>(mainRange.m_Start + ResolveWeakPtrOffset);
    std::uintptr_t regAddr = mainRange.m_Start + HudRegistryArrayOffset;
    void* slot0Pawn = resolveWeakPtr ? resolveWeakPtr(reinterpret_cast<void*>(regAddr)) : nullptr;
    void* slot0Ctx = resolveWeakPtr ? resolveWeakPtr(reinterpret_cast<void*>(regAddr + 0x8)) : nullptr;
    void* slot1Pawn = resolveWeakPtr ? resolveWeakPtr(reinterpret_cast<void*>(regAddr + 0x10)) : nullptr;
    void* slot1Ctx = resolveWeakPtr ? resolveWeakPtr(reinterpret_cast<void*>(regAddr + 0x18)) : nullptr;
    std::int32_t count = *reinterpret_cast<std::int32_t*>(regAddr + 0x20);

    Logging.Log("[PShowCoop] HudRegistry (0x0A1BAA50): count=%d, sP2HudContext=%p", count, sP2HudContext);
    Logging.Log("[PShowCoop] >> Slot 0: pawn=%p (P1=%p, P1Lp=%p), ctx=%p (P1Gauge=%p)",
                slot0Pawn, GetP1Pawn(), GetP1LocalPlayer(), slot0Ctx, sP1LifeGauge);
    Logging.Log("[PShowCoop] >> Slot 1: pawn=%p (P2=%p, P2Lp=%p), ctx=%p (P2Gauge=%p)",
                slot1Pawn, GetP2Pawn(), GetP2LocalPlayer(), slot1Ctx, sP2LifeGauge);

    if (IsHeapish(slot0Ctx)) {
        void* vt = *reinterpret_cast<void**>(slot0Ctx);
        std::uintptr_t vtOff = (reinterpret_cast<std::uintptr_t>(vt) >= mainRange.m_Start) ? (reinterpret_cast<std::uintptr_t>(vt) - mainRange.m_Start) : 0;
        void* field38 = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(slot0Ctx) + 0x38);
        Logging.Log("[PShowCoop] >> Slot 0 Ctx details: vt=NSO 0x%lx, +0x38=%p",
                    (unsigned long)vtOff, field38);
    }
    if (IsHeapish(slot1Ctx)) {
        void* vt = *reinterpret_cast<void**>(slot1Ctx);
        std::uintptr_t vtOff = (reinterpret_cast<std::uintptr_t>(vt) >= mainRange.m_Start) ? (reinterpret_cast<std::uintptr_t>(vt) - mainRange.m_Start) : 0;
        void* field38 = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(slot1Ctx) + 0x38);
        Logging.Log("[PShowCoop] >> Slot 1 Ctx details: vt=NSO 0x%lx, +0x38=%p",
                    (unsigned long)vtOff, field38);
    }
    // virtual-dispatches on it after our HudLookup routes there. Mirror the Slot
    // details pattern exactly; every hop guarded; zero behavior change.
    if (kDiag0204bCtxLinkageEnabled && IsHeapish(sP2HudContext)) {
        void* link38 = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2HudContext) + 0x38);
        void* linkVt = IsHeapish(link38) ? *reinterpret_cast<void**>(link38) : nullptr;
        std::uintptr_t linkVtOff = 0;
        if (IsHeapish(linkVt)) {
            const auto vtAddr = reinterpret_cast<std::uintptr_t>(linkVt);
            if (mainRange.InRange(vtAddr)) {
                linkVtOff = vtAddr - mainRange.m_Start;
            }
        }
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTICb CtxLink: sP2HudContext=%p +0x38=%p vtOff=NSO 0x%lx (P1 Slot-0 +0x38 shown above)",
                    sP2HudContext, link38, (unsigned long)linkVtOff);
    }
}

static void Diag0218FactoryProbe();

void RegisterP2HudPlayer(void* p2Controller, void* p2LocalPlayer) {
    if (!IsHeapish(sP2LifeGauge)) return;
    if (sP2HudContext != nullptr) return; // STRICT ONCE per level load

    void* p2Pawn = GetP2Pawn();
    if (!IsHeapish(p2Pawn) || sP1HudClass == nullptr) {
        return;
    }

    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    auto initPlayerHud = reinterpret_cast<InitPlayerHudFn>(mainRange.m_Start + InitPlayerHudOffset);
    std::uintptr_t regAddr = mainRange.m_Start + HudRegistryArrayOffset;

    Logging.Log("[PShowCoop] RegisterP2HudPlayer: BEGIN STRICT ONCE (p2Pawn=%p, p2Controller=%p, sP1HudClass=%p, sP2LifeGauge=%p)",
                p2Pawn, p2Controller, sP1HudClass, sP2LifeGauge);

    // Save Slot 0 raw weak pointers (pawn + context) before calling InitPlayerHud
    std::uint64_t savedSlot0Pawn = *reinterpret_cast<std::uint64_t*>(regAddr + 0x00);
    std::uint64_t savedSlot0Ctx = *reinterpret_cast<std::uint64_t*>(regAddr + 0x08);

    // Call the engine's authentic top-level HUD registration routine:
    // InitPlayerHud(0x01EF79F8): (AActor* pawn, UClass* hudClass)
    // 1. Validates hudClass via IsA(APlayerController)
    // 2. Allocates dedicated HUD context object via 0x7102a4bff0 with pawn as owner/instigator
    // 3. Initializes context object with pawn via 0x01EF7AFC(context, pawn)
    // 4. Calls RegisterHudPlayer(0x0A1BAA50, pawn, context, hudClass)
    // 5. Returns context object; leaves player memory completely pristine
    if (initPlayerHud != nullptr) {
        // call passes the GameInstance IDENTITY object (0x01F4C0C8(pawn,1))
        // as arg0, not the raw pawn. The identity object is long-lived and
        // roots the P1 ctx outer chain; the raw pawn unroots and our ctx gets
        // is what the engine itself does. Kill switch:
        // kPatch0230IdentityOuter.
        void* p2Identity = nullptr;
        auto identityOf = reinterpret_cast<IdentityResolverFn>(mainRange.m_Start + IdentityResolverOffset);
        if (identityOf != nullptr && IsHeapish(p2Pawn)) {
            p2Identity = identityOf(p2Pawn, 1);
        }
        void* ctxOuter = p2Pawn;
        if (kPatch0230IdentityOuter && IsHeapish(p2Identity)) {
            ctxOuter = p2Identity;
            Logging.Log("[PShowCoop] PATCH-0230: identity outer selected (%p replaces raw pawn %p)",
                        p2Identity, p2Pawn);
        } else if (kPatch0230IdentityOuter) {
            Logging.Log("[PShowCoop] PATCH-0230: identity unresolved (%p), falling back to raw pawn %p",
                        p2Identity, p2Pawn);
        }
        Logging.Log("[PShowCoop] RegisterP2HudPlayer: Calling native InitPlayerHud(0x01EF79F8) with outer=%p, hudClass=%p...",
                    ctxOuter, sP1HudClass);
        sP2HudContext = initPlayerHud(ctxOuter, sP1HudClass);
        Logging.Log("[PShowCoop] RegisterP2HudPlayer: InitPlayerHud completed. Returned hudContext=%p", sP2HudContext);
        // immediately at return, completing the creation-bind-damage timeline.
        // Guarded reads only, gated by the linkage DIAG switch; zero behavior change.
        if (kDiag0204bCtxLinkageEnabled && IsHeapish(sP2HudContext)) {
            void* createLink = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2HudContext) + 0x38);
            void* createLinkVt = IsHeapish(createLink) ? *reinterpret_cast<void**>(createLink) : nullptr;
            std::uintptr_t createLinkVtOff = 0;
            if (IsHeapish(createLinkVt)) {
                const auto createVtAddr = reinterpret_cast<std::uintptr_t>(createLinkVt);
                if (mainRange.InRange(createVtAddr)) {
                    createLinkVtOff = createVtAddr - mainRange.m_Start;
                }
            }
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTICb CtxLinkAtCreate: sP2HudContext=%p +0x38=%p vtOff=NSO 0x%lx",
                        sP2HudContext, createLink, (unsigned long)createLinkVtOff);
        }
    }
    // registry slot that the Slot0 restore below erases, or was adoption
    // refused? Same proven weak-ptr hops as InspectHudRegistry; guarded reads
    // only, zero behavior change. Runs once per level load (STRICT ONCE).
    if (kDiag0207PreRestoreEnabled) {
        auto preResolve = reinterpret_cast<ResolveWeakPtrFn>(mainRange.m_Start + ResolveWeakPtrOffset);
        std::int32_t preCount = *reinterpret_cast<std::int32_t*>(regAddr + 0x20);
        std::uint64_t preSlot0Raw = *reinterpret_cast<std::uint64_t*>(regAddr + 0x08);
        void* preS0Pawn = preResolve ? preResolve(reinterpret_cast<void*>(regAddr)) : nullptr;
        void* preS0Ctx = preResolve ? preResolve(reinterpret_cast<void*>(regAddr + 0x8)) : nullptr;
        void* preS1Pawn = preResolve ? preResolve(reinterpret_cast<void*>(regAddr + 0x10)) : nullptr;
        void* preS1Ctx = preResolve ? preResolve(reinterpret_cast<void*>(regAddr + 0x18)) : nullptr;
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC PreRestore: count=%d slot0Raw=0x%llx(saved=0x%llx) s0pawn=%p s0ctx=%p s1pawn=%p s1ctx=%p (P2pawn=%p P2ctx=%p)",
                    preCount,
                    (unsigned long long)preSlot0Raw, (unsigned long long)savedSlot0Ctx,
                    preS0Pawn, preS0Ctx, preS1Pawn, preS1Ctx, p2Pawn, sP2HudContext);
    }
    // adopts P2 into Slot 0 (identity collision); the old behavior erased it,
    // leaving Slot 1 empty so the HUD's tick driver never runs for P2's gauge
    // adopted entry to Slot 1 and restore P1's Slot 0 - exactly what native
    // would write for a second player slot.
    std::uint64_t adoptedSlot0Pawn = *reinterpret_cast<std::uint64_t*>(regAddr + 0x00);
    std::uint64_t adoptedSlot0Ctx = *reinterpret_cast<std::uint64_t*>(regAddr + 0x08);
    bool adoptionLanded = (adoptedSlot0Ctx != savedSlot0Ctx);
    if (kPatch0214SlotAdoption && adoptionLanded) {
        *reinterpret_cast<std::uint64_t*>(regAddr + 0x10) = adoptedSlot0Pawn;
        *reinterpret_cast<std::uint64_t*>(regAddr + 0x18) = adoptedSlot0Ctx;
        Logging.Log("[PShowCoop] PATCH-0214: adopted entry moved to Slot 1 (pawn=0x%llx ctx=0x%llx); restoring P1 Slot 0",
                    (unsigned long long)adoptedSlot0Pawn, (unsigned long long)adoptedSlot0Ctx);
    }

    // Restore Slot 0 raw weak pointers so Player 1 HUD remains 100% authentic and pristine
    if (savedSlot0Ctx != 0) {
        *reinterpret_cast<std::uint64_t*>(regAddr + 0x08) = savedSlot0Ctx;
    }
    if (savedSlot0Pawn != 0) {
        *reinterpret_cast<std::uint64_t*>(regAddr + 0x00) = savedSlot0Pawn;
    }
    Logging.Log("[PShowCoop] RegisterP2HudPlayer: Slot 0 restored (pawn=0x%llx ctx=0x%llx), adoptionLanded=%d",
                (unsigned long long)savedSlot0Pawn, (unsigned long long)savedSlot0Ctx, adoptionLanded ? 1 : 0);

    InspectHudRegistry();
    Diag0218FactoryProbe();
}
// context with native-sourced arguments (see the kill-switch comment above).
// Every pointer hop is heap-guarded; nothing is written except by the engine
// function itself. Runs once per level (caller is STRICT ONCE).
static void Diag0218FactoryProbe() {
    if (!kDiag0218FactoryProbe) return;
    if (!IsHeapish(sP2HudContext)) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: skipped, sP2HudContext=%p not heapish", sP2HudContext);
        return;
    }
    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    void* p2Vt = *reinterpret_cast<void**>(sP2HudContext);
    if (!mainRange.InRange(reinterpret_cast<std::uintptr_t>(p2Vt)) ||
        reinterpret_cast<std::uintptr_t>(p2Vt) - mainRange.m_Start != 0x95E1AF8UL) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: skipped, sP2HudContext vtable mismatch (%p)", p2Vt);
        return;
    }
    const std::uint64_t p2Flags =
        *reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(sP2HudContext) + 0x08);
    if ((p2Flags >> 29) & 1) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: skipped, sP2HudContext pending-kill set (flags=0x%llx)",
                    (unsigned long long)p2Flags);
        return;
    }

    auto resolveWeakPtr = reinterpret_cast<ResolveWeakPtrFn>(mainRange.m_Start + ResolveWeakPtrOffset);
    void* p1Ctx = resolveWeakPtr
        ? resolveWeakPtr(reinterpret_cast<void*>(mainRange.m_Start + HudRegistryArrayOffset + 0x8))
        : nullptr;
    if (!IsHeapish(p1Ctx)) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: skipped, P1 ctx unresolved (%p)", p1Ctx);
        return;
    }
    // inner = *(identity+0x30), container = *(inner+0x280).
    void* identity = *reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(p1Ctx) + 0x20);
    if (!IsHeapish(identity)) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: skipped, identity=%p not heapish", identity);
        return;
    }
    void* inner = *reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(identity) + 0x30);
    if (!IsHeapish(inner)) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: skipped, identity+0x30=%p not heapish", inner);
        return;
    }
    void* container = *reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(inner) + 0x280);
    if (!IsHeapish(container)) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: skipped, container=%p not heapish", container);
        return;
    }

    // classRef v2: the engine's own P1 gauge widget class (ClassPrivate +0x10).
    // ctx actor), so source the gauge BlueprintGeneratedClass from the engine
    // gauge itself - the class the factory gate validates (UserWidget).
    void* classRef = IsHeapish(sP1LifeGauge)
        ? *reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(sP1LifeGauge) + 0x10)
        : nullptr;
    if (!IsHeapish(classRef)) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: skipped, sP1LifeGauge class (%p) not heapish", classRef);
        return;
    }

    void* registryHeader = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(sP2HudContext) + 0x428);
    auto getLifeGauge = reinterpret_cast<GetLifeGaugeFn>(mainRange.m_Start + GetLifeGaugeOffset);
    if (getLifeGauge == nullptr) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: skipped, fetcher fn null");
        return;
    }

    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: calling 0x9A88F4(regHeader=%p, container=%p, classRef=%p, player=0xffffffff)...",
                registryHeader, container, classRef);
    void* gauge = getLifeGauge(registryHeader, container, classRef, 0xFFFFFFFFu);
    void* gaugeVt = IsHeapish(gauge) ? *reinterpret_cast<void**>(gauge) : nullptr;
    std::uintptr_t gaugeVtOff = (IsHeapish(gaugeVt) && mainRange.InRange(reinterpret_cast<std::uintptr_t>(gaugeVt)))
        ? (reinterpret_cast<std::uintptr_t>(gaugeVt) - mainRange.m_Start) : 0;

    const std::uint64_t reg428 = *reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(sP2HudContext) + 0x428);
    const std::int32_t regCount = *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(sP2HudContext) + 0x430);
    const std::uint64_t pair448 = *reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(sP2HudContext) + 0x448);
    void* slot450 = *reinterpret_cast<void* const*>(reinterpret_cast<std::uintptr_t>(sP2HudContext) + 0x450);
    void* slot450Vt = IsHeapish(slot450) ? *reinterpret_cast<void**>(slot450) : nullptr;
    std::uintptr_t slot450VtOff = (IsHeapish(slot450Vt) && mainRange.InRange(reinterpret_cast<std::uintptr_t>(slot450Vt)))
        ? (reinterpret_cast<std::uintptr_t>(slot450Vt) - mainRange.m_Start) : 0;

    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: result gauge=%p vt=NSO 0x%lx | p2ctx+0x428=0x%llx +0x430(count)=%d +0x448=0x%llx +0x450=%p vt=NSO 0x%lx",
                gauge,
                (unsigned long)gaugeVtOff,
                (unsigned long long)reg428, regCount,
                (unsigned long long)pair448, slot450, (unsigned long)slot450VtOff);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC: COMPLETE");
}

void BindP2NativeDamageDelegate(void* p2DamageComp) {
    if (!IsHeapish(p2DamageComp) || !IsHeapish(sP2LifeGauge)) return;
    if (sBoundP2DamageComp == p2DamageComp) return; // Already bound

    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    auto delegateAdd = reinterpret_cast<MulticastDelegateAddFn>(mainRange.m_Start + MulticastDelegateAddOffset);
    if (delegateAdd != nullptr) {
        void* multicastDelegate = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(p2DamageComp) + 0xD8);
        delegateAdd(multicastDelegate, sP2LifeGauge, reinterpret_cast<void*>(NativeP2DamageHUDCallback), nullptr);
        sBoundP2DamageComp = p2DamageComp;
        Logging.Log("[PShowCoop] Pure Native Delegate Bound: P2 LifeGauge (%p) -> P2 DamageReceiverComp (%p + 0xD8)",
                    sP2LifeGauge, p2DamageComp);
    }
}

void TriggerP2DamageHUD() {
    if (sP2LifeGauge != nullptr && IsHeapish(sP2LifeGauge)) {
        // PATCH-0204: never dispatch into a provably dead receiver.
        if (kPatch0204GuardDeadWidget && !GaugeVtableIntact(sP2LifeGauge)) {
            Logging.Log("[PShowCoop] PATCH-0204: TriggerP2DamageHUD skipped, sP2LifeGauge (%p) torn", sP2LifeGauge);
            return;
        }
        const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;

        // Invoke native OnLifeChanged to let the widget's internal state machine handle presentation
        auto onLifeChanged = reinterpret_cast<OnLifeChangedFn>(mainRange.m_Start + OnLifeChangedOffset);
        if (onLifeChanged != nullptr) {
            onLifeChanged(sP2LifeGauge);
        }

        Logging.Log("[PShowCoop] >> TriggerP2DamageHUD: Dispatched native OnLifeChanged to %p", sP2LifeGauge);
    } else {
        Logging.Log("[PShowCoop] >> TriggerP2DamageHUD: sP2LifeGauge is null or invalid (%p)", sP2LifeGauge);
    }
}

void RevealP2DamageHUD(std::int32_t newHearts) {
    if (!IsHeapish(sP2LifeGauge)) {
        Logging.Log("[PShowCoop] >> RevealP2DamageHUD: sP2LifeGauge is null or invalid (%p)", sP2LifeGauge);
        return;
    }

    // PATCH-0204: never dispatch into a provably dead receiver. Calling through a
    if (kPatch0204GuardDeadWidget && !GaugeVtableIntact(sP2LifeGauge)) {
        Logging.Log("[PShowCoop] PATCH-0204: RevealP2DamageHUD(%d) skipped, sP2LifeGauge (%p) torn",
                    newHearts, sP2LifeGauge);
        return;
    }
    // the gauge's WidgetTree root; on an empty tree (root null) the native
    // SetDisplayMode path faulted (Unmapped Read64 @0x28). Skip the dispatch and
    // log when the gauge has no populated tree root.
    {
        void* rvTree = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sP2LifeGauge) + 0x1D8);
        void* rvRoot = (IsHeapish(rvTree)) ?
            *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(rvTree) + 0x28) : nullptr;
        if (!IsHeapish(rvRoot)) {
            Logging.Log("[PShowCoop] PATCH-0226: RevealP2DamageHUD(%d) skipped, gauge (%p) tree=%p root=%p not populated",
                        newHearts, sP2LifeGauge, rvTree, rvRoot);
            return;
        }
    }

    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;

    // PATCH-0237: with the P2 gauge driver on, P2's count and visibility are set by
    // the driver from P2's native life slot on the next frame, as 0x009A6800 does
    // for P1 (P1 has no per-hit show call). Showing or setting the count here
    // would make the driver see no life change and never arm the show timer.
    const bool driverOwnsPresentation = P2GaugeDriverEnabled();

    // 1. Unhide / activate widget via native vtable[0x4C0 / 8] (SetActive(true))
    void** vtable = *reinterpret_cast<void***>(sP2LifeGauge);
    using SetActiveFn = void (*)(void*, bool);
    auto setActive = reinterpret_cast<SetActiveFn>(vtable[0x4C0 / 8]);
    if (setActive != nullptr && !driverOwnsPresentation) {
        setActive(sP2LifeGauge, true);
    }

    // 2. PATCH-0233: 0x03EA5CC0 is RemoveFromViewport, not SetVisibility; only the
    // pre-0233 rollback path still calls it.
    auto setVisibility = reinterpret_cast<SetVisibilityFn>(mainRange.m_Start + SetVisibilityOffset);
    if (!kPatch0233NoRemoveFromViewport && setVisibility != nullptr) {
        setVisibility(sP2LifeGauge, 0); // pre-0233 behaviour: actually RemoveFromViewport
    }
    {
        auto isInViewport = reinterpret_cast<IsInViewportFn>(mainRange.m_Start + IsInViewportOffset);
        Logging.Log("[PShowCoop] PATCH-0233 reveal: IsInViewport=%d newHearts=%d",
                    isInViewport(sP2LifeGauge) ? 1 : 0, newHearts);
    }
    // displayed count and both queue depths immediately before SetDisplayMode
    // (pre already at/below newHearts) vs converter-not-feeding vs increase
    // path. Same proven offsets, guarded reads only.
    if (kDiag0208PreRevealEnabled && IsHeapish(sP2LifeGauge)) {
        const auto* preBytes = reinterpret_cast<const std::uint8_t*>(sP2LifeGauge);
        const auto preDisp = *reinterpret_cast<const std::int32_t*>(preBytes + 0x508);
        const auto preDmgQ = *reinterpret_cast<const std::uint32_t*>(preBytes + 0x560);
        const auto preHealQ = *reinterpret_cast<const std::uint32_t*>(preBytes + 0x570);
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC Snapshot (pre-SetDisplayMode %d): displayed=%d dmgQ=%u healQ=%u",
                    newHearts, preDisp, (unsigned)preDmgQ, (unsigned)preHealQ);
    }

    // 3. Dispatch native display mode and icon shatter animation via SetDisplayMode (0x009D5F68)
    constexpr ptrdiff_t SetDisplayModeOffset = 0x009D5F68;
    using SetDisplayModeFn = void (*)(void*, std::int32_t, std::uint32_t, std::uint8_t);
    auto setDisplayMode = reinterpret_cast<SetDisplayModeFn>(mainRange.m_Start + SetDisplayModeOffset);
    if (setDisplayMode != nullptr && !driverOwnsPresentation) {
        setDisplayMode(sP2LifeGauge, newHearts, kPatch0211Param3Enable ? 1u : 0u, 1);
    }
    if (driverOwnsPresentation) {
        Logging.Log("[PShowCoop] PATCH-0237: RevealP2DamageHUD(%d) leaves SetShown/SetDisplayMode to the P2 gauge driver",
                    newHearts);
    }
    // and both queue depths immediately after SetDisplayMode returns, so the
    // count14 entry reading can be attributed to conversion, drain, or prior
    // state with no speculation. Guarded reads only.
    if (kDiag0205SnapshotEnabled && IsHeapish(sP2LifeGauge)) {
        const auto* snapBytes = reinterpret_cast<const std::uint8_t*>(sP2LifeGauge);
        const auto snapDisp = *reinterpret_cast<const std::int32_t*>(snapBytes + 0x508);
        const auto snapDmgQ = *reinterpret_cast<const std::uint32_t*>(snapBytes + 0x560);
        const auto snapHealQ = *reinterpret_cast<const std::uint32_t*>(snapBytes + 0x570);
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC Snapshot (post-SetDisplayMode %d): displayed=%d dmgQ=%u healQ=%u",
                    newHearts, snapDisp, (unsigned)snapDmgQ, (unsigned)snapHealQ);
    }

    Logging.Log("[PShowCoop] >> RevealP2DamageHUD: SetActive(true) + SetDisplayMode(%d, mode=0) dispatched to sP2LifeGauge (%p)",
                newHearts, sP2LifeGauge);

    // PATCH-0222 (shared content) superseded by PATCH-0223: the GDB probe
    // found the real difference is the gauge's +0xE8 WidgetTree pointer
    // class, fixed at spawn in SpawnDedicatedP2LifeGauge.
    if (kDiag0212FadeStateEnabled && IsHeapish(sP2LifeGauge)) {
        const auto* fb = reinterpret_cast<const std::uint8_t*>(sP2LifeGauge);
        float fade = 0.0f;
        std::memcpy(&fade, fb + 0x520, sizeof(float));
        void* animArr = *reinterpret_cast<void* const*>(fb + 0x510);
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC reveal fade-state: +0x550=%u +0x520=%.3f +0x5DC=%u states(A=%d,B=%d) dmgQ=%u animArr=%p animCount=%u",
                    fb[0x550], static_cast<double>(fade), fb[0x5DC],
                    *reinterpret_cast<const std::int32_t*>(fb + 0x524),
                    *reinterpret_cast<const std::int32_t*>(fb + 0x528),
                    *reinterpret_cast<const std::uint32_t*>(fb + 0x560),
                    animArr,
                    *reinterpret_cast<const std::uint32_t*>(fb + 0x518));
    }
}

void ResetHudForLevelTransition() {
    sP1LifeGauge = nullptr;
    sP2LifeGauge = nullptr;
    sP2HudContext = nullptr;
    sBoundP2DamageComp = nullptr;
    sLastBoundP2Controller = nullptr;
    sDiag0216CaptureDone = false;
    ResetP2GaugeDriver();
    Logging.Log("[PShowCoop] ResetHudForLevelTransition: Cleared HUD pointers for level transition.");
}

void UpdateHudAnchors() {
}

void* GetP1LifeGauge() {
    return sP1LifeGauge;
}

void* GetP2LifeGauge() {
    return sP2LifeGauge;
}

void* GetP2HudContext() {
    return sP2HudContext;
}
// equals the native LifeGauge vtable base. Pure reads behind IsHeapish guards;
// the same pointer hop already runs in the spawn and Reveal paths every session.
bool GaugeVtableIntact(void* gauge) {
    if (!IsHeapish(gauge)) return false;
    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    const auto expected = mainRange.m_Start + LifeGaugeVtableNso;
    const auto actual = *reinterpret_cast<std::uintptr_t*>(gauge);
    return actual == expected;
}

void LogGaugeLiveness(const char* tag) {
    if (!kDiag0203LivenessEnabled) return;
    const bool p1Live = GaugeVtableIntact(sP1LifeGauge);
    const bool p2Live = GaugeVtableIntact(sP2LifeGauge);
    const auto p1Vt = IsHeapish(sP1LifeGauge) ? *reinterpret_cast<std::uintptr_t*>(sP1LifeGauge) : 0;
    const auto p2Vt = IsHeapish(sP2LifeGauge) ? *reinterpret_cast<std::uintptr_t*>(sP2LifeGauge) : 0;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC Liveness (%s): sP1=%p live=%d vt=0x%lx | sP2=%p live=%d vt=0x%lx",
                tag, sP1LifeGauge, p1Live ? 1 : 0, (unsigned long)p1Vt,
                sP2LifeGauge, p2Live ? 1 : 0, (unsigned long)p2Vt);
}

void InstallHud() {
    CaptureAddToViewport::InstallAtOffset(AddToViewportOffset);
    if (kDiag0216UfCapture) {
        CaptureUfInvoke::InstallAtOffset(UFunctionDispatchOffset);
        CaptureWidgetCreate::InstallAtOffset(CreateWidgetWrapperOffset);
        CaptureWidgetCreateInner::InstallAtOffset(CreateWidgetOfClassOffset);
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC armed: dispatcher=0x%lx funnel=0x%lx inner=0x%lx",
                    static_cast<unsigned long>(UFunctionDispatchOffset),
                    static_cast<unsigned long>(CreateWidgetWrapperOffset),
                    static_cast<unsigned long>(CreateWidgetOfClassOffset));
    }
    if (kDiag0217AllocCapture) {
        CaptureAllocInternal::InstallAtOffset(StaticConstructInternalOffset);
        CaptureAllocPublic::InstallAtOffset(StaticConstructPublicOffset);
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC armed: alloc internal=0x%lx public=0x%lx",
                    static_cast<unsigned long>(StaticConstructInternalOffset),
                    static_cast<unsigned long>(StaticConstructPublicOffset));
    }
    DiagOnLifeChanged::InstallAtOffset(OnLifeChangedOffset);
    CaptureInitPlayerHud::InstallAtOffset(InitPlayerHudOffset);
    CaptureHudLookup::InstallAtOffset(HudLookupOffset);
    MirrorFadeTick::InstallAtOffset(GaugeFadeTickOffset);
    if (kPatch0235P2PositionSource) {
        P2GaugePositionSource::InstallAtOffset(GaugeUpdateScreenPosOffset);
        Logging.Log("[PShowCoop] PATCH-0235 installed: position=0x%lx",
                    static_cast<unsigned long>(GaugeUpdateScreenPosOffset));
    }
    Logging.Log("[PShowCoop] Strict True 2-Widget Architecture: Viewport=0x%lx, DiagOnLifeChanged=0x%lx, InitPlayerHud=0x%lx, HudLookup=0x%lx, FadeTickMirror=0x%lx",
                static_cast<unsigned long>(AddToViewportOffset),
                static_cast<unsigned long>(OnLifeChangedOffset),
                static_cast<unsigned long>(InitPlayerHudOffset),
                static_cast<unsigned long>(HudLookupOffset),
                static_cast<unsigned long>(GaugeFadeTickOffset));
    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    const auto* fnBytes9D2E94 = reinterpret_cast<const std::uint32_t*>(mainRange.m_Start + 0x009D2E94);
    const auto* fnBytes9D2EA0 = reinterpret_cast<const std::uint32_t*>(mainRange.m_Start + 0x009D2EA0);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC Native 0x009D2E94 Opcode Probe: [0]=0x%08x [1]=0x%08x [2]=0x%08x",
                fnBytes9D2E94[0], fnBytes9D2E94[1], fnBytes9D2E94[2]);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC Native 0x009D2EA0 Opcode Probe: [0]=0x%08x [1]=0x%08x [2]=0x%08x [3]=0x%08x",
                fnBytes9D2EA0[0], fnBytes9D2EA0[1], fnBytes9D2EA0[2], fnBytes9D2EA0[3]);
}

} // namespace pshowcoop::patches

