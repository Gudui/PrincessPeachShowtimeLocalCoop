#pragma once
#include "diagnostic_profile.hpp"

// Buffered SD logging adapted from the operator-referenced Galaxy helper.
// Reuses generic transport/test methodology only: no foreign title addresses,
// layouts, native symbols or investigation conclusions. Showtime SDK exports
// Kernel timing/log buffers below are instrumentation, never game state.
#ifndef PSHOW_SD_LOG_ENABLED
#define PSHOW_SD_LOG_ENABLED 1
#endif
#ifndef PSHOW_RELEASE_DIAGNOSTIC_ENABLED
#define PSHOW_RELEASE_DIAGNOSTIC_ENABLED 0
#endif

#include <lib/log/ilogger.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace pshowcoop::sdlog {

// Io provides: std::uint64_t Tick(); std::uint64_t TickHz(); bool Ready(); bool Open();
// bool Write(std::int64_t offset, const char* data, std::size_t size) (write and
// flush); void Yield().
template <class Io, bool StartupOnly = false>
class Core {
public:
    static constexpr std::size_t kBufferSize = 64 * 1024;
    static constexpr std::size_t kFlushBytes = 8 * 1024;
    static constexpr std::uint64_t kOpenDelaySeconds = 5;
    static constexpr std::uint64_t kFlushIntervalMs = 2000;
    static constexpr std::int64_t kMaxFileSize = 4 << 20;
    enum class State { Waiting, Open, Failed, Capped, Sealed };

    void Log(Io& io, std::string_view text) { Process(io, text, true); }
    void Pump(Io& io) { Process(io, {}, false); }

    State GetState() const { return mState; }
    std::uint32_t Dropped() const { return mDropped; }

private:
    void Process(Io& io, std::string_view text, bool append) {
        if (mSealed.load(std::memory_order_acquire)) return;
        while (mLock.test_and_set(std::memory_order_acquire)) io.Yield();
        if (mSealed.load(std::memory_order_acquire)) {
            mLock.clear(std::memory_order_release);
            return;
        }
        const std::uint64_t now = io.Tick();
        if (!mStarted && !append) { mLock.clear(std::memory_order_release); return; }
        if (!mStarted) {
            mStarted = true;
            mStart = mLastFlush = now;
        }
        if (append && (mState == State::Waiting || mState == State::Open)) Append(io, now, text);
        if (mState == State::Waiting && io.Ready() && now - mStart >= kOpenDelaySeconds * io.TickHz())
            mState = io.Open() ? State::Open : State::Failed;
        if (mState == State::Open && mUsed != 0 &&
            (mUsed >= kFlushBytes || now - mLastFlush >= kFlushIntervalMs * io.TickHz() / 1000))
            Flush(io, now);
        if (mState == State::Failed || mState == State::Capped) mUsed = 0;
        mLock.clear(std::memory_order_release);
    }

    // One line: "[seconds] text\n". A line that does not fit is counted; the
    // count is reported as its own line ahead of the next line that fits.
    void Append(Io& io, std::uint64_t now, std::string_view text) {
        const double seconds = static_cast<double>(now - mStart) / static_cast<double>(io.TickHz());
        char note[64];
        int n = 0;
        if (mDropped != 0)
            n = std::snprintf(note, sizeof(note), "[%10.3f] SD log dropped %u lines\n",
                              seconds, static_cast<unsigned>(mDropped));
        char prefix[24];
        const int p = std::snprintf(prefix, sizeof(prefix), "[%10.3f] ", seconds);
        if (n < 0 || static_cast<std::size_t>(n) >= sizeof(note) || p <= 0 || static_cast<std::size_t>(p) >= sizeof(prefix)) { ++mDropped; return; }
        const std::size_t line = static_cast<std::size_t>(p) + text.size() + 1;
        if (static_cast<std::size_t>(n) + line > kBufferSize - mUsed) { ++mDropped; return; }
        std::memcpy(mBuffer + mUsed, note, static_cast<std::size_t>(n));
        mUsed += static_cast<std::size_t>(n);
        mDropped = 0;
        std::memcpy(mBuffer + mUsed, prefix, static_cast<std::size_t>(p));
        std::memcpy(mBuffer + mUsed + p, text.data(), text.size());
        mBuffer[mUsed + line - 1] = '\n';
        mUsed += line;
    }

    void Flush(Io& io, std::uint64_t now) {
        if (mOffset + static_cast<std::int64_t>(mUsed) > kMaxFileSize) {
            static constexpr char kCap[] = "SD log size cap reached\n";
            io.Write(mOffset, kCap, sizeof(kCap) - 1);
            mState = State::Capped;
            return;
        }
        if (!io.Write(mOffset, mBuffer, mUsed)) { mState = State::Failed; return; }
        mOffset += static_cast<std::int64_t>(mUsed);
        mUsed = 0;
        mLastFlush = now;
        if constexpr (StartupOnly) {
            mState = State::Sealed;
            mSealed.store(true, std::memory_order_release);
        }
    }

    std::atomic_flag mLock = ATOMIC_FLAG_INIT;
    std::atomic<bool> mSealed{false};
    State mState = State::Waiting;
    bool mStarted = false;
    std::uint64_t mStart = 0, mLastFlush = 0;
    std::int64_t mOffset = 0;
    std::size_t mUsed = 0;
    std::uint32_t mDropped = 0;
    char mBuffer[kBufferSize]{};
};
struct SdLogger : public exl::log::ILogger {
    virtual void LogRaw(std::string_view string) final;
};

void AfterInputDrain();
void Guard(const char* site, const void* pointer, bool accepted);
void Overlap(const char* decision, const void* collection, const void* key, int index, int count);
void Player(int label, const void* pawn, const void* controller, const void* localPlayer, unsigned gate);

}  // namespace pshowcoop::sdlog
