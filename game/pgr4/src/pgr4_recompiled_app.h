// Project Gotham Racing 4 - ReXGlue recompiled project.

#pragma once

#include <rex/rex_app.h>

#include "title_launch.h"

class Pgr4RecompiledApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Pgr4RecompiledApp>(
        new Pgr4RecompiledApp(ctx, "pgr4_recompiled", PPCImageConfig));
  }

  void OnPostLoadXexImage() override { title_launch::Restore(runtime()); }
  // The garage's arcade cabinet launches gw.xex.
  void OnGuestThreadExit(rex::system::XThread*) override { title_launch::Continue(runtime()); }

  // The toast is an ImGui dialog, and one registered dialog keeps the presenter on the UI
  // thread's on-request path for the whole session instead of presenting from the guest output
  // thread at once.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    return nullptr;
  }
};
