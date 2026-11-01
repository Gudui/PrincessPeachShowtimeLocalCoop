#include "pointer_guard.hpp"
#include "patch_enemy_target.hpp"
#include "patch_core.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// An enemy's targets come from target-finder instances (vtable slot 0x290,
// (finder, TArray<AActor*>& targets) -> bool), merged by the finder groups
// (0xF0A160) and handed whole to the enemy's target component (+0x168,
// 0xF0767C/0xF07974). The player finders loop i < 0x1E481D8() over
// GetPlayerCharacter(finder, i) and add the first player that passes their
// test; 0x1E481D8 returns the constant 1, so only P1 is ever tested. That
// count, the 4-byte GetPlayerCharacter thunk and the 12-byte controller
// iterator are too short to hook, and GetPlayerCharacter's own entry moves an
// ADRP, so the loop cannot be widened in place. Instead, when a finder's
// original found no player, the loop body runs once more for player index 1
// with the finder's own test functions - what the game's loop does with a
// count of 2: P2 becomes a target only when P1 does not pass.
//   _Player      0xF0C66C: P2's character exists and is not pending kill.
//   _PlayerInRange 0xF0CB14: 0xF05F20(finder, weak(P2), range), range = the
//                finder config's +0x38 float (config = weak finder +0x58).
//   _PlayerInArea 0xF0CD1C: as its helper 0xF060EC for one player:
//                0xF06578(weak(P2), weak(area +0x220)), area = config +0x38.
// (_PlayerInSight 0xF0CEE8 runs the player queries below and is not hooked.)
// stays for courses whose enemies do. P2 is added as the finders add P1:
// append when not already present, growing the array through its own
// 0x166D5C. Entries (Ghidra listings, no PC-relative instruction in the moved
// bytes): 0xF0C66C stp x29,x30,[sp,#-0x50]! / stp x26,x25 / mov x29,sp /
// stp x24,x23 / stp x22,x21; 0xF0CB14 sub sp,#0x50 / str d8 / str x23 /
// stp x29,x30 / add x29; 0xF0CD1C sub sp,#0x40 / stp x29,x30 / add x29 /
// stp x22,x21 / stp x20,x19. Each is referenced only from its own vtable slot
// 0x290 (0x9189A08, 0x9189F58, 0x918A200).
// ---------------------------------------------------------------------------
constexpr bool kPatch0263EnemyTargetParity = true;

// ---------------------------------------------------------------------------
// PATCH-0265: the game's two player queries also consider P2 when an enemy
// collision shape) and 0x101CC34 (nearest visible player) loop
// i < 0x1E481D8() (= 1) over GetPlayerCharacter(world, i) and keep the nearest
// player that passes. Fencer_A's spotlight swordsmen start their tackle
// (action 9, 0xF46160) when 0xDCFE24 finds a player in their area through
// 0x101D0DC, and tackle the player the queries return (0xF461D0 stores it at
// +0x274); with P2 inside the area and P1 outside, the area query answered no
// the loop body runs once more for player index 1 as the listings show it
// (0x101D2A0..0x101D550, 0x101CD8C..0x101CE88): P2's character not pending
// kill, its root location (zero without a root); for 0x101D0DC padded by its
// shape root's GetCollisionShape (slot 0x768, inflation 0) first extent times
// scale X (+0x1DC) and its height (box Z, capsule half height, sphere radius)
// times scale Z (+0x1E4), inside the shape at its centre in the frame of its
// rotation; then, when asked, the line of sight - trace 0x1018FC0 from the
// actor's root (0x101D0DC) or the eye (0x101CC34) with a zeroed 0x88-byte hit
// result whose Time is 1, blocked when it hits an actor 0xDB9870 does not
// accept as the character. A passing P2 is kept when it is strictly nearer
// than the original's result (or there was none), as the loop keeps it; a
// caller that asks for no actor and already has a player is left alone.
// ---------------------------------------------------------------------------
constexpr bool kPatch0265PlayerQueryParity = true;

// ---------------------------------------------------------------------------
// swap allowed for arena enemy updates only). The arena's EnemyCombatAI
// coordinator rings its enemies around player 1 and picks attackers from
// player 1's facing, and their combat components read player 1 directly -
// about 20 sites, all through the by-number getter 0x1E481E0, which cannot be
// hooked. For the length of one arena update the world's player-controller
// list (world +0x1C0, TArray of 8-byte weak pointers, as GetPlayerCharacter
// 0x44FB574 walks it through 0x4A77620) has its first two entries exchanged
// when P2 is the nearer player, and exchanged back as soon as the update
// returns, so the game's own arena code runs unchanged against P2:
//   walker combat action 16 (0x1279E08, from the action machine only): the
//     player nearer the walker;
//   EnemyCombatAIBase::Tick (0xEE6404, vtable 0x9174B10 slot 0x468): the
//     player nearer the centre of its live enemies (+0x428, count +0x430);
//     no exchange without one.
// Only when the list's entries resolve to P1's and P2's controllers in that
// order; never nested. Entries: 0x1279E08 sub sp,sp,#0xf0 / stp x29,x30 /
// add x29 / str x21 (8-aligned, 16 bytes); 0xEE6404 str d8,[sp,#-0x20]! /
// stp x29,x30 / add x29 / str x19 / mov v8.16B,v0.16B (20 bytes).
// ---------------------------------------------------------------------------
constexpr bool kPatch0267ArenaNearerPlayer = true;

// ---------------------------------------------------------------------------
// version: only 0xDCFE24 (area, no actor wanted) and 0x100EB98 (view) asked,
// and the group woke (action 9) only while P1 stood in its box. Four
// instruments:
//  query  - both queries, per caller (two return addresses): calls, found,
//           P2-nearer and P2-added counts, the first call, up to four lines
//           while P2 is nearer the asking actor than P1 and the first three
//           where PATCH-0265 added P2;
//  walker - the walker Tick: each walker near a player when first seen and on
//           every change of its state bytes, with its distance to each;
//  state  - FixedEnemyBase::ChangeState 0x1056588 (+0x2D4) near a player: old
//           and new state, its two return addresses and the distances;
//  event  - the walker's event receiver 0x126BED8 (vtable slot 0x678): the
//           event's first 16 bytes, up to ten return addresses up the frame
//           chain (run 6 stopped at ProcessEvent's dispatcher 0x292EA0C, so
//           the sender's own code is further up), and whether it set the
//           action-13 trigger (+0xF50 -> +0x248) and +0x43B.
// Entries (Ghidra listings, no PC-relative instruction in the moved bytes):
// 0x101D0DC sub sp,sp,#0x1c0 / stp d15,d14 / stp d13,d12 / stp d11,d10 /
// stp d9,d8; 0x101CC34 sub sp,sp,#0x160 / stp d9,d8 / stp x29,x30 / add x29 /
// stp x28,x27; 0x12694E0 stp x29,x30,[sp,#-0x20]! / str x19 / mov x29,sp /
// mov x19,x0; 0x1056588 stp x29,x30,[sp,#-0x20]! / stp x20,x19 / mov x29,sp /
// ldr x8,[x0]; 0x126BED8 sub sp,sp,#0x40 / stp x29,x30 / add x29 / str x21.
// Remove once read.
// ---------------------------------------------------------------------------
constexpr bool kDiag0249EnemyCensus = true;

