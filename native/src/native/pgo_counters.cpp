// pgr4-native: the profile counters of an instrumented build (native/build.sh --pgo gen).
//
// The profile runtime writes its counters when the process exits normally, and the run scripts
// end the game with wineserver -k, which never gets there. So a thread writes them every few
// seconds, to a temporary file renamed over the profile once complete: a kill during a write
// leaves the previous profile whole. The profile is pgr4_recompiled.profraw beside the exe, or
// the file LLVM_PROFILE_FILE names (no % patterns: the rename needs the final name).
#if defined(__LLVM_INSTR_PROFILE_GENERATE)

#include <windows.h>

#include <string>

extern "C" {
int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char* name);
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

DWORD WINAPI WriteLoop(void*) {
  const std::string path = ProfilePath(), partial = path + ".partial";
  __llvm_profile_set_filename(partial.c_str());
  for (;;) {
    Sleep(5000);
    if (__llvm_profile_write_file() == 0)
      MoveFileExA(partial.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
}

[[maybe_unused]] const bool started = [] {
  if (HANDLE thread = CreateThread(nullptr, 0, WriteLoop, nullptr, 0, nullptr)) CloseHandle(thread);
  return true;
}();

}  // namespace

#endif
