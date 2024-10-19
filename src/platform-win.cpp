#include "platform.hpp"
#include "Windows.h"

void prepare_process() {
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
}

void prepare_current_thread(uint64_t affinity) {
    SetThreadAffinityMask(GetCurrentThread(), affinity);
    SetThreadPriority(GetCurrentThread(), THREAD_BASE_PRIORITY_LOWRT);
}
