#include "pointer_guard.hpp"
#include "diag_riding.hpp"
#include "patch_event_parity.hpp"
#include "patch_core.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>
#include <cstdio>

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// player" through a family of helpers that each resolve GetPlayerPawn(this, 0)
// and apply one actor-control operation to the pawn and its companion; scene
// placements end with the companion reset 0x2085F18 after moving and turning
// player 0. P2 is never resolved, so it stays visible and in place.
//
// PATCH-0253 (helper replay): after each helper below runs for player 0, the
// unchanged original runs once more while GetPlayerPawn(.., 0) answers player
// index 1 (hook on the native getter, active only inside that call). The
// game's own instructions then apply the same operation to P2's pawn and
// companion. Helpers that only query are not replayed (0x20800D0, 0x2080634).
//
// PATCH-0254 (placement): when a scene finishes placing player 0 (0x2085F18;
// every one of its 8 callers moves and turns the player just before), P2 is
// moved beside P1 through the same pawn-interface move the scenes use
// (0x1E8A364; PATCH-0015's +120 X beside-P1 offset) and given P1's facing
// through its own PlayerCore setters (0x1D3F370, 0x1D3F318), then the
// companion reset is replayed for P2.
//
// gate reason set (0x1E89558) and reason-0 set (0x1E89518) whose receiver is
// P1's or P2's pawn, logged once per call site, so scene sites still acting
// on player 0 only are named. Remove once the census is read.
// ---------------------------------------------------------------------------
constexpr bool kPatch0253HelperReplay = true;
constexpr bool kPatch0254PlacementParity = true;
constexpr bool kDiag0243ActionCensus = false; // Completed census; do not retain temporary hooks on Switch.

// ---------------------------------------------------------------------------
// EventPlayerInputManagerComponent update 0x1321928 resolves player index 0
// (0x1E481E0(w, 1)) and, when an event-input entry starts, calls on it
// iface(0x232688C) +0xA8(0) (PlayerCore gate bit 2 closed) and
// iface(0x252124C) +0xD8(1); when the entry ends +0xA8(1) and +0xD8(0) (its
// flag +0xC0 goes 1->0 at a start and 0->1 at an end). After the unchanged
// update, P2 gets the same two interface calls at each edge. At an end edge P2
// is placed beside P1 (PATCH-0254's placement, without facing) unless
// PATCH-0257 already moved it with P1 during the entry.
// ---------------------------------------------------------------------------
constexpr bool kPatch0255EventInputParity = true;
// closed in the later-stage lockup. Passive, bounded, existing update hook.
constexpr bool kDiag0254EventOwner = !PSHOW_DIAG0257_PERF_CONTROL;
// Update(this): entry stp x29,x30,[sp,#-0x40]! / str x23 / mov x29,sp /
// stp x22,x21 / stp x20,x19 (8-aligned: 4 instructions move).
constexpr ptrdiff_t EventInputUpdateFn = 0x1321928;
constexpr std::uintptr_t EventInputActiveFlag = 0xC0;  // ldrb/strb at 0x13219E8/0x1321A34
constexpr ptrdiff_t InputBlockIfaceClassFn = 0x232688C;
constexpr ptrdiff_t EventModeIfaceClassFn = 0x252124C;
constexpr ptrdiff_t InterfaceCastFn = 0x2A376CC;          // as 0x13219CC/0x13219E0
constexpr std::uintptr_t InputBlockSlot = 0xA8;
constexpr std::uintptr_t EventModeSlot = 0xD8;

// ---------------------------------------------------------------------------
// (class getter 0x232688C; subobject at pawn +0x5B8, vtable 0x9545620)
// resolved for player index 0 only. The course director's tick 0x132D300
// eases P1 between two points and calls slot +0x40 (SetLocation 0x1D2C164 ->
// x 1872 -> 2493 with no other mover). Named motions go through slot +0xE8
// (pawn vslot +0xB00, PlayMotion 0x1D2B154; the director's queue 0x13302D8),
// turns through +0x58 (rotation, 0x1D2C358) and +0x68 (facing direction,
// 0x1D2C53C), and +0x100 (0x1D2CCE0) hands a name to the PlayerCore. While P1
// is held by an event (PlayerCore reason bit 1 or 2 clear), each of these
// calls on P1 is repeated on P2 through the same native function, after the
// original:
//  - rotation, direction, motion and the +0x100 name: the same arguments;
//  - location: P2 walks P1's path FollowDistance behind it, through a ring of
//    the locations the event gave P1 (the one mod-held state). P2 measured
//    ahead of P1 along the first step waits until P1 has passed it; P2 behind
//    closes in on P1's starting point first. A single move longer than
//    TeleportStep is a placement: P2 goes beside P1 (PATCH-0015's +120 X).
// Calls while P1 is not held are left alone, so play never pins P2 to P1.
// ---------------------------------------------------------------------------
constexpr bool kPatch0257IfaceParity = true;
constexpr std::uintptr_t PawnPlayerIface = 0x5B8;  // sub x0,x0,#0x5b8 at 0x1D2C174
// Entries (verified 2026-10-17, no PC-relative instruction in the moved bytes):
// 0x1D2C164 stp x29,x30,[sp,#-0x20]! / stp x20,x19 / mov x29,sp / mov x19,x0 /
//   sub x0,x0,#0x5b8 (5 move; body 0x3C): (iface, const FVector*, sweep, hit, teleport) -> bool.
// 0x1D2C358 sub sp,sp,#0x30 / stp x29,x30 / add x29 / stp x20,x19 (4): (iface, const FRotator*).
// 0x1D2C53C sub sp,sp,#0x20 / stp x29,x30 / add x29 / ldr d0,[x1] / fmov s3,#1.0 (5):
//   (iface, const FVector* direction).
// 0x1D2CCE0 sub sp,sp,#0x30 / stp x29,x30 / add x29 / stp x20,x19 (4): (iface, const FName*, bool).
// 0x1D2B154 sub sp,sp,#0x70 / stp x29,x30 / add x29 / str x19 / ldr x19,[x0,#0x6a0]
//   (5; body 0x48): (pawn, const FName*); also reached from the 0x1B7514C override.
constexpr ptrdiff_t IfaceSetLocationFn = 0x1D2C164;
constexpr ptrdiff_t IfaceSetRotationFn = 0x1D2C358;
constexpr ptrdiff_t IfaceSetDirectionFn = 0x1D2C53C;
constexpr ptrdiff_t IfaceCoreNameFn = 0x1D2CCE0;
constexpr ptrdiff_t PawnPlayMotionFn = 0x1D2B154;
constexpr std::uintptr_t CoreReasonMask = 0x3E0;    // FIELD-PLAYERCORE-03E0
constexpr std::uint32_t EventReasonBits = 0x6;      // bit 1 scene/talk (PATCH-0251), bit 2 event input (PATCH-0255)
constexpr std::uintptr_t CoreFlagWord = 0x3E8;      // 0x1A634CC/0x1A634D4 return core +0x3E8
constexpr float FollowDistance = 100.0f;

// ---------------------------------------------------------------------------
// player" through its event-control component (found with actor vslot +0x628
// and class 0x23D52BC): BeginControl 0xD3F88C(comp, request{weak controller,
// options}) runs the pawn's begin-event 0x1D222EC through a delegate (PlayerCore
// +0x310, action state 'None' via 0x1A7A8B0, velocity reset), and EndControl
// 0xD3F99C(comp, request) runs the end-event 0x1D2241x (state 'WAIT') when the
// request's controller holds the component. Only player 0 is taken, so P2
// stays in its own WAIT action, whose state machine replaces every scene motion
// component while an event holds P1, P2's component gets the same request
// through the same function; every EndControl on P1's component is repeated on
// P2's, which the native check limits to a P2 taken by the same controller.
// Entries: 0xD3F88C sub sp,sp,#0x30 / stp x29,x30 / add x29 / stp x20,x19 /
// ldr x8,[x0,#0xa0] (5); 0xD3F99C stp x29,x30,[sp,#-0x30]! / str x21 / mov x29 /
// stp x20,x19 / ldr x8,[x0,#0xa0] (5); no PC-relative instruction in either.
// ---------------------------------------------------------------------------
constexpr bool kPatch0259EventControlParity = true;
constexpr ptrdiff_t EventControlBeginFn = 0xD3F88C;
constexpr ptrdiff_t EventControlEndFn = 0xD3F99C;
constexpr ptrdiff_t EventControlClassFn = 0x23D52BC;
constexpr std::uintptr_t ActorFindComponentSlot = 0x628;  // (actor, class) -> component, as 0xD3FD68
constexpr std::uint32_t MaxEventControlLines = 48;
constexpr float TeleportStep = 300.0f;
constexpr std::uint32_t PathSize = 128;
constexpr std::uint32_t WalkGapFrames = 2;          // P2 drains without a P1 move that end a walk
constexpr std::uint32_t MaxIfaceLines = 96;
constexpr std::uint32_t MaxIfaceKeys = 96;

// ---------------------------------------------------------------------------
// player" its reaction motions by adding a motion asset to the pawn's motion
// table: player-interface slot +0xE0 (0x1D2CC0C; pawn virtual 0x1D2CBEC) calls
// 0x1A462AC(comp +0xB0, asset), which runs 0x1A462B4(table, asset, 0) to copy
// each motion of the asset into the table. Every scene site resolves player 0
// only (course start 0x132DFFC, 0x1E77548, stage handlers 0x15BF470 and
// 0x15C513C, each through 0x1E481E0(ctx, 1)), so P2's table never holds the
// After 0x1A462B4 adds an asset to player 0's installed motion table A or B,
// P2's table of the same kind gets the same asset through the same function.
// The owner is the table's Outer (pawn setup 0x1D1D1C0 creates both tables with
// the pawn as Outer), confirmed by its motion component holding the table, and
// both players are resolved natively (GetPlayerPawn), not from the per-frame
// cache that a level load clears.
// Entry 0x1A462B4: sub sp,sp,#0xa0 / stp x29,x30,[sp,#0x40] / add x29 /
// stp x28,x27 / stp x26,x25 (5); no PC-relative instruction.
// ---------------------------------------------------------------------------
constexpr bool kPatch0260MotionSetParity = true;
constexpr ptrdiff_t MotionTableAddFn = 0x1A462B4;      // (table, asset, replace) -> table
constexpr std::uintptr_t PawnMotionComponent = 0x6A0;  // ldr x19,[x0,#0x6a0] in 0x1D2B154; ldr x8,[x0,#0x6a0] in 0x1D2CBEC
constexpr std::uintptr_t MotionTableA = 0xB0;          // stp x1,x2,[x0,#0xb0] in 0x1A47B2C
constexpr std::uintptr_t MotionTableB = 0xB8;
constexpr std::uintptr_t MotionTableNum = 0x30;        // entry array count; free count at +0x5C (0x1A484C0)
constexpr std::uintptr_t MotionTableFree = 0x5C;
constexpr std::uintptr_t ObjectOuterOffset = 0x20;
constexpr std::uint32_t MaxMotionSetLines = 48;

