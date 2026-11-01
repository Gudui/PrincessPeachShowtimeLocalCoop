#pragma once
#include <cstdint>

namespace pshowcoop::patches {

// PATCH-0232: give Player 2 its own native CourseGameStateBase life slot
void InstallLifeSlot();
bool LifeSlotPatchEnabled();

// Points P2's status component at slot 2 when the course GameState has one,
// and back at slot 1 when it does not (never at an out-of-range slot, which
// the native accessors would read as 0 health). Returns the index in effect,
// or 0 when the component is not readable.
int SyncP2LifeIndex(void* comp);

// Read-only log of both native life slots through the native accessor.
void LogLifeSlots(const char* tag, void* comp);

} // namespace pshowcoop::patches