constexpr ptrdiff_t FinderPlayerFn = 0xF0C66C;
constexpr ptrdiff_t FinderInRangeFn = 0xF0CB14;
constexpr ptrdiff_t FinderInAreaFn = 0xF0CD1C;
constexpr ptrdiff_t GetPlayerCharacterFn = 0x44FB574;  // UGameplayStatics::GetPlayerCharacter(ctx, index)
constexpr ptrdiff_t WeakMakeFn = 0x2A72F3C;            // FWeakObjectPtr = UObject* (index +0xC, serial)
constexpr ptrdiff_t WeakIsValidFn = 0x2A73028;         // serial match, not unreachable, not pending kill
constexpr ptrdiff_t WeakGetFn = 0x2A7312C;             // FUNC-WEAKOBJECT-RESOLVE
constexpr ptrdiff_t InRangeTestFn = 0xF05F20;          // (finder, weak target, float range) -> bool
constexpr ptrdiff_t AreaTestFn = 0xF06578;             // (weak character, weak area +0x220) -> bool
constexpr ptrdiff_t TargetArrayGrowFn = 0x166D5C;      // TArray<AActor*> ResizeGrow(array, old num)
constexpr std::uintptr_t FinderConfigWeak = 0x58;      // add x0,x0,#0x58 before the resolve in each finder
constexpr std::uintptr_t ConfigRangeOrArea = 0x38;     // ldr s8,[x0,#0x38] (range) / ldr x1,[x0,#0x38] (area)
constexpr std::uintptr_t AreaTargetObject = 0x220;     // *(area +0x220) in 0xF060EC
constexpr int P2PlayerIndex = 1;
constexpr std::uint32_t MaxP2Lines = 16;

constexpr ptrdiff_t AreaQueryFn = 0x101D0DC;    // (actor, shape*, centre*, quat*, trace, params, AActor** nearest) -> bool
constexpr ptrdiff_t ViewQueryFn = 0x101CC34;    // (actor, eye*, trace, params, AActor** nearest) -> bool
constexpr ptrdiff_t QueryTraceFn = 0x1018FC0;   // (actor, FHitResult*, start*, delta*, params) -> bool hit
constexpr ptrdiff_t HitBlocksFn = 0xDB9870;     // (character, weak hit actor*) -> bool: the hit is not the character's
constexpr ptrdiff_t EnemyBaseClassFn = 0x227285C;       // EnemyBase::StaticClass ("/Script/PJ037")
constexpr ptrdiff_t ShapeComponentClassFn = 0x4CD0CEC;  // UShapeComponent::StaticClass ("/Script/Engine")
constexpr std::uintptr_t GetWorldSlot = 0x168;           // actor vtable: GetWorld, as both queries call it
constexpr std::uintptr_t GetCollisionShapeSlot = 0x768;  // component vtable: GetCollisionShape(inflation)
constexpr std::uintptr_t ComponentScaleX = 0x1DC;        // the queries' padding scales (ldr s1,[x20,#0x1dc] / #0x1e4)
constexpr std::uintptr_t ComponentScaleZ = 0x1E4;
constexpr std::size_t HitResultSize = 0x88;              // memset(sp+0x60, 0, 0x88)
constexpr std::uintptr_t HitTime = 0x4;                  // str w8 (1.0f),[sp,#0x64]
constexpr std::uintptr_t HitActorWeak = 0x68;            // add x8,x8,#0x68: weak hit actor
// UStruct class-tree fields the native IsA reads (0x101D314..0x101D334).
constexpr std::uintptr_t ObjectClassOffset = 0x10;
constexpr std::uintptr_t ClassTreeArrayOffset = 0x30;
constexpr std::uintptr_t ClassTreeDepthOffset = 0x38;

constexpr ptrdiff_t ArenaActionFn = 0x1279E08;     // walker action 16: combat component (+0xF60/+0xF68) and movement
constexpr ptrdiff_t CoordinatorTickFn = 0xEE6404;  // EnemyCombatAIBase::Tick(this, DeltaSeconds)
constexpr std::uintptr_t WorldPlayerControllers = 0x1C0;  // TArray<TWeakObjectPtr<APlayerController>>: data, num +0x8
constexpr std::uintptr_t PawnControllerOffset = 0x258;    // patch_core's ControllerOffset
constexpr std::uintptr_t CoordinatorEnemies = 0x428;      // 0xEE2154: enemy pointers, count +0x430
constexpr std::uintptr_t CoordinatorEnemyCount = 0x430;
constexpr std::uint32_t MaxArenaLines = 40;
constexpr ptrdiff_t WalkerTickFn = 0x12694E0;   // EnemyWalker_C_Base::Tick (vtable 0x9226518 slot 0x468)
constexpr ptrdiff_t ChangeStateFn = 0x1056588;  // FixedEnemyBase::ChangeState(this, state): strb w20,[x19,#0x2d4]
constexpr ptrdiff_t WalkerEventFn = 0x126BED8;  // walker event receiver (vtable 0x9226518 slot 0x678; thunk 0x126C498)
// Actor and walker fields, as the walker Tick (0x12694E0, 0x1269664, 0x1269724,
// 0x126A7FC), EnemyBase's (0x1005730, 0x10058CC), the idle action 0x12779C0
// and the event receiver 0x126BED8 read them.
constexpr std::uintptr_t ActorRoot = 0x130;           // root component; world location at +0x1D0
constexpr std::uintptr_t RootLocation = 0x1D0;
constexpr std::uintptr_t EnemyState = 0x2D4;          // u8; the walker's state machines run only while 0
constexpr std::uintptr_t EnemyCollision = 0x388;      // collision component; u8 +0xB8 enabled (0xA3AFE0)
constexpr std::uintptr_t CollisionEnabled = 0xB8;
constexpr std::uintptr_t WalkerEventLatch = 0x43B;    // u8 set with the trigger by 0x126C478
constexpr std::uintptr_t NearSwitch = 0x539;          // u8: 0x10058CC sets collision from P1's distance
constexpr std::uintptr_t NearRadius = 0x53C;          // f32 radius of that test
constexpr std::uintptr_t WalkerSubComponent = 0xEE0;  // its u8 +0x108 is tested by the Tick
constexpr std::uintptr_t SubComponentState = 0x108;
constexpr std::uintptr_t WalkerAiComponent = 0xEF8;   // AI component run by action 2 (0xF14F10)
constexpr std::uintptr_t WalkerTriggerComponent = 0xF50;  // action 13 starts when its u8 +0x248 is set (0xF10894)
constexpr std::uintptr_t TriggerFlag = 0x248;
constexpr std::uintptr_t WalkerAction = 0x1020;       // u8 action machine (0x1269724)
constexpr std::uintptr_t WalkerState = 0x1058;        // u8 state machine (0x1269664)
constexpr std::uintptr_t WalkerNoticed = 0x11CD;      // u8 latch set by 0x126A7FC from player 1's angle/distance