// ---------------------------------------------------------------------------
// course events apply to P1? Once per (operation, player, call site): lock
// add +0x18 (0x1D2BDFC) and remove +0x20 (0x1D2BF84) on the pawn's +0x800
// list, +0x50 (0x1D2C210, speed 300/3000 on pawn +0x618), +0xA0 (0x1D2CA9C,
// pawn +0x758 component), PlayerCore flags +0x128 (0x1D2CE98, bit 38) and
// +0x130 (0x1D2CEF0, bit 43). Log only; remove once read.
// Entries: 0x1D2BDFC stp x29,x30,[sp,#-0x30]! / str x21 / mov / stp / ldr (5);
// 0x1D2BF84 stp x29,x30,[sp,#-0x50]! / str x25 / mov / stp / stp (5);
// 0x1D2C210 stp x29,x30,[sp,#-0x20]! / str x19 / mov / mov w8 (4);
// 0x1D2CA9C stp / str / mov / mov x19,x0 / ldr (5); 0x1D2CE98 and 0x1D2CEF0
// stp / str / mov / ldr x0,[x0,#0x218] (4).
// ---------------------------------------------------------------------------
constexpr bool kDiag0245IfaceCensus = false; // Completed interface census; gameplay parity hooks remain.
constexpr ptrdiff_t IfaceLockAddFn = 0x1D2BDFC;
constexpr ptrdiff_t IfaceLockRemoveFn = 0x1D2BF84;
constexpr ptrdiff_t IfaceSpeedModeFn = 0x1D2C210;
constexpr ptrdiff_t IfaceMoveResetFn = 0x1D2CA9C;
constexpr ptrdiff_t IfaceFlag38Fn = 0x1D2CE98;
constexpr ptrdiff_t IfaceFlag43Fn = 0x1D2CEF0;
constexpr std::uint32_t MaxCensus45Lines = 64;
constexpr std::uint32_t MaxCensus45Keys = 64;

// ---------------------------------------------------------------------------
// PATCH-0257 hands P2 every PlayMotion P1 gets while held (21 in the course
// players: (1) each PlayerCore state change by name (state machine at core
// +0x48, 0x1D3DEF4(sm, const FName*, bool)), once per (player, name, call
// site); (2) each request reaching the motion component (pawn +0x6A0,
// 0x1A47B34(comp, request): type at +0x4, FName at +0x8), once per (player,
// type, name, call site); (3) after each mirrored PlayMotion, IsPlayingMotion
// (0x1D2B364) on both players at once and then every frame until each stops.
// Names print through the name pool (block table NSO 0xA6FC2A8; entry header
// Entries: 0x1D3DEF4 stp x29,x30,[sp,#-0x40]! / str x23 / mov x29,sp /
// stp x22,x21 / stp x20,x19 (5); 0x1A47B34 sub sp,sp,#0x60 / str d8 / str x21 /
// stp x29,x30 / add x29 (5).
// ---------------------------------------------------------------------------
// The completed probe dereferenced cached pawns during next-map construction.
// temporary hooks and every polling helper off; native motion/state runs as-is.
constexpr bool kDiag0246MotionState = false;
constexpr ptrdiff_t CoreStateSetFn = 0x1D3DEF4;
constexpr ptrdiff_t MotionRequestFn = 0x1A47B34;
constexpr ptrdiff_t PawnIsPlayingMotionFn = 0x1D2B364;
constexpr ptrdiff_t NameBlocksNso = 0xA6FC2A8;
constexpr std::uintptr_t CoreStateMachine = 0x48;      // ldr x0,[x0,#0x48] in 0x1A7A8B0
constexpr std::uintptr_t MotionRequestType = 0x4;
constexpr std::uintptr_t MotionRequestName = 0x8;
constexpr std::uint32_t MaxDiag46Lines = 420;
constexpr std::uint32_t TrackMaxFrames = 1800;
// PlayerCore flag word +0x3E8 bit 0x10 is set. Each player's request sequence
// (with its current state, sm +0x88, and flag word), every named state change,
// and, for the first MaxDiffDumps scene motions P2 refuses, the words in which
// P1's and P2's PlayerCore, state machine, motion component and pawn differ.
constexpr std::uintptr_t StateCurrentName = 0x88;   // 0x1D3DEF4 stores the new state name at sm +0x88
// 0x40005) and still plays no scene motion, and its component keeps 'Idle' as
// the last motion found (+0xC8, set by 0x1A484C0 only when the name is in a
// motion table). At the first MaxLookupDumps refused motions: each player's
// pawn class, character data (+0x7B8), motion tables (+0xB0, +0xB8) with their
// entry counts, the remapped name (0x1A4919C) and whether each table holds it,
// using 0x1A484C0's own lookup (hash 0x287DF70 + Number into buckets at +0x60 or
// *(+0x68), size +0x70; 0xA0-byte entries at *(+0x28), key +0x0, next +0x98).
constexpr ptrdiff_t NameHashFn = 0x287DF70;        // pure: registers only
constexpr ptrdiff_t MotionRemapFn = 0x1A4919C;     // (comp, const FName*) -> FName (map at comp +0xD8)
constexpr std::uintptr_t ObjectNameOffset = 0x18;
constexpr std::uintptr_t PawnCharacterData = 0x7B8;  // written only by 0x1D1D290 (pawn setup 0x1D1D1C0)
constexpr std::uint32_t MaxLookupDumps = 4;
constexpr std::uint32_t MaxDiffDumps = 2;
constexpr std::uint32_t MaxDiffWords = 64;
constexpr std::uint32_t CoreDiffSize = 0x600;
constexpr std::uint32_t StateMachineDiffSize = 0x220;
constexpr std::uint32_t MotionCompDiffSize = 0x2C0;
constexpr std::uint32_t PawnDiffSize = 0xA00;

// ---------------------------------------------------------------------------
// positions, P1's reason mask and PATCH-0257's mirror counts, from the P2
// input drain: at most every TraceEvery frames while something changes, and
// every TraceIdleEvery frames otherwise, which also keeps Eden flushing its
// log so a session's last scene is kept.
// ---------------------------------------------------------------------------
constexpr bool kDiag0244KeepAlive = !PSHOW_DIAG0257_PERF_CONTROL;
constexpr std::uint32_t TraceEvery = 10;
constexpr std::uint32_t TraceIdleEvery = 60;

// UGameplayStatics::GetPlayerPawn native (ctx, int32 index) (FUNC-GAMEPLAYSTATICS-
// GETPLAYERPAWN-NATIVE). Entry stp x29,x30,[sp,#-0x30]! / str x21 / mov x29,sp /
// stp x20,x19; adrp at +0x10 lies past the 16-byte write at this 8-aligned entry.
constexpr ptrdiff_t GetPlayerPawnFn = 0x44FB668;

// Player-0 helpers (scene object, ...), each 'mov w1,wzr; bl GetPlayerPawn' and an
// IsA, then companion (pawn iface 0x247B24C +0x48, cast 0x236F3FC) and pawn ops.
constexpr ptrdiff_t HelperInputAndShow = 0x20803B0;     // (this, b): 0x1E89518(b) x2 + pawn +0x988(b)
constexpr ptrdiff_t HelperShow = 0x2080494;             // (this, b): 0x1E89518(b) companion + pawn (16 callers)
constexpr ptrdiff_t HelperReason2 = 0x2080560;          // (this, b): 0x1E89428(2, b) companion + pawn
constexpr ptrdiff_t HelperFloat = 0x20806AC;            // (this, float): cast 0x252124C -> vslot +0xD0(v0)
constexpr ptrdiff_t HelperFaceTarget = 0x2080774;       // (this, FVector*): core facing toward target (0x1D3F370)
constexpr ptrdiff_t HelperCompanionShow = 0x2082E94;    // (this, b): 0x1E89518(b) companion only
constexpr ptrdiff_t HelperCompanionReset = 0x2085F18;   // (this): iface +0x58(1) -> 0x1C28F24/0x1C28778

// Pawn-interface operations (iface = pawn + 0x4B8): resolve the gate target via
// iface vslot +0x1C8 and tail-branch into the ActorCtrl component.
constexpr ptrdiff_t IfaceMoveFn = 0x1E8A364;      // (iface, FVector*, sweep, hit, teleport, p6) -> SetActorLocation 0x41F2450
constexpr ptrdiff_t IfaceGateFn = 0x1E89558;      // (iface, reason, set) -> 0x1F132A8
constexpr ptrdiff_t IfaceGate0Fn = 0x1E89518;     // (iface, set) -> 0x1F132A8(target, 0, set)
constexpr std::uintptr_t PawnIface = 0x4B8;

// PlayerCore weak handle and facing pair, as the scenes call them (0x2023118..0x2023158).
constexpr ptrdiff_t MakeCoreWeakFn = 0x1D30E78;   // (pawn) -> weak(pawn +0x7D0)
constexpr ptrdiff_t CoreSetFacingFn = 0x1D3F370;  // (&weak, FVector*) -> core +0x348/+0x350
constexpr ptrdiff_t CoreApplyFacingFn = 0x1D3F318; // (&weak) -> 0x1A626EC turns the pawn to +0x348
constexpr std::uintptr_t PawnCore = 0x7D0;
constexpr std::uintptr_t CoreFacing = 0x348;      // FVector written by 0x1A626D8 (8 bytes +0x348, 4 bytes +0x350)
// Pawn root component +0x130; its world location at +0x1D0 (0x2080774, 0x1E8A364).
constexpr std::uintptr_t PawnRoot = 0x130;
constexpr std::uintptr_t RootLocation = 0x1D0;
// PATCH-0015's beside-P1 placement offset (patch_core.cpp ApplyLeash: P1 + 120 X).
constexpr float BesideP1OffsetX = 120.0f;

constexpr ptrdiff_t PawnClassFn = 0x2514FB8;
constexpr std::uintptr_t ObjectClassOffset = 0x10;
constexpr std::uintptr_t ClassTreeArrayOffset = 0x30;
constexpr std::uintptr_t ClassTreeDepthOffset = 0x38;

constexpr std::uint32_t MaxReplayLines = 48;
constexpr std::uint32_t MaxPlaceLines = 24;
constexpr std::uint32_t MaxCensusLines = 96;
constexpr std::uint32_t MaxCensusKeys = 64;
constexpr std::uint32_t MaxEventInputLines = 32;

