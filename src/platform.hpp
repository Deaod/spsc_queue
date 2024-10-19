#pragma once

#include <cstdint>

void prepare_process();
void prepare_current_thread(uint64_t affinity);

constexpr const uint64_t Thread1Affinity = 1 << 8;
constexpr const uint64_t Thread2Affinity = 1 << 10;
