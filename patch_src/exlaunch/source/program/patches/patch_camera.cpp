#include "pointer_guard.hpp"
#include "patch_camera.hpp"
#include "patch_core.hpp"
#include "patch_event_parity.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include <cstdint>
#include <cstdio>

namespace pshowcoop::patches {

namespace {

// ---------------------------------------------------------------------------
// PATCH-0017: frame both players - recentre and widen on X, push back on Y.
// Ported unchanged from the confirmed same-commit configuration in commit
//
// The stock pose is the game's finished shot for P1, so only P2's contribution
// stutters lightly when camera X is coupled to P2.
// ---------------------------------------------------------------------------
constexpr bool kPatch0017TwoPlayerCamera = true;

// ---------------------------------------------------------------------------
// recentres on world X and pushes back on world +Y, which fits only the
// Fencer_A's walk-round section left the camera unusable; with it off the
// along the ground-plane axes of the stock view's yaw (FMinimalViewInfo
// (-sin yaw, cos yaw), forward (cos yaw, sin yaw). At yaw -90 these are (1, 0)
// and (0, -1), so the side-on result is PATCH-0017's exactly. A view pitched
// further than MaxFramingPitchDegrees is left to the game: the corrections
// slew back to zero. false = PATCH-0017's fixed world axes.
// ---------------------------------------------------------------------------
constexpr bool kPatch0261ViewRelativeFraming = true;

// ---------------------------------------------------------------------------
// PATCH-0262: the FOV need reads the smoothed player positions (operator
// 2026-10-18: the widening stutters). The sampled positions of both players
// advance unevenly per camera frame, alternating like 9.3/28.5/9.9/28.1 units
// PATCH-0017's FOV need still read both raw positions, so the widened FOV
// alternated with them. Both players now pass through the same filter before
// the FOV need. false = raw positions.
// ---------------------------------------------------------------------------
constexpr bool kPatch0262SmoothFovNeed = true;

// ---------------------------------------------------------------------------
// found commits alternating about 17 and 50 ms apart while the game moves its
// camera and the players by elapsed time. The positions came from the players'
// input drains, earlier in the frame, so they lagged the stock pose by an
// alternating step (walking P2 advanced 28.5 units in the 17 ms frames and 8.7
// in the 50 ms ones), and every filter and slew stepped once per commit
// whatever the commit's length. Both players' locations are now read at the
// commit (pawn +0x130 -> +0x1D0, the path the game's sight query 0x101CC34
// reads), and the filters and slews scale with the time since the previous
// commit (nominal 33.3 ms). false = drain samples and per-commit steps.
// ---------------------------------------------------------------------------
constexpr bool kPatch0264CommitClock = true;

// ---------------------------------------------------------------------------
// PATCH-0266: scenes own the camera. While P1's controller is in cinematic
// mode - PlayerController +0x3E0 bit 0x2, which SetCinematicMode (0x4800514,
// 0x47FE92C; FUNC-PC-SETCINEMATICMODE) sets for the game's cutscenes - the
// the swordfighter stand's cutscene set it on both controllers from 508.1 to
// 514.1 s while PATCH-0017 kept recentring and widening the scene's camera
// (one frame pushed 1028 units and +30 degrees), so the operator could not see
// the scene. false = PATCH-0017 frames through cutscenes as before.
// ---------------------------------------------------------------------------
constexpr bool kPatch0266CinematicStandDown = true;
constexpr std::uintptr_t PawnController = 0x258;       // APawn::Controller (patch_core's ControllerOffset)
constexpr std::uintptr_t ControllerCinematic = 0x3E0;  // byte; bit 0x2 = bInCinematicMode
constexpr std::uint8_t CinematicModeBit = 0x2;
constexpr std::uint32_t MaxStandDownLines = 24;

// ---------------------------------------------------------------------------
// PATCH-0269: no pair framing across scenes. When the players are more than
// SceneGapUnits apart, the stock pose is left alone as for PATCH-0266. In play
// PATCH-0015 keeps P2 within its 2200-unit leash; a larger gap means the game
// at (205000,0,-150) with P2 left 85,000 units away in the boss room, and the
// clamped recentre/push-back aimed the results camera at the floor.
// false = frame any pair as before.
// ---------------------------------------------------------------------------
constexpr bool kPatch0269SceneGapStandDown = true;
constexpr float SceneGapUnits = 5000.0f;

// ---------------------------------------------------------------------------
// widens? For BurstFrames consecutive applied commits (about 4 s at 30 fps):
// the commit interval from the system tick, base and output FOV, the FOV need
// from raw and from smoothed positions, the stock yaw, base X/Y, recentre and
// push-back, the P1/P2 positions the framing used (src 1 = read at the commit,
// PATCH-0264; 0 = drain samples), and how far each player's pose stamp
// advanced since the previous commit. Up to MaxWideBursts while the FOV gain is above 1
// degree ('W') and MaxControlBursts with no gain while P1 moves ('C', the
// control), each starting only while no scene holds P1. Kept in memory and
// printed after each burst, three frames per line, so no log call falls inside
// a recorded frame. Remove once read.
// ---------------------------------------------------------------------------
constexpr bool kDiag0247CameraBursts = true;
constexpr ptrdiff_t GetCameraLocation = 0x47E8AF8;
// Pose deep-copy helper, tail-called by the commit thunk 0x47EB5D4 with x0
// holds the thunk on both observed manager vtables.
constexpr ptrdiff_t PoseDeepCopy = 0x059F1E4;
constexpr std::uintptr_t CommitThunk = 0x47EB5D4;
constexpr std::uintptr_t CommitSlot = 0x668;
// FOV +0x18.
constexpr std::uintptr_t PoseOffsetInManager = 0x1AF0;
constexpr std::uintptr_t LocYInPoseOffset = 0x4;
constexpr std::uintptr_t PitchInPoseOffset = 0xC;
constexpr std::uintptr_t YawInPoseOffset = 0x10;
constexpr std::uintptr_t FovInPoseOffset = 0x18;

// Empty space beyond the outermost player, world units.
constexpr float EdgeMarginUnits = 300.0f;
// Clamps that should never bind in ordinary play; they keep a garbage position
// (mid-teleport read, scene-load straggler) from throwing the camera away.
// Half of the 2200-unit ongoing leash, so midpoint framing covers it.
constexpr float MaxRecentreUnits = 1100.0f;
constexpr float MaxPushBackUnits = 1400.0f;
constexpr float MaxFovGainDegrees = 32.0f;
constexpr float MaxFovDegrees = 110.0f;
// Below this depth the perspective maths degenerates; refuse rather than clamp.
constexpr float MinUsableDepth = 250.0f;
constexpr float PlausibleFovMin = 10.0f;
constexpr float PlausibleFovMax = 170.0f;
// Consumer-clock skew allowed between the two published samples.
constexpr std::uint32_t MaxStampSkew = 600;

// Slew limits, not easing: camX = baseX + recentre are coupled terms, and
// through untouched and only engages on a discontinuity such as a leash snap.
// Applied once per frame, hence the x3 against the per-call figures.
constexpr float SlewUnitsPerCall = 8.0f;
constexpr float SlewFovDegreesPerCall = 0.25f;

// Low-pass on the stock camera depth (P1's distance in front of the stock view;
// jitter out of the depth term.
constexpr float DepthFilterAlpha = 0.015625f;
constexpr float PlausibleDepthMin = 200.0f;
constexpr float PlausibleDepthMax = 6000.0f;
// More than this in one frame is a new camera volume or scene: re-seed.
constexpr float DepthReseedUnits = 300.0f;

// P2 position filter matched to the game's own P1 follow damping (v5).
constexpr float P2FilterAlpha = 0.25f;

// PATCH-0261: the framing model assumes a near-level view (the side-on rig is
constexpr float MaxFramingPitchDegrees = 30.0f;
// A view line whenever the stock yaw or pitch moved this far since the last
// one, or the view crossed the pitch limit; plus one every SlowLineCommits.
// Scene camera cuts use some of the budget before the walk-round section.
constexpr float ViewLineYawDegrees = 20.0f;
constexpr float ViewLinePitchDegrees = 10.0f;
constexpr std::uint32_t SlowLineCommits = 1800;
constexpr std::uint32_t MaxViewLines = 240;

// PATCH-0262: P1 shares P2's filter, matched to the game's P1 follow damping.
constexpr float FovNeedFilterAlpha = P2FilterAlpha;

// PATCH-0264: a nominal commit, the clamp on how many of them one commit may
// count for, and ln(1 - alpha) of the per-commit filters, so that a commit of
// `scale` nominal commits weighs 1 - (1 - alpha)^scale.
constexpr float NominalCommitUs = 33333.0f;
constexpr float MinStepScale = 0.25f;
constexpr float MaxStepScale = 3.0f;
constexpr float LogKeepPositionFilter = -0.2876821f;  // ln(1 - 0.25)
constexpr float LogKeepDepthFilter = -0.0157484f;     // ln(1 - 1/64)
constexpr std::uint32_t BurstFrames = 120;
constexpr std::uint32_t BurstFramesPerLine = 3;  // about 120 characters a frame; lines hold 511
constexpr std::uint32_t MaxWideBursts = 3;
constexpr std::uint32_t MaxControlBursts = 1;
constexpr std::uint32_t BurstGapCommits = 600;
constexpr float BurstMinGainDegrees = 1.0f;

constexpr std::uint32_t MaxLines = 12;

struct Vec3 {
    float x;
    float y;
    float z;
};

// Opaque identity token: compared and logged, never dereferenced.
static const void* sActiveManagerToken = nullptr;
static std::uint32_t sApplied = 0;
static std::uint32_t sSkipped = 0;
static std::uint32_t sLines = 0;
static float sSmoothRecentre = 0.0f;
static float sSmoothPushBack = 0.0f;
static float sSmoothFovGain = 0.0f;
static float sStockDepth = 0.0f;
static bool sHaveStockDepth = false;
static float sP2SmoothX = 0.0f;
static float sP2SmoothY = 0.0f;
static bool sHaveP2Smooth = false;
static float sP1SmoothX = 0.0f;
static float sP1SmoothY = 0.0f;
static bool sHaveP1Smooth = false;
static std::uint32_t sViewLines = 0;
static float sLoggedYaw = 0.0f;
static float sLoggedPitch = 0.0f;
static bool sLoggedLevel = true;
static bool sHaveLoggedView = false;
static bool sStoodDown = false;
static std::uint32_t sStandDownLines = 0;
static bool sGapStoodDown = false;        // PATCH-0269
static std::uint32_t sGapLines = 0;

static bool HeapPointer(const void* value) {
    return pshowcoop::pointerguard::Readable(value);
}

// PATCH-0266: P1's controller is in the game's cinematic mode.
static bool P1InCinematic() {
    void* pawn = GetP1Pawn();
    if (!HeapPointer(pawn)) return false;
    void* controller = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(pawn) + PawnController);
    if (!HeapPointer(controller)) return false;
    return (*(static_cast<std::uint8_t*>(controller) + ControllerCinematic) & CinematicModeBit) != 0;
}

static float AbsF(float v) { return v < 0.0f ? -v : v; }

// e^x for -1 <= x <= 0 (Taylor to x^6, error below 1e-4); no libm here.
static float ExpSmall(float x) {
    if (x < -1.0f) x = -1.0f;
    if (x > 0.0f) x = 0.0f;
    return 1.0f + x * (1.0f + x / 2.0f * (1.0f + x / 3.0f * (1.0f + x / 4.0f * (1.0f + x / 5.0f * (1.0f + x / 6.0f)))));
}

constexpr float Pi = 3.14159265f;

// sin x for |x| <= pi/2: Taylor series to x^11, error about 1e-7; no libm here.
static float SinFolded(float x) {
    const float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}

// sin and cos of an angle in degrees; false for NaN or an angle no rotation holds.
static bool SinCosDegrees(float degrees, float* s, float* c) {
    if (!(degrees > -100000.0f && degrees < 100000.0f)) return false;
    float d = degrees - 360.0f * static_cast<float>(static_cast<int>(degrees / 360.0f));
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    const float x = d * (Pi / 180.0f);
    // sin(pi - x) = sin x folds [-pi, pi] into [-pi/2, pi/2].
    float xs = x;
    if (xs > Pi * 0.5f) xs = Pi - xs;
    if (xs < -Pi * 0.5f) xs = -Pi - xs;
    // cos x = sin(pi/2 - x), with pi/2 - x in [-pi/2, 3pi/2].
    float xc = Pi * 0.5f - x;
    if (xc > Pi * 0.5f) xc = Pi - xc;
    *s = SinFolded(xs);
    *c = SinFolded(xc);
    return true;
}

// Ground-plane axes of a view: lateral (right) and depth (forward) components
// of a world offset.
struct ViewAxes {
    float rightX;
    float rightY;
    float forwardX;
    float forwardY;

