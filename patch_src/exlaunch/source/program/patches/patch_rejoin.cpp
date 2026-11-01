#include "pointer_guard.hpp"
#include "patch_rejoin.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace pshowcoop::patches {
namespace {
// Placement remains a reversible experiment, not yet verified.
constexpr bool kPatch0271SmartInstantRejoin = true;

struct Vector3 { float x, y, z; };
struct alignas(16) Quaternion { float x, y, z, w; };
// the aligned parameter occupies 0x30 bytes. Do not use a SIMD UE layout.
struct alignas(16) Transform {
    Quaternion rotation;
    Vector3 translation;
    Vector3 scale;
    std::uint8_t padding[8];
};
struct alignas(16) TransformLocationParams {
    Transform transform;
    Vector3 location;
    Vector3 result;
};
struct FloorResult {
    std::uint8_t flags;
    std::uint8_t padding[3];
    float floorDistance;
    float lineDistance;
    std::uint8_t hit[0x88];
};
static_assert(sizeof(Vector3) == 12);
static_assert(sizeof(Quaternion) == 16);
static_assert(offsetof(Transform, translation) == 0x10);
static_assert(offsetof(Transform, scale) == 0x1C);
static_assert(sizeof(Transform) == 0x30);
static_assert(offsetof(TransformLocationParams, location) == 0x30);
static_assert(offsetof(TransformLocationParams, result) == 0x3C);
static_assert(sizeof(TransformLocationParams) == 0x50);
static_assert(offsetof(FloorResult, floorDistance) == 4);
static_assert(offsetof(FloorResult, hit) == 0xC);
static_assert(sizeof(FloorResult) == 0x94);

// NSO offsets for the recorded main; never runtime addresses.
constexpr std::uintptr_t ActorGetWorld = 0x041E28BC;
constexpr std::uintptr_t WorldTeleportFit = 0x045ECF88;
constexpr std::uintptr_t CmcFindFloor = 0x04381FD0;
constexpr std::uintptr_t QuaternionToRotator = 0x027DA0D4;
constexpr std::uintptr_t ActorTeleport = 0x041E2B38;
constexpr std::uintptr_t ProcessEvent = 0x02A2F548;
constexpr std::uintptr_t TransformLocationExec = 0x04BAA1A4;
constexpr std::uintptr_t TransformLocationGlobal = 0x0AEC5EB8;
constexpr std::uintptr_t MathLibraryClassGlobal = 0x0AEC6198;

constexpr std::size_t RootOffset = 0x130;
constexpr std::size_t CmcOffset = 0x288;
constexpr std::size_t CapsuleOffset = 0x290;
constexpr std::size_t CmcUpdatedOffset = 0xB0;
constexpr std::size_t CmcOwnerOffset = 0x148;
constexpr std::size_t MovementModeOffset = 0x168;
constexpr std::size_t CmcQueryFlagsOffset = 0x1F0;
constexpr std::size_t CurrentFloorOffset = 0x2F0;
constexpr std::size_t CollisionOriginOffset = 0x100;
constexpr std::size_t QuaternionOffset = 0x1C0;
constexpr std::size_t PositionOffset = 0x1D0;
constexpr std::size_t ClassDefaultOffset = 0x118;
constexpr std::size_t GetWorldSlot = 0x168;
constexpr std::size_t FindFloorSlot = 0x8F8;
constexpr std::size_t TeleportSlot = 0x508;
constexpr std::size_t ProcessEventSlot = 0x228;
constexpr std::uint32_t FloorCacheBit = 0x800;

using GetWorldFn = void* (*)(void* actor);
using FitFn = bool (*)(void* world, void* actor, Vector3* position, Vector3 rotation);
using FloorFn = void (*)(void* cmc, FloorResult* result, Vector3 position);
using RotatorFn = Vector3 (*)(const Quaternion* quaternion);
using TeleportFn = bool (*)(void* actor, const Vector3* position, const Vector3* rotation,
                           bool isTest, bool noCheck);
using ProcessEventFn = void (*)(void* object, void* function, void* parameters);

struct PlayerRead {
    void* pawn;
    void* root;
    void* cmc;
    Vector3 position;
    Vector3 boundsOrigin;
    Quaternion quaternion;
    std::uint8_t mode;
    std::uint8_t cachedFlags;
};

bool HeapPointer(const void* pointer) {
    return pshowcoop::pointerguard::Readable(pointer);
}

template <typename T> T Read(const void* object, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
    return value;
}

bool Finite(Vector3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Same(Vector3 a, Vector3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool SlotMatches(void* object, std::size_t slot, std::uintptr_t expected) {
    const auto& range = exl::util::GetMainModuleInfo().m_Total;
    const auto table = Read<std::uintptr_t>(object, 0);
    if ((table & 7) != 0 || !range.InRange(table) || !range.InRange(table + slot + 7)) return false;
    return Read<std::uintptr_t>(reinterpret_cast<const void*>(table), slot) == range.m_Start + expected;
}

bool ReadPlayer(void* pawn, PlayerRead& out) {
    // Existing title-player anchors, with Character/Capsule ancestry established
    if (!HeapPointer(pawn) || !SlotMatches(pawn, GetWorldSlot, ActorGetWorld)) return false;
    void* root = Read<void*>(pawn, RootOffset);
    void* cmc = Read<void*>(pawn, CmcOffset);
    if (!HeapPointer(root) || !HeapPointer(cmc)) return false;
    if (Read<void*>(pawn, CapsuleOffset) != root || Read<void*>(cmc, CmcOwnerOffset) != pawn ||
        Read<void*>(cmc, CmcUpdatedOffset) != root || !SlotMatches(cmc, FindFloorSlot, CmcFindFloor)) return false;
    out.pawn = pawn;
    out.root = root;
    out.cmc = cmc;
    out.position = Read<Vector3>(root, PositionOffset);
    out.boundsOrigin = Read<Vector3>(root, CollisionOriginOffset);
    out.quaternion = Read<Quaternion>(root, QuaternionOffset);
    out.mode = Read<std::uint8_t>(cmc, MovementModeOffset);
    out.cachedFlags = Read<std::uint8_t>(cmc, CurrentFloorOffset);
    return Finite(out.position) && Finite(out.boundsOrigin) &&
           std::isfinite(out.quaternion.x) && std::isfinite(out.quaternion.y) &&
           std::isfinite(out.quaternion.z) && std::isfinite(out.quaternion.w);
}

bool CacheBitRestored(void* cmc, std::uint32_t before) {
    return ((before ^ Read<std::uint32_t>(cmc, CmcQueryFlagsOffset)) & FloorCacheBit) == 0;
}

// Use the engine's registered native math UFunction with a complete parameter
// buffer. ProcessEvent constructs its own FFrame. No exec-thunk call, fake VM
// frame, bytecode, custom rotation arithmetic, or temporary actor movement.
bool TransformPoint(void* cdo, void* function, const Transform& transform, Vector3 local, Vector3& out) {
    TransformLocationParams parameters{};
    parameters.transform = transform;
    parameters.location = local;
    constexpr float invalid = std::numeric_limits<float>::quiet_NaN();
    parameters.result = {invalid, invalid, invalid};
    const auto main = exl::util::GetMainModuleInfo().m_Total.m_Start;
    reinterpret_cast<ProcessEventFn>(main + ProcessEvent)(cdo, function, &parameters);
    out = parameters.result;
    return Finite(out);
}

bool ResolveTransform(void*& cdo, void*& function) {
    const auto& range = exl::util::GetMainModuleInfo().m_Total;
    if (!range.InRange(range.m_Start + TransformLocationGlobal + 7) ||
        !range.InRange(range.m_Start + MathLibraryClassGlobal + 7)) return false;
    function = Read<void*>(reinterpret_cast<void*>(range.m_Start), TransformLocationGlobal);
    void* libraryClass = Read<void*>(reinterpret_cast<void*>(range.m_Start), MathLibraryClassGlobal);
    if (!HeapPointer(function) || !HeapPointer(libraryClass)) return false;
    const auto parameterBytes = Read<std::uint16_t>(function, 0xB6);
    const auto frameBytes = Read<std::int32_t>(function, 0x58);
    // These are reads of existing engine objects; do not create a library/CDO.
    // The last FVector ends at 0x48; the generated C++ buffer includes tail
    // alignment to 0x50. ParmsSize need not include that tail padding.
    if (Read<void*>(function, 0x20) != libraryClass ||
        Read<std::uintptr_t>(function, 0xD8) != range.m_Start + TransformLocationExec ||
        (Read<std::uint32_t>(function, 0xB0) & 0x2400) != 0x2400 ||
        parameterBytes < 0x48 || parameterBytes > 0x50 ||
        frameBytes < parameterBytes || frameBytes > 0x50 ||
        Read<std::uint16_t>(function, 0xB8) != offsetof(TransformLocationParams, result)) return false;
    cdo = Read<void*>(libraryClass, ClassDefaultOffset);
    return HeapPointer(cdo) && Read<void*>(cdo, 0x10) == libraryClass &&
           (Read<std::uint32_t>(cdo, 8) & 0x10) != 0 && SlotMatches(cdo, ProcessEventSlot, ProcessEvent);
}

RejoinResult Deferred(const char* reason) {
    // Caller uses PATCH-0015's existing cooldown to bound all attempt logging.
    Logging.Log("[PShowCoop] PATCH-0271 deferred reason=%s moved=0", reason);
    return RejoinResult::Deferred;
}

} // namespace

void InstallRejoin() {
    if (!kPatch0271SmartInstantRejoin) return;
    Logging.Log("[PShowCoop] PATCH-0271 armed ongoing_only=1 preference=behind,left,right checked_teleport=1 cdo_event_slot=0x228");
}

RejoinResult TrySmartRejoin(void* p1Pawn, void* p2Pawn) {
    if (!kPatch0271SmartInstantRejoin) return RejoinResult::Disabled;
    PlayerRead p1{}, p2{};
    if (p1Pawn == p2Pawn || !ReadPlayer(p1Pawn, p1) || !ReadPlayer(p2Pawn, p2) || p1.cmc == p2.cmc ||
        !SlotMatches(p2Pawn, TeleportSlot, ActorTeleport)) return Deferred("player-component-or-vslot-gate");
    if (!Same(p1.position, p1.boundsOrigin) || !Same(p2.position, p2.boundsOrigin))
        return Deferred("capsule-origin-differs");
    // P1 supplies a grounded destination. P2 remains in its own walking/falling
    // lifecycle; native teleport and subsequent native movement handle landing.
    if (p1.mode != 1 || (p1.cachedFlags & 3) != 3 || (p2.mode != 1 && p2.mode != 3))
        return Deferred("movement-state");
    const auto main = exl::util::GetMainModuleInfo().m_Total.m_Start;
    auto getWorld = reinterpret_cast<GetWorldFn>(main + ActorGetWorld);
    void* world = getWorld(p1Pawn);
    if (!HeapPointer(world) || getWorld(p2Pawn) != world) return Deferred("world-mismatch");

    void* cdo = nullptr;
    void* transformFunction = nullptr;
    if (!ResolveTransform(cdo, transformFunction)) return Deferred("native-transform-layout");
    const Transform identity{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {}};
    const Transform p1Frame{p1.quaternion, p1.position, {1.0f, 1.0f, 1.0f}, {}};
    constexpr Vector3 behind{-120.0f, 0.0f, 0.0f};
    Vector3 control{};
    if (!TransformPoint(cdo, transformFunction, identity, behind, control) || !Same(control, behind) ||
        !TransformPoint(cdo, transformFunction, p1Frame, {0.0f, 0.0f, 0.0f}, control) ||
        !Same(control, p1.position)) return Deferred("native-transform-control");

    auto floorQuery = reinterpret_cast<FloorFn>(main + CmcFindFloor);
    const auto before1 = Read<std::uint32_t>(p1.cmc, CmcQueryFlagsOffset);
    const auto before2 = Read<std::uint32_t>(p2.cmc, CmcQueryFlagsOffset);
    FloorResult p1Floor{};
    floorQuery(p1.cmc, &p1Floor, p1.position);
    if ((p1Floor.flags & 3) != 3 || !std::isfinite(p1Floor.floorDistance) ||
        p1Floor.floorDistance < 0.0f || !CacheBitRestored(p1.cmc, before1)) return Deferred("p1-floor-control");
    const Vector3 rotation = reinterpret_cast<RotatorFn>(main + QuaternionToRotator)(&p2.quaternion);
    if (!Finite(rotation)) return Deferred("rotation-nonfinite");

    // Preferred local offsets are parameters to native TransformLocation.
    // The native solver must accept the requested point unchanged: its former
    // +80 world-X correction is not a substitute for the chosen preference.
    constexpr Vector3 choices[] = {behind, {0.0f, -120.0f, 0.0f}, {0.0f, 120.0f, 0.0f}};
    constexpr const char* names[] = {"behind", "left", "right"};
    auto fit = reinterpret_cast<FitFn>(main + WorldTeleportFit);
    for (std::size_t index = 0; index < 3; ++index) {
        Vector3 requested{};
        if (!TransformPoint(cdo, transformFunction, p1Frame, choices[index], requested) ||
            Same(requested, p1.position) || requested.z != p1.position.z) continue;
        Vector3 candidate = requested;
        const bool clear = fit(world, p2Pawn, &candidate, rotation);
        if (!clear || !Same(candidate, requested)) {
            Logging.Log("[PShowCoop] PATCH-0271 rejected choice=%s reason=clearance fit=%d unchanged=%d",
                        names[index], (int)clear, (int)Same(candidate, requested));
            continue;
        }
        FloorResult landing{};
        floorQuery(p2.cmc, &landing, candidate);
        if (!CacheBitRestored(p2.cmc, before2)) return Deferred("floor-cache-state");
        // Conservative first prototype: no upward fit and no greater floor gap
        // than native walking under P1. Steps, slopes and edge cases may defer.
        if ((landing.flags & 3) != 3 || !std::isfinite(landing.floorDistance) ||
            landing.floorDistance < 0.0f || landing.floorDistance > p1Floor.floorDistance) {
            Logging.Log("[PShowCoop] PATCH-0271 rejected choice=%s reason=support flags=0x%x floor=%.3f control=%.3f",
                        names[index], (unsigned)landing.flags, landing.floorDistance, p1Floor.floorDistance);
            continue;
        }
        // Preserve the native collision, root-movement and teleport notification
        // chain. Do not write transforms, velocity, movement mode or floor state.
        const bool ok = reinterpret_cast<TeleportFn>(main + ActorTeleport)(p2Pawn, &candidate, &rotation, false, false);
        const Vector3 actual = Read<Vector3>(p2.root, PositionOffset);
        Logging.Log("[PShowCoop] PATCH-0271 return choice=%s ok=%d exact=%d p2mode=%u flags=0x%x floor=%.3f P1=(%.3f,%.3f,%.3f) target=(%.3f,%.3f,%.3f) actual=(%.3f,%.3f,%.3f)",
                    names[index], (int)ok, (int)Same(actual, candidate), (unsigned)p2.mode,
                    (unsigned)landing.flags, landing.floorDistance, p1.position.x, p1.position.y, p1.position.z,
                    candidate.x, candidate.y, candidate.z, actual.x, actual.y, actual.z);
        return ok ? RejoinResult::Moved : RejoinResult::Deferred;
    }
    return Deferred("no-supported-clear-choice");
}

} // namespace pshowcoop::patches
