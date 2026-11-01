#include "pointer_guard.hpp"
#include "patch_core.hpp"
#include "patch_hud.hpp"
#include "patch_input_parity.hpp"
#include "patch_rejoin.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include "program/pshow_config.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>

namespace pshowcoop::patches {

namespace {

struct FVector3f {
    float X;
    float Y;
    float Z;
};

static void* sP1Pawn = nullptr;
static void* sP2Pawn = nullptr;
static void* sP2Controller = nullptr;
static void* sP1LocalPlayer = nullptr;
static void* sP2LocalPlayer = nullptr;

static float sP1LocX = 0.0f, sP1LocY = 0.0f, sP1LocZ = 0.0f;
static float sP2LocX = 0.0f, sP2LocY = 0.0f, sP2LocZ = 0.0f;
static bool sHasP1Loc = false;
static bool sHasP2Loc = false;
static std::uint32_t sLeashCooldown = 0;
// Pose clock for PATCH-0017: one tick per label-0/1 Enforce call; each
// player's stamp is the tick of its last usable position.
static std::uint32_t sPoseCalls = 0;
static std::uint32_t sP1Stamp = 0;
static std::uint32_t sP2Stamp = 0;
static float sPoseP1[3] = {};
static float sPoseP2[3] = {};
// the switch off, config::Get() returns the built-in defaults, which equal the
// previously hard-coded leash (800/800/120) and tint (cyan x8.0).
constexpr bool kPatch0242ConfigRead = true;
constexpr std::uint32_t ConfigReloadMinCalls = 120;
static std::uint32_t sP2Calls = 0;       // label-1 Enforce calls
static void* sLastConfigPawn = nullptr;  // identity token, never dereferenced

inline bool IsHeapish(const void* value) {
    return pshowcoop::pointerguard::Readable(value);
}

// ---------------------------------------------------------------------------
// PATCH-0007 & PATCH-0009: Force 2-Player Input Count & Max Controllers CVar
// ---------------------------------------------------------------------------
constexpr ptrdiff_t NpadPollerGate = 0x1EE0AC4;
constexpr ptrdiff_t CVarMaxControllersOffset1 = 0x0A153B80;
constexpr ptrdiff_t CVarMaxControllersOffset2 = 0x0A153B98;

static void EnforceMaxControllers() {
    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;

    // CVar cell 1 (0x0A153B80) - platform pad poller InitialMaxControllers
    auto* cell1 = *reinterpret_cast<int**>(mainRange.m_Start + CVarMaxControllersOffset1);
    if (cell1 != nullptr) {
        static bool sLogged1 = false;
        if (cell1[0] < 2 || cell1[1] < 2) {
            cell1[0] = 2;
            cell1[1] = 2;
            if (!sLogged1) {
                sLogged1 = true;
                Logging.Log("[PShowCoop] PATCH-0009 set max_controllers=2 at CVar cell 0x0A153B80 (%p)", cell1);
            }
        }
    }

    // CVar cell 2 (0x0A153B98)
    auto* cell2 = *reinterpret_cast<int**>(mainRange.m_Start + CVarMaxControllersOffset2);
    if (cell2 != nullptr) {
        if (cell2[0] < 2) cell2[0] = 2;
        if (cell2[1] < 2) cell2[1] = 2;
    }
}

HOOK_DEFINE_TRAMPOLINE(ForceTwoPlayerInputCount) {
    static void Callback(void* mgr, void* out, int playerIndex) {
        EnforceMaxControllers();
        if (mgr != nullptr) {
            auto* mgrBytes = static_cast<std::uint8_t*>(mgr);
            auto* count = reinterpret_cast<int*>(mgrBytes + 0x78);
            if (*count < 2) {
                *count = 2;
            }
        }
        Orig(mgr, out, playerIndex);
    }
};

// ---------------------------------------------------------------------------
// PATCH-0001: Explicit Local Player Creation
// ---------------------------------------------------------------------------
constexpr ptrdiff_t ExplicitLocalPlayerCreator = 0x44F1AF0;
// (and patch_hud skips P2's gauge), so gdb_memory_budget_probe measures the
// single-player baseline under the same build. Diagnostic only; ship false.
constexpr bool kDiag0240SinglePlayerControl = false;
using CreateLocalPlayerFn = void* (*)(void* gameInstance, int controllerId, void* outError, bool bSpawnActor);

HOOK_DEFINE_TRAMPOLINE(ExplicitLocalPlayerCreation) {
    static void* Callback(void* gameInstance, int controllerId, void* outError, bool bSpawnActor) {
        void* result = Orig(gameInstance, controllerId, outError, bSpawnActor);
        static bool sP2Spawned = false;
        if (!sP2Spawned && controllerId == 0) {
            sP2Spawned = true;
            if (kDiag0240SinglePlayerControl) {
                Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC single-player control: second local player NOT created");
                return result;
            }
            EnforceMaxControllers();
            auto createFn = reinterpret_cast<CreateLocalPlayerFn>(
                exl::util::modules::GetTargetOffset(ExplicitLocalPlayerCreator));
            int p2Id = 1;
            void* p2Result = createFn(gameInstance, p2Id, outError, false);
            Logging.Log("[PShowCoop] PATCH-0001 created second local player id=%d ptr=%p", p2Id, p2Result);
        }
        return result;
    }
};

// ---------------------------------------------------------------------------
// PATCH-0004: Suppress Redundant Two-Player Applet
// ---------------------------------------------------------------------------
constexpr ptrdiff_t ShowControllerSupportOffset = 0x1EED874;

HOOK_DEFINE_TRAMPOLINE(SuppressControllerApplet) {
    static void Callback(long* param_1) {
        if (param_1 != nullptr && *param_1 != 0) {
            auto* lVar7 = reinterpret_cast<std::uint8_t*>(*param_1);
            int requestedPlayers = *reinterpret_cast<int*>(lVar7 + 8);
            if (requestedPlayers == 2) {
                *reinterpret_cast<int*>(lVar7 + 0x10) = 2;
                *reinterpret_cast<std::uint8_t*>(lVar7 + 0xC) = 1;
                auto* atomicSeq = reinterpret_cast<int*>(lVar7 + 0x14);
                (*atomicSeq)++;
                Logging.Log("[PShowCoop] PATCH-0004 auto-accepted 2-player controller applet.");
                return;
            }
        }
        Orig(param_1);
    }
};

// ---------------------------------------------------------------------------
// PATCH-0010: Input Enable Gate, Pawn Tracking & Leash
// ---------------------------------------------------------------------------
constexpr ptrdiff_t QueueDrainConsumer = 0x1A8DA94;
constexpr ptrdiff_t WeakObjectGet = 0x2A7312C;
constexpr ptrdiff_t PlayerControllerMeta = 0x4C9CEE4;
constexpr ptrdiff_t ResolveLocalPlayer = 0x47FD218;
constexpr std::uintptr_t ConsumerToOuter = 0xD8;
constexpr std::uintptr_t WeakPawnOffset = 0x28;
constexpr std::uintptr_t ControllerOffset = 0x258;
constexpr std::uintptr_t PawnGateByteOffset = 0x228;
constexpr std::uintptr_t LocalPlayerIdOffset = 0xB8;
constexpr std::uint8_t InputEnableBit = 0x10;
// decision 2026-10-14: PATCH-0251 opens and closes P2's PlayerCore gate through
// P2's own pawn virtuals instead. true restores the legacy forcing.
constexpr bool kPatch0010ForceP2Input = false;

using WeakGetFn = void* (*)(void* weakRef);
using MetaFn = void* (*)();
using ResolveLpFn = void* (*)(void* controller);

// PATCH-0015: SetActorLocation with correct HFA FVector3f ABI (s0, s1, s2 in AArch64)
constexpr ptrdiff_t SetActorLocationFnOffset = 0x41F5578;
using SetActorLocationFn = bool (*)(void* actor, FVector3f newLoc, bool bSweep, void* hitResult, bool bTeleport);

// PATCH-0016: Tint Mesh FNames and Functions (by-value FName uint64_t in x1)
constexpr ptrdiff_t SkeletalMeshClassGlobal = 0xAED83F0;
constexpr ptrdiff_t GetComponentByClassFnOffset = 0x41EE618;
constexpr ptrdiff_t SetVectorParamFnOffset = 0x43A1FA4;
constexpr ptrdiff_t SetScalarParamFnOffset = 0x43A17A0;
constexpr ptrdiff_t FNameFromAnsiOffset = 0x02882898;

using GetComponentByClassFn = void* (*)(void* actor, void* uclass);
using SetVectorParamFn = void (*)(void* comp, std::uint64_t fname, float r, float g, float b);
using SetScalarParamFn = void (*)(void* comp, std::uint64_t fname, float val);
using FNameCtorFn = void (*)(void* out, const char* name, int findType);

static std::uint64_t sEmissiveColorFName = 0;
static std::uint64_t sEmissiveIntensityFName = 0;
static char sBuiltColorParam[64] = {};
static char sBuiltIntensityParam[64] = {};
static bool sNamesBuilt = false;
static void* sLastTintedPawn = nullptr;
static std::uint32_t sLastTintCall = 0;

static bool IsPlayerController(void* meta, void* controller) {
    if (meta == nullptr || controller == nullptr) return false;
    auto* m = static_cast<std::uint8_t*>(meta);
    auto* cls = *reinterpret_cast<std::uint8_t**>(
        static_cast<std::uint8_t*>(controller) + 0x10);
    if (cls == nullptr) return false;
    const std::int32_t idx = *reinterpret_cast<std::int32_t*>(m + 0x38);
    const std::int32_t count = *reinterpret_cast<std::int32_t*>(cls + 0x38);
    if (idx < 0 || idx > count) return false;
    auto* table = *reinterpret_cast<std::uint8_t**>(cls + 0x30);
    if (table == nullptr) return false;
    void* entry = *reinterpret_cast<void**>(table + static_cast<std::int64_t>(idx) * 8);
    return entry == (m + 0x30);
}

static void CopyName(char* destination, std::size_t capacity, const char* source) {
    std::size_t i = 0;
    while (source[i] != '\0' && i + 1 < capacity) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

// Built once per distinct name pair. FName construction inserts into the global
// name pool, so it must not run per call - but it must run again when the ini
// changes a parameter name, or the edit would appear to do nothing.
static void BuildTintNames(const pshowcoop::config::Settings& settings,
                           std::uintptr_t mainStart) {
    if (sNamesBuilt &&
        std::strcmp(sBuiltColorParam, settings.p2TintColorParam) == 0 &&
        std::strcmp(sBuiltIntensityParam, settings.p2TintIntensityParam) == 0) {
        return;
    }
    auto fnameCtor = reinterpret_cast<FNameCtorFn>(mainStart + FNameFromAnsiOffset);
    fnameCtor(&sEmissiveColorFName, settings.p2TintColorParam, 2);
    fnameCtor(&sEmissiveIntensityFName, settings.p2TintIntensityParam, 2);
    CopyName(sBuiltColorParam, sizeof(sBuiltColorParam), settings.p2TintColorParam);
    CopyName(sBuiltIntensityParam, sizeof(sBuiltIntensityParam), settings.p2TintIntensityParam);
    sNamesBuilt = true;
}

static void ApplyP2Tint(void* p2Pawn, const char* trigger) {
    if (!IsHeapish(p2Pawn)) return;
    const auto& settings = pshowcoop::config::Get();
    if (!settings.p2TintEnabled) {
        sLastTintedPawn = p2Pawn;  // disabled is a decision, not a retry
        sLastTintCall = sP2Calls;
        return;
    }
    const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
    auto* skeletalClassPtr = reinterpret_cast<void**>(mainRange.m_Start + SkeletalMeshClassGlobal);
    if (skeletalClassPtr == nullptr || !IsHeapish(*skeletalClassPtr)) return;

    auto getComp = reinterpret_cast<GetComponentByClassFn>(mainRange.m_Start + GetComponentByClassFnOffset);
    void* meshComp = getComp(p2Pawn, *skeletalClassPtr);
    if (!IsHeapish(meshComp)) return;

    BuildTintNames(settings, mainRange.m_Start);

    auto setVector = reinterpret_cast<SetVectorParamFn>(mainRange.m_Start + SetVectorParamFnOffset);
    auto setScalar = reinterpret_cast<SetScalarParamFn>(mainRange.m_Start + SetScalarParamFnOffset);

    setVector(meshComp, sEmissiveColorFName, settings.p2TintR, settings.p2TintG, settings.p2TintB);
    setScalar(meshComp, sEmissiveIntensityFName, settings.p2TintIntensity);
    sLastTintedPawn = p2Pawn;
    sLastTintCall = sP2Calls;
    // A pawn change happens once per course load, so it always logs; the
    // backstop repeats every p2TintBackstopCalls and gets its own small cap so
    static std::uint32_t sBackstopLines = 0;
    const bool backstop = trigger[0] == 'b';
    if (!backstop || sBackstopLines < 4) {
        if (backstop) sBackstopLines++;
        Logging.Log("[PShowCoop] PATCH-0016 tint pawn=%p rgb=(%.2f,%.2f,%.2f) intensity=%.2f params=\"%s\"/\"%s\" trigger=%s",
                    p2Pawn, settings.p2TintR, settings.p2TintG, settings.p2TintB,
                    settings.p2TintIntensity, settings.p2TintColorParam,
                    settings.p2TintIntensityParam, trigger);
    }
}

static void ReadPawnLocation(void* pawn, float& x, float& y, float& z, bool& valid) {
    valid = false;
    if (!IsHeapish(pawn)) return;
    void* rootComp = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(pawn) + 0x130);
    if (!IsHeapish(rootComp)) return;
    auto* loc = reinterpret_cast<float*>(static_cast<std::uint8_t*>(rootComp) + 0x1D0);
    x = loc[0];
    y = loc[1];
    z = loc[2];
    valid = (x != 0.0f || y != 0.0f || z != 0.0f);
}

// Spawn placement vs ongoing leash (settings p2.leash.spawn_distance and
// p2.leash.distance). A new P2 pawn opens the placement window; the window
// closes on the first move or after SpawnWindowCalls, so spawn_distance=0 does
constexpr std::uint32_t SpawnWindowCalls = 240;
static void* sLeashPawn = nullptr;  // identity token, never dereferenced
static std::uint32_t sLeashPawnFirstCall = 0;
static bool sSpawnWindowClosed = false;

static void ApplyLeash() {
    if (!sHasP1Loc || !sHasP2Loc || !IsHeapish(sP2Pawn)) return;
    if (sP2Pawn != sLeashPawn) {
        sLeashPawn = sP2Pawn;
        sLeashPawnFirstCall = sP2Calls;
        sSpawnWindowClosed = false;
    }
    if (!sSpawnWindowClosed && sP2Calls - sLeashPawnFirstCall > SpawnWindowCalls) {
        sSpawnWindowClosed = true;  // expired unused; the ongoing leash takes over
    }
    if (sLeashCooldown > 0) {
        sLeashCooldown--;
        return;
    }
    const auto& settings = pshowcoop::config::Get();
    const bool inSpawnWindow = !sSpawnWindowClosed;
    const float threshold = inSpawnWindow ? settings.p2LeashSpawnDistance
                                          : settings.p2LeashDistance;
    if (threshold <= 0.0f) return;  // documented "disabled" value

    float dx = sP2LocX - sP1LocX;
    float dy = sP2LocY - sP1LocY;
    float dz = sP2LocZ - sP1LocZ;
    float distSq = dx * dx + dy * dy + dz * dz;

    // PATCH-0015 Leash: if P2 is further than the threshold from P1, teleport beside P1
    if (distSq > threshold * threshold) {
        // PATCH-0270 changes ongoing placement only. Use the existing attempt
        // cooldown for successful and deferred checks; do not snap into an
        // unchecked destination when the native selector refuses a return.
        if (!inSpawnWindow) {
            const auto rejoin = TrySmartRejoin(sP1Pawn, sP2Pawn);
            if (rejoin != RejoinResult::Disabled) {
                sLeashCooldown = settings.p2LeashCooldownCalls;
                return;
            }
        }
        const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
        auto setLoc = reinterpret_cast<SetActorLocationFn>(mainRange.m_Start + SetActorLocationFnOffset);
        if (setLoc != nullptr) {
            FVector3f newLoc { sP1LocX + 120.0f, sP1LocY, sP1LocZ };
            bool ok = setLoc(sP2Pawn, newLoc, false, nullptr, true);
            sLeashCooldown = settings.p2LeashCooldownCalls;
            sSpawnWindowClosed = true;  // placement is once per pawn
            Logging.Log("[PShowCoop] PATCH-0015 leash trigger=%s P2 to (%.1f,%.1f,%.1f) distSq=%.1f threshold=%.1f cooldown=%u ok=%d",
                        inSpawnWindow ? "spawn-place" : "leash",
                        newLoc.X, newLoc.Y, newLoc.Z, distSq, threshold,
                        settings.p2LeashCooldownCalls, (int)ok);
        }
    }
}

static void Enforce(void* consumer) {
    if (!pointerguard::Check(pointerguard::Site::Consumer, consumer)) return;

    auto* c = static_cast<std::uint8_t*>(consumer);
    std::uint8_t* outer = c - ConsumerToOuter;
    if (!pointerguard::Check(pointerguard::Site::Outer, outer)) return;

    auto weakGet = reinterpret_cast<WeakGetFn>(
        exl::util::modules::GetTargetOffset(WeakObjectGet));
    auto pcMeta = reinterpret_cast<MetaFn>(
        exl::util::modules::GetTargetOffset(PlayerControllerMeta));
    auto resolveLp = reinterpret_cast<ResolveLpFn>(
        exl::util::modules::GetTargetOffset(ResolveLocalPlayer));

    if (weakGet == nullptr || pcMeta == nullptr || resolveLp == nullptr) return;

    void* pawn = weakGet(outer + WeakPawnOffset);
    if (!pointerguard::Check(pointerguard::Site::Pawn, pawn)) return;
    auto* pawnBytes = static_cast<std::uint8_t*>(pawn);
    void* controller = *reinterpret_cast<void**>(pawnBytes + ControllerOffset);
    if (!pointerguard::Check(pointerguard::Site::Controller, controller)) return;
    if (!IsPlayerController(pcMeta(), controller)) return;
    void* lp = resolveLp(controller);
    if (!pointerguard::Check(pointerguard::Site::LocalPlayer, lp)) return;
    const std::int32_t label = *reinterpret_cast<std::int32_t*>(
        static_cast<std::uint8_t*>(lp) + LocalPlayerIdOffset);
    sdlog::Player(label, pawn, controller, lp, pawnBytes[PawnGateByteOffset]);
    if constexpr (PSHOW_DIAG0255_ENABLED) {
        static unsigned seenPlayers = 0;
        if ((label == 0 || label == 1) && !(seenPlayers & (1U << label))) {
            seenPlayers |= 1U << label;
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC player=%d pawn=%p lp=%p tracking=live", label, pawn, lp);
        }
    }

    if (label == 0) {
        sP1Pawn = pawn;
        sP1LocalPlayer = lp;
        static void* sLastLoggedP1Hud = nullptr;
        void* p1Hud = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(controller) + 0x2B0);
        if (p1Hud != sLastLoggedP1Hud) {
            sLastLoggedP1Hud = p1Hud;
            Logging.Log("[PShowCoop] PlayerController 0 (%p)->MyHUD (+0x2B0) is: %p", controller, p1Hud);
        }
        ReadPawnLocation(pawn, sP1LocX, sP1LocY, sP1LocZ, sHasP1Loc);
        ++sPoseCalls;
        if (sHasP1Loc) {
            sPoseP1[0] = sP1LocX; sPoseP1[1] = sP1LocY; sPoseP1[2] = sP1LocZ;
            sP1Stamp = sPoseCalls;
        }
        UpdateHudAnchors();
    } else if (label == 1) {
        ++sP2Calls;
        // PATCH-0242: re-read settings.ini on the P2 pawn-change edge, before
        // the tint and leash consume it. A scene load rebuilds P2's pawn, so
        // editing the ini and changing scene applies the edit. Throttled so a
        // churning pawn cannot become per-call file I/O.
        if (kPatch0242ConfigRead && pawn != sLastConfigPawn) {
            sLastConfigPawn = pawn;
            pshowcoop::config::ReloadThrottled(sP2Calls, ConfigReloadMinCalls);
        }
        sP2Pawn = pawn;
        sP2Controller = controller;
        sP2LocalPlayer = lp;
        BindP2ControllerHUD(controller, lp);
        ReadPawnLocation(pawn, sP2LocX, sP2LocY, sP2LocZ, sHasP2Loc);
        ++sPoseCalls;
        if (sHasP2Loc) {
            sPoseP2[0] = sP2LocX; sPoseP2[1] = sP2LocY; sPoseP2[2] = sP2LocZ;
            sP2Stamp = sPoseCalls;
        }
        UpdateHudAnchors();

        // PATCH-0251: P2's PlayerCore gate follows P1's scene reasons natively.
        InputGateParityOnP2Drain(pawn);

        // Force P2 input enable (bit4 at pawn+0x228); off since PATCH-0251.
        if (kPatch0010ForceP2Input) {
            std::uint8_t before = pawnBytes[PawnGateByteOffset];
            if ((before & InputEnableBit) == 0) {
                pawnBytes[PawnGateByteOffset] = static_cast<std::uint8_t>(before | InputEnableBit);
                Logging.Log("[PShowCoop] PATCH-0010 enable-input label=1 pawn=%p ctrl=%p lp=%p before=0x%x after=0x%x",
                            pawn, controller, lp, before, pawnBytes[PawnGateByteOffset]);
            }
        }

        // PATCH-0016: tint on pawn change. The backstop (PATCH-0242 only)
        // covers a material rebuild that keeps the same pawn; 0 disables it.
        if (sLastTintedPawn != pawn) {
            ApplyP2Tint(pawn, "pawn-change");
        } else if (kPatch0242ConfigRead) {
            const std::uint32_t backstop = pshowcoop::config::Get().p2TintBackstopCalls;
            if (backstop != 0 && sP2Calls - sLastTintCall >= backstop) {
                ApplyP2Tint(pawn, "backstop");
            }
        }

        // PATCH-0015: spawn placement, then the ongoing leash (settings.ini)
        ApplyLeash();
    }
}

HOOK_DEFINE_TRAMPOLINE(CaptureQueueDrain) {
    static void Callback(void* consumer, void* p1, void* p2) {
        Enforce(consumer);
        Orig(consumer, p1, p2);
        sdlog::AfterInputDrain();
    }
};

// ---------------------------------------------------------------------------
// PATCH-0014 & PATCH-0023: Deduplicate Overlap Keys & Suppress Player-Player Bumping
// ---------------------------------------------------------------------------
constexpr ptrdiff_t OverlapBeginOffset = 0x00A39CFC;
constexpr std::uintptr_t IssuerWeakOffset = 0x28;
constexpr std::uintptr_t OwnerOffset = 0xA0;
constexpr std::uintptr_t CollectionBaseOffset = 0x40;
constexpr std::uintptr_t CollectionCountOffset = 0x48;
constexpr std::uintptr_t RecordIndexOffset = 0x8;
constexpr std::uintptr_t RecordStride = 0x30;
constexpr std::uint32_t MaxRecords = 32;

static bool ShouldDropPlayerPair(void* issuer, void* otherActor) {
    if (!IsHeapish(issuer) || !IsHeapish(otherActor) || sP1Pawn == nullptr || sP2Pawn == nullptr) {
        return false;
    }
    auto weakGet = reinterpret_cast<WeakGetFn>(exl::util::modules::GetTargetOffset(WeakObjectGet));
    if (weakGet == nullptr) return false;

    void* comp = weakGet(static_cast<std::uint8_t*>(issuer) + IssuerWeakOffset);
    if (!IsHeapish(comp)) return false;

    void* owner = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(comp) + OwnerOffset);
    return (owner == sP1Pawn && otherActor == sP2Pawn) || (owner == sP2Pawn && otherActor == sP1Pawn);
}

