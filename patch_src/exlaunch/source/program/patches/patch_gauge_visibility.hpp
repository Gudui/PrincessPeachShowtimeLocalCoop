#pragma once
#include <cstdint>

namespace pshowcoop::patches {
// 2026-09-23). Runs the rules of the native CourseUI driver 0x009A6800 for P2's
// pawn, P2's life slot and P2's gauge, right after the native pass for P1.
void InstallGaugeVisibility();
bool P2GaugeDriverEnabled();
void ResetP2GaugeDriver();

} // namespace pshowcoop::patches