constexpr float NearPlayer = 2500.0f;      // walker and state lines only within this distance of a player
constexpr float FirstSeenNear = 4000.0f;
constexpr float EventNear = 6000.0f;
constexpr std::uint32_t QueryCallers = 64;
constexpr std::uint32_t MaxQuerySamples = 4;   // per caller
constexpr std::uint32_t QuerySampleGap = 32;   // calls of that caller between samples
constexpr std::uint32_t MaxAddedSamples = 3;   // per caller
constexpr std::uint32_t MaxQueryLines = 240;
constexpr std::uint32_t QuerySummaryFirst = 256;
constexpr std::uint32_t MaxQuerySummaries = 16;
constexpr std::uint32_t WalkerSlots = 256;
constexpr std::uint32_t WalkerBytes = 9;
constexpr std::uint32_t MaxWalkerLines = 600;
constexpr std::uint32_t MaxStateLines = 200;
constexpr std::uint32_t MaxEventLines = 150;
constexpr int EventChainDepth = 10;  // return addresses walked up from the event receiver

using GetPlayerCharacterFnT = void* (*)(void*, int);
using WeakMakeFnT = void (*)(void*, void*);
using WeakIsValidFnT = bool (*)(const void*);
using WeakGetFnT = void* (*)(const void*);
using InRangeTestFnT = std::uint64_t (*)(void*, const void*, float);
using AreaTestFnT = std::uint64_t (*)(const void*, const void*);
using TargetArrayGrowFnT = void (*)(void*, int);
using ClassGetterFn = void* (*)();
using GetWorldFnT = void* (*)(void*);
using QueryTraceFnT = std::uint64_t (*)(void*, void*, const float*, const float*, void*);
using HitBlocksFnT = std::uint64_t (*)(void*, const void*);

enum Kind : int { KindPlayer = 0, KindInRange = 1, KindInArea = 2, KindCount = 3 };
const char* const kKindNames[KindCount] = {"player", "in_range", "in_area"};

// PATCH-0265's result for one query call.
enum class P2Step : int { Off = 0, NotEnemy, Skipped, NoP2, Failed, Farther, Added };
const char* const kStepNames[] = {"off", "not-enemy", "skipped", "no-p2", "failed", "farther", "added"};

// FWeakObjectPtr: object index, serial number.
struct WeakPtr {
    std::int32_t index;
    std::int32_t serial;
};

// The finders' TArray<AActor*>: data, num (+0x8), max (+0xC) - as read by 0xF0CBA0..0xF0CBFC.
struct ActorArray {
    void** data;
    std::int32_t num;
    std::int32_t max;
};

// FCollisionShape: type (1 box, 2 sphere, 3 capsule) and three floats; GetCollisionShape returns it in x0:x1.
struct CollisionShape {
    std::int32_t type;
    float v[3];
};
using GetCollisionShapeFnT = CollisionShape (*)(void*, float);

static std::uint32_t sP2Lines = 0;
struct QueryCaller {
    std::uintptr_t ra;
    std::uintptr_t ra2;
    std::uint32_t kind;
    std::uint32_t calls;
    std::uint32_t found;
    std::uint32_t p2Nearer;
    std::uint32_t p2Added;
    std::uint32_t samples;
    std::uint32_t addedSamples;
    std::uint32_t lastSample;
};

struct WalkerSeen {
    void* walker;
    std::uint8_t bytes[WalkerBytes];
    std::uint32_t ai;
};

static QueryCaller sCallers[QueryCallers] = {};
static std::uint32_t sCallerCount = 0;
static std::uint32_t sQueryCalls = 0;
static std::uint32_t sQueryLines = 0;
static std::uint32_t sQuerySummaries = 0;
static std::uint32_t sP2Added[2] = {};
static WalkerSeen sWalkers[WalkerSlots] = {};
static std::uint32_t sWalkerLines = 0;
static std::uint32_t sStateLines = 0;
static std::uint32_t sEventLines = 0;

template <typename Fn>
static Fn At(ptrdiff_t offset) {
    return reinterpret_cast<Fn>(exl::util::GetMainModuleInfo().m_Total.m_Start + offset);
}

static std::uintptr_t MainStart() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

static bool InMain(std::uintptr_t p) { return exl::util::GetMainModuleInfo().m_Total.InRange(p); }

static std::uintptr_t ToNso(std::uintptr_t p) { return InMain(p) ? p - MainStart() : 0; }

template <typename T>
static T Read(const void* base, std::uintptr_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(base) + offset);
}

template <typename Fn>
static Fn Virtual(const void* object, std::uintptr_t slot) {
    return Read<Fn>(Read<void*>(object, 0), slot);
}

static bool IsHeapish(const void* value) {
    return pshowcoop::pointerguard::Readable(value);
}

// A pointer the caller passed to a float array (stack or heap).
static bool FloatsReadable(const void* value) {
    return value != nullptr && (reinterpret_cast<std::uintptr_t>(value) & 0x3) == 0;
}

static WeakPtr MakeWeak(void* object) {
    WeakPtr weak = {-1, 0};
    At<WeakMakeFnT>(WeakMakeFn)(&weak, object);
    return weak;
}

// A live object the engine itself would accept (not unreachable, not pending kill).
static bool LiveObject(void* object) {
    if (!IsHeapish(object)) return false;
    const WeakPtr weak = MakeWeak(object);
    return At<WeakIsValidFnT>(WeakIsValidFn)(&weak);
}

static bool IsA(const void* object, const void* cls) {
    const void* objClass = Read<void*>(object, ObjectClassOffset);
    if (objClass == nullptr || cls == nullptr) return false;
    const std::int32_t depth = Read<std::int32_t>(cls, ClassTreeDepthOffset);
    if (depth > Read<std::int32_t>(objClass, ClassTreeDepthOffset)) return false;
    void* const* tree = Read<void* const*>(objClass, ClassTreeArrayOffset);
    if (tree == nullptr) return false;
    return tree[depth] == static_cast<const std::uint8_t*>(cls) + ClassTreeArrayOffset;
}

// The finder's config object (weak at finder +0x58), null when it is gone.
static void* FinderConfig(void* finder) {
    const void* weak = static_cast<std::uint8_t*>(finder) + FinderConfigWeak;
    if (!At<WeakIsValidFnT>(WeakIsValidFn)(weak)) return nullptr;
    void* config = At<WeakGetFnT>(WeakGetFn)(weak);
    return IsHeapish(config) ? config : nullptr;
}

// Add `actor` when not present, as the finders do (0xF0CBA0..0xF0CBFC).
static void AddUnique(void* out, void* actor) {
    auto* array = static_cast<ActorArray*>(out);
    for (std::int32_t i = 0; i < array->num; ++i) {
        if (array->data[i] == actor) return;
    }
    const std::int32_t old = array->num;
    array->num = old + 1;
    if (array->max <= old) At<TargetArrayGrowFnT>(TargetArrayGrowFn)(out, old);
    array->data[old] = actor;
}

// The finder's own test for P2; true when P2 passes it.
static bool P2Passes(Kind kind, void* finder, void* p2) {
    switch (kind) {
    case KindPlayer:
        return true;
    case KindInRange: {
        void* config = FinderConfig(finder);
        if (config == nullptr) return false;
        const float range = *reinterpret_cast<const float*>(static_cast<std::uint8_t*>(config) + ConfigRangeOrArea);
        const WeakPtr weak = MakeWeak(p2);
        return (At<InRangeTestFnT>(InRangeTestFn)(finder, &weak, range) & 1) != 0;
    }
    case KindInArea: {
        void* config = FinderConfig(finder);
        if (config == nullptr) return false;
        void* area = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(config) + ConfigRangeOrArea);
        if (!LiveObject(area)) return false;
        const WeakPtr character = MakeWeak(p2);
        const WeakPtr target = MakeWeak(*reinterpret_cast<void**>(static_cast<std::uint8_t*>(area) + AreaTargetObject));
        return (At<AreaTestFnT>(AreaTestFn)(&character, &target) & 1) != 0;
    }
    default:
        return false;
    }
}