static bool ContainsExactKey(void* self, void* keyObject, int keyIndex) {
    const bool mapped = pointerguard::Check(pointerguard::Site::Overlap, self);
    if (!mapped || keyObject == nullptr) {
        sdlog::Overlap("key-guard-skip", self, keyObject, keyIndex, -1);
        return false;
    }

    int count = *reinterpret_cast<int*>(static_cast<std::uint8_t*>(self) + CollectionCountOffset);
    if (count <= 0 || count > static_cast<int>(MaxRecords)) {
        sdlog::Overlap("count-skip", self, keyObject, keyIndex, count);
        return false;
    }

    auto* records = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(self) + CollectionBaseOffset);
    if (records == nullptr || (reinterpret_cast<std::uintptr_t>(records) & 0x7) != 0) {
        sdlog::Overlap("records-skip", self, keyObject, keyIndex, count);
        return false;
    }

    auto weakGet = reinterpret_cast<WeakGetFn>(exl::util::modules::GetTargetOffset(WeakObjectGet));
    if (weakGet == nullptr) return false;

    for (int i = 0; i < count; i++) {
        std::uint8_t* rec = records + i * RecordStride;
        void* recObj = weakGet(rec); // rec+0x0 is stored FWeakObjectPtr
        int recIdx = *reinterpret_cast<int*>(rec + RecordIndexOffset);
        if (recObj == keyObject && recIdx == keyIndex) {
            sdlog::Overlap("duplicate-skip", self, keyObject, keyIndex, count);
            if constexpr (PSHOW_DIAG0255_ENABLED) {
                static unsigned samples = 0;
                if (samples < 4) {
                    ++samples;
                    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC duplicate-filter live collection=%p key=%p index=%d count=%d", self, keyObject, keyIndex, count);
                }
            }
            return true; // Exact duplicate overlap key detected
        }
    }
    sdlog::Overlap("no-match", self, keyObject, keyIndex, count);
    return false;
}

