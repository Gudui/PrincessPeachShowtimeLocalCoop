#pragma once
#include <cstdint>

namespace pshowcoop::patches {

void InstallHud();
void UpdateHudAnchors();
void* GetP1LifeGauge();
void* GetP2LifeGauge();
// pointers still equal the native LifeGauge vtable. Zero behavior change.
void LogGaugeLiveness(const char* tag);
// PATCH-0204 memory-safety guard: true iff the gauge is heap-valid AND its live
// vtable pointer still equals the native LifeGauge vtable. Calls into a gauge
// failing this check are skipped with an explicit log line (never silently).
bool GaugeVtableIntact(void* gauge);
void TriggerP2DamageHUD();
void RevealP2DamageHUD(std::int32_t newHearts);
void ResetHudForLevelTransition();
void BindP2NativeDamageDelegate(void* p2DamageComp);
void BindP2ControllerHUD(void* p2Controller, void* p2LocalPlayer = nullptr);
void RegisterP2HudPlayer(void* p2Controller, void* p2LocalPlayer = nullptr);
void InspectHudRegistry();
void* GetP2HudContext();

} // namespace pshowcoop::patches
