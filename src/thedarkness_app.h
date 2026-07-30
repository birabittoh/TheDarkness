// thedarkness - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/system/game_data_selector.h>
#include <rex/ui/window.h>
#include <rex/version.h>

#include "icon.generated.h"
#include "debug_overlay.h"
#include "settings.h"

class ThedarknessApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<ThedarknessApp>(new ThedarknessApp(ctx, "thedarkness",
        PPCImageConfig));
  }  

  bool SetupEnvironment() override {
    // Game defaults for SDK cvars must land before the SDK loads any config
    // file, so a saved/CLI/env override still wins.
    thedarkness::ApplySettingDefaults();

    if (!rex::ReXApp::SetupEnvironment())
      return false;

    // User-facing settings (Fullscreen, Resolution, ...) live in their own
    // file, separate from the advanced cvars in the app's normal config, and
    // are loaded last so they win over both.
    if (std::filesystem::exists(user_settings_path()))
      rex::cvar::LoadConfig(user_settings_path());

    rex::system::GameDataSelectorSettings settings;
    settings.default_xex_sha256 = "aace35a8f9bcdc7f28aeab9ff8cf3bdf200353f5c83705f6284487347acb3c5f";
    settings.config_path = config_path();

    return rex::system::GameDataSelector::EnsureGameData(settings);
  }

  void OnPostSetup() override {
    window()->SetIcon(thedarkness::kIconPNG, thedarkness::kIconPNGSize);
    window()->SetTitle("The Darkness " + std::string(REXGLUE_BUILD_TITLE));
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    drawer->AddDialog(new rex::ui::DarknessDebugOverlay(drawer));
  }

  std::unique_ptr<rex::ui::ImGuiDialog> OnCreateUserSettingsOverlay() override {
    return thedarkness::CreateSettingsDialog(
        imgui_drawer(), window(), user_settings_path(), config_path(),
        static_cast<rex::input::InputSystem*>(runtime()->input_system()));
  }

 private:
  std::filesystem::path user_settings_path() const { return user_data_root() / "settings.toml"; }
};
