// pgr4-native: the profile counters of an instrumented build (native/build.sh --pgo gen).
//
// The profile runtime writes its counters when the process exits normally, and the run scripts
// end the game with wineserver -k, which never gets there. So a thread writes them every few
// seconds, to a temporary file renamed over the profile once complete: a kill during a write
// leaves the previous profile whole. A normal exit writes and renames once more, after the
// thread's last write, and the runtime's own write at exit then does nothing. The profile is
// pgr4_recompiled.profraw beside the exe, or the file LLVM_PROFILE_FILE names (no % patterns:
// the rename needs the final name).
#if defined(__LLVM_INSTR_PROFILE_GENERATE)

#include <windows.h>

#include <cstdlib>
#include <string>

extern "C" {
int __llvm_profile_write_file(void);
int __llvm_profile_dump(void);
void __llvm_profile_set_filename(const char* name);
int __llvm_profile_register_write_file_atexit(void);
}

namespace {

std::string ProfilePath() {
  char buffer[MAX_PATH];
  const DWORD n = GetEnvironmentVariableA("LLVM_PROFILE_FILE", buffer, MAX_PATH);
  if (n && n < MAX_PATH) return std::string(buffer, n);
  std::string path(buffer, GetModuleFileNameA(nullptr, buffer, MAX_PATH));
  const size_t dot = path.rfind('.');
  if (dot != std::string::npos && _stricmp(&path[dot], ".exe") == 0) path.resize(dot);
  return path + ".profraw";
}

// Never destroyed: the exit handler runs during static destruction.
const std::string& Path() {
  static const std::string* path = new std::string(ProfilePath());
  return *path;
}
const std::string& PartialPath() {
  static const std::string* partial = new std::string(Path() + ".partial");
  return *partial;
}

// Guards the writes and `stopped`: the runtime's writer is not reentrant.
SRWLOCK g_lock = SRWLOCK_INIT;
bool g_stopped = false;

void Rename() { MoveFileExA(PartialPath().c_str(), Path().c_str(), MOVEFILE_REPLACE_EXISTING); }

DWORD WINAPI WriteLoop(void*) {
  for (;;) {
    Sleep(5000);
    AcquireSRWLockExclusive(&g_lock);
    if (g_stopped) {
      ReleaseSRWLockExclusive(&g_lock);
      return 0;
    }
    if (__llvm_profile_write_file() == 0) Rename();
    ReleaseSRWLockExclusive(&g_lock);
  }
}

void WriteAtExit() {
  AcquireSRWLockExclusive(&g_lock);
  g_stopped = true;
  // Marks the profile written, so that the runtime's own exit handler, which runs after this
  // one, writes nothing more to the temporary file.
  if (__llvm_profile_dump() == 0) Rename();
  ReleaseSRWLockExclusive(&g_lock);
}

[[maybe_unused]] const bool started = [] {
  __llvm_profile_set_filename(PartialPath().c_str());
  // Registers the runtime's exit handler now if it is not yet, so that WriteAtExit, registered
  // after it, runs before it.
  __llvm_profile_register_write_file_atexit();
  std::atexit(WriteAtExit);
  if (HANDLE thread = CreateThread(nullptr, 0, WriteLoop, nullptr, 0, nullptr)) CloseHandle(thread);
  return true;
}();

}  // namespace

#endif