// After a finder's original: P1's result stands; with none, P2 gets the same test.
static std::uint64_t AfterFinder(Kind kind, void* finder, void* out, std::uint64_t result) {
    if ((result & 1) != 0 || !kPatch0263EnemyTargetParity || !IsHeapish(finder) || out == nullptr) return result;
    void* p2 = At<GetPlayerCharacterFnT>(GetPlayerCharacterFn)(finder, P2PlayerIndex);
    if (!LiveObject(p2) || !P2Passes(kind, finder, p2)) return result;
    AddUnique(out, p2);
    if (sP2Lines < MaxP2Lines) {
        sP2Lines++;
        Logging.Log("[PShowCoop] PATCH-0263 P2 target finder=%s (%p) p2=%p targets=%d line=%u", kKindNames[kind],
                    finder, p2, static_cast<ActorArray*>(out)->num, sP2Lines);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// PATCH-0265
// ---------------------------------------------------------------------------
// The queries' location of an actor: root +0x130 -> +0x1D0, zero without a root (FVector::ZeroVector).
static void* LocationOrZero(const void* actor, float* out) {
    void* root = Read<void*>(actor, ActorRoot);
    out[0] = out[1] = out[2] = 0.0f;
    if (root == nullptr) return nullptr;
    out[0] = Read<float>(root, RootLocation);
    out[1] = Read<float>(root, RootLocation + 4);
    out[2] = Read<float>(root, RootLocation + 8);
    return root;
}

// Player index 1's character for the asking actor's world, as the queries' loop resolves player i.
static void* SecondPlayer(void* actor) {
    void* world = Virtual<GetWorldFnT>(actor, GetWorldSlot)(actor);
    void* p2 = At<GetPlayerCharacterFnT>(GetPlayerCharacterFn)(world, P2PlayerIndex);
    return LiveObject(p2) ? p2 : nullptr;
}

static bool AskedByEnemy(void* actor) {
    return LiveObject(actor) && IsA(actor, At<ClassGetterFn>(EnemyBaseClassFn)());
}

// The queries' line of sight from `start` to the character at `loc` (0x101CE04..0x101CE88).
static bool InSight(void* actor, void* character, const float* start, const float* loc, void* params) {
    alignas(16) std::uint8_t hit[HitResultSize];
    std::memset(hit, 0, sizeof(hit));
    const float one = 1.0f;
    std::memcpy(hit + HitTime, &one, sizeof(one));
    const float delta[3] = {loc[0] - start[0], loc[1] - start[1], loc[2] - start[2]};
    if ((At<QueryTraceFnT>(QueryTraceFn)(actor, hit, start, delta, params) & 1) == 0) return true;
    const void* hitActor = hit + HitActorWeak;
    if (!At<WeakIsValidFnT>(WeakIsValidFn)(hitActor)) return false;
    return (At<HitBlocksFnT>(HitBlocksFn)(character, hitActor) & 1) == 0;
}

// 0x101D1E4..0x101D238: distance from the asking actor's location.
__attribute__((optimize("fp-contract=off"))) static float AreaDistance(const float* self, const float* loc) {
    const float dx = loc[0] - self[0];
    const float dy = loc[1] - self[1];
    const float dz = loc[2] - self[2];
    const float xx = dx * dx;
    const float yy = dy * dy;
    const float zz = dz * dz;
    const float xy = xx + yy;
    return __builtin_sqrtf(zz + xy);
}

// 0x101CD14..0x101CD40: distance from the eye.
__attribute__((optimize("fp-contract=off"))) static float EyeDistance(const float* eye, const float* loc) {
    const float dy = loc[1] - eye[1];
    const float dx = loc[0] - eye[0];
    const float dz = loc[2] - eye[2];
    const float yy = dy * dy;
    const float xx = dx * dx;
    const float xy = xx + yy;
    const float zz = dz * dz;
    return __builtin_sqrtf(xy + zz);
}

// 0x101D348..0x101D48C: the padded location inside the shape at `centre`, in the frame of `quat`.
// Kept to the listing's order of separate multiplies and adds.
__attribute__((optimize("fp-contract=off"))) static bool InShape(const float* loc, float padR, float padH,
                                                                 const void* shape, const float* centre,
                                                                 const float* quat) {
    const std::int32_t type = Read<std::int32_t>(shape, 0);
    const float s0 = Read<float>(shape, 4);
    const float s1 = Read<float>(shape, 8);
    const float s2 = Read<float>(shape, 12);
    float extX = 0.0f;
    float extY = 0.0f;
    float extZ = 0.0f;
    if (type == 3) {
        extX = s0;
        extY = s0;
        extZ = s1;
    } else if (type == 2) {
        extX = s0;
        extY = s0;
        extZ = s0;
    } else if (type == 1) {
        extX = s0;
        extY = s1;
        extZ = s2;
    }
    const float dy = loc[1] - centre[1];
    const float dz = loc[2] - centre[2];
    const float qz = quat[2];
    const float qw = quat[3];
    const float dx = loc[0] - centre[0];
    const float qx = quat[0];
    const float qy = quat[1];
    const float a = dy * qz;
    const float b = dz * qy;
    const float c = dy * qx;
    const float ab = a - b;
    const float d = dz * qx;
    const float e = dx * qz;
    const float de = d - e;
    const float f = dx * qy;
    const float tx = ab + ab;
    const float fc = f - c;
    const float ty = de + de;
    const float wtx = qw * tx;
    const float tz = fc + fc;
    const float x0 = dx + wtx;
    const float zty = qz * ty;
    const float ytz = qy * tz;
    const float x1 = zty - ytz;
    const float lx = x0 + x1;
    if (!(__builtin_fabsf(lx) - padR < extX)) return false;
    const float wty = qw * ty;
    const float ztx = qz * tx;
    const float y0 = dy + wty;
    const float xtz = qx * tz;
    const float y1 = xtz - ztx;
    const float ly = y0 + y1;
    if (!(__builtin_fabsf(ly) - padR < extY)) return false;
    const float wtz = qw * tz;
    const float xty = qx * ty;
    const float z0 = dz + wtz;
    const float ytx = qy * tx;
    const float z1 = ytx - xty;
    const float lz = z0 + z1;
    return __builtin_fabsf(lz) - padH < extZ;
}

// 0x101D2D8..0x101D600: the padding from the character's shape root.
static void ShapePadding(void* root, float* padR, float* padH) {
    *padR = 0.0f;
    *padH = 0.0f;
    if (root == nullptr || !IsA(root, At<ClassGetterFn>(ShapeComponentClassFn)()) || !LiveObject(root)) return;
    const CollisionShape own = Virtual<GetCollisionShapeFnT>(root, GetCollisionShapeSlot)(root, 0.0f);
    const float first = own.type >= 1 && own.type <= 3 ? own.v[0] : 0.0f;
    *padR = Read<float>(root, ComponentScaleX) * first;
    float height = 0.0f;
    if (own.type == 1) {
        height = own.v[2];
    } else if (own.type == 3) {
        height = own.v[1];
    } else if (own.type == 2) {
        height = own.v[0];
    }
    *padH = height * Read<float>(root, ComponentScaleZ);
}

// The area query's loop body for player index 1; P2 is kept when strictly nearer than the original's actor.
static P2Step AreaWithP2(void* actor, const void* shape, const float* centre, const float* quat,
                         std::uint64_t trace, void* params, void** nearest, std::uint64_t* found) {
    if (!kPatch0265PlayerQueryParity) return P2Step::Off;
    if (!AskedByEnemy(actor) || !FloatsReadable(shape) || !FloatsReadable(centre) || !FloatsReadable(quat))
        return P2Step::NotEnemy;
    if (nearest == nullptr && (*found & 1) != 0) return P2Step::Skipped;
    void* p2 = SecondPlayer(actor);
    if (p2 == nullptr) return P2Step::NoP2;
    float loc[3];
    void* root = LocationOrZero(p2, loc);
    float padR = 0.0f;
    float padH = 0.0f;
    ShapePadding(root, &padR, &padH);
    if (!InShape(loc, padR, padH, shape, centre, quat)) return P2Step::Failed;
    float self[3];
    LocationOrZero(actor, self);
    if ((trace & 1) != 0 && !InSight(actor, p2, self, loc, params)) return P2Step::Failed;
    void* previous = nearest != nullptr ? *nearest : nullptr;
    if (IsHeapish(previous)) {
        float other[3];
        LocationOrZero(previous, other);
        if (!(AreaDistance(self, loc) < AreaDistance(self, other))) return P2Step::Farther;
    }
    if (nearest != nullptr) *nearest = p2;
    *found = 1;
    return P2Step::Added;
}

// The view query's loop body for player index 1.
static P2Step ViewWithP2(void* actor, const float* eye, std::uint64_t trace, void* params, void** nearest,
                         std::uint64_t* found) {
    if (!kPatch0265PlayerQueryParity) return P2Step::Off;
    if (!AskedByEnemy(actor) || !FloatsReadable(eye)) return P2Step::NotEnemy;
    if (nearest == nullptr && (*found & 1) != 0) return P2Step::Skipped;
    void* p2 = SecondPlayer(actor);
    if (p2 == nullptr) return P2Step::NoP2;
    float loc[3];
    LocationOrZero(p2, loc);
    if ((trace & 1) != 0 && !InSight(actor, p2, eye, loc, params)) return P2Step::Failed;
    void* previous = nearest != nullptr ? *nearest : nullptr;
    if (IsHeapish(previous)) {
        float other[3];
        LocationOrZero(previous, other);
        if (!(EyeDistance(eye, loc) < EyeDistance(eye, other))) return P2Step::Farther;
    }
    if (nearest != nullptr) *nearest = p2;
    *found = 1;
    return P2Step::Added;
}

// ---------------------------------------------------------------------------
// PATCH-0267
// ---------------------------------------------------------------------------
static bool sArenaSwapped = false;
static std::uint32_t sArenaUpdates[2] = {};
static std::uint32_t sArenaSwaps[2] = {};
static std::uint32_t sArenaLines = 0;
const char* const kArenaScopes[2] = {"walker", "coordinator"};

// The controller a weak list entry names, or null.
static void* ResolveWeak(const void* weak) {
    if (!At<WeakIsValidFnT>(WeakIsValidFn)(weak)) return nullptr;
    return At<WeakGetFnT>(WeakGetFn)(weak);
}

static void* ControllerOf(void* pawn) {
    return IsHeapish(pawn) ? Read<void*>(pawn, PawnControllerOffset) : nullptr;
}

// The world's player list when its first two entries are P1's and P2's controllers, in that order.
static std::uint64_t* PlayerOrderList(void* actor) {
    void* world = Virtual<GetWorldFnT>(actor, GetWorldSlot)(actor);
    if (!IsHeapish(world)) return nullptr;
    auto* data = Read<std::uint64_t*>(world, WorldPlayerControllers);
    const std::int32_t num = Read<std::int32_t>(world, WorldPlayerControllers + 8);
    if (!IsHeapish(data) || num < 2) return nullptr;
    void* c1 = ControllerOf(GetP1Pawn());
    void* c2 = ControllerOf(GetP2Pawn());
    if (!IsHeapish(c1) || !IsHeapish(c2) || ResolveWeak(&data[0]) != c1 || ResolveWeak(&data[1]) != c2) return nullptr;
    return data;
}

static void ExchangeFirstTwo(std::uint64_t* data) {
    const std::uint64_t first = data[0];
    data[0] = data[1];
    data[1] = first;
}

// Whether P2 is strictly nearer `at` than P1 (both players live with a location).
static bool P2Nearer(const float* at, float* d1, float* d2) {
    float p1[3];
    float p2[3];
    void* pawn1 = GetP1Pawn();
    void* pawn2 = GetP2Pawn();
    if (!LiveObject(pawn1) || !LiveObject(pawn2)) return false;
    LocationOrZero(pawn1, p1);
    LocationOrZero(pawn2, p2);
    *d1 = AreaDistance(at, p1);
    *d2 = AreaDistance(at, p2);
    return *d2 < *d1;
}

// Starts a scoped exchange for `actor` when P2 is the nearer player to `at`; returns the list to restore.
static std::uint64_t* BeginArenaScope(int scope, void* actor, const float* at) {
    if (!kPatch0267ArenaNearerPlayer || sArenaSwapped || !LiveObject(actor)) return nullptr;
    sArenaUpdates[scope]++;
    float d1 = 0.0f;
    float d2 = 0.0f;
    if (!P2Nearer(at, &d1, &d2)) return nullptr;
    std::uint64_t* data = PlayerOrderList(actor);
    if (data == nullptr) return nullptr;
    ExchangeFirstTwo(data);
    sArenaSwapped = true;
    const std::uint32_t n = ++sArenaSwaps[scope];
    if (sArenaLines < MaxArenaLines && (n <= 8 || (n & (n - 1)) == 0)) {
        sArenaLines++;
        Logging.Log("[PShowCoop] PATCH-0267 %s=%p takes P2 d1=%.0f d2=%.0f swaps=%u/%u updates=%u/%u line=%u",
                    kArenaScopes[scope], actor, d1, d2, sArenaSwaps[0], sArenaSwaps[1], sArenaUpdates[0],
                    sArenaUpdates[1], sArenaLines);
    }
    return data;
}

static void EndArenaScope(std::uint64_t* data) {
    if (data == nullptr) return;
    ExchangeFirstTwo(data);
    sArenaSwapped = false;
}

// The centre of a coordinator's live enemies; false without one.
static bool EnemyCentre(const void* coordinator, float* out) {
    const std::int32_t count = Read<std::int32_t>(coordinator, CoordinatorEnemyCount);
    void* const* enemies = Read<void* const*>(coordinator, CoordinatorEnemies);
    if (count <= 0 || count > 64 || !IsHeapish(enemies)) return false;
    float sum[3] = {0.0f, 0.0f, 0.0f};
    int live = 0;
    for (std::int32_t i = 0; i < count; ++i) {
        void* enemy = enemies[i];
        if (!LiveObject(enemy) || !IsHeapish(Read<void*>(enemy, ActorRoot))) continue;
        float at[3];
        LocationOrZero(enemy, at);
        sum[0] += at[0];
        sum[1] += at[1];
        sum[2] += at[2];
        live++;
    }
    if (live == 0) return false;
    out[0] = sum[0] / live;
    out[1] = sum[1] / live;
    out[2] = sum[2] / live;
    return true;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
static bool RootLocation3(const void* actor, float* out) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!IsHeapish(actor)) return false;
    const void* root = Read<void*>(actor, ActorRoot);
    if (!IsHeapish(root)) return false;
    out[0] = Read<float>(root, RootLocation);
    out[1] = Read<float>(root, RootLocation + 4);
    out[2] = Read<float>(root, RootLocation + 8);
    return true;
}

static float Dist3(const float* a, const float* b) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
}

