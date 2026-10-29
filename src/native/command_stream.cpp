#include "command_stream.h"

#include <windows.h>

#include <cmath>  // before simde, whose math uses std::isnan

#include <simde/x86/sse.h>

#include "clock.h"

namespace pgr4::native {

void CommandStream::Start(List* list, const wchar_t* name) {
  list_ = list;
  list9_.Reset();
  list->QueryInterface(IID_PPV_ARGS(list9_.GetAddressOf()));
  stop_ = false;
  thread_ = std::thread([this, name] {
    SetThreadDescription(GetCurrentThread(), name);
    Main();
  });
}

void CommandStream::Stop() {
  if (!thread_.joinable()) return;
  Flush();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  work_cv_.notify_one();
  thread_.join();
}

CommandStream::Chunk* CommandStream::TakeChunk() {
  if (free_.empty()) return chunks_.emplace_back(std::make_unique<Chunk>()).get();
  Chunk* c = free_.back();
  free_.pop_back();
  return c;
}

std::byte* CommandStream::Reserve(size_t size) {
  if (!recording_ || recording_->used + size > kChunkBytes) {
    Flush();
    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = TakeChunk();
  }
  std::byte* p = recording_->data + recording_->used;
  recording_->used += size;
  return p;
}

void CommandStream::Flush() {
  if (!recording_ || !recording_->used) return;
  // The constant uploads are streaming stores, which are weakly ordered: visible before the
  // thread can submit the calls that read them.
  simde_mm_sfence();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::exchange(recording_, nullptr));
    ++flushed_;
  }
  work_cv_.notify_one();
}

void CommandStream::Sync() {
  Flush();
  std::unique_lock<std::mutex> lock(mutex_);
  done_cv_.wait(lock, [&] { return done_ == flushed_; });
}

void CommandStream::Main() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    work_cv_.wait(lock, [&] { return queue_head_ < queue_.size() || stop_; });
    if (queue_head_ == queue_.size()) return;
    Chunk* c = queue_[queue_head_++];
    if (queue_head_ == queue_.size()) {
      queue_.clear();
      queue_head_ = 0;
    }
    lock.unlock();
    const uint64_t t0 = NowNs();
    for (size_t at = 0; at < c->used;) {
      std::byte* p = c->data + at;
      const Entry& e = *reinterpret_cast<const Entry*>(p);
      at += e.size;
      e.run(p + kEntryBytes, list_);
    }
    busy_ns_.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    c->used = 0;
    lock.lock();
    free_.push_back(c);
    ++done_;
    if (done_ == flushed_) done_cv_.notify_all();
  }
}

}  // namespace pgr4::native
