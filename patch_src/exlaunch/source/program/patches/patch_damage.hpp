#pragma once
#include <cstdint>

namespace pshowcoop::patches {
void InstallDamage();
void ResetP2HealthTracking();
bool IsPlayer2StatusComponent(void* comp);
}