// Both players' root locations and the distances from `at`; -1 for a player without one.
struct PlayerSpots {
    float p1[3];
    float p2[3];
    float d1;
    float d2;
};

static PlayerSpots SpotsFrom(const float* at) {
    PlayerSpots s;
    s.d1 = RootLocation3(GetP1Pawn(), s.p1) ? Dist3(at, s.p1) : -1.0f;
    s.d2 = RootLocation3(GetP2Pawn(), s.p2) ? Dist3(at, s.p2) : -1.0f;
    return s;
}

static float Nearest(const PlayerSpots& s) {
    if (s.d1 < 0.0f) return s.d2;
    if (s.d2 < 0.0f) return s.d1;
    return s.d1 < s.d2 ? s.d1 : s.d2;
}

static std::uintptr_t VtableNso(const void* object) {
    return IsHeapish(object) ? ToNso(Read<std::uintptr_t>(object, 0)) : 0;
}

static const char* WhoIs(const void* actor) {
    if (actor == nullptr) return "-";
    if (actor == GetP1Pawn()) return "p1";
    if (actor == GetP2Pawn()) return "p2";
    return "other";
}

static std::uintptr_t CallerReturn(const void* frame) {
    const auto* ourFrame = static_cast<const std::uintptr_t*>(frame);
    const std::uintptr_t callerFp = ourFrame != nullptr ? ourFrame[0] : 0;
    if (callerFp == 0 || (callerFp & 0xF) != 0) return 0;
    return reinterpret_cast<const std::uintptr_t*>(callerFp)[1];
}

