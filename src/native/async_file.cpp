// pgr4-native PGR4 native renderer: a text file written off the threads that append to it
// (async_file.h).
#include "async_file.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>

namespace pgr4::native {

std::unique_ptr<AsyncFile> AsyncFile::Create(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "w");
  return file ? std::unique_ptr<AsyncFile>(new AsyncFile(file)) : nullptr;
}

AsyncFile::AsyncFile(std::FILE* file) : file_(file), thread_([this] { Run(); }) {}

AsyncFile::~AsyncFile() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
  }
  wake_.notify_one();
  thread_.join();
  std::fclose(file_);
}

void AsyncFile::Append(std::string_view text) {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_.append(text);
}

void AsyncFile::Printf(const char* format, ...) {
  char text[512];
  va_list args, again;
  va_start(args, format);
  va_copy(again, args);
  const int n = std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (n >= 0 && size_t(n) < sizeof(text)) {
    Append({text, size_t(n)});
  } else if (n >= 0) {
    std::string long_text(size_t(n), '\0');
    std::vsnprintf(long_text.data(), long_text.size() + 1, format, again);
    Append(long_text);
  }
  va_end(again);
}

void AsyncFile::Run() {
  SetThreadDescription(GetCurrentThread(), L"PGR4 native file");
  // Swapped with pending_, so both keep their capacity and appending stops allocating.
  std::string writing;
  std::unique_lock<std::mutex> lock(mutex_);
  for (bool closing = false; !closing;) {
    closing = wake_.wait_for(lock, kWriteInterval, [this] { return closing_; });
    writing.swap(pending_);
    lock.unlock();
    if (!writing.empty()) {
      std::fwrite(writing.data(), 1, writing.size(), file_);
      std::fflush(file_);
      writing.clear();
    }
    lock.lock();
  }
}

}  // namespace pgr4::native