HOOK_DEFINE_TRAMPOLINE(SuppressPlayerOverlap) {
    static void Callback(void* param_1, void* param_2, void* otherActor, void* incomingRawObj, int incomingIndex, unsigned int param_6, void* param_7) {
        // 1. PATCH-0023: Exact player-on-player overlap bypass
        if (ShouldDropPlayerPair(param_1, otherActor)) {
            sdlog::Overlap("player-pair-skip", param_1, incomingRawObj, incomingIndex, -1);
            return;
        }

        // 2. PATCH-0014: Deduplicate exact overlap keys in container records (stops neutral hopping loop)
        if (ContainsExactKey(param_1, incomingRawObj, incomingIndex)) {
            return;
        }

        Orig(param_1, param_2, otherActor, incomingRawObj, incomingIndex, param_6, param_7);
    }
};

} // namespace

// PATCH-0239: the pawns and P2's controller belong to one world. A course
// restart destroys them, and the P2 gauge spawn then used the freed controller
// in the Outer walk after 0x03EA2F7C). Enforce() repopulates them from the new
// world's input consumers. The local players are GameInstance-owned and persist.
void ResetCoreForLevelTransition() {
    sP1Pawn = nullptr;
    sP2Pawn = nullptr;
    sP2Controller = nullptr;
    sHasP1Loc = false;
    sHasP2Loc = false;
    sLastTintedPawn = nullptr;
    // Identity tokens only, but a new world's pawn can land on a recycled
    // address; clearing them makes the restart re-read the ini and re-place P2.
    sLastConfigPawn = nullptr;
    sLeashPawn = nullptr;
    // PATCH-0017 must not frame the new world from the old world's positions.
    sP1Stamp = 0;
    sP2Stamp = 0;
    Logging.Log("[PShowCoop] PATCH-0239 ResetCoreForLevelTransition: cleared pawns and P2 controller.");
}

