// Geometry Wars: Retro Evolved (PGR4's gw.xex) - ReXGlue recompiled project.

#pragma once

#include <rex/rex_app.h>
#include <rex/system/user_module.h>

#include "title_launch.h"

class GwRecompiledApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GwRecompiledApp>(
        new GwRecompiledApp(ctx, "gw_recompiled", PPCImageConfig));
  }

  // The disc's game:\ holds both titles; this one is gw.xex. Its code is generated from the
  // disc's gw.xex, so the title update, which patches both titles, is not applied.
  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = "game:\\gw.xex";
    REXCVAR_SET(apply_title_update, false);
  }
  void OnPostLoadXexImage() override { title_launch::Restore(runtime()); }
  // Back to the garage: the guest launches default.xex with its launch data.
  void OnGuestThreadExit(rex::system::XThread*) override { title_launch::Continue(runtime()); }

  // See pgr4_recompiled_app.h.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    return nullptr;
  }
};
