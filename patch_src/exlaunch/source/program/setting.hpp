#pragma once

#include "common.hpp"

// Required ExLaunch library configuration. The lib (jit.hpp, inline_impl.cpp,
// logging) does not compile without these; keep them in sync with the shared
// exlaunch checkout's expectations. Title-specific co-op settings are added
// below this block after their evidence gate.

#define EXL_MODULE_NAME "PShowCoop"

#define EXL_DEBUG
#define EXL_USE_FAKEHEAP

namespace exl::setting {
    /* How large the fake .bss heap will be. */
    constexpr size_t HeapSize = 0x5000;

    
    constexpr size_t JitSize = 0x4000;

    /* How large the area will be inline hook pool. */
    constexpr size_t InlinePoolSize = 0x1000;

    /* How large the formatting buffer should be for logging. The buffer will be on the stack. */
    constexpr size_t LogBufferSize = 512;

    /* Sanity checks. */
    static_assert(ALIGN_UP(JitSize, PAGE_SIZE) == JitSize, "");
    static_assert(ALIGN_UP(InlinePoolSize, PAGE_SIZE) == InlinePoolSize, "");
}