// Up to `max` return addresses up the frame-record chain from our frame (the first is our own return
// address). Stops at a record that is unaligned, not above the previous one, or over 64 KiB up the
// stack: the receiver runs deep inside the game thread's tick, so that span stays inside its stack.
static int CallerChain(const void* frame, std::uintptr_t* out, int max) {
    auto fp = reinterpret_cast<std::uintptr_t>(frame);
    const std::uintptr_t limit = fp + 0x10000;
    int n = 0;
    while (n < max && fp != 0 && (fp & 0xF) == 0 && fp < limit) {
        const auto* record = reinterpret_cast<const std::uintptr_t*>(fp);
        out[n++] = record[1];
        const std::uintptr_t next = record[0];
        if (next <= fp) break;
        fp = next;
    }
    return n;
}

static QueryCaller* CallerSlot(std::uintptr_t ra, std::uintptr_t ra2, std::uint32_t kind) {
    for (std::uint32_t i = 0; i < sCallerCount; ++i) {
        if (sCallers[i].ra == ra && sCallers[i].ra2 == ra2 && sCallers[i].kind == kind) return &sCallers[i];
    }
    if (sCallerCount >= QueryCallers) return nullptr;
    QueryCaller* c = &sCallers[sCallerCount++];
    c->ra = ra;
    c->ra2 = ra2;
    c->kind = kind;
    return c;
}

static void LogQuerySummary() {
    if (sQuerySummaries >= MaxQuerySummaries) return;
    sQuerySummaries++;
    // The busiest callers first, as many as fit the line.
    bool listed[QueryCallers] = {};
    char buf[380];
    buf[0] = '\0';
    int len = 0;
    for (std::uint32_t round = 0; round < sCallerCount; ++round) {
        std::uint32_t best = QueryCallers;
        for (std::uint32_t i = 0; i < sCallerCount; ++i) {
            if (!listed[i] && (best == QueryCallers || sCallers[i].calls > sCallers[best].calls)) best = i;
        }
        listed[best] = true;
        const QueryCaller& c = sCallers[best];
        const int n = std::snprintf(buf + len, sizeof(buf) - static_cast<std::size_t>(len), " %c%lx<%lx:%u/%u/%u/%u",
                                    c.kind == 0 ? 'a' : 'v', static_cast<unsigned long>(ToNso(c.ra)),
                                    static_cast<unsigned long>(ToNso(c.ra2)), c.calls, c.found, c.p2Nearer,
                                    c.p2Added);
        if (n <= 0 || len + n >= static_cast<int>(sizeof(buf))) {
            buf[len] = '\0';  // drop the entry that did not fit
            break;
        }
        len += n;
    }
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC queries calls=%u callers=%u p2_added=%u/%u "
                "(kind ra<ra2:calls/found/p2_nearer/p2_added)%s",
                sQueryCalls, sCallerCount, sP2Added[0], sP2Added[1], buf);
}

// kind 0 = area query 0x101D0DC (shape at centre), 1 = view query 0x101CC34 (from eye).
static void AfterQuery(std::uint32_t kind, std::uintptr_t ra, std::uintptr_t ra2, void* actor, const void* shape,
                       const float* at, const float* quat, std::uint64_t trace, void** nearest, std::uint64_t found,
                       P2Step step) {
    sQueryCalls++;
    if (step == P2Step::Added) sP2Added[kind]++;
    if (sQueryCalls >= QuerySummaryFirst && (sQueryCalls & (sQueryCalls - 1)) == 0) LogQuerySummary();
    QueryCaller* c = CallerSlot(ra, ra2, kind);
    if (c == nullptr) return;
    c->calls++;
    if ((found & 1) != 0) c->found++;
    if (step == P2Step::Added) c->p2Added++;
    float self[3];
    if (!RootLocation3(actor, self)) return;
    const PlayerSpots s = SpotsFrom(self);
    const bool p2Nearer = s.d2 >= 0.0f && (s.d1 < 0.0f || s.d2 < s.d1);
    if (p2Nearer) c->p2Nearer++;
    const bool first = c->calls == 1;
    const bool added = step == P2Step::Added && c->addedSamples < MaxAddedSamples;
    const bool sample = !added && p2Nearer && s.d2 < NearPlayer && c->samples < MaxQuerySamples &&
                        c->calls - c->lastSample >= QuerySampleGap;
    if (!(first || added || sample) || sQueryLines >= MaxQueryLines) return;
    if (added) c->addedSamples++;
    if (sample) {
        c->samples++;
        c->lastSample = c->calls;
    }
    sQueryLines++;
    std::int32_t shapeType = -1;
    float ext[3] = {0.0f, 0.0f, 0.0f};
    if (kind == 0 && FloatsReadable(shape)) {
        shapeType = Read<std::int32_t>(shape, 0);
        ext[0] = Read<float>(shape, 4);
        ext[1] = Read<float>(shape, 8);
        ext[2] = Read<float>(shape, 12);
    }
    float o[3] = {0.0f, 0.0f, 0.0f};
    if (FloatsReadable(at)) {
        o[0] = at[0];
        o[1] = at[1];
        o[2] = at[2];
    }
    float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (kind == 0 && FloatsReadable(quat)) {
        for (int i = 0; i < 4; ++i) q[i] = quat[i];
    }
    const void* got = nearest != nullptr ? *nearest : nullptr;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC query %s kind=%s ra=0x%lx<0x%lx actor=%p vt=0x%lx n=%u found=%u nearest=%s "
                "p2step=%s shape=%d(%.0f,%.0f,%.0f) at=(%.0f,%.0f,%.0f) q=(%.3f,%.3f,%.3f,%.3f) trace=%u "
                "self=(%.0f,%.0f,%.0f) p1=(%.0f,%.0f,%.0f) p2=(%.0f,%.0f,%.0f) d1=%.0f d2=%.0f line=%u",
                first ? "first" : (added ? "p2-added" : "p2-nearer"), kind == 0 ? "area" : "view",
                static_cast<unsigned long>(ToNso(ra)), static_cast<unsigned long>(ToNso(ra2)), actor,
                static_cast<unsigned long>(VtableNso(actor)), c->calls, static_cast<unsigned>(found & 1),
                nearest != nullptr ? WhoIs(got) : "no-out", kStepNames[static_cast<int>(step)], shapeType, ext[0],
                ext[1], ext[2], o[0], o[1], o[2], q[0], q[1], q[2], q[3], static_cast<unsigned>(trace & 1), self[0],
                self[1], self[2], s.p1[0], s.p1[1], s.p1[2], s.p2[0], s.p2[1], s.p2[2], s.d1, s.d2, sQueryLines);
}