using GetPlayerFnT = void* (*)(void*, std::int32_t);
using ClassGetterFn = void* (*)();
using IfaceMoveFnT = std::uint32_t (*)(void*, const float*, std::uint32_t, void*, std::uint32_t, void*);
using MakeCoreWeakFnT = std::uint64_t (*)(void*);
using CoreWeakFn = void (*)(std::uint64_t*);
using CoreWeakVecFn = void (*)(std::uint64_t*, const float*);
using InterfaceCastFnT = void* (*)(void*, void*);
using IfaceSetBoolFn = void (*)(void*, bool);

// PATCH-0257 follow state: the current event walk of P1 and where P2 started it.
static std::uint32_t sFrame = 0;          // P2 input drains (one per frame), from EventParityFrame
static std::uint32_t sLastMoveFrame = 0;
static bool sWalkActive = false;
static bool sWalkHasAxis = false;         // its first real step fixed the walk axis
static float sPath[PathSize][3] = {};     // P1's event locations in this walk (ring)
static float sPathAt[PathSize] = {};      // path length from the walk origin at each point
static std::uint32_t sPathCount = 0;
static float sPathLength = 0.0f;
static float sOrigin[3] = {};             // P1's location when the walk started
static float sP2Start[3] = {};            // P2's location when the walk started
static float sAxis[3] = {};               // unit direction of P1's first step
static float sP2Along = 0.0f;             // P2's start along that axis from the origin (+ = ahead)
static float sP2Side[3] = {};             // P2's start minus its point on the axis
static bool sP2EventMoved = false;        // PATCH-0257 moved P2 during the current event-input entry
static std::uint32_t sMirrorLoc = 0;
static std::uint32_t sMirrorRot = 0;
static std::uint32_t sMirrorDir = 0;
static std::uint32_t sMirrorMotion = 0;
static std::uint32_t sMirrorName = 0;
static const char* sLastMode = "-";
static std::uint32_t sEventControlDepth = 0;   // PATCH-0259: inside a mirrored begin/end
static std::uint32_t sEventControlLines = 0;
static std::uint32_t sMotionSetDepth = 0;      // PATCH-0260: inside a mirrored table add
static std::uint32_t sMotionSetLines = 0;
static std::uint32_t sIfaceLines = 0;
static std::uint64_t sIfaceKeys[MaxIfaceKeys] = {};
static std::uint32_t sIfaceKeyCount = 0;
static std::uint32_t sCensus45Lines = 0;
static std::uint64_t sCensus45Keys[MaxCensus45Keys] = {};
static std::uint32_t sCensus45KeyCount = 0;
static std::uint32_t sDiag46Lines = 0;
static std::uint32_t sTrackName[2] = {};
static bool sTrackActive = false;
static bool sTrackP1 = false;
static bool sTrackP2 = false;
static std::uint32_t sTrackFrame = 0;
static std::uint32_t sTrackCount = 0;
static std::uint32_t sDiffDumps = 0;
static std::uint32_t sLookupDumps = 0;
static std::uint32_t sLastRequest[3][2] = {};
static std::uintptr_t sLastRequestRa[3] = {};
static std::uint32_t sRepeatCount[3] = {};
static std::uint32_t sTraceFrame = 0;
static std::uint32_t sTraceMirrors = 0;
static float sTraceP1[3] = {};
static float sTraceP2[3] = {};
static std::int32_t sLastEventInputFlag = -1;  // value of EPIM +0xC0 after its last update

// Depth of an active replay; GetPlayerPawn(.., 0) answers index 1 while > 0.
static std::uint32_t sReplayDepth = 0;
static std::uint32_t sReplayLines = 0;
static std::uint32_t sPlaceLines = 0;
static std::uint32_t sEventInputLines = 0;
static std::uint32_t sCensusLines = 0;
static std::uint64_t sCensusKeys[MaxCensusKeys] = {};
static std::uint32_t sCensusKeyCount = 0;

inline bool IsHeapish(const void* value) {
    return pshowcoop::pointerguard::Readable(value);
}

static std::uintptr_t MainStart() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

static std::uintptr_t ToNso(std::uintptr_t p) {
    return exl::util::GetMainModuleInfo().m_Total.InRange(p) ? p - MainStart() : 0;
}

template <typename Fn>
static Fn At(ptrdiff_t offset) {
    return reinterpret_cast<Fn>(MainStart() + offset);
}

template <typename T>
static T Read(const void* base, std::uintptr_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(base) + offset);
}

static bool InMain(const void* p) {
    return exl::util::GetMainModuleInfo().m_Total.InRange(reinterpret_cast<std::uintptr_t>(p));
}

// Virtual at `slot` of `object`'s vtable, only when both lie in main.
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

// One frame up: our saved frame record holds the caller's x29, whose +8 is the
static std::uintptr_t CallerReturn(const void* frame) {
    const auto* ourFrame = static_cast<const std::uintptr_t*>(frame);
    const std::uintptr_t callerFp = ourFrame != nullptr ? ourFrame[0] : 0;
    if (callerFp == 0 || (callerFp & 0xF) != 0) return 0;
    return reinterpret_cast<const std::uintptr_t*>(callerFp)[1];
}

// P2 exists as a player pawn distinct from P1, resolved natively (no redirect: depth 0).
static void* NativeP2Pawn(void* ctx) {
    if (!IsHeapish(ctx)) return nullptr;
    auto get = At<GetPlayerFnT>(GetPlayerPawnFn);
    void* p2 = get(ctx, 1);
    if (!IsPlayerPawn(p2) || p2 == get(ctx, 0)) return nullptr;
    return p2;
}

static void RootLocation3(void* pawn, float* out) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!IsHeapish(pawn)) return;
    void* root = Read<void*>(pawn, PawnRoot);
    if (!IsHeapish(root)) return;
    out[0] = Read<float>(root, RootLocation);
    out[1] = Read<float>(root, RootLocation + 4);
    out[2] = Read<float>(root, RootLocation + 8);
}

static float Dist3(const float* a, const float* b) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
}

static void Copy3(float* out, const float* in) {
    out[0] = in[0];
    out[1] = in[1];
    out[2] = in[2];
}

struct ReplayScope {
    ReplayScope() { ++sReplayDepth; }
    ~ReplayScope() { --sReplayDepth; }
};

static void LogReplay(const char* helper, void* scene, std::uintptr_t ra) {
    if (sReplayLines >= MaxReplayLines) return;
    sReplayLines++;
    Logging.Log("[PShowCoop] PATCH-0253 replay %s scene=%p p2=%p raNSO=0x%lx", helper, scene,
                GetP2Pawn(), static_cast<unsigned long>(ToNso(ra)));
}

// PATCH-0254: P2 beside P1 with P1's facing, through the scenes' own calls.
static void PlaceP2BesideP1(void* scene, std::uintptr_t ra, bool withFacing, const char* trigger) {
    auto get = At<GetPlayerFnT>(GetPlayerPawnFn);
    void* p1 = get(scene, 0);
    void* p2 = get(scene, 1);
    if (!IsPlayerPawn(p1) || !IsPlayerPawn(p2) || p1 == p2) return;
    void* root1 = Read<void*>(p1, PawnRoot);
    if (!IsHeapish(root1)) return;
    const float target[3] = {Read<float>(root1, RootLocation) + BesideP1OffsetX,
                             Read<float>(root1, RootLocation + 4), Read<float>(root1, RootLocation + 8)};
    const std::uint32_t moved = At<IfaceMoveFnT>(IfaceMoveFn)(static_cast<std::uint8_t*>(p2) + PawnIface, target,
                                                              0, nullptr, 0, nullptr);
    void* core1 = Read<void*>(p1, PawnCore);
    void* core2 = Read<void*>(p2, PawnCore);
    float facing[3] = {0.0f, 0.0f, 0.0f};
    const bool turn = withFacing && IsHeapish(core1) && IsHeapish(core2);
    if (turn) {
        facing[0] = Read<float>(core1, CoreFacing);
        facing[1] = Read<float>(core1, CoreFacing + 4);
        facing[2] = Read<float>(core1, CoreFacing + 8);
        std::uint64_t weakSet = At<MakeCoreWeakFnT>(MakeCoreWeakFn)(p2);
        At<CoreWeakVecFn>(CoreSetFacingFn)(&weakSet, facing);
        std::uint64_t weakApply = At<MakeCoreWeakFnT>(MakeCoreWeakFn)(p2);
        At<CoreWeakFn>(CoreApplyFacingFn)(&weakApply);
    }
    if (sPlaceLines < MaxPlaceLines) {
        sPlaceLines++;
        Logging.Log("[PShowCoop] PATCH-0254 place(%s) P2=%p to (%.1f,%.1f,%.1f) moved=%u facing=(%.2f,%.2f,%.2f)%s raNSO=0x%lx",
                    trigger, p2, target[0], target[1], target[2], moved & 1, facing[0], facing[1], facing[2],
                    turn ? "" : " no-turn", static_cast<unsigned long>(ToNso(ra)));
    }
}

HOOK_DEFINE_TRAMPOLINE(GetPlayerPawnRedirect) {
    static void* Callback(void* ctx, std::int32_t index) {
        if (sReplayDepth > 0 && index == 0) index = 1;
        return Orig(ctx, index);
    }
};

// Replays the unchanged helper once for player index 1, unless already inside a replay.
#define PSHOW_REPLAY(NAME, ...)                                                          \
    do {                                                                                  \
        if (!kPatch0253HelperReplay || sReplayDepth > 0) break;                           \
        if (NativeP2Pawn(scene) == nullptr) break;                                        \
        {                                                                                 \
            ReplayScope scope;                                                            \
            Orig(__VA_ARGS__);                                                            \
        }                                                                                 \
        LogReplay(NAME, scene, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))); \
    } while (0)

HOOK_DEFINE_TRAMPOLINE(ReplayInputAndShow) {
    static void Callback(void* scene, std::uint32_t on) {
        Orig(scene, on);
        PSHOW_REPLAY("input+show", scene, on);
    }
};

HOOK_DEFINE_TRAMPOLINE(ReplayShow) {
    static void Callback(void* scene, std::uint32_t on) {
        Orig(scene, on);
        PSHOW_REPLAY("show", scene, on);
    }
};

HOOK_DEFINE_TRAMPOLINE(ReplayReason2) {
    static void Callback(void* scene, std::uint32_t on) {
        Orig(scene, on);
        PSHOW_REPLAY("reason2", scene, on);
    }
};

HOOK_DEFINE_TRAMPOLINE(ReplayFloat) {
    static void Callback(void* scene, float value) {
        Orig(scene, value);
        PSHOW_REPLAY("float", scene, value);
    }
};

