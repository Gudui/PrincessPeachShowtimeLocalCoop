#include "lib.hpp"
#include "program/loggers.hpp"
#include "program/patches/patches.hpp"

extern "C" void exl_main(void* env, void* dynamic_context) {
    exl::hook::Initialize();

    Logging.Log("[PShowCoop] Initializing modular Princess Peach Showtime local co-op mod...");
    pshowcoop::patches::InstallAll();
    Logging.Log("[PShowCoop] All modular baseline patches initialized successfully.");
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
