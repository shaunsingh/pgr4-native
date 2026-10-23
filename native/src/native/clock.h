// pgr4-native PGR4 native renderer: the CPU clock.
//
// Ticks() reads the time-stamp counter, which runs at a constant rate on every CPU the build
// targets and costs a few ns, where QueryPerformanceCounter under Wine is a syscall (about
// 0.15 µs). NowNs() is the same clock in nanoseconds, calibrated against QueryPerformanceCounter
// over 20 ms at its first use. Hot paths sum ticks and convert the sum once (TicksToNs).
#pragma once

#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#include <intrin.h>
#endif

namespace pgr4::native {

inline uint64_t Ticks() {
#if defined(_M_X64) || defined(__x86_64__)
  return __rdtsc();
#elif defined(__aarch64__)
  uint64_t t;
  asm volatile("mrs %0, cntvct_el0" : "=r"(t));
  return t;
#else
#error "Ticks: no cycle counter for this architecture"
#endif
}

// Monotonic, in nanoseconds.
uint64_t NowNs();
// A tick count (a difference of Ticks()) in nanoseconds.
uint64_t TicksToNs(uint64_t ticks);

}  // namespace pgr4::native
