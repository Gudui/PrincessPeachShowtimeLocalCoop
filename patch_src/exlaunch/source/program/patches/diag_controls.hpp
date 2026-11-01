#pragma once
#include "diagnostic_profile.hpp"

#include <cstdint>

namespace pshowcoop::diagcontrols {
// reassignment, state forcing, extra hooks, or synthetic gameplay state.
inline constexpr bool Enabled = PSHOW_DIAG0253_ENABLED;
void Observe(void* actionInput, void* pawn, int player, std::uintptr_t caller);
}
