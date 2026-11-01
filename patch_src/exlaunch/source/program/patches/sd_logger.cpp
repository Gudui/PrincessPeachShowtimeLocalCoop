#include "sd_log.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <nn/fs.hpp>
#include <switch/arm/counter.h>
// Symbol imports, not NSO-offset calls or hooks. Mount a name owned by this mod.
namespace nn::fs {
Result MountSdCard(const char* mount);
Result DeleteFile(const char* path);
Result RenameFile(const char* from, const char* to);
}

namespace pshowcoop::sdlog {
namespace {
#if PSHOW_SD_LOG_ENABLED
constinit std::atomic<bool> sFsReady{false};
constexpr char kMount[] = "pshowlog";
constexpr char kDirectory[] = "pshowlog:/PShowCoop";
constexpr char kPath[] = "pshowlog:/PShowCoop/showtime_log.txt";
constexpr char kPrevious[] = "pshowlog:/PShowCoop/showtime_log_prev.txt";

struct HorizonIo {
    nn::fs::FileHandle file{};
    std::uint64_t Tick() { return svcGetSystemTick(); }
    std::uint64_t TickHz() { return armGetSystemTickFreq(); }
    bool Ready() { return sFsReady.load(std::memory_order_acquire); }
    void Yield() { svcSleepThread(50000); }
    bool Open() {
        // Permission is explicit SD access, not a dependency on debug mode.
        if (nn::fs::MountSdCard(kMount) != 0) return false;
        nn::fs::CreateDirectory(kDirectory);
        nn::fs::DeleteFile(kPrevious);
        nn::fs::RenameFile(kPath, kPrevious);
        if (nn::fs::CreateFile(kPath, 0) != 0) return false;
        return nn::fs::OpenFile(&file, kPath, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append) == 0;
    }
    bool Write(std::int64_t offset, const char* data, std::size_t size) {
        return nn::fs::WriteFile(file, offset, data, size,
            nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush)) == 0;
    }
};
constinit HorizonIo sIo;
constinit Core<HorizonIo, PSHOW_SD_LOG_STARTUP_ONLY != 0> sCore;
#endif

#if PSHOW_RELEASE_DIAGNOSTIC_ENABLED
// Bounded observation counts only. They do not gate or simulate game behavior.
struct GuardCount { const char* site{}; unsigned pass{}, fail{}; };
GuardCount sGuards[8]{};
unsigned sGuardSites = 0, sOverlapLines = 0;
std::uint64_t sDrainCalls = 0;
std::atomic_flag sTraceBusy = ATOMIC_FLAG_INIT;
struct TraceScope {
    bool locked = !sTraceBusy.test_and_set(std::memory_order_acquire);
    ~TraceScope() { if (locked) sTraceBusy.clear(std::memory_order_release); }
    explicit operator bool() const { return locked; }
};

void Mapping(const char* site, const void* pointer, bool accepted) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    MemoryInfo info{};
    u32 page = 0;
    const Result result = svcQueryMemory(&info, &page, address);
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC guard site=%s ptr=%p old_accept=%d query=0x%x map=0x%lx size=0x%lx type=0x%x perm=0x%x",
        site, pointer, (int)accepted, result, result == 0 ? info.addr : 0,
        result == 0 ? info.size : 0, result == 0 ? info.type : 0, result == 0 ? info.perm : 0);
}
#endif
} // namespace

void SdLogger::LogRaw(std::string_view string) {
#if PSHOW_SD_LOG_ENABLED
    sCore.Log(sIo, string);
#else
    (void)string;
#endif
}

void AfterInputDrain() {
#if PSHOW_SD_LOG_ENABLED
    // Independent of IsHeapish: a rejected native consumer must still reach I/O.
    // Wait for a completed native input drain and the 5s startup buffer delay.
    sFsReady.store(true, std::memory_order_release);
#endif
#if PSHOW_RELEASE_DIAGNOSTIC_ENABLED
    {
        TraceScope scope;
        if (scope) {
            ++sDrainCalls;
            if (sDrainCalls == 1) {
                const auto& heap = exl::util::mem_layout::s_Heap;
                const auto& main = exl::util::GetMainModuleInfo().m_Total;
                Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC armed read_only=1 old_guards_unchanged=1 heap=0x%lx..0x%lx main=0x%lx..0x%lx sd_log=%d path=/PShowCoop/showtime_log.txt",
                    heap.m_Start, heap.GetEnd(), main.m_Start, main.GetEnd(), PSHOW_SD_LOG_ENABLED);
            }
            if (sDrainCalls == 120 || sDrainCalls == 600 || sDrainCalls == 1800 || sDrainCalls == 3600) {
                Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC heartbeat drain_calls=%lu", sDrainCalls);
                for (unsigned i = 0; i < sGuardSites; ++i) {
                    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC summary site=%s pass=%u fail=%u", sGuards[i].site, sGuards[i].pass, sGuards[i].fail);
                }
            }
        }
    }
#endif
#if PSHOW_SD_LOG_ENABLED
    sCore.Pump(sIo);
#endif
}

void Guard(const char* site, const void* pointer, bool accepted) {
#if PSHOW_RELEASE_DIAGNOSTIC_ENABLED
    TraceScope scope;
    if (!scope) return;
    unsigned i = 0;
    for (; i < sGuardSites; ++i) if (std::strcmp(sGuards[i].site, site) == 0) break;
    if (i == sGuardSites) {
        if (sGuardSites == 8) return;
        sGuards[sGuardSites++].site = site;
    }
    auto& count = sGuards[i];
    unsigned& value = accepted ? count.pass : count.fail;
    ++value;
    if (value <= (accepted ? 2U : 6U)) Mapping(site, pointer, accepted);
#else
    (void)site; (void)pointer; (void)accepted;
#endif
}

void Overlap(const char* decision, const void* collection, const void* key, int index, int count) {
#if PSHOW_RELEASE_DIAGNOSTIC_ENABLED
    TraceScope scope;
    if (!scope) return;
    // One budget per decision so initial no-match traffic cannot consume proof
    // of later duplicate skips or a distinct guard refusal.
    static const char* reasons[8]{};
    static unsigned counts[8]{}, used = 0;
    unsigned i = 0;
    for (; i < used; ++i) if (std::strcmp(reasons[i], decision) == 0) break;
    if (i == used) { if (used == 8) return; reasons[used++] = decision; }
    if (++counts[i] <= 8) {
        ++sOverlapLines;
        Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC overlap decision=%s self=%p key=%p index=%d count=%d line=%u",
            decision, collection, key, index, count, sOverlapLines);
    }
#else
    (void)decision; (void)collection; (void)key; (void)index; (void)count;
#endif
}

void Player(int label, const void* pawn, const void* controller, const void* localPlayer, unsigned gate) {
#if PSHOW_RELEASE_DIAGNOSTIC_ENABLED
    TraceScope scope;
    if (!scope) return;
    static const void* last[2]{};
    if (label < 0 || label > 1 || last[label] == pawn) return;
    last[label] = pawn; // Log identity token only; never dereference or reuse it.
    Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC player label=%d pawn=%p controller=%p local_player=%p input_gate=0x%x",
        label, pawn, controller, localPlayer, gate);
#else
    (void)label; (void)pawn; (void)controller; (void)localPlayer; (void)gate;
#endif
}
} // namespace pshowcoop::sdlog
