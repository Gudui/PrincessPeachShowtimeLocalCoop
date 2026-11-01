#include "diag_riding.hpp"
#include "diag_riding_state.hpp"
#include "diag_controls_state.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace pshowcoop::riding {
namespace {
constexpr std::uintptr_t HorseSync = 0x1AD17B8; // 0x68-byte body, 4 relocated prologue instructions.
constexpr std::uintptr_t WagonSelector = 0x162219C; // 0x458-byte body; original x0/x1/w2 captured only.
constexpr std::uintptr_t WeakValid = 0x2A73028, WeakGet = 0x2A7312C;
constexpr std::uintptr_t PawnClass = 0x2514FB8, NameBlocks = 0xA6FC2A8;
constexpr std::size_t Class = 0x10, Name = 0x18, Root = 0x130, Location = 0x1D0;
constexpr std::size_t Parent = 0xC0, Owner = 0xA0, Movement = 0x288, Base = 0x298;
constexpr std::size_t CharOwner = 0x148, MovementMode = 0x168, Core = 0x7D0, Mask = 0x3E0;
constexpr std::size_t HorseWeak = 0x338;
static_assert(sizeof(exl::hook::nx64::FloatRegisters) == FloatSaveBytes);
static_assert(sizeof(exl::hook::InlineCtx) <= 0x100);
ResolvePlayer sPlayer{};
TracePolicy sPolicy[2]{};
std::uintptr_t sWorld{}; // Identity token only, never dereferenced later.
unsigned sUpdates{}, sHorseCalls{}, sHorseLines{}, sWagonCalls{}, sWagonLines{}, sEventLines{}, sHorseUntracked{};
struct HorseSlot { std::uintptr_t object{}; std::uint64_t weak{}; unsigned calls{}, lines{}; };
HorseSlot sHorses[16]{}; // Tokens only, never dereferenced on later calls.
std::atomic_flag sLock = ATOMIC_FLAG_INIT;
std::atomic<unsigned> sSkipped{};
struct Lock {
    bool held = !sLock.test_and_set(std::memory_order_acquire);
    Lock() { if (!held) ++sSkipped; }
    ~Lock() { if (held) sLock.clear(std::memory_order_release); }
};
std::uintptr_t Main() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
template<class Fn> Fn At(std::uintptr_t offset) { return reinterpret_cast<Fn>(Main() + offset); }
bool Span(const void* p, std::size_t n) {
    if (!p) return false;
    MemoryInfo info{};
    std::uint32_t page{};
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    return R_SUCCEEDED(svcQueryMemory(&info, &page, a)) && (info.perm & Perm_R) &&
        diagcontrols::Contains(info.addr, info.size, a, n);
}
template<class T> bool Read(const void* p, std::size_t off, T& value) {
    return CheckedRead(p, off, value, Span);
}
// Read-only FName decode. Every block/entry/character span is checked; this
// decoder never follows unvalidated name bytes from an old cached pawn.
void ObjectName(const void* object, char* out, std::size_t cap) {
    if (!object) { std::snprintf(out, cap, "<null>"); return; }
    std::snprintf(out, cap, "<unreadable>");
    std::uint32_t words[2]{};
    if (!Read(object, Name, words)) return;
    if (!words[0]) { std::snprintf(out, cap, "None"); return; }
    const unsigned block = words[0] >> 16;
    const std::uint8_t* data{};
    if (block >= 0x2000 || !Read(reinterpret_cast<const void*>(Main() + NameBlocks), block * 8, data)) return;
    const std::size_t offset = (words[0] & 0xffffU) * 2;
    std::uint16_t header{};
    if (!Read(data, offset, header)) return;
    const unsigned length = header >> 6, stride = (header & 1) ? 2 : 1;
    if (!length || length > 1023) return;
    const auto* chars = data + offset + 2;
    if (!Span(chars, static_cast<std::size_t>(length) * stride)) return;
    unsigned used = 0;
    for (; used < length && used + 12 < cap; ++used) {
        const auto c = chars[used * stride];
        out[used] = c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    out[used] = 0;
    if (words[1]) std::snprintf(out + used, cap - used, "_%u", words[1] - 1);
}
bool Princess(void* pawn, void* cls) {
    if (!pawn || !cls) return false;
    void* expected = At<void* (*)()>(PawnClass)();
    int depth{}, actualDepth{};
    const void* tree{};
    const void* candidate{};
    return Read(expected, 0x38, depth) && depth >= 0 && depth < 256 &&
        Read(cls, 0x38, actualDepth) && depth <= actualDepth && Read(cls, 0x30, tree) &&
        Read(tree, static_cast<std::size_t>(depth) * 8, candidate) &&
        candidate == static_cast<const std::uint8_t*>(expected) + 0x30;
}
struct Sample { State state{}; float xyz[3]{}; void* core{}; };
Sample Capture(void* pawn) {
    Sample out{};
    auto& s = out.state;
    s.pawn = reinterpret_cast<std::uintptr_t>(pawn);
    void* cls{}; void* root{}; void* parent{}; void* parentActor{};
    if (!Read(pawn, Class, cls)) return out;
    s.valid |= 1; s.cls = reinterpret_cast<std::uintptr_t>(cls);
    // sPlayer returns APawn; actor/root fields are valid for that native type.
    if (Read(pawn, Root, root)) {
        s.root = reinterpret_cast<std::uintptr_t>(root);
        if (Read(root, Location, out.xyz)) s.valid |= 2;
        if (Read(root, Parent, parent)) {
            s.valid |= 4; s.parent = reinterpret_cast<std::uintptr_t>(parent);
            if (!parent || Read(parent, Owner, parentActor)) {
                s.valid |= 8; s.parentActor = reinterpret_cast<std::uintptr_t>(parentActor);
            }
        }
    }
    // Character/Princess fields are never applied to another pawn subclass.
    if (!Princess(pawn, cls)) return out;
    s.valid |= 16;
    void* base{}; void* cmc{}; void* cmcOwner{};
    if (Read(pawn, Base, base)) { s.valid |= 32; s.base = reinterpret_cast<std::uintptr_t>(base); }
    if (Read(pawn, Movement, cmc) && Read(cmc, CharOwner, cmcOwner) && cmcOwner == pawn) {
        std::uint8_t mode{};
        if (Read(cmc, MovementMode, mode)) { s.valid |= 64; s.mode = mode; s.cmc = reinterpret_cast<std::uintptr_t>(cmc); }
    }
    if (Read(pawn, Core, out.core)) {
        s.core = reinterpret_cast<std::uintptr_t>(out.core);
        if (Read(out.core, Mask, s.mask)) s.valid |= 128;
    }
    return out;
}
void PlayerLine(const char* phase, unsigned which, void* pawn, bool force, bool heartbeat) {
    const auto sample = Capture(pawn);
    const auto& s = sample.state;
    if (!sPolicy[which].Take(s, force, heartbeat)) return;
    char name[48], cls[48], base[48], parent[48], parentActor[48];
    ObjectName(pawn, name, sizeof(name)); ObjectName(reinterpret_cast<const void*>(s.cls), cls, sizeof(cls));
    ObjectName(reinterpret_cast<const void*>(s.base), base, sizeof(base));
    ObjectName(reinterpret_cast<const void*>(s.parent), parent, sizeof(parent));
    ObjectName(reinterpret_cast<const void*>(s.parentActor), parentActor, sizeof(parentActor));
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC player phase=%s P%u pawn=%p name=%s class=%s valid=0x%x xyz=(%.1f,%.1f,%.1f) mask=0x%x mode=%u cmc=%p core=%p",
        phase, which + 1, pawn, name, cls, s.valid, sample.xyz[0], sample.xyz[1], sample.xyz[2], s.mask, s.mode, reinterpret_cast<void*>(s.cmc), sample.core);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC links phase=%s P%u root=%p base=%p:%s parent=%p:%s parentActor=%p:%s",
        phase, which + 1, reinterpret_cast<void*>(s.root), reinterpret_cast<void*>(s.base), base,
        reinterpret_cast<void*>(s.parent), parent, reinterpret_cast<void*>(s.parentActor), parentActor);
}
void Players(void* ctx, const char* phase, bool force, bool heartbeat) {
    if (!sPlayer || !Span(ctx, 0x28)) return;
    // Resolver invokes the native getter's original body, so helper replay
    // redirection cannot relabel index 0 as player 2 inside the observation.
    void* p1 = sPlayer(ctx, 0); void* p2 = sPlayer(ctx, 1);
    if (p1 && p1 == p2) {
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC invalid-player-pair phase=%s same=%p", phase, p1);
        return;
    }
    PlayerLine(phase, 0, p1, force, heartbeat); PlayerLine(phase, 1, p2, force, heartbeat);
}
void ActorLine(const char* phase, void* actor, void* target, unsigned selector) {
    char selfName[48], selfClass[48], otherName[48], otherClass[48];
    void* cls{}; void* otherCls{}; void* root{}; float xyz[3]{};
    Read(actor, Class, cls); Read(target, Class, otherCls);
    ObjectName(actor, selfName, sizeof(selfName)); ObjectName(cls, selfClass, sizeof(selfClass));
    ObjectName(target, otherName, sizeof(otherName)); ObjectName(otherCls, otherClass, sizeof(otherClass));
    const bool positionOk = Read(actor, Root, root) && Read(root, Location, xyz);
    void* p1 = sPlayer && Span(actor, 0x28) ? sPlayer(actor, 0) : nullptr;
    void* p2 = sPlayer && Span(actor, 0x28) ? sPlayer(actor, 1) : nullptr;
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC vehicle phase=%s self=%p:%s class=%s target=%p:%s targetClass=%s who=%s selector=%u root=%p pos_ok=%d xyz=(%.1f,%.1f,%.1f)",
        phase, actor, selfName, selfClass, target, otherName, otherClass,
        target && target == p1 ? "P1" : target && target == p2 ? "P2" : target ? "other" : "none",
        selector, root, positionOk, xyz[0], xyz[1], xyz[2]);
}
HOOK_DEFINE_INLINE(HorseSyncEntry) {
    static void Callback(exl::hook::InlineFloatCtx* frame) {
        Lock lock; if (!lock.held) return;
        ++sHorseCalls;
        auto* horse = reinterpret_cast<void*>(SavedX(frame, 0));
        std::uint64_t weak{};
        const bool readable = Read(horse, HorseWeak, weak);
        HorseSlot* slot = nullptr;
        const auto token = reinterpret_cast<std::uintptr_t>(horse);
        for (auto& entry : sHorses) {
            if (entry.object == token || !entry.object) { slot = &entry; break; }
        }
        if (!slot || !token) { ++sHorseUntracked; return; }
        const bool change = !slot->calls || weak != slot->weak;
        slot->object = token; slot->weak = weak; ++slot->calls;
        if (sHorseLines >= 512 || slot->lines >= 96 || (!change && slot->calls % 120 != 1)) return;
        ++slot->lines;
        ++sHorseLines;
        const bool valid = readable && At<bool (*)(const void*)>(WeakValid)(&weak);
        void* target = valid ? At<void* (*)(const void*)>(WeakGet)(&weak) : nullptr;
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC horse calls=%u sample=%u weak_read=%d weak_valid=%d token=0x%lx", sHorseCalls, sHorseLines, readable, valid, weak);
        ActorLine("horse-sync-entry", horse, target, 0);
    }
};
HOOK_DEFINE_INLINE(WagonSelectorEntry) {
    static void Callback(exl::hook::InlineFloatCtx* frame) {
        Lock lock; if (!lock.held) return;
        ++sWagonCalls;
        if (sWagonLines >= 128) return;
        ++sWagonLines;
        auto* wagon = reinterpret_cast<void*>(SavedX(frame, 0));
        auto* actor = reinterpret_cast<void*>(SavedX(frame, 1));
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC wagon-selector calls=%u sample=%u entry_only=1", sWagonCalls, sWagonLines);
        ActorLine("wagon-selector-entry", wagon, actor, static_cast<unsigned>(SavedX(frame, 2)));
        // Entry proves the actor was supplied, not that later native gates passed.
        Players(wagon, "wagon-entry", false, false);
    }
};
} // namespace

