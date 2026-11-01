#include "diag_controls.hpp"
#include "diag_controls_state.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>

// SDK-qualified imported symbols, verified against this title's sdk hash.
// Use the Result-returning detail readers: unlike public wrappers they report
// unavailable HID state without calling the SDK's fatal-error path. No HID
// initialize/configure/assignment calls are made by this diagnostic.
using Raw = pshowcoop::diagcontrols::RawSample;
extern "C" std::uint32_t DiagStyle(std::uint32_t*, const std::uint32_t&)
    asm("_ZN2nn3hid6detail15GetNpadStyleSetEPNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEERKj");
extern "C" std::uint32_t DiagFull(int*, Raw*, int, const std::uint32_t&)
    asm("_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_16NpadFullKeyStateEiRKj");
extern "C" std::uint32_t DiagHandheld(int*, Raw*, int, const std::uint32_t&)
    asm("_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_17NpadHandheldStateEiRKj");
extern "C" std::uint32_t DiagDual(int*, Raw*, int, const std::uint32_t&)
    asm("_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_16NpadJoyDualStateEiRKj");
extern "C" std::uint32_t DiagLeft(int*, Raw*, int, const std::uint32_t&)
    asm("_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_16NpadJoyLeftStateEiRKj");
extern "C" std::uint32_t DiagRight(int*, Raw*, int, const std::uint32_t&)
    asm("_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_17NpadJoyRightStateEiRKj");

