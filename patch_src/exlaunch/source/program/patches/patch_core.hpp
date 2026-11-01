#pragma once
#include <cstdint>

namespace pshowcoop::patches {

// Latest usable P1/P2 world positions, stamped with the shared label-0/1
// consumer call on which each was read (0 = none since the last level reset).
// Read by PATCH-0017 to frame both players.
struct CoopPoseSample {
    float p1[3];
    float p2[3];
    std::uint32_t stamp1;
    std::uint32_t stamp2;
};

void InstallCore();
void GetCoopPose(CoopPoseSample* out);
// PATCH-0264: both players' root locations (pawn +0x130 -> +0x1D0) read now, at
// the caller's moment; false unless both pawns give a usable location.
bool ReadCoopPositionsNow(float* p1, float* p2);
void* GetP1Pawn();
void* GetP2Pawn();
void* GetP2Controller();
void* GetP1LocalPlayer();
void* GetP2LocalPlayer();
void ResetCoreForLevelTransition();
bool SinglePlayerControl();
}