static WalkerSeen* WalkerSlot(void* walker, bool* fresh) {
    std::uint32_t h = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(walker) >> 4) % WalkerSlots;
    for (std::uint32_t probe = 0; probe < WalkerSlots; ++probe) {
        WalkerSeen* w = &sWalkers[(h + probe) % WalkerSlots];
        if (w->walker == walker) {
            *fresh = false;
            return w;
        }
        if (w->walker == nullptr) {
            w->walker = walker;
            *fresh = true;
            return w;
        }
    }
    return nullptr;
}

static std::uint8_t ByteThrough(const void* walker, std::uintptr_t component, std::uintptr_t field) {
    const void* object = Read<void*>(walker, component);
    return IsHeapish(object) ? Read<std::uint8_t>(object, field) : 0xFF;
}

static void AfterWalkerTick(void* walker) {
    if (sWalkerLines >= MaxWalkerLines || !IsHeapish(walker)) return;
    float self[3];
    if (!RootLocation3(walker, self)) return;
    const PlayerSpots s = SpotsFrom(self);
    const float near = Nearest(s);
    if (near < 0.0f || near > FirstSeenNear) return;
    std::uint8_t bytes[WalkerBytes];
    bytes[0] = Read<std::uint8_t>(walker, EnemyState);
    bytes[1] = Read<std::uint8_t>(walker, WalkerState);
    bytes[2] = Read<std::uint8_t>(walker, WalkerAction);
    bytes[3] = ByteThrough(walker, WalkerSubComponent, SubComponentState);
    bytes[4] = Read<std::uint8_t>(walker, WalkerNoticed);
    bytes[5] = ByteThrough(walker, EnemyCollision, CollisionEnabled);
    bytes[6] = Read<std::uint8_t>(walker, NearSwitch);
    bytes[7] = ByteThrough(walker, WalkerTriggerComponent, TriggerFlag);
    bytes[8] = Read<std::uint8_t>(walker, WalkerEventLatch);
    const auto ai = static_cast<std::uint32_t>(VtableNso(Read<void*>(walker, WalkerAiComponent)));
    bool fresh = false;
    WalkerSeen* w = WalkerSlot(walker, &fresh);
    if (w == nullptr) return;
    if (!fresh && std::memcmp(w->bytes, bytes, sizeof(bytes)) == 0 && w->ai == ai) return;
    const bool report = fresh || near <= NearPlayer;
    std::memcpy(w->bytes, bytes, sizeof(bytes));
    w->ai = ai;
    if (!report) return;
    sWalkerLines++;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC walker %s %p vt=0x%lx st=%u ws=%u act=%u sub=%u noticed=%u coll=%u near=%u "
                "trig=%u latch=%u r=%.0f ai=0x%x at=(%.0f,%.0f,%.0f) d1=%.0f d2=%.0f line=%u",
                fresh ? "new" : "chg", walker, static_cast<unsigned long>(VtableNso(walker)), bytes[0], bytes[1],
                bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], Read<float>(walker, NearRadius),
                ai, self[0], self[1], self[2], s.d1, s.d2, sWalkerLines);
}

static void AfterChangeState(void* enemy, std::uint32_t before, std::uint32_t requested, std::uintptr_t ra,
                             std::uintptr_t ra2) {
    if (sStateLines >= MaxStateLines || !IsHeapish(enemy)) return;
    float self[3];
    if (!RootLocation3(enemy, self)) return;
    const PlayerSpots s = SpotsFrom(self);
    const float near = Nearest(s);
    if (near < 0.0f || near > NearPlayer) return;
    sStateLines++;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC state %p vt=0x%lx %u->%u (now %u) ra=0x%lx ra2=0x%lx d1=%.0f d2=%.0f line=%u",
                enemy, static_cast<unsigned long>(VtableNso(enemy)), before, requested & 0xFF,
                Read<std::uint8_t>(enemy, EnemyState), static_cast<unsigned long>(ToNso(ra)),
                static_cast<unsigned long>(ToNso(ra2)), s.d1, s.d2, sStateLines);
}

static void AfterWalkerEvent(void* walker, const void* sender, const void* event, const std::uintptr_t* chain,
                             int depth, std::uint8_t trigBefore, std::uint8_t latchBefore) {
    if (sEventLines >= MaxEventLines || !IsHeapish(walker)) return;
    float self[3];
    if (!RootLocation3(walker, self)) return;
    const PlayerSpots s = SpotsFrom(self);
    const float near = Nearest(s);
    if (near < 0.0f || near > EventNear) return;
    std::uint32_t words[4] = {0, 0, 0, 0};
    if (FloatsReadable(event)) std::memcpy(words, event, sizeof(words));
    // The return addresses as NSO offsets (0 outside main), innermost first.
    char path[200];
    path[0] = '\0';
    int len = 0;
    for (int i = 0; i < depth; ++i) {
        const int n = std::snprintf(path + len, sizeof(path) - static_cast<std::size_t>(len), "%s%lx", i ? "<" : "",
                                    static_cast<unsigned long>(ToNso(chain[i])));
        if (n <= 0 || len + n >= static_cast<int>(sizeof(path))) {
            path[len] = '\0';
            break;
        }
        len += n;
    }
    sEventLines++;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC event walker=%p sender=%p (vt=0x%lx) event=%08x %08x %08x %08x trig=%u->%u "
                "latch=%u->%u chain=%s d1=%.0f d2=%.0f line=%u",
                walker, sender, static_cast<unsigned long>(VtableNso(sender)), words[0], words[1], words[2], words[3],
                trigBefore, ByteThrough(walker, WalkerTriggerComponent, TriggerFlag), latchBefore,
                Read<std::uint8_t>(walker, WalkerEventLatch), path, s.d1, s.d2, sEventLines);
}