bool SinglePlayerControl() { return kDiag0240SinglePlayerControl; }

void GetCoopPose(CoopPoseSample* out) {
    for (int i = 0; i < 3; ++i) {
        out->p1[i] = sPoseP1[i];
        out->p2[i] = sPoseP2[i];
    }
    out->stamp1 = sP1Stamp;
    out->stamp2 = sP2Stamp;
}

bool ReadCoopPositionsNow(float* p1, float* p2) {
    bool ok1 = false;
    bool ok2 = false;
    ReadPawnLocation(sP1Pawn, p1[0], p1[1], p1[2], ok1);
    ReadPawnLocation(sP2Pawn, p2[0], p2[1], p2[2], ok2);
    return ok1 && ok2;
}

void* GetP1Pawn() { return sP1Pawn; }
void* GetP2Pawn() { return sP2Pawn; }
void* GetP2Controller() { return sP2Controller; }
void* GetP1LocalPlayer() { return sP1LocalPlayer; }
void* GetP2LocalPlayer() { return sP2LocalPlayer; }

void InstallCore() {
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC performance_control=%d sd_startup_only=%d input_trace=%d riding_trace=%d guard_trace=%d retained_mapping_guard=%d",
        PSHOW_DIAG0257_PERF_CONTROL, PSHOW_SD_LOG_STARTUP_ONLY,
        PSHOW_DIAG0253_ENABLED, PSHOW_DIAG0256_ENABLED, PSHOW_DIAG0255_ENABLED, PSHOW_PATCH0273_ENABLED);
    pointerguard::Installed();
    EnforceMaxControllers();
    ExplicitLocalPlayerCreation::InstallAtOffset(ExplicitLocalPlayerCreator);
    SuppressControllerApplet::InstallAtOffset(ShowControllerSupportOffset);
    ForceTwoPlayerInputCount::InstallAtOffset(NpadPollerGate);
    CaptureQueueDrain::InstallAtOffset(QueueDrainConsumer);
    SuppressPlayerOverlap::InstallAtOffset(OverlapBeginOffset);
    Logging.Log("[PShowCoop] Core 2-Player (PATCH-0001/0004/0007/0009/0010/0014/0015/0016/0023) hooks installed.");
}

} // namespace pshowcoop::patches
