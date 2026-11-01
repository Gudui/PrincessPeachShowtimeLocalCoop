#include "lib.hpp"
#include "nn/fs/fs_files.hpp"

#include "program/pshow_config.hpp"
#include "program/loggers.hpp"

#include <cstddef>

// KILL SWITCH. Set this to 0 to make the module do no filesystem work at all
// and run on the constinit defaults below.
//
// Reading through an UNREGISTERED mount does not return an error: nn::fs
// ABORTS inside nnSdk, so the `if (result != 0)` fallbacks below never get a
// fs result 2002-6906) using "content:", which is the Odyssey co-op mod's
// mount name. A mount name is a per-title finding and never transfers.
#define PSHOW_CONFIG_FILE_READ_ENABLED 1

namespace pshowcoop::config {
namespace {
// call is at NSO 0x26B5B74, and the mount name it passes in x0 is materialized
// at 0x26B5B6C as adrp 0x8805000 + 0x291 -> the C string "rom" at 0x8805291.
// LayeredFS merges the mod's romfs into that same mount, which is what makes
// our own file visible under it.
//
// exists in the image: the mount name is the bare string "rom" and paths are
// composed from it, so there was never a "rom:/" literal to find.
constexpr const char* kConfigPath = "rom:/PShowCoop/settings.ini";
// This was 2048 with the buffer on the stack, kept small because Reload runs on
// the game thread and EXL_USE_FAKEHEAP leaves little room for a large frame.
// That silently broke the leash settings: documenting them grew the shipped ini
// to ~4 KiB, which put all three keys past the cap, so they were truncated away
// and kept their defaults while CONFIG-0001 cheerfully reported "loaded". A
// read cap that drops trailing keys without saying so is a trap, so the buffer
// is now static (no stack cost at all, which is what forced the small cap) and
// truncation is logged. Do not move this back onto the stack to shrink it.
constexpr size_t kConfigMaxBytes = 8192;

// constexpr so sSettings below is constant-initialized into .data. The first
// file read is now lazy (see exl_main), which means these defaults are what
// PATCH-0016 uses until gameplay starts - they must be valid without any
// runtime initializer having run, including init_array ordering.
constexpr Settings Defaults() {
    return Settings{
        true,
        0.0f, 1.0f, 1.0f,
        8.0f,
        "EmissiveColor",
        "emissive_Intensity",
        1800,
        // Both leash defaults are PATCH-0015's original compile-time 800.0f, so
        // a missing or unreadable ini reproduces the previously shipped
        // behaviour exactly rather than silently unleashing P2.
        800.0f,
        800.0f,
        120,
        "Fencer_A",
    };
}

// constinit is the assertion, not decoration: if this ever stops being
// constant-initialized, PATCH-0016 would read an all-zero Settings before
// init_array runs - a disabled tint with empty parameter names. Fail the build
// instead.
constinit Settings sSettings = Defaults();
unsigned sLastReloadCall = 0;
bool sEverLoaded = false;

float Clamp(float value, float minValue, float maxValue) {
    if (value < minValue)
        return minValue;
    if (value > maxValue)
        return maxValue;
    return value;
}

const char* SkipSpaces(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
        ++p;
    return p;
}

const char* TrimSpacesEnd(const char* begin, const char* end) {
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        --end;
    return end;
}

bool KeyEquals(const char* begin, const char* end, const char* key) {
    while (begin < end && *key) {
        if (*begin != *key)
            return false;
        ++begin;
        ++key;
    }
    return begin == end && *key == '\0';
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

bool ParseFloat(const char* begin, const char* end, float* outValue) {
    const char* p = SkipSpaces(begin, end);
    bool negative = false;
    if (p < end && (*p == '-' || *p == '+')) {
        negative = *p == '-';
        ++p;
    }

    bool sawDigit = false;
    float value = 0.0f;
    while (p < end && IsDigit(*p)) {
        sawDigit = true;
        value = value * 10.0f + static_cast<float>(*p - '0');
        ++p;
    }

    if (p < end && *p == '.') {
        ++p;
        float scale = 0.1f;
        while (p < end && IsDigit(*p)) {
            sawDigit = true;
            value += static_cast<float>(*p - '0') * scale;
            scale *= 0.1f;
            ++p;
        }
    }

    if (!sawDigit)
        return false;
    *outValue = negative ? -value : value;
    return true;
}

void CopyString(char* destination, size_t capacity,
                const char* begin, const char* end) {
    begin = SkipSpaces(begin, end);
    end = TrimSpacesEnd(begin, end);
    size_t length = static_cast<size_t>(end - begin);
    if (length >= capacity)
        length = capacity - 1;
    for (size_t i = 0; i < length; ++i)
        destination[i] = begin[i];
    destination[length] = '\0';
}

void AssignLiteral(char* destination, size_t capacity, const char* source) {
    size_t i = 0;
    while (source[i] != '\0' && i + 1 < capacity) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

// maybe_unused so this still compiles when PSHOW_CONFIG_FILE_READ_ENABLED is
// 0, which leaves it unreferenced. The helpers above stay referenced through
// this function.
[[maybe_unused]] void ParseConfigText(const char* text, size_t length,
                                      Settings* settings) {
    const char* p = text;
    const char* end = text + length;
    while (p < end) {
        const char* lineBegin = p;
        while (p < end && *p != '\n')
            ++p;
        const char* lineEnd = p;
        if (p < end)
            ++p;

        const char* cur = SkipSpaces(lineBegin, lineEnd);
        if (cur >= lineEnd || *cur == '#' || *cur == ';')
            continue;

        const char* equals = cur;
        while (equals < lineEnd && *equals != '=')
            ++equals;
        if (equals >= lineEnd)
            continue;

        const char* keyEnd = TrimSpacesEnd(cur, equals);
        const char* valueBegin = equals + 1;
        float value = 0.0f;

        if (KeyEquals(cur, keyEnd, "p2.tint.color_param")) {
            CopyString(settings->p2TintColorParam,
                       sizeof(settings->p2TintColorParam), valueBegin, lineEnd);
        } else if (KeyEquals(cur, keyEnd, "warp.target")) {
            CopyString(settings->warpTarget, sizeof(settings->warpTarget),
                       valueBegin, lineEnd);
        } else if (KeyEquals(cur, keyEnd, "p2.tint.intensity_param")) {
            CopyString(settings->p2TintIntensityParam,
                       sizeof(settings->p2TintIntensityParam), valueBegin, lineEnd);
        } else if (!ParseFloat(valueBegin, lineEnd, &value)) {
            continue;
        } else if (KeyEquals(cur, keyEnd, "p2.tint.enabled")) {
            settings->p2TintEnabled = value != 0.0f;
        } else if (KeyEquals(cur, keyEnd, "p2.tint.r")) {
            settings->p2TintR = value;
        } else if (KeyEquals(cur, keyEnd, "p2.tint.g")) {
            settings->p2TintG = value;
        } else if (KeyEquals(cur, keyEnd, "p2.tint.b")) {
            settings->p2TintB = value;
        } else if (KeyEquals(cur, keyEnd, "p2.tint.intensity")) {
            settings->p2TintIntensity = value;
        } else if (KeyEquals(cur, keyEnd, "p2.tint.backstop_calls")) {
            settings->p2TintBackstopCalls = static_cast<unsigned>(value);
        } else if (KeyEquals(cur, keyEnd, "p2.leash.spawn_distance")) {
            settings->p2LeashSpawnDistance = value;
        } else if (KeyEquals(cur, keyEnd, "p2.leash.distance")) {
            settings->p2LeashDistance = value;
        } else if (KeyEquals(cur, keyEnd, "p2.leash.cooldown_calls")) {
            settings->p2LeashCooldownCalls = static_cast<unsigned>(value);
        }
    }
}

// Clamped rather than rejected, so a typo degrades to a legal value instead of
// silently disabling the feature. The colour ceiling is above 1.0 on purpose:
// an emissive parameter is not limited to the 0-1 display range and
// oversaturating a channel is a legitimate way to make P2 louder.
void Apply(Settings settings) {
    settings.p2TintR = Clamp(settings.p2TintR, 0.0f, 20.0f);
    settings.p2TintG = Clamp(settings.p2TintG, 0.0f, 20.0f);
    settings.p2TintB = Clamp(settings.p2TintB, 0.0f, 20.0f);
    settings.p2TintIntensity = Clamp(settings.p2TintIntensity, 0.0f, 100.0f);
    if (settings.p2TintBackstopCalls != 0 && settings.p2TintBackstopCalls < 60)
        settings.p2TintBackstopCalls = 60;
    // Negative folds to 0, which is the documented "disabled" value, so a typo
    // cannot produce a threshold that is always exceeded. The ceiling is far
    // above it is already equivalent to disabled; it exists only to keep the
    // squared comparison well inside float range.
    settings.p2LeashSpawnDistance = Clamp(settings.p2LeashSpawnDistance, 0.0f, 100000.0f);
    settings.p2LeashDistance = Clamp(settings.p2LeashDistance, 0.0f, 100000.0f);
    // A zero cooldown would let the leash re-fire on every call and fight the
    // game's own placement, so the floor is enforced rather than offered.
    if (settings.p2LeashCooldownCalls < 30)
        settings.p2LeashCooldownCalls = 30;
    // An empty parameter name would build an FName of NAME_None and set a
    // parameter that cannot exist. Fall back rather than call with it.
    const Settings defaults = Defaults();
    if (settings.p2TintColorParam[0] == '\0')
        AssignLiteral(settings.p2TintColorParam, sizeof(settings.p2TintColorParam),
                      defaults.p2TintColorParam);
    if (settings.p2TintIntensityParam[0] == '\0')
        AssignLiteral(settings.p2TintIntensityParam,
                      sizeof(settings.p2TintIntensityParam),
                      defaults.p2TintIntensityParam);
    // A map name becomes an FName and a travel destination; refuse anything
    // that is not a bare identifier rather than travel somewhere malformed.
    bool warpOk = settings.warpTarget[0] != '\0';
    for (const char* c = settings.warpTarget; *c != '\0'; ++c) {
        const bool ident = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
                           (*c >= '0' && *c <= '9') || *c == '_';
        if (!ident) {
            warpOk = false;
            break;
        }
    }
    if (!warpOk)
        AssignLiteral(settings.warpTarget, sizeof(settings.warpTarget),
                      defaults.warpTarget);
    sSettings = settings;
}

void LogSettings(const char* state) {
    Logging.Log("[PShowCoop] CONFIG-0001 %s p2.tint enabled=%d rgb=(%.2f,%.2f,%.2f) intensity=%.2f color_param=\"%s\" intensity_param=\"%s\" backstop=%u p2.leash spawn=%.1f distance=%.1f cooldown=%u warp.target=%s path=%s",
                state, sSettings.p2TintEnabled ? 1 : 0,
                sSettings.p2TintR, sSettings.p2TintG, sSettings.p2TintB,
                sSettings.p2TintIntensity, sSettings.p2TintColorParam,
                sSettings.p2TintIntensityParam, sSettings.p2TintBackstopCalls,
                sSettings.p2LeashSpawnDistance, sSettings.p2LeashDistance,
                sSettings.p2LeashCooldownCalls,
                sSettings.warpTarget,
                kConfigPath);
}

}  // namespace

const Settings& Get() {
    return sSettings;
}

void Reload() {
    Settings settings = Defaults();
#if !PSHOW_CONFIG_FILE_READ_ENABLED
    // No fs call is made at all. Not a guarded attempt - an unregistered mount
    // aborts rather than returning, so there is nothing to guard.
    Apply(settings);
    if (!sEverLoaded)
        LogSettings("defaults (file read disabled)");
    sEverLoaded = true;
    return;
#else
    nn::fs::FileHandle handle = {};
    Result result = nn::fs::OpenFile(&handle, kConfigPath, nn::fs::OpenMode_Read);
    if (result != 0) {
        Apply(settings);
        if (!sEverLoaded)
            LogSettings("defaults (open failed)");
        sEverLoaded = true;
        return;
    }

    long fileSize = 0;
    result = nn::fs::GetFileSize(&fileSize, handle);
    if (result != 0 || fileSize <= 0) {
        nn::fs::CloseFile(handle);
        Apply(settings);
        if (!sEverLoaded)
            LogSettings("defaults (unreadable)");
        sEverLoaded = true;
        return;
    }

    // Static, not stack: an 8 KiB frame on the game thread under
    // EXL_USE_FAKEHEAP is exactly what the old 2048 cap was avoiding. Reload is
    // only ever called from the single game thread, so a shared buffer is safe.
    static char buffer[kConfigMaxBytes + 1];
    size_t readSize = static_cast<size_t>(fileSize);
    const bool truncated = readSize > kConfigMaxBytes;
    if (truncated)
        readSize = kConfigMaxBytes;
    result = nn::fs::ReadFile(handle, 0, buffer, static_cast<ulong>(readSize));
    nn::fs::CloseFile(handle);
    if (result != 0) {
        Apply(settings);
        if (!sEverLoaded)
            LogSettings("defaults (read failed)");
        sEverLoaded = true;
        return;
    }

    buffer[readSize] = '\0';
    ParseConfigText(buffer, readSize, &settings);
    Apply(settings);
    if (truncated) {
        // Loud, because the failure mode is invisible: every key past the cap
        // keeps its default while the settings line still reads "loaded".
        Logging.Log("[PShowCoop] CONFIG-0001 TRUNCATED file=%ld bytes cap=%u - every key past the cap was IGNORED and kept its default",
                    fileSize, (unsigned)kConfigMaxBytes);
    }
    LogSettings(truncated ? "loaded (truncated)" : "loaded");
    sEverLoaded = true;
#endif
}

bool ReloadThrottled(unsigned callCounter, unsigned minIntervalCalls) {
    if (sLastReloadCall != 0 && callCounter - sLastReloadCall < minIntervalCalls) {
        return false;
    }
    sLastReloadCall = callCounter;
    Reload();
    return true;
}

}  // namespace pshowcoop::config
