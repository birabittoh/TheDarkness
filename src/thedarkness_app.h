// thedarkness - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/system/game_data_selector.h>
#include <rex/ui/window.h>

#include "icon.generated.h"
#include "debug_overlay.h"

class ThedarknessApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<ThedarknessApp>(new ThedarknessApp(ctx, "thedarkness",
        PPCImageConfig));
  }  

  bool SetupEnvironment() override {
    if (!rex::ReXApp::SetupEnvironment())
      return false;

    rex::system::GameDataSelectorSettings settings;
    settings.default_xex_sha256 = "aace35a8f9bcdc7f28aeab9ff8cf3bdf200353f5c83705f6284487347acb3c5f";

    return rex::system::GameDataSelector::EnsureGameData(settings);
  }

  void OnPostSetup() override {
    window()->SetIcon(thedarkness::kIconPNG, thedarkness::kIconPNGSize);
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    drawer->AddDialog(new rex::ui::DarknessDebugOverlay(drawer));
  }
};
