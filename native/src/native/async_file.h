// pgr4-native PGR4 native renderer: a text file written off the threads that append to it.
//
// The frame CSV and the pacer's present log were written and flushed once a frame on the render
// and pacer threads, and one such write held the render thread for 27 ms (td-153). Here Printf
// and Append only copy into memory; the file's own thread writes the text out every
// kWriteInterval and when the file is destroyed, so a killed process loses at most an interval.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace pgr4::native {

class AsyncFile {
 public:
  static constexpr std::chrono::milliseconds kWriteInterval{250};

  // Creates or truncates the file; null if it cannot be opened.
  static std::unique_ptr<AsyncFile> Create(const std::string& path);
  ~AsyncFile();  // writes what is left, then closes
  AsyncFile(const AsyncFile&) = delete;
  AsyncFile& operator=(const AsyncFile&) = delete;

  // Any thread.
  void Append(std::string_view text);
  __attribute__((format(printf, 2, 3))) void Printf(const char* format, ...);

 private:
  explicit AsyncFile(std::FILE* file);
  void Run();

  std::FILE* const file_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::string pending_;   // mutex_: appended, not written yet
  bool closing_ = false;  // mutex_
  std::thread thread_;    // last: starts once the members above exist
};

}  // namespace pgr4::native
