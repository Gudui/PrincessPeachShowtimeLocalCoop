#pragma once

// User-editable settings for PShowCoop, read from a LayeredFS romfs file so the
// values can be tweaked without a subsdk9 rebuild. Structure follows the same
// generic pattern the Odyssey co-op mod uses; no offsets, layouts, or findings
// are carried over from that title, per AGENTS.md.

namespace pshowcoop::config {

struct Settings {
    // PATCH-0016 P2 mesh tint.
    bool p2TintEnabled;
    float p2TintR;
    float p2TintG;
    float p2TintB;
    float p2TintIntensity;
    // Material parameter names are exposed deliberately: which parameters
    // Peach's material actually exposes is not statically known, so trying a
    // different name must not require a rebuild.
    char p2TintColorParam[64];
    char p2TintIntensityParam[64];
    // Backstop only. The tint is normally reapplied on a pawn-change edge; this
    // bounds how long a missed edge can leave P2 untinted. 0 disables it.
    unsigned p2TintBackstopCalls;

    // PATCH-0015 P2 leash. These were compile-time constants until the camera
    // work needed them tunable: the leash caps how far apart the players can
    // get, so it also caps what the camera is ever asked to frame. With the
    // threshold compiled in, "how far can P2 go before leaving the screen?"
    // could not be answered without a rebuild per guess.
    //
    // The two thresholds are deliberately separate because PATCH-0015 does two
    // different jobs with one number. Placement fixes the ~1100-unit off-camera
    // choice. Splitting them makes p2.leash.distance=0 a legal configuration:
    // P2 is still placed beside P1 on spawn, then roams freely.
    float p2LeashSpawnDistance;   // first relocation only. 0 disables placement.
    float p2LeashDistance;        // every later relocation. 0 disables the leash.
    unsigned p2LeashCooldownCalls;

    // PATCH-0244: startup warp destination map (PATCH-0032/PATCH-0241). Read
    // once, at the first warp use, which is earlier than any pawn. A bare map
    // name such as Fencer_A; anything other than [A-Za-z0-9_] falls back to
    // the default.
    char warpTarget[64];
};

const Settings& Get();

// Reads the ini and replaces the live settings. Safe to call when the file is
// absent: every field falls back to its built-in default.
void Reload();

// Bounded reload used from a gameplay hook: performs at most one file read per
// minIntervalCalls invocations and returns true when a read actually happened.
bool ReloadThrottled(unsigned callCounter, unsigned minIntervalCalls);

}  // namespace pshowcoop::config