void Install(ResolvePlayer nativePlayer) {
    if constexpr (!PSHOW_DIAG0256_ENABLED) return;
    sPlayer = nativePlayer;
    HorseSyncEntry::InstallAtOffset(HorseSync);
    WagonSelectorEntry::InstallAtOffset(WagonSelector);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC armed readonly=1 horse=0x%lx wagon=0x%lx float_context=1 transitions=pre-post native_player_resolver=1", HorseSync, WagonSelector);
}
void ObserveUpdate(void* manager) {
    if constexpr (!PSHOW_DIAG0256_ENABLED) return;
    Lock lock; if (!lock.held) return;
    void* world{};
    if (!Read(manager, 0xA8, world)) return;
    if (sWorld != reinterpret_cast<std::uintptr_t>(world)) {
        sWorld = reinterpret_cast<std::uintptr_t>(world); sUpdates = 0;
        sPolicy[0] = {}; sPolicy[1] = {};
        for (auto& slot : sHorses) slot = {};
        sHorseLines = 0; sWagonLines = 0; sEventLines = 0;
    }
    ++sUpdates;
    Players(manager, "post-native", false, sUpdates % 60 == 1);
    if (sUpdates % 600 == 1)
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC health updates=%u horse_calls=%u wagon_calls=%u observer_busy=%u horse_untracked=%u player_lines=%u,%u,%u/%u,%u,%u",
            sUpdates, sHorseCalls, sWagonCalls, sSkipped.load(), sHorseUntracked,
            sPolicy[0].changes, sPolicy[0].periodic, sPolicy[0].forced,
            sPolicy[1].changes, sPolicy[1].periodic, sPolicy[1].forced);
}
void ObserveEvent(void* manager, bool afterP2, unsigned inputAllowed) {
    if constexpr (!PSHOW_DIAG0256_ENABLED) return;
    Lock lock; if (!lock.held) return;
    if (sEventLines >= 192) return;
    ++sEventLines;
    // An unchanged sample must still be emitted on each side of P2's native
    // edge/placement calls; the phase is evidence about ordering.
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC event phase=%s allowed=%u", afterP2 ? "after-p2" : "before-p2", inputAllowed);
    Players(manager, afterP2 ? "after-p2" : "before-p2", true, false);
    if (sEventLines == 192) Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC cap stream=event limit=192");
}
void ObserveControl(void* component, bool afterP2, bool begin) {
    if constexpr (!PSHOW_DIAG0256_ENABLED) return;
    Lock lock; if (!lock.held || sEventLines >= 192) return;
    ++sEventLines;
    const char* phase = begin ? (afterP2 ? "after-p2-begin" : "before-p2-begin")
                              : (afterP2 ? "after-p2-end" : "before-p2-end");
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC control phase=%s p1comp=%p", phase, component);
    Players(component, phase, true, false);
    if (sEventLines == 192) Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC cap stream=event limit=192");
}
} // namespace pshowcoop::riding
