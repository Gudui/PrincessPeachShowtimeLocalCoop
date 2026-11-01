#pragma once
#include "lib.hpp"
#include "pointer_policy.hpp"
#include "program/loggers.hpp"
#include <atomic>

#ifndef PSHOW_PATCH0273_ENABLED
#define PSHOW_PATCH0273_ENABLED 1
#endif
#ifndef PSHOW_DIAG0255_ENABLED
#define PSHOW_DIAG0255_ENABLED 1
#endif

namespace pshowcoop::pointerguard {
inline Layout CurrentLayout() {
    const auto& alias = exl::util::mem_layout::s_Alias;
    const auto& heap = exl::util::mem_layout::s_Heap;
    const auto& main = exl::util::GetMainModuleInfo().m_Total;
    return {{alias.m_Start, alias.m_Size}, {heap.m_Start, heap.m_Size},
            {main.m_Start, main.m_Size}};
}
inline bool Readable(const void* p, std::size_t bytes = sizeof(void*)) {
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    if constexpr (!PSHOW_PATCH0273_ENABLED) return Legacy(a);
    return Accept(CurrentLayout(), a, bytes, [](std::uintptr_t address, Mapping& out) {
        MemoryInfo info{};
        std::uint32_t page{};
        if (R_FAILED(svcQueryMemory(&info, &page, address))) return false;
        out = {{info.addr, info.size}, (info.perm & Perm_R) != 0};
        return true;
    });
}
enum class Site : unsigned { Consumer, Outer, Pawn, Controller, LocalPlayer, Overlap };
// At most one accept and one reject per site per process. No mapping/pawn cache:
// QueryMemory is repeated so scene teardown cannot leave a stale permission hit.
inline bool Check(Site site, const void* p) {
    const bool accepted = Readable(p);
    if constexpr (PSHOW_DIAG0255_ENABLED) {
        static std::atomic<unsigned> seen{};
        const unsigned bit = 1U << (2 * static_cast<unsigned>(site) + accepted);
        if ((seen.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
            Logging.Log("[PShowCoop] RELEASE-DIAGNOSTIC guard site=%u ptr=%p old=%d mapped=%d",
                        static_cast<unsigned>(site), p, Legacy(reinterpret_cast<std::uintptr_t>(p)), accepted);
    }
    return accepted;
}
inline void Installed() {
    Logging.Log("[PShowCoop] PATCH-0273 mapped_pointer_guard=%d alias_heap_main=1 query_readable=1; RELEASE-DIAGNOSTIC enabled=%d sites=consumer,outer,pawn,controller,localplayer,overlap",
                PSHOW_PATCH0273_ENABLED, PSHOW_DIAG0255_ENABLED);
}
} // namespace pshowcoop::pointerguard