    float Lateral(float dx, float dy) const { return dx * rightX + dy * rightY; }
    float Depth(float dx, float dy) const { return dx * forwardX + dy * forwardY; }
};
constexpr ViewAxes SideOnAxes = {1.0f, 0.0f, 0.0f, -1.0f};

// atan in degrees for t >= 0, within ~0.22 degrees; no libm in this module.
static float AtanDegrees(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) {
        const float inv = 1.0f / t;
        return 90.0f - (inv * 45.0f + 15.6408f * inv * (1.0f - inv));
    }
    return t * 45.0f + 15.6408f * t * (1.0f - t);
}

static float Clamp(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float Slew(float current, float target, float step) {
    const float d = target - current;
    if (d > step) return current + step;
    if (d < -step) return current - step;
    return target;
}

// FOV (degrees) that keeps both ground-plane points, plus the edge margin,
// inside a view at (camX, camY); *ok false when either is too close to the view.
static float FovNeed(const ViewAxes& axes, float camX, float camY, float ax, float ay, float bx, float by, bool* ok) {
    const float depthA = axes.Depth(ax - camX, ay - camY);
    const float depthB = axes.Depth(bx - camX, by - camY);
    *ok = depthA >= MinUsableDepth && depthB >= MinUsableDepth;
    if (!*ok) return 0.0f;
    const float tA = (AbsF(axes.Lateral(ax - camX, ay - camY)) + EdgeMarginUnits) / depthA;
    const float tB = (AbsF(axes.Lateral(bx - camX, by - camY)) + EdgeMarginUnits) / depthB;
    return 2.0f * AtanDegrees(tA > tB ? tA : tB);
}
struct BurstFrame {
    std::uint32_t intervalUs;
    float baseFov;
    float outFov;
    float needRaw;
    float needUsed;
    float yaw;
    float baseX;
    float baseY;
    float recentre;
    float pushBack;
    float p1x;
    float p1y;
    float p2x;
    float p2y;
    std::uint32_t stampStep1;
    std::uint32_t stampStep2;
    std::uint32_t source;  // 1 = positions read at the commit (PATCH-0264), 0 = drain samples
};

static BurstFrame sBurst[BurstFrames];
static std::uint32_t sBurstLen = 0;
static std::uint32_t sBursts = 0;
static std::uint32_t sWideBursts = 0;
static std::uint32_t sControlBursts = 0;
static char sBurstKind = '-';
static std::uint32_t sBurstStartApplied = 0;
static std::uint32_t sNextBurstApplied = 0;
static std::uint64_t sLastCommitTick = 0;
static std::uint32_t sLastStamp1 = 0;
static std::uint32_t sLastStamp2 = 0;
static float sLastRawP1X = 0.0f;
static float sLastRawP1Y = 0.0f;

static void DumpBurst() {
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC burst=%u kind=%c start_applied=%u frames=%u fields=interval_us base_fov "
                "out_fov need_raw need_used yaw base_x base_y recentre pushback p1x p1y p2x p2y stamp1 stamp2 src",
                sBursts, sBurstKind, sBurstStartApplied, sBurstLen);
    for (std::uint32_t i = 0; i < sBurstLen; i += BurstFramesPerLine) {
        char line[512];
        int used = std::snprintf(line, sizeof(line), "[PShowCoop] RELEASE-DIAGNOSTIC b=%u i=%u", sBursts, i);
        for (std::uint32_t j = i; j < sBurstLen && j < i + BurstFramesPerLine && used > 0; ++j) {
            const BurstFrame& f = sBurst[j];
            const int n = std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used),
                                        " |%u %.2f %.2f %.2f %.2f %.2f %.0f %.0f %.1f %.1f %.1f %.1f %.1f %.1f %u %u %u",
                                        f.intervalUs, f.baseFov, f.outFov, f.needRaw, f.needUsed, f.yaw, f.baseX,
                                        f.baseY, f.recentre, f.pushBack, f.p1x, f.p1y, f.p2x, f.p2y, f.stampStep1,
                                        f.stampStep2, f.source);
            if (n <= 0 || used + n >= static_cast<int>(sizeof(line))) break;
            used += n;
        }
        Logging.Log(line);
    }
}