HOOK_DEFINE_TRAMPOLINE(ReplayFaceTarget) {
    static void Callback(void* scene, const float* target) {
        Orig(scene, target);
        PSHOW_REPLAY("face", scene, target);
    }
};

HOOK_DEFINE_TRAMPOLINE(ReplayCompanionShow) {
    static void Callback(void* scene, std::uint32_t on) {
        Orig(scene, on);
        PSHOW_REPLAY("companion-show", scene, on);
    }
};

HOOK_DEFINE_TRAMPOLINE(ReplayCompanionReset) {
    static void Callback(void* scene) {
        Orig(scene);
        if (sReplayDepth > 0) return;
        if (kPatch0254PlacementParity) {
            PlaceP2BesideP1(scene, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)), true, "scene");
        }
        PSHOW_REPLAY("companion-reset", scene);
    }
};

#undef PSHOW_REPLAY

// PATCH-0255: P2 gets the event-input edge calls P1 got; placed beside P1 at an end.
static void EventInputEdgeForP2(void* manager, bool started, std::uintptr_t ra) {
    void* p2 = NativeP2Pawn(manager);
    if (p2 == nullptr) return;
    auto cast = At<InterfaceCastFnT>(InterfaceCastFn);
    void* blockIface = cast(p2, At<ClassGetterFn>(InputBlockIfaceClassFn)());
    void* modeIface = cast(p2, At<ClassGetterFn>(EventModeIfaceClassFn)());
    auto setBlock = blockIface != nullptr ? Slot<IfaceSetBoolFn>(blockIface, InputBlockSlot) : nullptr;
    auto setMode = modeIface != nullptr ? Slot<IfaceSetBoolFn>(modeIface, EventModeSlot) : nullptr;
    // The native order: +0xA8 on the input interface, then +0xD8 on the mode interface.
    if (setBlock != nullptr) setBlock(blockIface, !started);
    if (setMode != nullptr) setMode(modeIface, started);
    if (sEventInputLines < MaxEventInputLines) {
        sEventInputLines++;
        Logging.Log("[PShowCoop] PATCH-0255 event-input %s P2=%p block=%p mode=%p iface_moves=%u followed=%d raNSO=0x%lx",
                    started ? "start" : "end", p2, setBlock, setMode, sMirrorLoc, sP2EventMoved ? 1 : 0,
                    static_cast<unsigned long>(ToNso(ra)));
    }
    if (started) {
        sP2EventMoved = false;
        return;
    }
    // P2 already moved with P1 during the entry (PATCH-0257); otherwise rejoin P1.
    if (!sP2EventMoved && kPatch0254PlacementParity) PlaceP2BesideP1(manager, ra, false, "event-end");
    sP2EventMoved = false;
}

static void Diag54EventOwner(void* manager, bool edge);

HOOK_DEFINE_TRAMPOLINE(EventInputParity) {
    static void Callback(void* manager) {
        const bool tracked = kPatch0255EventInputParity && IsHeapish(manager);
        const std::uint8_t before = tracked ? Read<std::uint8_t>(manager, EventInputActiveFlag) : 0;
        Orig(manager);
        if (!tracked) return;
        const std::uint8_t after = Read<std::uint8_t>(manager, EventInputActiveFlag);
        Diag54EventOwner(manager, before != after);
        riding::ObserveUpdate(manager);
        sLastEventInputFlag = after;
        if (before == after) return;
        riding::ObserveEvent(manager, false, after);
        // +0xC0 goes 1->0 when an entry starts (input closed) and 0->1 when it ends.
        EventInputEdgeForP2(manager, after == 0, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
        riding::ObserveEvent(manager, true, after);
    }
};

// PATCH-0257 helpers.

// P1 is held by an event: its PlayerCore reason bit 1 or 2 is clear.
static bool HeldByEvent(void* pawn, std::uint32_t* maskOut) {
    void* core = Read<void*>(pawn, PawnCore);
    if (!IsHeapish(core)) return false;
    const std::uint32_t mask = Read<std::uint32_t>(core, CoreReasonMask);
    if (maskOut != nullptr) *maskOut = mask;
    return (mask & EventReasonBits) != EventReasonBits;
}

// P2's pawn when `pawn` is P1's and P1 is held by an event, else null.
static void* MirrorTarget(void* pawn, std::uint32_t* mask) {
    if (!kPatch0257IfaceParity || sReplayDepth > 0) return nullptr;
    void* p1 = GetP1Pawn();
    if (p1 == nullptr || pawn != p1 || !IsPlayerPawn(p1)) return nullptr;
    void* p2 = GetP2Pawn();
    if (!IsPlayerPawn(p2) || p2 == p1) return nullptr;
    if (!HeldByEvent(p1, mask)) return nullptr;
    return p2;
}

// PlayerCore flag word (+0x3E8) of a pawn, 0 when the core is missing.
static std::uint64_t CoreFlags(void* pawn) {
    void* core = IsHeapish(pawn) ? Read<void*>(pawn, PawnCore) : nullptr;
    return IsHeapish(core) ? Read<std::uint64_t>(core, CoreFlagWord) : 0;
}

// PATCH-0259: an actor's event-control component, found as the game finds it
// (actor vslot +0x628 with the class from 0x23D52BC, as 0xD3FD68/0xD30E4C).
using FindComponentFnT = void* (*)(void*, void*);

static void* EventControlOf(void* actor) {
    if (!IsHeapish(actor)) return nullptr;
    auto find = Slot<FindComponentFnT>(actor, ActorFindComponentSlot);
    if (find == nullptr) return nullptr;
    void* comp = find(actor, At<ClassGetterFn>(EventControlClassFn)());
    return IsHeapish(comp) ? comp : nullptr;
}

static void LogEventControl(const char* what, void* comp1, void* comp2, const std::uint8_t* request,
                            std::uint64_t r1, std::uint64_t r2, std::uint32_t mask, std::uintptr_t ra,
                            std::uintptr_t ra2) {
    if (sEventControlLines >= MaxEventControlLines) return;
    sEventControlLines++;
    Logging.Log("[PShowCoop] PATCH-0259 %s P1 comp=%p P2 comp=%p result=%u/%u controller=0x%lx options=0x%lx mask=0x%x "
                "raNSO=0x%lx ra2NSO=0x%lx",
                what, comp1, comp2, static_cast<unsigned>(r1 & 1), static_cast<unsigned>(r2 & 1),
                static_cast<unsigned long>(Read<std::uint64_t>(request, 0)),
                static_cast<unsigned long>(Read<std::uint64_t>(request, 8)), mask,
                static_cast<unsigned long>(ToNso(ra)), static_cast<unsigned long>(ToNso(ra2)));
}

HOOK_DEFINE_TRAMPOLINE(EventControlBegin) {
    static std::uint64_t Callback(void* comp, const std::uint8_t* request) {
        const std::uint64_t result = Orig(comp, request);
        if (!kPatch0259EventControlParity || sEventControlDepth > 0 || !IsHeapish(comp) || request == nullptr) {
            return result;
        }
        void* p1 = GetP1Pawn();
        if (p1 == nullptr || (result & 1) == 0 || comp != EventControlOf(p1)) return result;
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        std::uint32_t mask = 0;
        void* p2 = GetP2Pawn();
        void* comp2 = (IsPlayerPawn(p2) && p2 != p1) ? EventControlOf(p2) : nullptr;
        // Only scenes: a P1 not held by an event (gameplay control) keeps P2 free.
        if (comp2 == nullptr || comp2 == comp || !IsPlayerPawn(p1) || !HeldByEvent(p1, &mask)) {
            LogEventControl("begin-skip", comp, comp2, request, result, 0, mask, ra, ra2);
            return result;
        }
        riding::ObserveControl(comp, false, true);
        ++sEventControlDepth;
        const std::uint64_t result2 = Orig(comp2, request);
        --sEventControlDepth;
        riding::ObserveControl(comp, true, true);
        LogEventControl("begin", comp, comp2, request, result, result2, mask, ra, ra2);
        return result;
    }
};

HOOK_DEFINE_TRAMPOLINE(EventControlEnd) {
    static std::uint64_t Callback(void* comp, const std::uint8_t* request) {
        const std::uint64_t result = Orig(comp, request);
        if (!kPatch0259EventControlParity || sEventControlDepth > 0 || !IsHeapish(comp) || request == nullptr) {
            return result;
        }
        void* p1 = GetP1Pawn();
        if (p1 == nullptr || comp != EventControlOf(p1)) return result;
        void* p2 = GetP2Pawn();
        void* comp2 = (IsPlayerPawn(p2) && p2 != p1) ? EventControlOf(p2) : nullptr;
        if (comp2 == nullptr || comp2 == comp) return result;
        // The native end releases only a component held by the request's controller,
        // so P2 is released exactly when it was taken by the same controller.
        riding::ObserveControl(comp, false, false);
        ++sEventControlDepth;
        const std::uint64_t result2 = Orig(comp2, request);
        --sEventControlDepth;
        riding::ObserveControl(comp, true, false);
        std::uint32_t mask = 0;
        HeldByEvent(p1, &mask);
        LogEventControl("end", comp, comp2, request, result, result2, mask,
                        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),
                        CallerReturn(__builtin_frame_address(0)));
        return result;
    }
};

static void* PlayerIfaceOf(void* pawn) { return static_cast<std::uint8_t*>(pawn) + PawnPlayerIface; }

static void* PawnOfPlayerIface(void* iface) { return static_cast<std::uint8_t*>(iface) - PawnPlayerIface; }

// Once per (operation, call site) while lines remain.
static bool IfaceFirst(char op, std::uintptr_t ra) {
    const std::uint64_t key = (static_cast<std::uint64_t>(ToNso(ra)) << 8) | static_cast<std::uint8_t>(op);
    for (std::uint32_t i = 0; i < sIfaceKeyCount; ++i) {
        if (sIfaceKeys[i] == key) return false;
    }
    if (sIfaceKeyCount >= MaxIfaceKeys || sIfaceLines >= MaxIfaceLines) return false;
    sIfaceKeys[sIfaceKeyCount++] = key;
    sIfaceLines++;
    return true;
}

static void NoteIface(char op, const char* what, bool mirrored, std::uint32_t mask, std::uintptr_t ra,
                      std::uintptr_t ra2, const float* v, const std::uint32_t* name) {
    if (!IfaceFirst(op, ra)) return;
    if (name != nullptr) {
        Logging.Log("[PShowCoop] PATCH-0257 %s P1 name=0x%x/%u mirrored=%d mask=0x%x raNSO=0x%lx ra2NSO=0x%lx",
                    what, name[0], name[1], mirrored ? 1 : 0, mask, static_cast<unsigned long>(ToNso(ra)),
                    static_cast<unsigned long>(ToNso(ra2)));
    } else {
        Logging.Log("[PShowCoop] PATCH-0257 %s P1 v=(%.2f,%.2f,%.2f) mirrored=%d mask=0x%x raNSO=0x%lx ra2NSO=0x%lx",
                    what, v != nullptr ? v[0] : 0.0f, v != nullptr ? v[1] : 0.0f, v != nullptr ? v[2] : 0.0f,
                    mirrored ? 1 : 0, mask, static_cast<unsigned long>(ToNso(ra)),
                    static_cast<unsigned long>(ToNso(ra2)));
    }
}

