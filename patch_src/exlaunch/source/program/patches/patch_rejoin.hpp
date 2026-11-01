#pragma once

namespace pshowcoop::patches {

void InstallRejoin();
enum class RejoinResult { Disabled, Deferred, Moved };
// PATCH-0270: native checked placement for an existing ongoing return.
// Disabled restores the baseline; Deferred must not fall through to its snap.
RejoinResult TrySmartRejoin(void* p1Pawn, void* p2Pawn);

} // namespace pshowcoop::patches