// One applied commit: start a burst when one is due, record while one runs,
// print it when it is full.
static void Diag47Commit(const BurstFrame& frame, const float* p1, bool p1Moved) {
    if (!kDiag0247CameraBursts) return;
    // Bursts start in play only: scene cameras widen too, and the stutter was seen in play.
    if (sBurstLen == 0 && sApplied >= sNextBurstApplied && !P1HeldByEvent()) {
        if (sWideBursts < MaxWideBursts && sSmoothFovGain > BurstMinGainDegrees) {
            sBurstKind = 'W';
            sWideBursts++;
        } else if (sControlBursts < MaxControlBursts && sSmoothFovGain == 0.0f && p1Moved) {
            sBurstKind = 'C';
            sControlBursts++;
        } else {
            sBurstKind = '-';
        }
        if (sBurstKind != '-') {
            sBursts++;
            sBurstStartApplied = sApplied;
            sBurst[sBurstLen++] = frame;
        }
    } else if (sBurstLen > 0) {
        sBurst[sBurstLen++] = frame;
        if (sBurstLen == BurstFrames) {
            DumpBurst();
            sBurstLen = 0;
            sNextBurstApplied = sApplied + BurstGapCommits;
        }
    }
    sLastRawP1X = p1[0];
    sLastRawP1Y = p1[1];
}