static void PathPush(const float* p) {
    float along = 0.0f;
    if (sPathCount > 0) {
        const std::uint32_t last = (sPathCount - 1) % PathSize;
        const float step = Dist3(sPath[last], p);
        if (step < 0.01f) return;
        along = sPathAt[last] + step;
    }
    const std::uint32_t slot = sPathCount % PathSize;
    Copy3(sPath[slot], p);
    sPathAt[slot] = along;
    sPathLength = along;
    sPathCount++;
}

// The point `along` units from the walk origin on P1's path (the oldest kept point
// once the ring no longer reaches back that far).
static void PathPointAt(float along, float* out) {
    const std::uint32_t kept = sPathCount < PathSize ? sPathCount : PathSize;
    std::uint32_t idx = sPathCount - 1;
    for (std::uint32_t n = 1; n < kept; ++n) {
        const std::uint32_t cur = idx % PathSize;
        const std::uint32_t prev = (idx - 1) % PathSize;
        if (sPathAt[prev] <= along) {
            const float span = sPathAt[cur] - sPathAt[prev];
            float t = span > 0.0f ? (along - sPathAt[prev]) / span : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            for (int k = 0; k < 3; ++k) out[k] = sPath[prev][k] + (sPath[cur][k] - sPath[prev][k]) * t;
            return;
        }
        idx--;
    }
    Copy3(out, sPath[idx % PathSize]);
}

// Where P2 goes when the event moves P1 from `before` to `loc`.
static const char* FollowTarget(void* p2, const float* before, const float* loc, float* target) {
    const bool newWalk = !sWalkActive || sFrame - sLastMoveFrame > WalkGapFrames;
    sLastMoveFrame = sFrame;
    const float step = Dist3(before, loc);
    if (step > TeleportStep) {
        // A placement: P2 beside P1 (PATCH-0015's offset); a later walk starts from here.
        target[0] = loc[0] + BesideP1OffsetX;
        target[1] = loc[1];
        target[2] = loc[2];
        sWalkActive = false;
        return "place";
    }
    if (newWalk) {
        sPathCount = 0;
        sPathLength = 0.0f;
        sWalkHasAxis = false;
        Copy3(sOrigin, before);
        PathPush(before);
        RootLocation3(p2, sP2Start);
        if (Dist3(sP2Start, before) > TeleportStep) {
            // P2 is elsewhere: it starts beside P1's starting point.
            sP2Start[0] = before[0] + BesideP1OffsetX;
            sP2Start[1] = before[1];
            sP2Start[2] = before[2];
        }
        sWalkActive = true;
    }
    if (!sWalkHasAxis && step >= 0.01f) {
        // The first real step fixes the walk axis; P2's start is measured along it.
        float rel[3];
        for (int k = 0; k < 3; ++k) {
            sAxis[k] = (loc[k] - before[k]) / step;
            rel[k] = sP2Start[k] - sOrigin[k];
        }
        sP2Along = rel[0] * sAxis[0] + rel[1] * sAxis[1] + rel[2] * sAxis[2];
        for (int k = 0; k < 3; ++k) sP2Side[k] = rel[k] - sAxis[k] * sP2Along;
        sWalkHasAxis = true;
    }
    PathPush(loc);
    if (!sWalkHasAxis) {
        Copy3(target, sP2Start);  // P1 held in place: so is P2
        return "hold";
    }
    const float along = sPathLength - FollowDistance;  // P2's place on P1's path
    if (along <= sP2Along) {
        Copy3(target, sP2Start);  // P1 has not passed P2 yet
        return "wait";
    }
    if (along < 0.0f) {
        // P2 started behind P1's starting point: it closes in on that point first.
        const float t = (along - sP2Along) / (0.0f - sP2Along);
        for (int k = 0; k < 3; ++k) target[k] = sP2Start[k] + (sOrigin[k] - sP2Start[k]) * t;
        return "join";
    }
    PathPointAt(along, target);
    if (sP2Along >= 0.0f) {
        // P2 waited beside the path: its sideways offset fades over its first FollowDistance.
        const float walked = along - sP2Along;
        const float fade = walked < FollowDistance ? 1.0f - walked / FollowDistance : 0.0f;
        for (int k = 0; k < 3; ++k) target[k] += sP2Side[k] * fade;
    }
    return "follow";
}
static void CopyText(char* out, const char* text, std::uint32_t size) {
    std::uint32_t n = 0;
    for (; text[n] != '\0' && n + 1 < size; ++n) out[n] = text[n];
    out[n] = '\0';
}

// FName text from the name pool, read only (as tools/gdb_decode_fname.py).
static void NameText(const std::uint32_t* name, char* out, std::uint32_t size) {
    out[0] = '\0';
    if (name == nullptr) {
        CopyText(out, "<null>", size);
        return;
    }
    const std::uint32_t index = name[0];
    std::uint32_t n = 0;
    if (index == 0) {
        CopyText(out, "None", size);
        n = 4;
    } else {
        const std::uint32_t block = index >> 16;
        const auto* blocks = reinterpret_cast<const std::uint8_t* const*>(MainStart() + NameBlocksNso);
        const std::uint8_t* base = block < 0x2000 ? blocks[block] : nullptr;
        if (!IsHeapish(base)) {
            CopyText(out, "<badblock>", size);
            return;
        }
        const std::uint8_t* entry = base + (index & 0xFFFF) * 2;
        const std::uint16_t header = *reinterpret_cast<const std::uint16_t*>(entry);
        const std::uint32_t length = header >> 6;
        const bool wide = (header & 1) != 0;
        if (length == 0 || length > 96) {
            CopyText(out, "<badlen>", size);
            return;
        }
        for (; n < length && n + 12 < size; ++n) {
            const std::uint8_t c = wide ? entry[2 + n * 2] : entry[2 + n];
            out[n] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
        }
    }
    out[n < size ? n : size - 1] = '\0';
    if (name[1] != 0 && n + 12 < size) {
        // FName Number is the instance suffix + 1.
        char digits[11];
        std::uint32_t value = name[1] - 1;
        std::uint32_t d = 0;
        do {
            digits[d++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && d < sizeof(digits));
        out[n++] = '_';
        while (d > 0) out[n++] = digits[--d];
        out[n] = '\0';
    }
}

// 1 or 2 when `p1`/`p2` is the player whose `offset` member equals `object`, else 0.
static int Diag46Who(void* p1, void* p2, void* object, bool coreMember, std::uintptr_t offset) {
    void* owners[2] = {p1, p2};
    for (int i = 0; i < 2; ++i) {
        void* holder = owners[i];
        if (!IsHeapish(holder)) continue;
        if (coreMember) holder = Read<void*>(holder, PawnCore);
        if (!IsHeapish(holder)) continue;
        if (Read<void*>(holder, offset) == object) return i + 1;
    }
    return 0;
}

// P1 and P2 while P1 is held by an event; false otherwise.
static bool Diag46Players(void** p1Out, void** p2Out, std::uint32_t* mask) {
    if (!kDiag0246MotionState) return false;
    void* p1 = GetP1Pawn();
    void* p2 = GetP2Pawn();
    if (!IsPlayerPawn(p1) || !IsPlayerPawn(p2) || p1 == p2) return false;
    if (!HeldByEvent(p1, mask)) return false;
    *p1Out = p1;
    *p2Out = p2;
    return true;
}

// Current PlayerCore state name of a pawn (state machine core +0x48, name at +0x88).
static void StateText(void* pawn, char* out, std::uint32_t size) {
    CopyText(out, "-", size);
    void* core = IsHeapish(pawn) ? Read<void*>(pawn, PawnCore) : nullptr;
    if (!IsHeapish(core)) return;
    void* sm = Read<void*>(core, CoreStateMachine);
    if (!IsHeapish(sm)) return;
    NameText(reinterpret_cast<const std::uint32_t*>(static_cast<std::uint8_t*>(sm) + StateCurrentName), out, size);
}

static bool PointerLike(std::uint64_t value) {
    return pshowcoop::pointerguard::Readable(reinterpret_cast<const void*>(value));
}

// 8-byte words that differ between P1's and P2's copy of one object (pointer pairs skipped).
static void Diag46Diff(const char* what, const void* a, const void* b, std::uint32_t size) {
    if (!IsHeapish(a) || !IsHeapish(b)) return;
    char line[360];
    std::uint32_t used = 0;
    std::uint32_t shown = 0;
    line[0] = '\0';
    for (std::uint32_t off = 0; off < size && shown < MaxDiffWords; off += 8) {
        const std::uint64_t va = Read<std::uint64_t>(a, off);
        const std::uint64_t vb = Read<std::uint64_t>(b, off);
        if (va == vb || (PointerLike(va) && PointerLike(vb))) continue;
        shown++;
        const int n = std::snprintf(line + used, sizeof(line) - used, " +0x%x=%lx/%lx", off,
                                    static_cast<unsigned long>(va), static_cast<unsigned long>(vb));
        if (n > 0) used += static_cast<std::uint32_t>(n);
        if (used > sizeof(line) - 48) {
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC diff %s P1/P2:%s", what, line);
            used = 0;
            line[0] = '\0';
        }
    }
    if (used > 0 || shown == 0) Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC diff %s P1/P2:%s%s", what, line, shown == 0 ? " none" : "");
}

static void Diag46State(void* sm, const std::uint32_t* name, std::uint32_t force, std::uintptr_t ra,
                        std::uintptr_t ra2) {
    void* p1 = nullptr;
    void* p2 = nullptr;
    std::uint32_t mask = 0;
    if (sm == nullptr || !Diag46Players(&p1, &p2, &mask)) return;
    const int who = Diag46Who(p1, p2, sm, true, CoreStateMachine);
    if (who == 0 || sDiag46Lines >= MaxDiag46Lines) return;
    sDiag46Lines++;
    char text[64];
    NameText(name, text, sizeof(text));
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC state P%d -> %s force=%u flags=0x%lx mask=0x%x frame=%u raNSO=0x%lx ra2NSO=0x%lx",
                who, text, force & 1, static_cast<unsigned long>(CoreFlags(who == 1 ? p1 : p2)), mask, sFrame,
                static_cast<unsigned long>(ToNso(ra)), static_cast<unsigned long>(ToNso(ra2)));
}

