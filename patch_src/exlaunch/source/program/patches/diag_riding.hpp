#pragma once
#include "diagnostic_profile.hpp"
#include <cstdint>
#ifndef PSHOW_DIAG0256_ENABLED
#define PSHOW_DIAG0256_ENABLED 1
#endif
namespace pshowcoop::riding {
using ResolvePlayer = void* (*)(void*, std::int32_t);
void Install(ResolvePlayer nativePlayer);
void ObserveUpdate(void* manager);
void ObserveEvent(void* manager, bool afterP2, unsigned inputAllowed);
void ObserveControl(void* component, bool afterP2, bool begin);
}