namespace pshowcoop::diagcontrols {
namespace {
// no array index is assumed to mean Jump.
constexpr std::uintptr_t Actions = 0x38, ActionCount = 0x40, ActionStride = 0x48;
constexpr std::uintptr_t Keys = 0x8, KeyCount = 0x10, KeyStride = 0x18, State = 0x38;
constexpr std::uintptr_t PawnController = 0x258, ControllerBindings = 0x348;
constexpr std::uintptr_t PawnCore = 0x7D0, CoreMask = 0x3E0, Allowed = 0x30;
constexpr std::uintptr_t PawnInput = 0x228, PawnMovement = 0x288;
constexpr std::uintptr_t VelocityZ = 0xCC, MovementOwner = 0x148, Mode = 0x168;
constexpr std::uintptr_t Names = 0xA6FC2A8;
constexpr std::uintptr_t KeyQuery = 0x48058C4;
constexpr unsigned MaxActions = 64, MaxKeys = 8;
using QueryFn = bool (*)(void*, const void*);
using HidFn = std::uint32_t (*)(int*, Raw*, int, const std::uint32_t&);

struct ActionSample {
    std::uint32_t mode{}, state{};
    std::int32_t keys{};
    std::uint64_t names[MaxKeys]{};
    bool operator==(const ActionSample& o) const {
        return mode == o.mode && state == o.state && keys == o.keys &&
            std::memcmp(names, o.names, sizeof(names)) == 0;
    }
};
struct Player {
    const void* pawn{};  // Identity tokens only; never dereferenced later.
    const void* input{};
    const void* array{};
    const void* controller{};
    const void* bindings{};
    unsigned generation{}, calls{}, count{}, mask{}, gate{}, allowed{};
    unsigned rawEpoch{};
    int movement{-1};
    bool have[MaxActions]{};
    ActionSample last[MaxActions]{};
    bool queryHave[MaxActions][MaxKeys]{}, queryLast[MaxActions][MaxKeys]{};
    Budget actionLines{}, bindingLines{}, responseLines{}, healthLines{};
};
struct Pad {
    bool have{};
    std::uint32_t style{}, result{};
    RawHistory history[5]{};
    bool stateHave[5]{};
    std::uint32_t stateResult[5]{};
    int stateStatus[5]{};
    Budget lines{};
    unsigned fresh{}, gaps{};
    std::uint64_t missing{}, maxGap{};
};
static Player sPlayers[2]{};
static Pad sPads[9]{};
static std::atomic_flag sLock = ATOMIC_FLAG_INIT;
static std::atomic<unsigned> sSkipped{};
static bool sArmed{};
static unsigned sRawEpoch{};
static bool sButtonsHeld{};

static std::uintptr_t Main() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
static bool Readable(const void* p, std::size_t n) {
    if (!p) return false;
    MemoryInfo info{};
    std::uint32_t page{};
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    return R_SUCCEEDED(svcQueryMemory(&info, &page, a)) && (info.perm & Perm_R) &&
        Contains(info.addr, info.size, a, n);
}
template<class T> static T Read(const void* p, std::uintptr_t off) {
    T v;
    std::memcpy(&v, static_cast<const std::uint8_t*>(p) + off, sizeof(v));
    return v;
}
static bool Take(Budget& b, unsigned limit, const char* stream, int player) {
    bool marker;
    if (b.Take(limit, marker)) return true;
    if (marker) Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC cap stream=%s player=%d limit=%u", stream, player, limit);
    return false;
}
static void Name(std::uint64_t id, char (&out)[72]) {
    const auto index = static_cast<std::uint32_t>(id);
    if (index == 0) { std::snprintf(out, sizeof(out), "None#%u", static_cast<unsigned>(id >> 32)); return; }
    const unsigned block = index >> 16;
    const auto* blocks = reinterpret_cast<const std::uint8_t* const*>(Main() + Names);
    if (block >= 0x2000 || !Readable(blocks + block, sizeof(void*))) {
        std::snprintf(out, sizeof(out), "<name-pool-unavailable>"); return;
    }
    const auto* base = blocks[block];
    const auto entryAddress = reinterpret_cast<std::uintptr_t>(base) + (index & 0xffffU) * 2;
    const auto* entry = reinterpret_cast<const std::uint8_t*>(entryAddress);
    if (!base || !Readable(entry, 2)) { std::snprintf(out, sizeof(out), "<name-entry-unavailable>"); return; }
    const auto header = Read<std::uint16_t>(entry, 0);
    const unsigned len = header >> 6, wide = header & 1;
    if (!len || len > 1024 || !Readable(entry, 2 + len * (wide ? 2 : 1))) {
        std::snprintf(out, sizeof(out), "<name-length-unavailable>"); return;
    }
    unsigned n = 0;
    for (; n < len && n < sizeof(out) - 16; ++n) {
        const auto c = wide ? Read<std::uint16_t>(entry, 2 + n * 2) : entry[2 + n];
        out[n] = c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    std::snprintf(out + n, sizeof(out) - n, "%s#%u", n < len ? "..." : "", static_cast<unsigned>(id >> 32));
}

static void PollPads() {
    constexpr HidFn readers[] = {DiagFull, DiagHandheld, DiagDual, DiagLeft, DiagRight};
    constexpr const char* styles[] = {"full", "handheld", "dual", "left", "right"};
    sButtonsHeld = false;
    for (unsigned slot = 0; slot < 9; ++slot) {
        // SDK accepts 0..7 and handheld 0x20. Do not assume npad slot == player.
        const std::uint32_t id = slot == 8 ? 0x20 : slot;
        auto& p = sPads[slot];
        std::uint32_t mask = 0;
        const auto result = DiagStyle(&mask, id);
        const bool changed = !p.have || p.style != mask || p.result != result;
        if (changed) {
            if (Take(p.lines, 256, "raw", slot))
                Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC pad id=0x%x style=0x%x result=0x%x unsupported=0x%x", id, mask, result, mask & ~31U);
            for (auto& h : p.history) h = {};
            for (auto& h : p.stateHave) h = false;
            p.have = true; p.style = mask; p.result = result;
        }
        if (result != 0) continue;
        for (unsigned style = 0; style < 5; ++style) {
            if (!(mask & (1U << style))) continue;
            Raw samples[16]{};
            int count = 0;
            const auto rc = readers[style](&count, samples, 16, id);
            const int status = rc ? 2 : count == 0 ? 1 : count < 0 || count > 16 ? 3 : 0;
            const bool statusChanged = !p.stateHave[style] || p.stateResult[style] != rc || p.stateStatus[style] != status;
            p.stateHave[style] = true; p.stateResult[style] = rc; p.stateStatus[style] = status;
            if (rc || count < 0 || count > 16 || !count) {
                if (statusChanged && Take(p.lines, 256, "raw", slot))
                    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC raw-unavailable id=0x%x style=%s result=0x%x count=%d", id, styles[style], rc, count);
                continue;
            }
            // Stick-direction pseudo-buttons must not request a face-button
            // binding dump on every analog twitch.
            sButtonsHeld |= (samples[0].buttons & 0xfU) != 0;
            auto& history = p.history[style];
            history.Replay(samples, count, [&](const Raw& s, const RawEdge& e) {
                if (FaceEdge(e)) ++sRawEpoch;
                ++p.fresh;
                if (e.gap) {
                    ++p.gaps;
                    p.missing += e.gap;
                    if (e.gap > p.maxGap) p.maxGap = e.gap;
                    // Summarize losses without spending the raw edge budget on
                    // hundreds of otherwise identical neutral samples.
                    if (GapMilestone(p.gaps))
                        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC raw-gap id=0x%x events=%u missing=%lu max=%lu", id, p.gaps, p.missing, p.maxGap);
                }
                if (e.changed && Take(p.lines, 256, "raw", slot))
                    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC raw id=0x%x style=%s sample=%ld baseline=%d buttons=0x%lx down=0x%lx up=0x%lx stick=%d,%d,%d,%d attrs=0x%x gap=%lu",
                        id, styles[style], s.sample, e.baseline, s.buttons, e.down, e.up, s.lx, s.ly, s.rx, s.ry, s.attributes, e.gap);
            });
        }
    }
}

static void ObservePlayer(void* input, void* pawn, int who, std::uintptr_t caller) {
    auto& p = sPlayers[who];
    ++p.calls;
    if (!Readable(input, 0x58) || !Readable(pawn, PawnCore + sizeof(void*))) {
        if (Take(p.healthLines, 32, "health", who))
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC unavailable P%d input=%p pawn=%p", who + 1, input, pawn);
        return;
    }
    void* controller = Read<void*>(pawn, PawnController);
    void* core = Read<void*>(pawn, PawnCore);
    void* array = Read<void*>(input, Actions);
    const int count = Read<std::int32_t>(input, ActionCount);
    const bool coreOk = Readable(core, CoreMask + sizeof(std::uint32_t));
    const bool ctrlOk = Readable(controller, ControllerBindings + sizeof(void*));
    void* bindings = ctrlOk ? Read<void*>(controller, ControllerBindings) : nullptr;
    const unsigned mask = coreOk ? Read<std::uint32_t>(core, CoreMask) : 0;
    const unsigned gate = (Read<std::uint8_t>(pawn, PawnInput) & 0x10) != 0;
    const unsigned allowed = Read<std::uint8_t>(input, Allowed);
    const bool identity = p.pawn != pawn || p.input != input || p.array != array ||
        p.controller != controller || p.bindings != bindings || p.count != static_cast<unsigned>(count);
    if (identity) {
        ++p.generation;
        for (auto& h : p.have) h = false;
        std::memset(p.queryHave, 0, sizeof(p.queryHave));
        p.movement = -1;
    }
    if (identity || mask != p.mask || gate != p.gate || allowed != p.allowed || p.calls % 300 == 0) {
        if (Take(p.healthLines, 128, "health", who))
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC route P%d gen=%u frame=%u caller=0x%lx input=%p pawn=%p controller=%p bindings=%p array=%p count=%d gate=%u allowed=%u mask=0x%x core_ok=%d ctrl_ok=%d skipped=%u",
                who + 1, p.generation, p.calls, caller, input, pawn, controller, bindings, array, count, gate, allowed, mask, coreOk, ctrlOk, sSkipped.load(std::memory_order_relaxed));
    }
    p.pawn = pawn; p.input = input; p.array = array; p.controller = controller; p.bindings = bindings;
    p.count = static_cast<unsigned>(count); p.mask = mask; p.gate = gate; p.allowed = allowed;
    if (count < 0 || count > static_cast<int>(MaxActions) ||
        (count && !Readable(array, static_cast<std::size_t>(count) * ActionStride))) {
        if (Take(p.healthLines, 128, "health", who))
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC array-unavailable P%d count=%d max=%u", who + 1, count, MaxActions);
        return;
    }
    bool edge = false;
    std::uint64_t queriedSlots = 0, heldSlots = 0;
    const bool rawEdge = p.rawEpoch != sRawEpoch;
    p.rawEpoch = sRawEpoch;
    for (int i = 0; i < count; ++i) {
        const auto* a = static_cast<const std::uint8_t*>(array) + i * ActionStride;
        ActionSample now;
        now.mode = Read<std::uint32_t>(a, 0);
        now.state = Read<std::uint32_t>(a, State);
        now.keys = Read<std::int32_t>(a, KeyCount);
        void* keys = Read<void*>(a, Keys);
        const bool keysOk = now.keys >= 0 && now.keys <= 64 &&
            (!now.keys || Readable(keys, static_cast<std::size_t>(now.keys) * KeyStride));
        const unsigned shown = keysOk ? (static_cast<unsigned>(now.keys) < MaxKeys ? now.keys : MaxKeys) : 0;
        for (unsigned k = 0; k < shown; ++k) now.names[k] = Read<std::uint64_t>(keys, k * KeyStride);
        const bool changed = !p.have[i] || !(p.last[i] == now);
        const bool mappingChanged = !p.have[i] || p.last[i].mode != now.mode || p.last[i].keys != now.keys ||
            std::memcmp(p.last[i].names, now.names, sizeof(now.names)) != 0;
        const auto bytes = now.state;
        edge |= (bytes & 0x00ffff00U) != 0;
        if (changed && Take(p.actionLines, 768, "action", who))
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC action P%d gen=%u frame=%u slot=%d mode=%u cur=%u pressed=%u released=%u prev=%u keys=%d keys_ok=%d keys_shown=%u",
                who + 1, p.generation, p.calls, i, now.mode, bytes & 255, (bytes >> 8) & 255, (bytes >> 16) & 255, bytes >> 24, now.keys, keysOk, shown);
        if ((changed || rawEdge || sButtonsHeld || p.calls % 300 == 0) && ctrlOk && keysOk) {
            for (unsigned k = 0; k < shown; ++k) {
                // Explicitly a post-dispatch native re-query (pure held-state
                const bool held = reinterpret_cast<QueryFn>(Main() + KeyQuery)(controller,
                    static_cast<const std::uint8_t*>(keys) + k * KeyStride);
                queriedSlots |= std::uint64_t{1} << i;
                if (held) heldSlots |= std::uint64_t{1} << i;
                if (BindingChanged(mappingChanged, p.queryHave[i][k], p.queryLast[i][k], held) &&
                    Take(p.bindingLines, 1024, "binding", who)) {
                    char name[72]; Name(now.names[k], name);
                    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC binding P%d gen=%u frame=%u slot=%d mode=%u key=%s fname=0x%lx post_query=%d raw_epoch=%u", who + 1, p.generation, p.calls, i, now.mode, name, now.names[k], held, sRawEpoch);
                }
                p.queryHave[i][k] = true; p.queryLast[i][k] = held;
            }
        }
        p.last[i] = now; p.have[i] = true;
    }
    // A failed/missing action edge still gets a query result at each face
    // press/release. One compact line replaces the old all-key dump, which
    // exhausted both players' budgets before the stage transition.
    if ((identity || rawEdge || p.calls % 300 == 0) && Take(p.healthLines, 128, "health", who))
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC key-snapshot P%d gen=%u frame=%u raw_epoch=%u queried_slots=0x%lx held_slots=0x%lx ctrl_ok=%d", who + 1, p.generation, p.calls, sRawEpoch, queriedSlots, heldSlots, ctrlOk);
    void* cmc = Read<void*>(pawn, PawnMovement);
    if (Readable(cmc, Mode + 2) && Read<void*>(cmc, MovementOwner) == pawn) {
        const int mode = Read<std::uint8_t>(cmc, Mode);
        if (mode != p.movement || edge || p.calls % 30 == 0) {
            if (Take(p.responseLines, 768, "response", who))
                Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC response P%d gen=%u frame=%u cmc=%p mode=%d custom=%u vz=%.3f action_edge=%d", who + 1, p.generation, p.calls, cmc, mode, Read<std::uint8_t>(cmc, Mode + 1), Read<float>(cmc, VelocityZ), edge);
        }
        p.movement = mode;
    } else if (Take(p.healthLines, 128, "health", who)) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC movement-unavailable P%d cmc=%p", who + 1, cmc);
    }
}
}

void Observe(void* actionInput, void* pawn, int player, std::uintptr_t caller) {
    if (!Enabled || player < 0 || player > 1) return;
    if (sLock.test_and_set(std::memory_order_acquire)) {
        sSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!sArmed) {
        sArmed = true;
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC armed read_only=1 raw_ids=0..7,0x20 styles=full,handheld,dual,left,right action_mode_names=unassigned binding=post_dispatch_query caps=per_player raw_history=16");
    }
    // Poll on either stream: loss of one player's dispatcher must not suppress
    // raw evidence for that controller. SDK sample identities remove overlap.
    PollPads();
    ObservePlayer(actionInput, pawn, player, caller);
    sLock.clear(std::memory_order_release);
}
}