HOOK_DEFINE_TRAMPOLINE(FinderPlayer) {
    static std::uint64_t Callback(void* finder, void* out) {
        return AfterFinder(KindPlayer, finder, out, Orig(finder, out));
    }
};

HOOK_DEFINE_TRAMPOLINE(FinderInRange) {
    static std::uint64_t Callback(void* finder, void* out) {
        return AfterFinder(KindInRange, finder, out, Orig(finder, out));
    }
};

HOOK_DEFINE_TRAMPOLINE(FinderInArea) {
    static std::uint64_t Callback(void* finder, void* out) {
        return AfterFinder(KindInArea, finder, out, Orig(finder, out));
    }
};
HOOK_DEFINE_TRAMPOLINE(AreaQuery) {
    static std::uint64_t Callback(void* actor, const void* shape, const float* centre, const float* quat,
                                  std::uint64_t trace, void* params, void** nearest, void* a7) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        std::uint64_t found = Orig(actor, shape, centre, quat, trace, params, nearest, a7);
        const P2Step step = AreaWithP2(actor, shape, centre, quat, trace, params, nearest, &found);
        if (kDiag0249EnemyCensus) AfterQuery(0, ra, ra2, actor, shape, centre, quat, trace, nearest, found, step);
        return found;
    }
};

HOOK_DEFINE_TRAMPOLINE(ViewQuery) {
    static std::uint64_t Callback(void* actor, const float* eye, std::uint64_t trace, void* params, void** nearest,
                                  void* a5, void* a6, void* a7) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        std::uint64_t found = Orig(actor, eye, trace, params, nearest, a5, a6, a7);
        const P2Step step = ViewWithP2(actor, eye, trace, params, nearest, &found);
        if (kDiag0249EnemyCensus) AfterQuery(1, ra, ra2, actor, nullptr, eye, nullptr, trace, nearest, found, step);
        return found;
    }
};

// PATCH-0267: the walker's combat action and the coordinator's tick, with the nearer player as player 1.
HOOK_DEFINE_TRAMPOLINE(ArenaAction) {
    static void Callback(void* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
        std::uint64_t* scope = nullptr;
        if (IsHeapish(self) && IsHeapish(Read<void*>(self, ActorRoot))) {
            float at[3];
            LocationOrZero(self, at);
            scope = BeginArenaScope(0, self, at);
        }
        Orig(self, a1, a2, a3, a4, a5, a6, a7);
        EndArenaScope(scope);
    }
};

HOOK_DEFINE_TRAMPOLINE(CoordinatorTick) {
    static void Callback(void* self, float deltaSeconds) {
        std::uint64_t* scope = nullptr;
        float centre[3];
        if (IsHeapish(self) && EnemyCentre(self, centre)) scope = BeginArenaScope(1, self, centre);
        Orig(self, deltaSeconds);
        EndArenaScope(scope);
    }
};

HOOK_DEFINE_TRAMPOLINE(WalkerTick) {
    static void Callback(void* self, float deltaSeconds, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6,
                         void* a7) {
        Orig(self, deltaSeconds, a1, a2, a3, a4, a5, a6, a7);
        AfterWalkerTick(self);
    }
};

HOOK_DEFINE_TRAMPOLINE(ChangeState) {
    static std::uint64_t Callback(void* self, std::uint32_t state, void* a2, void* a3, void* a4, void* a5, void* a6,
                                  void* a7) {
        const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uintptr_t ra2 = CallerReturn(__builtin_frame_address(0));
        const std::uint32_t before = IsHeapish(self) ? Read<std::uint8_t>(self, EnemyState) : 0xFFu;
        const std::uint64_t result = Orig(self, state, a2, a3, a4, a5, a6, a7);
        AfterChangeState(self, before, state, ra, ra2);
        return result;
    }
};

HOOK_DEFINE_TRAMPOLINE(WalkerEvent) {
    static std::uint64_t Callback(void* self, void* sender, const void* event, void* a3, void* a4, void* a5, void* a6,
                                  void* a7) {
        std::uintptr_t chain[EventChainDepth];
        const int depth = CallerChain(__builtin_frame_address(0), chain, EventChainDepth);
        const std::uint8_t trig = IsHeapish(self) ? ByteThrough(self, WalkerTriggerComponent, TriggerFlag) : 0xFF;
        const std::uint8_t latch = IsHeapish(self) ? Read<std::uint8_t>(self, WalkerEventLatch) : 0xFF;
        const std::uint64_t result = Orig(self, sender, event, a3, a4, a5, a6, a7);
        AfterWalkerEvent(self, sender, event, chain, depth, trig, latch);
        return result;
    }
};

} // namespace

void InstallEnemyTarget() {
    if (kPatch0263EnemyTargetParity) {
        FinderPlayer::InstallAtOffset(FinderPlayerFn);
        FinderInRange::InstallAtOffset(FinderInRangeFn);
        FinderInArea::InstallAtOffset(FinderInAreaFn);
    }
    if (kPatch0265PlayerQueryParity || kDiag0249EnemyCensus) {
        AreaQuery::InstallAtOffset(AreaQueryFn);
        ViewQuery::InstallAtOffset(ViewQueryFn);
    }
    if (kPatch0267ArenaNearerPlayer) {
        ArenaAction::InstallAtOffset(ArenaActionFn);
        CoordinatorTick::InstallAtOffset(CoordinatorTickFn);
        Logging.Log("[PShowCoop] PATCH-0267 installed arena_action=0x%lx coordinator_tick=0x%lx list=+0x%lx",
                    static_cast<unsigned long>(ArenaActionFn), static_cast<unsigned long>(CoordinatorTickFn),
                    static_cast<unsigned long>(WorldPlayerControllers));
    }
    if (kDiag0249EnemyCensus) {
        WalkerTick::InstallAtOffset(WalkerTickFn);
        ChangeState::InstallAtOffset(ChangeStateFn);
        WalkerEvent::InstallAtOffset(WalkerEventFn);
    }
    Logging.Log("[PShowCoop] PATCH-0263 installed parity=%d player=0x%lx in_range=0x%lx in_area=0x%lx p2_index=%d; "
                "PATCH-0265 query_parity=%d area=0x%lx view=0x%lx trace=0x%lx scope=EnemyBase; RELEASE-DIAGNOSTIC census=%d "
                "walker_tick=0x%lx change_state=0x%lx walker_event=0x%lx",
                kPatch0263EnemyTargetParity ? 1 : 0, static_cast<unsigned long>(FinderPlayerFn),
                static_cast<unsigned long>(FinderInRangeFn), static_cast<unsigned long>(FinderInAreaFn), P2PlayerIndex,
                kPatch0265PlayerQueryParity ? 1 : 0, static_cast<unsigned long>(AreaQueryFn),
                static_cast<unsigned long>(ViewQueryFn), static_cast<unsigned long>(QueryTraceFn),
                kDiag0249EnemyCensus ? 1 : 0, static_cast<unsigned long>(WalkerTickFn),
                static_cast<unsigned long>(ChangeStateFn), static_cast<unsigned long>(WalkerEventFn));
}

} // namespace pshowcoop::patches
