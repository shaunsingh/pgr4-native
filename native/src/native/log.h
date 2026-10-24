// pgr4-native PGR4 native renderer: log messages that do not repeat.
//
// PGR4_LOG_FIRST(n, LEVEL, ...) logs the first n messages of its call site at REXLOG_<LEVEL>
// (INFO, WARN, ERROR), PGR4_WARN_ONCE the first one. Every call site counts on its own, so two
// reasons never share a limit; past it the cost is one relaxed load. Thread-safe.
#pragma once

#include <atomic>
#include <cstdint>

#ifndef REXLOG_WARN  // the offline texture harness brings its own REXLOG_* (textures.cpp)
#include <rex/logging.h>
#endif

#define PGR4_LOG_FIRST(n, level, ...)                                         \
  do {                                                                        \
    static std::atomic<uint32_t> pgr4_log_count{0};                           \
    if (pgr4_log_count.load(std::memory_order_relaxed) < uint32_t(n) &&       \
        pgr4_log_count.fetch_add(1, std::memory_order_relaxed) < uint32_t(n)) \
      REXLOG_##level(__VA_ARGS__);                                            \
  } while (0)

#define PGR4_WARN_ONCE(...) PGR4_LOG_FIRST(1, WARN, __VA_ARGS__)