// Each player's motion requests in order (a line whenever the name or the call site changes).
static void Diag46Request(void* comp, const std::uint8_t* request, std::uintptr_t ra, std::uintptr_t ra2) {
    void* p1 = nullptr;
    void* p2 = nullptr;
    std::uint32_t mask = 0;
    if (comp == nullptr || request == nullptr || !Diag46Players(&p1, &p2, &mask)) return;
    const int who = Diag46Who(p1, p2, comp, false, PawnMotionComponent);
    if (who == 0) return;
    const std::uint32_t type = Read<std::uint32_t>(request, MotionRequestType);
    const auto* name = reinterpret_cast<const std::uint32_t*>(request + MotionRequestName);
    if (name[0] == sLastRequest[who][0] && name[1] == sLastRequest[who][1] && ra == sLastRequestRa[who]) {
        sRepeatCount[who]++;
        return;
    }
    const std::uint32_t repeats = sRepeatCount[who];
    sLastRequest[who][0] = name[0];
    sLastRequest[who][1] = name[1];
    sLastRequestRa[who] = ra;
    sRepeatCount[who] = 0;
    if (sDiag46Lines >= MaxDiag46Lines) return;
    sDiag46Lines++;
    void* pawn = who == 1 ? p1 : p2;
    char text[64];
    char state[48];
    NameText(type == 0 ? name : nullptr, text, sizeof(text));
    StateText(pawn, state, sizeof(state));
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC request P%d %s type=%u forced=%u once=%u state=%s flags=0x%lx prev_repeats=%u frame=%u "
                "raNSO=0x%lx ra2NSO=0x%lx",
                who, type == 0 ? text : "-", type, request[0], request[1], state,
                static_cast<unsigned long>(CoreFlags(pawn)), repeats, sFrame, static_cast<unsigned long>(ToNso(ra)),
                static_cast<unsigned long>(ToNso(ra2)));
}

// Class name of a UObject (class at +0x10, its FName at +0x18).
static void ClassText(void* object, char* out, std::uint32_t size) {
    CopyText(out, "-", size);
    if (!IsHeapish(object)) return;
    void* cls = Read<void*>(object, ObjectClassOffset);
    if (!IsHeapish(cls)) return;
    NameText(reinterpret_cast<const std::uint32_t*>(static_cast<std::uint8_t*>(cls) + ObjectNameOffset), out, size);
}
// signed count +0xB8, stride 0x28; weak UObject at entry+0x18, done byte +0x20.
// The game selects the first valid weak entry whose done byte is zero. It is
// the native reason input stays blocked; never clear that reason artificially.
static bool Diag54Readable(const void* ptr, std::size_t size) {
    if (!ptr || !size) return false;
    MemoryInfo info{};
    std::uint32_t page{};
    const auto a = reinterpret_cast<std::uintptr_t>(ptr);
    return R_SUCCEEDED(svcQueryMemory(&info, &page, a)) && (info.perm & Perm_R) &&
        a >= info.addr && size <= info.size && a - info.addr <= info.size - size;
}

static void Diag54EventOwner(void* manager, bool edge) {
    if (!kDiag0254EventOwner) return;
    static unsigned calls = 0, lines = 0;
    ++calls; // Instrument cadence only; no gameplay state or timer.
    if ((!edge && calls != 1 && calls % 120 != 0) || lines >= 96) return;
    if (!Diag54Readable(manager, 0xC8)) return;
    const int count = Read<std::int32_t>(manager, 0xB8);
    void* data = Read<void*>(manager, 0xB0);
    const unsigned allowed = Read<std::uint8_t>(manager, 0xC0);
    const bool listOk = count >= 0 && count <= 64 &&
        (!count || Diag54Readable(data, static_cast<std::size_t>(count) * 0x28));
    int selected = -1;
    void* owner = nullptr;
    std::uint64_t weakToken = 0;
    using ValidFn = bool (*)(const void*);
    using GetWeakFn = void* (*)(const void*);
    if (listOk) {
        for (int i = 0; i < count; ++i) {
            const auto* entry = static_cast<const std::uint8_t*>(data) + i * 0x28;
            if (Read<std::uint8_t>(entry, 0x20) || !At<ValidFn>(0x2A73028)(entry + 0x18)) continue;
            owner = At<GetWeakFn>(0x2A7312C)(entry + 0x18);
            selected = i;
            weakToken = Read<std::uint64_t>(entry, 0x18);
            break;
        }
    }
    char ownerName[64] = "<none>", ownerClass[64] = "<none>", worldName[64] = "<unavailable>";
    if (Diag54Readable(owner, 0x28)) {
        NameText(reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(owner) + ObjectNameOffset), ownerName, sizeof(ownerName));
        void* cls = Read<void*>(owner, ObjectClassOffset);
        if (Diag54Readable(cls, 0x20))
            NameText(reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(cls) + ObjectNameOffset), ownerClass, sizeof(ownerClass));
    }
    // +0xA8 is the optional world pointer used by this exact native updater.
    void* world = Read<void*>(manager, 0xA8);
    if (Diag54Readable(world, 0x20))
        NameText(reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(world) + ObjectNameOffset), worldName, sizeof(worldName));
    ++lines;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC event manager=%p world=%s input_allowed=%u entries=%d list_ok=%d selected=%d weak=0x%lx owner=%s class=%s edge=%d calls=%u",
        manager, worldName, allowed, count, listOk, selected, weakToken, ownerName, ownerClass, edge, calls);
    if (lines == 96) Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC cap limit=96");
}

using NameHashFnT = std::uint32_t (*)(std::uint32_t);
using MotionRemapFnT = std::uint64_t (*)(void*, const std::uint32_t*);

// 1 when `id` is a key of a motion table (0x1A484C0's lookup), 0 when not, -1 when the table is absent or empty.
static int MotionTableHas(const void* table, std::uint64_t id) {
    if (!IsHeapish(table)) return -1;
    if (Read<std::int32_t>(table, 0x30) == Read<std::int32_t>(table, 0x5C)) return -1;
    const std::uint32_t hash =
        At<NameHashFnT>(NameHashFn)(static_cast<std::uint32_t>(id)) + static_cast<std::uint32_t>(id >> 32);
    const std::uint8_t* buckets = Read<const std::uint8_t*>(table, 0x68);
    if (buckets == nullptr) buckets = static_cast<const std::uint8_t*>(table) + 0x60;
    const std::int32_t size = Read<std::int32_t>(table, 0x70);
    if (size <= 0) return -1;
    std::int32_t index = Read<std::int32_t>(buckets, static_cast<std::uintptr_t>((size - 1) & hash) * 4);
    const std::uint8_t* entries = Read<const std::uint8_t*>(table, 0x28);
    if (!IsHeapish(entries)) return -1;
    for (int guard = 0; index >= 0 && guard < 4096; ++guard) {
        if (Read<std::uint64_t>(entries, static_cast<std::uintptr_t>(index) * 0xA0) == id) return 1;
        index = Read<std::int32_t>(entries, static_cast<std::uintptr_t>(index) * 0xA0 + 0x98);
    }
    return 0;
}

static void Diag46LookupFor(const char* who, void* pawn, const std::uint32_t* name) {
    char cls[48];
    char data[48];
    char mapped[64];
    ClassText(pawn, cls, sizeof(cls));
    void* characterData = Read<void*>(pawn, PawnCharacterData);
    ClassText(characterData, data, sizeof(data));
    void* comp = Read<void*>(pawn, PawnMotionComponent);
    if (!IsHeapish(comp)) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC lookup %s class=%s data=%s(%p) comp=missing", who, cls, data, characterData);
        return;
    }
    const std::uint64_t id = At<MotionRemapFnT>(MotionRemapFn)(comp, name);
    const std::uint32_t idWords[2] = {static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(id >> 32)};
    NameText(idWords, mapped, sizeof(mapped));
    void* tableA = Read<void*>(comp, MotionTableA);
    void* tableB = Read<void*>(comp, MotionTableB);
    const int countA = IsHeapish(tableA) ? Read<std::int32_t>(tableA, 0x30) - Read<std::int32_t>(tableA, 0x5C) : -1;
    const int countB = IsHeapish(tableB) ? Read<std::int32_t>(tableB, 0x30) - Read<std::int32_t>(tableB, 0x5C) : -1;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC lookup %s class=%s data=%s(%p) A=%p n=%d has=%d B=%p n=%d has=%d id=%s last=0x%x",
                who, cls, data, characterData, tableA, countA, MotionTableHas(tableA, id), tableB, countB,
                MotionTableHas(tableB, id), mapped, Read<std::uint32_t>(comp, 0xC8));
}

using IsPlayingMotionFnT = bool (*)(void*, const std::uint32_t*);

static bool MotionPlaying(void* pawn, const std::uint32_t* name) {
    if (!IsHeapish(Read<void*>(pawn, PawnMotionComponent))) return false;
    return At<IsPlayingMotionFnT>(PawnIsPlayingMotionFn)(pawn, name);
}

// After a mirrored PlayMotion: is it playing on each player now? Then follow it per frame.
static void Diag46Track(void* p1, void* p2, const std::uint32_t* name) {
    if (!kDiag0246MotionState) return;
    const bool a = MotionPlaying(p1, name);
    const bool b = MotionPlaying(p2, name);
    sTrackCount++;
    if (sDiag46Lines < MaxDiag46Lines) {
        sDiag46Lines++;
        char text[64];
        char s1[48];
        char s2[48];
        NameText(name, text, sizeof(text));
        StateText(p1, s1, sizeof(s1));
        StateText(p2, s2, sizeof(s2));
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC motion #%u %s playing P1=%d P2=%d state P1=%s P2=%s flags P1=0x%lx P2=0x%lx frame=%u",
                    sTrackCount, text, a ? 1 : 0, b ? 1 : 0, s1, s2, static_cast<unsigned long>(CoreFlags(p1)),
                    static_cast<unsigned long>(CoreFlags(p2)), sFrame);
    }
    if (a && !b && sLookupDumps < MaxLookupDumps) {
        // Is the motion in each player's motion tables at all?
        sLookupDumps++;
        Diag46LookupFor("P1", p1, name);
        Diag46LookupFor("P2", p2, name);
    }
    if (a && !b && sDiffDumps < MaxDiffDumps) {
        // P2 refused what P1 plays: show every field in which the two players differ.
        sDiffDumps++;
        void* core1 = Read<void*>(p1, PawnCore);
        void* core2 = Read<void*>(p2, PawnCore);
        Diag46Diff("core", core1, core2, CoreDiffSize);
        if (IsHeapish(core1) && IsHeapish(core2)) {
            Diag46Diff("statemachine", Read<void*>(core1, CoreStateMachine), Read<void*>(core2, CoreStateMachine),
                       StateMachineDiffSize);
        }
        Diag46Diff("motioncomp", Read<void*>(p1, PawnMotionComponent), Read<void*>(p2, PawnMotionComponent),
                   MotionCompDiffSize);
        Diag46Diff("pawn", p1, p2, PawnDiffSize);
    }
    sTrackName[0] = name[0];
    sTrackName[1] = name[1];
    sTrackActive = true;
    sTrackP1 = a;
    sTrackP2 = b;
    sTrackFrame = sFrame;
}

