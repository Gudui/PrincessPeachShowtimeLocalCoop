#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace pshowcoop::riding {
// ExLaunch's float-inline assembly saves q0..q31 BELOW the 0x100-byte GP
// block. This is the actual callback stack layout, not InlineFloatCtx's C++
// inherited-member order. See inline_asm.s armBackup* / armRecover*.
inline constexpr std::size_t FloatSaveBytes = 32 * 16;
template<class T, class Readable>
bool CheckedRead(const void* object, std::size_t offset, T& value, Readable readable) {
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    if (!object || offset > UINTPTR_MAX - address) return false;
    const auto* source = reinterpret_cast<const void*>(address + offset);
    if (!readable(source, sizeof(T))) return false;
    std::memcpy(&value, source, sizeof(T));
    return true;
}
inline std::uintptr_t SavedX(const void* frame, unsigned index) {
    std::uintptr_t value = 0;
    if (frame && index < 31)
        std::memcpy(&value, static_cast<const std::uint8_t*>(frame) + FloatSaveBytes + index * 8, 8);
    return value;
}
struct State {
    std::uintptr_t pawn{}, cls{}, root{}, base{}, parent{}, parentActor{}, cmc{}, core{};
    unsigned valid{}, mask{}, mode{};
    bool operator==(const State&) const = default;
};
struct TracePolicy {
    State last{};
    bool have{};
    unsigned changes{}, periodic{}, forced{};
    // Three independent quotas: movement edges cannot consume the event-edge
    // before/after budget or the later periodic window. Instrument state only.
    bool Take(const State& state, bool force, bool heartbeat) {
        const bool changed = !have || !(state == last);
        last = state;
        have = true;
        if (force) { if (forced < 96) { ++forced; return true; } return false; }
        if (changed && changes < 128) { ++changes; return true; }
        if (heartbeat && periodic < 256) { ++periodic; return true; }
        return false;
    }
};
} // namespace pshowcoop::riding
