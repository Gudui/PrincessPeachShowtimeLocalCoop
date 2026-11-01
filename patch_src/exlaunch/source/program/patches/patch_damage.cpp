#include "pointer_guard.hpp"
#include "patch_damage.hpp"
#include "patch_core.hpp"
#include "patch_hud.hpp"
#include "patch_life_slot.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>

namespace pshowcoop::patches {

namespace {

inline bool IsHeapish(const void* value) {
    return pshowcoop::pointerguard::Readable(value);
}

// Removed legacy diagnostic at 0xA3A8FC: this is a shared delegate broadcaster,
// walk could resolve non-weak fields as FWeakObjectPtr and flooded SD logs.
// Leave the broadcaster unhooked; real native player-damage hooks below remain.

constexpr ptrdiff_t OnDamageReceivedCallbackOffset = 0x01D226A8;

HOOK_DEFINE_TRAMPOLINE(CaptureOnDamageReceivedCallback) {
    static void Callback(void* peachActor, void* damageParams, void* result) {
        void* p1Pawn = GetP1Pawn();
        void* p2Pawn = GetP2Pawn();
        if (peachActor != nullptr && peachActor == p2Pawn) {
            Logging.Log("[PShowCoop] OnDamageReceivedCallback: Player 2 Pawn (%p) damaged. Executing full native pipeline pass-through.", peachActor);
        } else if (peachActor != nullptr && peachActor == p1Pawn) {
            Logging.Log("[PShowCoop] OnDamageReceivedCallback: Player 1 Pawn (%p) damaged. Executing full native pipeline pass-through.", peachActor);
        }

        // Full native pass-through: preserves engine side-effect chain:
        // iframes (0x01D27A28), flinch animation (0x01D27B80), knockback (0x00A385D4), and TakeDamage (0x0435C1F0).
        Orig(peachActor, damageParams, result);
    }
};
// ---------------------------------------------------------------------------
// Step 3: Pure Native Component Health & HUD Presentation Dispatch
// ---------------------------------------------------------------------------
constexpr ptrdiff_t GetPlayerHealthOffset = 0x01A87E38;
constexpr ptrdiff_t SetPlayerHealthOffset = 0x01A87E70;
static void* sCachedP2Comp = nullptr;
static void* sLastInitializedP2Comp = nullptr;

HOOK_DEFINE_TRAMPOLINE(CaptureGetPlayerHealth) {
    static std::int32_t Callback(void* comp) {
        if (IsPlayer2StatusComponent(comp) && LifeSlotPatchEnabled()) {
            // PATCH-0232: P2 reads its own native life slot through the native
            SyncP2LifeIndex(comp);
            return Orig(comp);
        }
        if (IsPlayer2StatusComponent(comp)) {
            // Rollback path (kPatch0232P2LifeSlot=false): mod-managed shadow,
            auto* compBytes = reinterpret_cast<std::uint8_t*>(comp);
            auto* hpPtr = reinterpret_cast<std::int32_t*>(compBytes + 0x58);

            // Initialize fresh scene / pawn component health to 5
            if (comp != sLastInitializedP2Comp) {
                sLastInitializedP2Comp = comp;
                *hpPtr = 5;
                Logging.Log("[PShowCoop] GetPlayerHealth: Initialized fresh P2 UPlayerStatusComponent (%p) health to 5", comp);
            }

            return *hpPtr;
        }

        return Orig(comp);
    }
};

HOOK_DEFINE_TRAMPOLINE(CaptureSetPlayerHealth) {
    static void Callback(void* comp, std::int32_t newHearts) {
        if (IsPlayer2StatusComponent(comp)) {
            auto* compBytes = reinterpret_cast<std::uint8_t*>(comp);
            const bool nativeSlot = LifeSlotPatchEnabled() && SyncP2LifeIndex(comp) != 0;
            std::int32_t oldHearts = nativeSlot
                ? CaptureGetPlayerHealth::Orig(comp)
                : *reinterpret_cast<std::int32_t*>(compBytes + 0x58);

            Logging.Log("[PShowCoop] SetPlayerHealth (P2): comp=%p oldHearts=%d newHearts=%d -> native Orig() pass-through",
                        comp, oldHearts, newHearts);
            // is the smoking gun). Read-only.
            LogGaugeLiveness("P2-damage");

            InspectHudRegistry();

            // Dispatch native presentation to Player 2's dedicated widget
            RevealP2DamageHUD(newHearts);

            // Zero Bypassing: Execute full native UPlayerStatusComponent::SetPlayerHealth (0x01A87E70)
            // This executes the complete engine side-effect chain:
            // 1. Native player health state write (0x0138F0A8)
            // 2. Subsystem notification & HUD alert dispatch (0x01A8808C)
            // 3. Scalar calculation at +0xAC and delegate broadcast (0x01A87F30)
            // 4. Component state timer resets (+0x55, +0x50, +0x78)
            Orig(comp, newHearts);
            LogLifeSlots("P2-set", comp);
            return;
        }

        Logging.Log("[PShowCoop] SetPlayerHealth (P1): comp=%p newHearts=%d -> native Orig()",
                    comp, newHearts);
        LogGaugeLiveness("P1-damage");
        Orig(comp, newHearts);
        LogLifeSlots("P1-set", comp);
    }
};

constexpr ptrdiff_t HudAlertDispatcherOffset = 0x01A8808C;

HOOK_DEFINE_TRAMPOLINE(CaptureHudAlertDispatch) {
    static void Callback(void* comp) {
        void* p1Pawn = GetP1Pawn();
        void* p2Pawn = GetP2Pawn();
        bool isP2 = IsPlayer2StatusComponent(comp);

        Logging.Log("[PShowCoop] 0x01A8808C Alert Dispatcher: comp=%p (isP2=%d, P1Pawn=%p, P2Pawn=%p)",
                    comp, isP2 ? 1 : 0, p1Pawn, p2Pawn);

        Orig(comp);

        Logging.Log("[PShowCoop] 0x01A8808C Alert Dispatcher: completed for comp=%p", comp);
    }
};

} // namespace

bool IsPlayer2StatusComponent(void* comp) {
    if (!IsHeapish(comp)) return false;
    if (sCachedP2Comp != nullptr && comp == sCachedP2Comp) {
        return true;
    }

    void* p2Pawn = GetP2Pawn();
    if (!IsHeapish(p2Pawn)) return false;

    // Direct 2-hop navigation:
    // pawn + 0x7D0 is APeachCharacter::ExtensionWrapper (pawn[0xFA])
    // wrapper + 0x68 is UPlayerStatusComponent (func_1a726c0)
    void* p2Wrapper = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p2Pawn) + 0x7D0);
    if (IsHeapish(p2Wrapper)) {
        void* p2Comp = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(p2Wrapper) + 0x68);
        if (IsHeapish(p2Comp) && p2Comp != sCachedP2Comp) {
            sCachedP2Comp = p2Comp;
            Logging.Log("[PShowCoop] Resolved P2 UPlayerStatusComponent: pawn=%p -> wrapper=%p (+0x7D0) -> comp=%p (+0x68)",
                        p2Pawn, p2Wrapper, p2Comp);
        }
        if (comp == p2Comp) return true;
    }

    // Fallback: Check if comp is cached initialized component
    if (sLastInitializedP2Comp != nullptr && comp == sLastInitializedP2Comp) {
        return true;
    }

    return false;
}

void ResetP2HealthTracking() {
    sCachedP2Comp = nullptr;
    sLastInitializedP2Comp = nullptr;
}

void InstallDamage() {
    CaptureOnDamageReceivedCallback::InstallAtOffset(OnDamageReceivedCallbackOffset);
    CaptureGetPlayerHealth::InstallAtOffset(GetPlayerHealthOffset);
    CaptureSetPlayerHealth::InstallAtOffset(SetPlayerHealthOffset);
    CaptureHudAlertDispatch::InstallAtOffset(HudAlertDispatcherOffset);
    Logging.Log("[PShowCoop] Strict Native Engine Integration: legacy_broadcast_probe=off, OnDamage=0x%lx, GetHealth=0x%lx, SetHealth=0x%lx, AlertDispatch=0x%lx",
                static_cast<unsigned long>(OnDamageReceivedCallbackOffset),
                static_cast<unsigned long>(GetPlayerHealthOffset),
                static_cast<unsigned long>(SetPlayerHealthOffset),
                static_cast<unsigned long>(HudAlertDispatcherOffset));
}

} // namespace pshowcoop::patches