static void Diag46Poll(void* p1, void* p2) {
    if (!kDiag0246MotionState || !sTrackActive) return;
    const bool a = MotionPlaying(p1, sTrackName);
    const bool b = MotionPlaying(p2, sTrackName);
    const std::uint32_t frames = sFrame - sTrackFrame;
    if ((a != sTrackP1 || b != sTrackP2) && sDiag46Lines < MaxDiag46Lines) {
        sDiag46Lines++;
        char text[64];
        NameText(sTrackName, text, sizeof(text));
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC motion #%u %s after %u frames P1 %d->%d P2 %d->%d", sTrackCount, text,
                    frames, sTrackP1 ? 1 : 0, a ? 1 : 0, sTrackP2 ? 1 : 0, b ? 1 : 0);
    }
    sTrackP1 = a;
    sTrackP2 = b;
    if ((!a && !b) || frames > TrackMaxFrames) sTrackActive = false;
}

HOOK_DEFINE_TRAMPOLINE(Diag46CoreState) {
    static std::uint64_t Callback(void* sm, const std::uint32_t* name, std::uint32_t force) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        const std::uint64_t result = Orig(sm, name, force);
        Diag46State(sm, name, force, ra, ra2);
        return result;
    }
};

HOOK_DEFINE_TRAMPOLINE(Diag46MotionRequest) {
    static std::uint64_t Callback(void* comp, const std::uint8_t* request, void* a2, void* a3) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        const std::uint64_t result = Orig(comp, request, a2, a3);
        Diag46Request(comp, request, ra, ra2);
        return result;
    }
};

// PATCH-0260: entries in a motion table, -1 when it is missing.
static int MotionTableCount(const void* table) {
    if (!IsHeapish(table)) return -1;
    return Read<std::int32_t>(table, MotionTableNum) - Read<std::int32_t>(table, MotionTableFree);
}

static void LogMotionSet(const char* what, void* asset, std::uint32_t replace, char kind, void* table1, int before1,
                         void* table2, int before2, std::uintptr_t ra, std::uintptr_t ra2) {
    if (sMotionSetLines >= MaxMotionSetLines) return;
    sMotionSetLines++;
    char name[64];
    char cls[48];
    NameText(reinterpret_cast<const std::uint32_t*>(static_cast<std::uint8_t*>(asset) + ObjectNameOffset), name,
             sizeof(name));
    ClassText(asset, cls, sizeof(cls));
    Logging.Log("[PShowCoop] PATCH-0260 %s asset=%s (%s %p) replace=%u table %c P1 %p n=%d->%d P2 %p n=%d->%d "
                "frame=%u raNSO=0x%lx ra2NSO=0x%lx",
                what, name, cls, asset, replace, kind, table1, before1, MotionTableCount(table1), table2, before2,
                MotionTableCount(table2), sFrame, static_cast<unsigned long>(ToNso(ra)),
                static_cast<unsigned long>(ToNso(ra2)));
}

HOOK_DEFINE_TRAMPOLINE(MotionTableAdd) {
    static void* Callback(void* table, void* asset, std::uint32_t replace, void* a3, void* a4, void* a5, void* a6,
                          void* a7) {
        const int before = MotionTableCount(table);
        void* result = Orig(table, asset, replace, a3, a4, a5, a6, a7);
        if (!kPatch0260MotionSetParity || sMotionSetDepth > 0 || sReplayDepth > 0 || !IsHeapish(asset) ||
            !IsHeapish(table)) {
            return result;
        }
        // Only an installed table of a player pawn: tables still being built in
        // pawn setup are not yet held by the component.
        void* owner = Read<void*>(table, ObjectOuterOffset);
        if (!IsPlayerPawn(owner)) return result;
        void* comp = Read<void*>(owner, PawnMotionComponent);
        if (!IsHeapish(comp)) return result;
        std::uintptr_t which = 0;
        if (Read<void*>(comp, MotionTableA) == table) {
            which = MotionTableA;
        } else if (Read<void*>(comp, MotionTableB) == table) {
            which = MotionTableB;
        } else {
            return result;
        }
        if (At<GetPlayerFnT>(GetPlayerPawnFn)(owner, 0) != owner) return result;
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        const char kind = which == MotionTableA ? 'A' : 'B';
        void* p2 = NativeP2Pawn(owner);
        void* comp2 = p2 != nullptr ? Read<void*>(p2, PawnMotionComponent) : nullptr;
        void* table2 = IsHeapish(comp2) ? Read<void*>(comp2, which) : nullptr;
        if (!IsHeapish(table2) || table2 == table) {
            LogMotionSet("skip", asset, replace, kind, table, before, table2, -1, ra, ra2);
            return result;
        }
        const int before2 = MotionTableCount(table2);
        ++sMotionSetDepth;
        Orig(table2, asset, replace, a3, a4, a5, a6, a7);
        --sMotionSetDepth;
        LogMotionSet("add", asset, replace, kind, table, before, table2, before2, ra, ra2);
        return result;
    }
};

// PATCH-0257 hooks: the original for P1, then the same native function for P2.
HOOK_DEFINE_TRAMPOLINE(IfaceSetLocation) {
    static std::uint32_t Callback(void* iface, const float* loc, std::uint32_t sweep, void* hit,
                                  std::uint32_t teleport) {
        void* pawn = PawnOfPlayerIface(iface);
        std::uint32_t mask = 0;
        void* p2 = loc != nullptr ? MirrorTarget(pawn, &mask) : nullptr;
        float before[3] = {0.0f, 0.0f, 0.0f};
        if (p2 != nullptr) RootLocation3(pawn, before);
        const std::uint32_t result = Orig(iface, loc, sweep, hit, teleport);
        if (loc == nullptr || pawn != GetP1Pawn()) return result;
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        if (p2 == nullptr) {
            NoteIface('l', "loc", false, mask, ra, ra2, loc, nullptr);
            return result;
        }
        float target[3];
        const char* mode = FollowTarget(p2, before, loc, target);
        const std::uint32_t moved = Orig(PlayerIfaceOf(p2), target, sweep, nullptr, teleport);
        sMirrorLoc++;
        sP2EventMoved = true;
        sLastMode = mode;
        // One line per (mode, call site): which follow phases each mover reached.
        if (IfaceFirst(mode[0], ra)) {
            Logging.Log("[PShowCoop] PATCH-0257 loc %s P1 (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f) P2 to (%.1f,%.1f,%.1f) moved=%u "
                        "sweep=%u tp=%u path=%.1f along=%.1f mask=0x%x raNSO=0x%lx ra2NSO=0x%lx",
                        mode, before[0], before[1], before[2], loc[0], loc[1], loc[2], target[0], target[1],
                        target[2], moved & 1, sweep & 1, teleport, sPathLength, sP2Along, mask,
                        static_cast<unsigned long>(ToNso(ra)), static_cast<unsigned long>(ToNso(ra2)));
        }
        return result;
    }
};

HOOK_DEFINE_TRAMPOLINE(IfaceSetRotation) {
    static void Callback(void* iface, const float* rotator) {
        Orig(iface, rotator);
        void* pawn = PawnOfPlayerIface(iface);
        if (rotator == nullptr || pawn != GetP1Pawn()) return;
        std::uint32_t mask = 0;
        void* p2 = MirrorTarget(pawn, &mask);
        if (p2 != nullptr) {
            Orig(PlayerIfaceOf(p2), rotator);
            sMirrorRot++;
        }
        NoteIface('r', "rot", p2 != nullptr, mask, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),
                  CallerReturn(__builtin_frame_address(0)), rotator, nullptr);
    }
};

HOOK_DEFINE_TRAMPOLINE(IfaceSetDirection) {
    static void Callback(void* iface, const float* direction) {
        Orig(iface, direction);
        void* pawn = PawnOfPlayerIface(iface);
        if (direction == nullptr || pawn != GetP1Pawn()) return;
        std::uint32_t mask = 0;
        void* p2 = MirrorTarget(pawn, &mask);
        if (p2 != nullptr) {
            Orig(PlayerIfaceOf(p2), direction);
            sMirrorDir++;
        }
        NoteIface('d', "dir", p2 != nullptr, mask, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),
                  CallerReturn(__builtin_frame_address(0)), direction, nullptr);
    }
};

HOOK_DEFINE_TRAMPOLINE(IfaceCoreName) {
    static void Callback(void* iface, const std::uint32_t* name, std::uint32_t flag) {
        Orig(iface, name, flag);
        void* pawn = PawnOfPlayerIface(iface);
        if (name == nullptr || pawn != GetP1Pawn()) return;
        std::uint32_t mask = 0;
        void* p2 = MirrorTarget(pawn, &mask);
        if (p2 != nullptr) {
            Orig(PlayerIfaceOf(p2), name, flag);
            sMirrorName++;
        }
        NoteIface('n', "core-name", p2 != nullptr, mask, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),
                  CallerReturn(__builtin_frame_address(0)), nullptr, name);
    }
};

HOOK_DEFINE_TRAMPOLINE(PawnPlayMotion) {
    static void Callback(void* pawn, const std::uint32_t* name) {
        Orig(pawn, name);
        if (name == nullptr || pawn != GetP1Pawn()) return;
        std::uint32_t mask = 0;
        void* p2 = MirrorTarget(pawn, &mask);
        if (p2 != nullptr) {
            Orig(p2, name);
            sMirrorMotion++;
            Diag46Track(pawn, p2, name);
        }
        NoteIface('m', "motion", p2 != nullptr, mask, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)),
                  CallerReturn(__builtin_frame_address(0)), nullptr, name);
    }
};
static void Census45(char op, void* iface, std::uint64_t arg, std::uintptr_t ra, std::uintptr_t ra2) {
    void* pawn = PawnOfPlayerIface(iface);
    void* p1 = GetP1Pawn();
    const int who = (p1 != nullptr && pawn == p1) ? 1 : (pawn == GetP2Pawn() && pawn != nullptr) ? 2 : 0;
    if (who == 0) return;
    const std::uint64_t key = (static_cast<std::uint64_t>(ToNso(ra)) << 16) | (static_cast<std::uint64_t>(op) << 8) |
                              static_cast<std::uint64_t>(who);
    for (std::uint32_t i = 0; i < sCensus45KeyCount; ++i) {
        if (sCensus45Keys[i] == key) return;
    }
    if (sCensus45KeyCount >= MaxCensus45Keys || sCensus45Lines >= MaxCensus45Lines) return;
    sCensus45Keys[sCensus45KeyCount++] = key;
    sCensus45Lines++;
    std::uint32_t mask = 0;
    const bool held = p1 != nullptr && IsPlayerPawn(p1) && HeldByEvent(p1, &mask);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC iface%c who=P%d arg=0x%lx p1held=%d mask=0x%x replay=%u raNSO=0x%lx ra2NSO=0x%lx",
                op, who, static_cast<unsigned long>(arg), held ? 1 : 0, mask, sReplayDepth,
                static_cast<unsigned long>(ToNso(ra)), static_cast<unsigned long>(ToNso(ra2)));
}

