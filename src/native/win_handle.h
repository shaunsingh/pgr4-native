// pgr4-native PGR4 native renderer: owning Win32 handles.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <type_traits>

namespace pgr4::native {

// For objects whose creation returns null on failure (events, waitable timers, threads), not
// INVALID_HANDLE_VALUE (files).
struct HandleCloser {
  void operator()(HANDLE h) const noexcept { CloseHandle(h); }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

// An unnamed auto-reset event, not set; null on failure.
inline UniqueHandle CreateAutoResetEvent() {
  return UniqueHandle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

}  // namespace pgr4::native
