#include "patch_warp.hpp"
#include "patch_hud.hpp"
#include "lib.hpp"
#include "lib/patch/patcher_impl.hpp"
#include "program/loggers.hpp"
#include "program/pshow_config.hpp"
#include <cstddef>
#include <cstdint>

namespace pshowcoop::patches {

namespace warp_impl {
constexpr ptrdiff_t TravelProducer = 0x0099A704;
constexpr ptrdiff_t FNameFromAnsi = 0x02882898;
// Title level cache "replace" (FUN_7101f49a48): (cacheManager, const FName*)
// clears every cache entry, then caches the map and its sublevels as
// "<map>_Cached" packages. Sole caller: the title travel coordinator at
// 0x999E7C, which caches obj+0x28 right before travelling to the same obj+0x28
// through the producer (0x99A2BC). Prologue stp x29,x30 / stp x20,x19 / mov
// x29,sp at 0x01F49A48 (PATCH-0241).
constexpr ptrdiff_t LevelCacheReplace = 0x01F49A48;
using FNameCtorFn = void (*)(void* out, const char* name, int findType);

// The map the startup warp lands in when settings.ini has no usable
// warp.target (and with PATCH-0244 off). The cache request and the travel both
// use the resolved name.
constexpr const char* kWarpTargetMap = "Fencer_A";
// PATCH-0244 kill switch. true = read warp.target from settings.ini at the
// first warp use (the first level-cache request, before any pawn). The rom
// mount is established by then: the Title map that issues the request was
constexpr bool kPatch0244IniWarpTarget = true;
static char sTargetMapName[64] = "Fencer_A";

static std::uint32_t sCalls = 0;
static std::uint32_t sCacheCalls = 0;
static std::uint64_t sFirstCacheFName = 0;
// PATCH-0032 kill switch. false = the first travel call keeps its native
// destination and the operator enters the course through the menus.
constexpr bool kPatch0032StartupWarp = false;
// PATCH-0241 kill switch. true = the warp also redirects the title's level
// cache request, so the target map is cached the way the menu route caches the
// course. Without it the cache keeps Title and the course is loaded uncached:
// a death restart then re-creates its textures while the old ones are still
constexpr bool kPatch0241WarpLevelCache = true;
static std::uint64_t sTargetFName = 0;

std::uint64_t TargetFName() {
    if (sTargetFName == 0) {
        if (kPatch0244IniWarpTarget) {
            pshowcoop::config::Reload();
            const char* name = pshowcoop::config::Get().warpTarget;
            std::size_t i = 0;
            for (; name[i] != '\0' && i + 1 < sizeof(sTargetMapName); ++i) {
                sTargetMapName[i] = name[i];
            }
            sTargetMapName[i] = '\0';
            Logging.Log("[PShowCoop] PATCH-0244 warp target from settings.ini: %s", sTargetMapName);
        }
        const auto& mainRange = exl::util::GetMainModuleInfo().m_Total;
        auto fnameCtor = reinterpret_cast<FNameCtorFn>(mainRange.m_Start + FNameFromAnsi);
        // findType = 2 (FNAME_Add) ensures the name is created even before map load
        fnameCtor(&sTargetFName, sTargetMapName, 2);
        if (sTargetFName == 0) {
            sTargetFName = 60801ULL;
        }
    }
    return sTargetFName;
}

HOOK_DEFINE_TRAMPOLINE(RedirectLevelCache) {
    static void Callback(void* cacheManager, std::uint64_t* fname) {
        sCacheCalls++;
        if (sCacheCalls == 1 && fname != nullptr) {
            sFirstCacheFName = *fname;
        }
        if (sCacheCalls == 1 && kPatch0032StartupWarp && kPatch0241WarpLevelCache && fname != nullptr) {
            std::uint64_t target = TargetFName();
            Logging.Log("[PShowCoop] PATCH-0241 cache call=1 original=0x%llx target=%s fname=0x%llx",
                        static_cast<unsigned long long>(*fname), sTargetMapName,
                        static_cast<unsigned long long>(target));
            Orig(cacheManager, &target);
            return;
        }
        Orig(cacheManager, fname);
    }
};

HOOK_DEFINE_TRAMPOLINE(CaptureTravelProducer) {
    static std::uint32_t Callback(void* context, void* fname_ptr, void* options_ptr) {
        sCalls++;
        if (sCalls == 1 && !kPatch0032StartupWarp) {
            Logging.Log("[PShowCoop] PATCH-0032 disabled: travel call=1 keeps its native target");
            ResetHudForLevelTransition();
            return Orig(context, fname_ptr, options_ptr);
        }
        if (sCalls == 1) {
            std::uint64_t original = fname_ptr != nullptr ? *reinterpret_cast<std::uint64_t*>(fname_ptr) : 0;
            TargetFName();
            // The coordinator caches and travels to the same FName (obj+0x28), so
            // the first cache request must have carried this call's original.
            Logging.Log("[PShowCoop] PATCH-0032 travel call=1 applied=1 target=%s fname=0x%llx original=0x%llx cache_calls=%u cache_pair=%d rollback=relaunch",
                        sTargetMapName, static_cast<unsigned long long>(sTargetFName),
                        static_cast<unsigned long long>(original), sCacheCalls,
                        (sCacheCalls >= 1 && sFirstCacheFName == original) ? 1 : 0);
            ResetHudForLevelTransition();
            return Orig(context, &sTargetFName, options_ptr);
        }
        return Orig(context, fname_ptr, options_ptr);
    }
};
} // namespace warp_impl

void InstallWarp() {
    warp_impl::CaptureTravelProducer::InstallAtOffset(warp_impl::TravelProducer);
    warp_impl::RedirectLevelCache::InstallAtOffset(warp_impl::LevelCacheReplace);
    Logging.Log("[PShowCoop] PATCH-0032 armed travel_producer=0x%lx default_target=%s ini_target=%d warp=%d; PATCH-0241 level_cache=0x%lx enabled=%d",
                (unsigned long)warp_impl::TravelProducer, warp_impl::kWarpTargetMap,
                warp_impl::kPatch0244IniWarpTarget ? 1 : 0,
                warp_impl::kPatch0032StartupWarp ? 1 : 0,
                (unsigned long)warp_impl::LevelCacheReplace, warp_impl::kPatch0241WarpLevelCache ? 1 : 0);
}

} // namespace pshowcoop::patches
