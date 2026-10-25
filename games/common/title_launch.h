// Launches between the titles built from the disc: Project Gotham Racing 4 (default.xex) and the
// Geometry Wars arcade cabinet in its garage (gw.xex). The two are separate executables, because
// their guest code overlaps in address space and each has its own function table. When the guest
// asks XAM for another title (XamLoaderLaunchTitle, which ends the running one), the running
// executable starts the other one beside it, passing the launch data on the command line, and
// exits.

#pragma once

namespace rex {
class Runtime;
}

namespace title_launch {

// Applies the launch data this process was started with (OnPostLoadXexImage).
void Restore(rex::Runtime* runtime);

// After the title's main thread ends (OnGuestThreadExit): when the guest asked for one of the
// disc's titles, starts its executable and ends this process. Otherwise returns.
void Continue(rex::Runtime* runtime);

}  // namespace title_launch