HOOK_DEFINE_TRAMPOLINE(CommitCameraPose) {
    static void* Callback(void* dst, const void* src) {
        void* result = Orig(dst, src);
        if (dst == nullptr || src == nullptr) return result;

        // Only a manager pose commit: dst - 0x1AF0 must carry a main-module
        // vtable whose +0x668 slot is the commit thunk.
        const std::uintptr_t dstAddress = reinterpret_cast<std::uintptr_t>(dst);
        if (dstAddress < PoseOffsetInManager) return result;
        auto* owner = reinterpret_cast<std::uint8_t*>(dstAddress - PoseOffsetInManager);
        if (owner != sActiveManagerToken) {
            // accessor saw is changed. A new scene gets one stock frame.
            sSkipped++;
            return result;
        }
        void* vtable = *reinterpret_cast<void**>(owner);
        const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
        if (!mainRange.InRange(reinterpret_cast<std::uintptr_t>(vtable))) return result;
        void* slotTarget = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(vtable) + CommitSlot);
        if (reinterpret_cast<std::uintptr_t>(slotTarget) != mainRange.m_Start + CommitThunk) {
            return result;
        }

        float* const locX = static_cast<float*>(dst);
        float* const locY = reinterpret_cast<float*>(static_cast<std::uint8_t*>(dst) + LocYInPoseOffset);
        float* const fov = reinterpret_cast<float*>(static_cast<std::uint8_t*>(dst) + FovInPoseOffset);
        const float* const pitch =
            reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(dst) + PitchInPoseOffset);
        const float* const yaw =
            reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(dst) + YawInPoseOffset);

        // This commit is the frame clock: read stock, compute from the current
        const float baseX = *locX;
        const float baseY = *locY;
        const float baseFov = *fov;
        const float basePitch = *pitch;
        const float baseYaw = *yaw;

        // PATCH-0261: the stock view's ground-plane axes.
        ViewAxes axes = SideOnAxes;
        bool plausibleYaw = true;
        bool level = true;
        if (kPatch0261ViewRelativeFraming) {
            float s = 0.0f;
            float c = 0.0f;
            plausibleYaw = SinCosDegrees(baseYaw, &s, &c);
            axes = {-s, c, c, s};
            level = AbsF(basePitch) <= MaxFramingPitchDegrees;
        }

        CoopPoseSample pose;
        GetCoopPose(&pose);
        const bool havePair =
            pose.stamp1 != 0 && pose.stamp2 != 0 &&
            (pose.stamp1 > pose.stamp2 ? pose.stamp1 - pose.stamp2
                                       : pose.stamp2 - pose.stamp1) <= MaxStampSkew;
        const bool plausibleFov = baseFov >= PlausibleFovMin && baseFov <= PlausibleFovMax;
        const bool cinematic = kPatch0266CinematicStandDown && P1InCinematic();
        if (cinematic != sStoodDown) {
            sStoodDown = cinematic;
            if (sStandDownLines < MaxStandDownLines) {
                sStandDownLines++;
                Logging.Log("[PShowCoop] PATCH-0266 %s base=(%.1f,%.1f) fov=%.2f yaw=%.1f applied=%u line=%u",
                            cinematic ? "stand-down cinematic" : "resume", baseX, baseY, baseFov, baseYaw, sApplied,
                            sStandDownLines);
            }
        }
        // PATCH-0269: a pair this far apart is in two scenes.
        bool sceneGap = false;
        if (kPatch0269SceneGapStandDown && havePair) {
            const float gx = pose.p2[0] - pose.p1[0];
            const float gy = pose.p2[1] - pose.p1[1];
            const float gz = pose.p2[2] - pose.p1[2];
            sceneGap = gx * gx + gy * gy + gz * gz > SceneGapUnits * SceneGapUnits;
        }
        if (sceneGap != sGapStoodDown) {
            sGapStoodDown = sceneGap;
            if (sGapLines < MaxStandDownLines) {
                sGapLines++;
                Logging.Log("[PShowCoop] PATCH-0269 %s p1=(%.0f,%.0f,%.0f) p2=(%.0f,%.0f,%.0f) applied=%u line=%u",
                            sceneGap ? "stand-down scene-gap" : "resume", pose.p1[0], pose.p1[1], pose.p1[2],
                            pose.p2[0], pose.p2[1], pose.p2[2], sApplied, sGapLines);
            }
        }
        if (!havePair || !plausibleFov || !plausibleYaw || cinematic || sceneGap) {
            // Leave the stock pose alone; drop state that described the old room.
            sSkipped++;
            sHaveStockDepth = false;
            sHaveP2Smooth = false;
            sHaveP1Smooth = false;
            if (kDiag0247CameraBursts && sBurstLen > 0) {
                DumpBurst();
                sBurstLen = 0;
                sNextBurstApplied = sApplied + BurstGapCommits;
            }
            sLastCommitTick = 0;
            return result;
        }

        // PATCH-0264: the time since the previous applied commit, in nominal commits.
        const std::uint64_t tick = svcGetSystemTick();  // 19.2 MHz
        const std::uint32_t intervalUs =
            sLastCommitTick != 0 ? static_cast<std::uint32_t>((tick - sLastCommitTick) * 10u / 192u) : 0u;
        sLastCommitTick = tick;
        float stepScale = 1.0f;
        if (kPatch0264CommitClock && intervalUs != 0) {
            stepScale = Clamp(static_cast<float>(intervalUs) / NominalCommitUs, MinStepScale, MaxStepScale);
        }
        const float positionAlpha =
            kPatch0264CommitClock ? 1.0f - ExpSmall(LogKeepPositionFilter * stepScale) : P2FilterAlpha;
        const float depthAlpha =
            kPatch0264CommitClock ? 1.0f - ExpSmall(LogKeepDepthFilter * stepScale) : DepthFilterAlpha;

        // Positions: the drain samples, or (PATCH-0264) both players read now.
        float p1[3] = {pose.p1[0], pose.p1[1], pose.p1[2]};
        float p2[3] = {pose.p2[0], pose.p2[1], pose.p2[2]};
        bool commitPositions = false;
        if (kPatch0264CommitClock) {
            float now1[3];
            float now2[3];
            if (ReadCoopPositionsNow(now1, now2)) {
                for (int i = 0; i < 3; ++i) {
                    p1[i] = now1[i];
                    p2[i] = now2[i];
                }
                commitPositions = true;
            }
        }

        if (!sHaveP2Smooth) {
            sP2SmoothX = p2[0];
            sP2SmoothY = p2[1];
            sHaveP2Smooth = true;
        } else {
            sP2SmoothX += (p2[0] - sP2SmoothX) * positionAlpha;
            sP2SmoothY += (p2[1] - sP2SmoothY) * positionAlpha;
        }
        // PATCH-0262: P1 through the same filter, for the FOV need only.
        if (!sHaveP1Smooth) {
            sP1SmoothX = p1[0];
            sP1SmoothY = p1[1];
            sHaveP1Smooth = true;
        } else {
            sP1SmoothX += (p1[0] - sP1SmoothX) * (kPatch0264CommitClock ? positionAlpha : FovNeedFilterAlpha);
            sP1SmoothY += (p1[1] - sP1SmoothY) * (kPatch0264CommitClock ? positionAlpha : FovNeedFilterAlpha);
        }

        // P2 across and along the stock view; P1's depth in front of it.
        const float p2Lateral = axes.Lateral(sP2SmoothX - baseX, sP2SmoothY - baseY);
        const float p2Depth = axes.Depth(sP2SmoothX - baseX, sP2SmoothY - baseY);
        const float recentre = level ? Clamp(p2Lateral * 0.5f, -MaxRecentreUnits, MaxRecentreUnits) : 0.0f;
        const float depthSample = axes.Depth(p1[0] - baseX, p1[1] - baseY);
        if (depthSample >= PlausibleDepthMin && depthSample <= PlausibleDepthMax) {
            const float drift = depthSample - sStockDepth;
            if (!sHaveStockDepth || drift > DepthReseedUnits || drift < -DepthReseedUnits) {
                sStockDepth = depthSample;
                sHaveStockDepth = true;
            } else {
                sStockDepth += drift * depthAlpha;
            }
        }

        // Back off until P2 is no nearer the camera than P1's filtered depth.
        const float pushBack =
            (level && sHaveStockDepth) ? Clamp(sStockDepth - p2Depth, 0.0f, MaxPushBackUnits) : 0.0f;
        sSmoothRecentre = Slew(sSmoothRecentre, recentre, SlewUnitsPerCall * 3.0f * stepScale);
        sSmoothPushBack = Slew(sSmoothPushBack, pushBack, SlewUnitsPerCall * 3.0f * stepScale);

        const float camX = baseX + axes.rightX * sSmoothRecentre - axes.forwardX * sSmoothPushBack;
        const float camY = baseY + axes.rightY * sSmoothRecentre - axes.forwardY * sSmoothPushBack;

        // FOV both players need from the corrected camera (PATCH-0262: smoothed positions).
        bool rawOk = false;
        const float neededRaw = FovNeed(axes, camX, camY, p1[0], p1[1], p2[0], p2[1], &rawOk);
        bool depthOk = rawOk;
        float needed = neededRaw;
        if (kPatch0262SmoothFovNeed) {
            needed = FovNeed(axes, camX, camY, sP1SmoothX, sP1SmoothY, sP2SmoothX, sP2SmoothY, &depthOk);
        }
        if (!level) {
            depthOk = false;
            needed = 0.0f;
        }

        float fovGain = 0.0f;
        if (depthOk && needed > baseFov) {
            fovGain = Clamp(needed - baseFov, 0.0f, MaxFovGainDegrees);
        }
        sSmoothFovGain = Slew(sSmoothFovGain, fovGain, SlewFovDegreesPerCall * 3.0f * stepScale);
        const float camFov = Clamp(baseFov + sSmoothFovGain, baseFov, MaxFovDegrees);

        *locX = camX;
        *locY = camY;
        *fov = camFov;
        sApplied++;

        if (kDiag0247CameraBursts) {
            BurstFrame frame = {};
            frame.intervalUs = intervalUs;
            frame.baseFov = baseFov;
            frame.outFov = camFov;
            frame.needRaw = neededRaw;
            frame.needUsed = needed;
            frame.yaw = baseYaw;
            frame.baseX = baseX;
            frame.baseY = baseY;
            frame.recentre = sSmoothRecentre;
            frame.pushBack = sSmoothPushBack;
            frame.p1x = p1[0];
            frame.p1y = p1[1];
            frame.p2x = p2[0];
            frame.p2y = p2[1];
            frame.stampStep1 = pose.stamp1 - sLastStamp1;
            frame.stampStep2 = pose.stamp2 - sLastStamp2;
            frame.source = commitPositions ? 1u : 0u;
            const bool p1Moved = AbsF(p1[0] - sLastRawP1X) + AbsF(p1[1] - sLastRawP1Y) > 1.0f;
            sLastStamp1 = pose.stamp1;
            sLastStamp2 = pose.stamp2;
            Diag47Commit(frame, p1, p1Moved);
        }

        if ((sApplied % 120u) == 0u && sLines < MaxLines) {
            sLines++;
            Logging.Log("[PShowCoop] PATCH-0017 same-commit owner=%p base=(%.1f,%.1f,%.2f) wrote=(%.1f,%.1f,%.2f) correction=(%.1f,%.1f,%.2f) p1=(%.1f,%.1f) p2=(%.1f,%.1f) applied=%u skipped=%u line=%u",
                        owner, baseX, baseY, baseFov, camX, camY, camFov,
                        sSmoothRecentre, sSmoothPushBack, sSmoothFovGain,
                        p1[0], p1[1], p2[0], p2[1], sApplied, sSkipped, sLines);
        }

        // PATCH-0261: which way the stock view looks, at each large change.
        if (kPatch0261ViewRelativeFraming && sViewLines < MaxViewLines) {
            const bool changed = !sHaveLoggedView || AbsF(baseYaw - sLoggedYaw) >= ViewLineYawDegrees ||
                                 AbsF(basePitch - sLoggedPitch) >= ViewLinePitchDegrees || level != sLoggedLevel;
            if (changed || (sApplied % SlowLineCommits) == 0u) {
                sViewLines++;
                sLoggedYaw = baseYaw;
                sLoggedPitch = basePitch;
                sLoggedLevel = level;
                sHaveLoggedView = true;
                Logging.Log("[PShowCoop] PATCH-0261 view %s yaw=%.1f pitch=%.1f level=%d base=(%.1f,%.1f) depth1=%.1f "
                            "p2_across=%.1f p2_depth=%.1f correction=(%.1f,%.1f,%.2f) p1=(%.1f,%.1f) p2=(%.1f,%.1f) "
                            "applied=%u line=%u",
                            changed ? "change" : "tick", baseYaw, basePitch, level ? 1 : 0, baseX, baseY,
                            depthSample, p2Lateral, p2Depth, sSmoothRecentre, sSmoothPushBack, sSmoothFovGain,
                            p1[0], p1[1], p2[0], p2[1], sApplied, sViewLines);
            }
        }
        return result;
    }
};

