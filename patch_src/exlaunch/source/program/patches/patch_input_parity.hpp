#pragma once

namespace pshowcoop::patches {
void InstallInputParity();
// PATCH-0251 drain parity; called by the core input drain on every P2 drain.
void InputGateParityOnP2Drain(void* p2Pawn);
}
