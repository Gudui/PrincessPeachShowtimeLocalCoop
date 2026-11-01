#pragma once

#include <cstddef>
#include <cstdint>

namespace pshowcoop::diagcontrols {
// Showtime SDK detail::GetNpadStates copies five qwords per sample. The first
// is its consecutive sample identity, +8 is the button word the SDK masks,
// +16..28 are the four stick integers used by Showtime's platform poller.
struct RawSample {
    std::int64_t sample;
    std::uint64_t buttons;
    std::int32_t lx, ly, rx, ry;
    std::uint32_t attributes, reserved;
};
static_assert(sizeof(RawSample) == 0x28);
static_assert(offsetof(RawSample, buttons) == 8);
static_assert(offsetof(RawSample, lx) == 0x10);

struct RawEdge {
    bool fresh{}, baseline{}, reset{}, changed{};
    std::uint64_t down{}, up{}, gap{};
};

constexpr bool FaceEdge(const RawEdge& e) {
    return e.baseline || ((e.down | e.up) & 0xfU) != 0;
}
constexpr bool BindingChanged(bool mappingChanged, bool have, bool previous, bool current) {
    return mappingChanged || !have || previous != current;
}
constexpr bool GapMilestone(unsigned events) {
    return events && (events & (events - 1)) == 0;
}

struct RawHistory {
    bool have{};
    RawSample last{};
    static unsigned StickBand(const RawSample& s) {
        // Instrument-only coarse activity. Avoid logging analog jitter; the
        // actual four integers are retained in every emitted raw line.
        unsigned b = 0;
        const std::int32_t v[] = {s.lx, s.ly, s.rx, s.ry};
        for (unsigned i = 0; i < 4; ++i) {
            if (v[i] > 8192) b |= 1U << (i * 2);
            if (v[i] < -8192) b |= 2U << (i * 2);
        }
        return b;
    }
    RawEdge Push(const RawSample& s) {
        RawEdge e;
        if (have && s.sample == last.sample) return e;
        e.fresh = true;
        e.baseline = !have;
        e.reset = have && s.sample < last.sample;
        if (have && !e.reset) {
            e.down = s.buttons & ~last.buttons;
            e.up = last.buttons & ~s.buttons;
            if (s.sample > last.sample && last.sample >= 0)
                e.gap = static_cast<std::uint64_t>(s.sample) - static_cast<std::uint64_t>(last.sample) - 1;
        }
        // Initial/reconnected held state is a baseline, never a fabricated edge.
        e.changed = e.baseline || e.reset || e.down || e.up ||
            s.attributes != last.attributes || StickBand(s) != StickBand(last);
        last = s;
        have = true;
        return e;
    }
    template<class Emit> bool Replay(const RawSample* samples, int count, Emit emit) {
        if (!samples || count < 1 || count > 16) return false;
        if (have && samples[0].sample < last.sample) have = false;
        // SDK returns newest first. Discard history overlap before Push so a
        // normal replay is not mistaken for a reconnect/sample-counter reset.
        for (int j = count - 1; j >= 0; --j) {
            const auto& s = samples[j];
            if (have && s.sample <= last.sample) continue;
            const auto e = Push(s);
            if (e.fresh) emit(s, e);
        }
        return true;
    }
};

// Logging budgets are independent by stream/player. A noisy P1 cannot consume
// P2's allowance. Take() emits exactly one visible cap marker via its caller.
struct Budget {
    unsigned used{};
    bool capped{};
    bool Take(unsigned limit, bool& marker) {
        marker = false;
        if (used < limit) { ++used; return true; }
        if (!capped) { capped = true; marker = true; }
        return false;
    }
};

// Safe half-open span check used by the native QueryMemory adapter, including
// overflow cases. No dereference or kernel address guess occurs in this helper.
constexpr bool Contains(std::uintptr_t base, std::uint64_t size,
                        std::uintptr_t address, std::size_t bytes) {
    return bytes && address >= base && bytes <= size && address - base <= size - bytes;
}
}