HOOK_DEFINE_TRAMPOLINE(FrameBothPlayers) {
    static Vec3 Callback(void* self) {
        const Vec3 v = Orig(self);
        if (self != nullptr) sActiveManagerToken = self;
        return v;
    }
};

} // namespace

void InstallCamera() {
    if (!kPatch0017TwoPlayerCamera) return;
    CommitCameraPose::InstallAtOffset(PoseDeepCopy);
    FrameBothPlayers::InstallAtOffset(GetCameraLocation);
    Logging.Log("[PShowCoop] PATCH-0017 installed accessor=0x%lx helper=0x%lx commit=0x%lx slot=+0x%lx margin=%.0f max_recentre=%.0f max_pushback=%.0f max_gain=%.0fdeg p2_alpha=%.2f same_commit=1",
                (unsigned long)GetCameraLocation, (unsigned long)PoseDeepCopy,
                (unsigned long)CommitThunk, (unsigned long)CommitSlot,
                EdgeMarginUnits, MaxRecentreUnits, MaxPushBackUnits,
                MaxFovGainDegrees, P2FilterAlpha);
    Logging.Log("[PShowCoop] PATCH-0261 view_relative=%d max_pitch=%.0f; PATCH-0262 smooth_fov_need=%d "
                "alpha=%.2f; PATCH-0264 commit_clock=%d nominal_us=%.0f; PATCH-0266 cinematic_stand_down=%d; "
                "PATCH-0269 scene_gap=%.0f; RELEASE-DIAGNOSTIC bursts=%d frames=%u wide=%u control=%u",
                kPatch0261ViewRelativeFraming ? 1 : 0, MaxFramingPitchDegrees, kPatch0262SmoothFovNeed ? 1 : 0,
                FovNeedFilterAlpha, kPatch0264CommitClock ? 1 : 0, NominalCommitUs,
                kPatch0266CinematicStandDown ? 1 : 0,
                kPatch0269SceneGapStandDown ? SceneGapUnits : 0.0f, kDiag0247CameraBursts ? 1 : 0, BurstFrames,
                MaxWideBursts, MaxControlBursts);
}

} // namespace pshowcoop::patches