#define PSHOW_CENSUS45(NAME, OP)                                                                       \
    HOOK_DEFINE_TRAMPOLINE(NAME) {                                                                     \
        static void Callback(void* iface, void* a1, void* a2, void* a3) {                              \
            const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));            \
            const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));                       \
            Orig(iface, a1, a2, a3);                                                                   \
            Census45(OP, iface, reinterpret_cast<std::uint64_t>(a1), ra, ra2);                         \
        }                                                                                              \
    };

PSHOW_CENSUS45(CensusLockAdd, '+')
PSHOW_CENSUS45(CensusLockRemove, '-')
PSHOW_CENSUS45(CensusSpeedMode, 's')
PSHOW_CENSUS45(CensusMoveReset, 'x')
PSHOW_CENSUS45(CensusFlag38, '6')
PSHOW_CENSUS45(CensusFlag43, '3')

#undef PSHOW_CENSUS45
static void Census(char op, void* iface, std::uintptr_t ra, std::uintptr_t ra2, float x, float y, float z,
                   std::uint32_t a, std::uint32_t b) {
    void* p1 = GetP1Pawn();
    void* p2 = GetP2Pawn();
    const int who = (p1 != nullptr && iface == static_cast<std::uint8_t*>(p1) + PawnIface) ? 1
                  : (p2 != nullptr && iface == static_cast<std::uint8_t*>(p2) + PawnIface) ? 2 : 0;
    if (who == 0) return;
    const std::uint64_t key = (static_cast<std::uint64_t>(ToNso(ra)) << 16) | (static_cast<std::uint64_t>(op) << 8) |
                              static_cast<std::uint64_t>(who);
    for (std::uint32_t i = 0; i < sCensusKeyCount; ++i) {
        if (sCensusKeys[i] == key) return;
    }
    if (sCensusKeyCount < MaxCensusKeys) sCensusKeys[sCensusKeyCount++] = key;
    if (sCensusLines >= MaxCensusLines) return;
    sCensusLines++;
    if (op == 'm') {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC move who=P%d to (%.1f,%.1f,%.1f) sweep=%u raNSO=0x%lx ra2NSO=0x%lx replay=%u",
                    who, x, y, z, a & 1, static_cast<unsigned long>(ToNso(ra)),
                    static_cast<unsigned long>(ToNso(ra2)), sReplayDepth);
    } else {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC gate%c who=P%d reason=%u set=%u raNSO=0x%lx ra2NSO=0x%lx replay=%u",
                    op, who, a, b & 1, static_cast<unsigned long>(ToNso(ra)),
                    static_cast<unsigned long>(ToNso(ra2)), sReplayDepth);
    }
}

HOOK_DEFINE_TRAMPOLINE(CensusMove) {
    static std::uint32_t Callback(void* iface, const float* loc, std::uint32_t sweep, void* hit,
                                  std::uint32_t teleport, void* extra) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        const std::uint32_t result = Orig(iface, loc, sweep, hit, teleport, extra);
        if (loc != nullptr) Census('m', iface, ra, ra2, loc[0], loc[1], loc[2], sweep, teleport);
        return result;
    }
};

HOOK_DEFINE_TRAMPOLINE(CensusGate) {
    static void Callback(void* iface, std::uint32_t reason, std::uint32_t set) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        Orig(iface, reason, set);
        Census('r', iface, ra, ra2, 0.0f, 0.0f, 0.0f, reason, set);
    }
};

HOOK_DEFINE_TRAMPOLINE(CensusGate0) {
    static void Callback(void* iface, std::uint32_t set) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        Orig(iface, set);
        Census('0', iface, ra, ra2, 0.0f, 0.0f, 0.0f, 0, set);
    }
};

} // namespace

bool P1HeldByEvent() {
    void* p1 = GetP1Pawn();
    return IsPlayerPawn(p1) && HeldByEvent(p1, nullptr);
}

void EventParityFrame(void* p1Pawn, void* p2Pawn) {
    sFrame++;
    Diag46Poll(p1Pawn, p2Pawn);
    if (!kDiag0244KeepAlive) return;
    float a[3];
    float b[3];
    RootLocation3(p1Pawn, a);
    RootLocation3(p2Pawn, b);
    const std::uint32_t mirrors = sMirrorLoc + sMirrorRot + sMirrorDir + sMirrorMotion + sMirrorName;
    const bool changed = mirrors != sTraceMirrors || Dist3(a, sTraceP1) >= 1.0f || Dist3(b, sTraceP2) >= 1.0f;
    const std::uint32_t since = sFrame - sTraceFrame;
    if (since < (changed ? TraceEvery : TraceIdleEvery)) return;
    sTraceFrame = sFrame;
    sTraceMirrors = mirrors;
    Copy3(sTraceP1, a);
    Copy3(sTraceP2, b);
    std::uint32_t mask = 0;
    HeldByEvent(p1Pawn, &mask);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC t=%u P1(%.0f,%.0f,%.0f) P2(%.0f,%.0f,%.0f) epim=%d mask1=0x%x walk=%s "
                "mirror loc=%u rot=%u dir=%u motion=%u name=%u",
                sFrame, a[0], a[1], a[2], b[0], b[1], b[2], sLastEventInputFlag, mask, sLastMode, sMirrorLoc,
                sMirrorRot, sMirrorDir, sMirrorMotion, sMirrorName);
}

void InstallEventParity() {
    if (kPatch0253HelperReplay || kPatch0254PlacementParity) {
        GetPlayerPawnRedirect::InstallAtOffset(GetPlayerPawnFn);
        ReplayCompanionReset::InstallAtOffset(HelperCompanionReset);
    }
    riding::Install(+[](void* ctx, std::int32_t index) -> void* {
        if constexpr (kPatch0253HelperReplay || kPatch0254PlacementParity)
            return GetPlayerPawnRedirect::Orig(ctx, index);
        return At<GetPlayerFnT>(GetPlayerPawnFn)(ctx, index);
    });
    if (kPatch0253HelperReplay) {
        ReplayInputAndShow::InstallAtOffset(HelperInputAndShow);
        ReplayShow::InstallAtOffset(HelperShow);
        ReplayReason2::InstallAtOffset(HelperReason2);
        ReplayFloat::InstallAtOffset(HelperFloat);
        ReplayFaceTarget::InstallAtOffset(HelperFaceTarget);
        ReplayCompanionShow::InstallAtOffset(HelperCompanionShow);
    }
    if (kPatch0255EventInputParity) {
        EventInputParity::InstallAtOffset(EventInputUpdateFn);
    }
    if (kPatch0257IfaceParity) {
        IfaceSetLocation::InstallAtOffset(IfaceSetLocationFn);
        IfaceSetRotation::InstallAtOffset(IfaceSetRotationFn);
        IfaceSetDirection::InstallAtOffset(IfaceSetDirectionFn);
        IfaceCoreName::InstallAtOffset(IfaceCoreNameFn);
        PawnPlayMotion::InstallAtOffset(PawnPlayMotionFn);
    }
    if (kPatch0259EventControlParity) {
        EventControlBegin::InstallAtOffset(EventControlBeginFn);
        EventControlEnd::InstallAtOffset(EventControlEndFn);
    }
    if (kPatch0260MotionSetParity) {
        MotionTableAdd::InstallAtOffset(MotionTableAddFn);
    }
    if (kDiag0245IfaceCensus) {
        CensusLockAdd::InstallAtOffset(IfaceLockAddFn);
        CensusLockRemove::InstallAtOffset(IfaceLockRemoveFn);
        CensusSpeedMode::InstallAtOffset(IfaceSpeedModeFn);
        CensusMoveReset::InstallAtOffset(IfaceMoveResetFn);
        CensusFlag38::InstallAtOffset(IfaceFlag38Fn);
        CensusFlag43::InstallAtOffset(IfaceFlag43Fn);
    }
    if (kDiag0246MotionState) {
        Diag46CoreState::InstallAtOffset(CoreStateSetFn);
        Diag46MotionRequest::InstallAtOffset(MotionRequestFn);
    }
    if (kDiag0243ActionCensus) {
        CensusMove::InstallAtOffset(IfaceMoveFn);
        CensusGate::InstallAtOffset(IfaceGateFn);
        CensusGate0::InstallAtOffset(IfaceGate0Fn);
    }
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC armed event_owner=%d retired_motion46=1 legacy_broadcast_probe=off", kDiag0254EventOwner ? 1 : 0);
    Logging.Log("[PShowCoop] PATCH-0253/0254/0255/0257/0259/0260 installed replay=%d place=%d event_input=%d iface=%d "
                "event_control=%d motion_set=%d census43=%d census45=%d motion46=%d getpawn=0x%lx event_update=0x%lx "
                "set_location=0x%lx play_motion=0x%lx control_begin=0x%lx control_end=0x%lx table_add=0x%lx",
                kPatch0253HelperReplay ? 1 : 0, kPatch0254PlacementParity ? 1 : 0, kPatch0255EventInputParity ? 1 : 0,
                kPatch0257IfaceParity ? 1 : 0, kPatch0259EventControlParity ? 1 : 0, kPatch0260MotionSetParity ? 1 : 0,
                kDiag0243ActionCensus ? 1 : 0, kDiag0245IfaceCensus ? 1 : 0, kDiag0246MotionState ? 1 : 0,
                static_cast<unsigned long>(GetPlayerPawnFn), static_cast<unsigned long>(EventInputUpdateFn),
                static_cast<unsigned long>(IfaceSetLocationFn), static_cast<unsigned long>(PawnPlayMotionFn),
                static_cast<unsigned long>(EventControlBeginFn), static_cast<unsigned long>(EventControlEndFn),
                static_cast<unsigned long>(MotionTableAddFn));
}

} // namespace pshowcoop::patches
