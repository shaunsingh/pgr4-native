#include "title_launch.h"

#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

REXCVAR_DEFINE_INT32(launch_flags, 0, "Title launch",
                     "Launch flags from the title that started this one");
REXCVAR_DEFINE_STRING(launch_data, "", "Title launch",
                      "Launch data from the title that started this one, in hex; '-' for "
                      "present but empty");

namespace title_launch {
namespace {

struct Title {
  std::string_view guest_path;
  const wchar_t* executable;
};
constexpr Title kTitles[] = {
    {"game:\\default.xex", L"pgr4_recompiled.exe"},
    {"game:\\gw.xex", L"gw_recompiled.exe"},
};
// XAM's limit (XLAUNCH_DATA_MAX_SIZE).
constexpr size_t kMaxLaunchData = 3072;

rex::system::object_ref<rex::kernel::xam::XamModule> Xam(rex::Runtime* runtime) {
  return runtime->kernel_state()->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
}

// One argument quoted for CommandLineToArgvW and the CRT: backslashes double only before a quote.
void AppendQuoted(std::wstring& line, std::wstring_view arg) {
  if (!line.empty()) line += L' ';
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    line += arg;
    return;
  }
  line += L'"';
  size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    line.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    line += c;
  }
  line.append(slashes * 2, L'\\');
  line += L'"';
}

bool IsLaunchArg(std::wstring_view arg) {
  for (std::wstring_view name : {L"--launch_flags", L"--launch_data"})
    if (arg.starts_with(name) && (arg.size() == name.size() || arg[name.size()] == L'='))
      return true;
  return false;
}

}  // namespace

void Restore(rex::Runtime* runtime) {
  const std::string hex = REXCVAR_GET(launch_data);
  const int32_t flags = REXCVAR_GET(launch_flags);
  if (hex.empty() && flags == 0) return;
  const auto xam = Xam(runtime);
  if (!xam) return;
  std::vector<uint8_t> data;
  if (hex != "-") {
    if (hex.size() % 2 || hex.size() / 2 > kMaxLaunchData) {
      REXLOG_ERROR("Title launch: malformed --launch_data ({} characters)", hex.size());
      return;
    }
    data.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
      char* end = nullptr;
      const std::string byte = hex.substr(i, 2);
      const unsigned long value = std::strtoul(byte.c_str(), &end, 16);
      if (end != byte.c_str() + 2) {
        REXLOG_ERROR("Title launch: malformed --launch_data");
        return;
      }
      data.push_back(uint8_t(value));
    }
  }
  auto& loader = xam->loader_data();
  loader.launch_flags = uint32_t(flags);
  loader.launch_data_present = !hex.empty();
  loader.launch_data = std::move(data);
  REXLOG_INFO("Title launch: {} bytes of launch data, flags {:#x}", loader.launch_data.size(),
              loader.launch_flags);
}

void Continue(rex::Runtime* runtime) {
  const auto xam = Xam(runtime);
  if (!xam) return;
  const auto& loader = xam->loader_data();
  std::string target = loader.launch_path;
  std::ranges::transform(target, target.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const auto title = std::ranges::find(kTitles, std::string_view(target), &Title::guest_path);
  if (title == std::end(kTitles)) return;
  if (loader.launch_data.size() > kMaxLaunchData) {
    REXLOG_ERROR("Title launch: {} bytes of launch data, more than XAM allows", loader.launch_data.size());
    return;
  }

  // This process's arguments without its own launch data, then the new title's.
  std::wstring line;
  wchar_t module[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
  if (length == 0 || length == MAX_PATH) return;
  std::wstring executable(module, length);
  executable.replace(executable.find_last_of(L"\\/") + 1, std::wstring::npos, title->executable);
  AppendQuoted(line, executable);
  int argc = 0;
  if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i)
      if (!IsLaunchArg(argv[i])) AppendQuoted(line, argv[i]);
    LocalFree(argv);
  }
  line += L" --launch_flags=" + std::to_wstring(int32_t(loader.launch_flags));
  if (loader.launch_data_present) {
    std::wstring hex;
    constexpr wchar_t kDigits[] = L"0123456789abcdef";
    for (uint8_t byte : loader.launch_data) {
      hex += kDigits[byte >> 4];
      hex += kDigits[byte & 15];
    }
    line += L" --launch_data=" + (hex.empty() ? std::wstring(L"-") : hex);
  }

  REXLOG_INFO("Title launch: {} ({} bytes of launch data, flags {:#x})", loader.launch_path,
              loader.launch_data.size(), loader.launch_flags);
  STARTUPINFOW startup{.cb = sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      nullptr, &startup, &process)) {
    REXLOG_ERROR("Title launch: starting {} failed ({})", loader.launch_path, GetLastError());
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  // As the window's close path does: TerminateTitle can leave guest threads running, so the
  // runtime is not torn down under them. The guest saved before it asked for the launch.
  rex::FlushLogging();
  std::_Exit(0);
}

}  // namespace title_launch
