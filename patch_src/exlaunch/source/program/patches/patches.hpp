#pragma once

#include "patch_core.hpp"
#include "patch_warp.hpp"
#include "patch_damage.hpp"
#include "patch_hud.hpp"
#include "patch_life_slot.hpp"
#include "patch_gauge_visibility.hpp"
#include "patch_camera.hpp"
#include "patch_ribbon.hpp"
#include "patch_costume.hpp"
#include "patch_player_count.hpp"
#include "patch_input_parity.hpp"
#include "patch_event_parity.hpp"
#include "patch_diag_hide.hpp"
#include "patch_diag_input.hpp"
#include "patch_enemy_target.hpp"
#include "patch_player_line.hpp"
#include "patch_rejoin.hpp"
#include "diag_riding.hpp" // Installed by event parity after its native-player getter trampoline.

namespace pshowcoop::patches {

inline void InstallAll() {
    InstallCore();
    InstallWarp();
    InstallDamage();
    InstallLifeSlot();
    InstallHud();
    InstallGaugeVisibility();
    InstallCamera();
    InstallRibbon();
    InstallCostume();
    InstallPlayerCount();
    InstallInputParity();
    InstallEventParity();
    InstallDiagHide();
    InstallDiagInput();
    InstallEnemyTarget();
    InstallPlayerLine();
    InstallRejoin();
}

} // namespace pshowcoop::patches
