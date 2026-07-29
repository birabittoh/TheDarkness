#pragma once
#include <cstdint>
#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

#include <string>
#include <vector>

namespace rex::ui {

class DarknessDebugOverlay : public ImGuiDialog {
 public:
  explicit DarknessDebugOverlay(ImGuiDrawer* imgui_drawer);
  ~DarknessDebugOverlay();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  uint8_t* GetMembase();
  uint32_t GetPawn();
  uint32_t GetPawnFlag();
  void SetPawnFlag(uint32_t flags);

  // Read/write game memory at pawn + offset.
  uint32_t ReadPawnU32(uint32_t offset);
  void WritePawnU32(uint32_t offset, uint32_t value);
  float ReadPawnFloat(uint32_t offset);

  // --- Guest console ---
  // Submits `line` to the game's script console (queued for the guest thread)
  // and echoes it into the overlay log.
  void SubmitConsoleLine(const std::string& line);
  void LogLine(const std::string& text);
  static int ConsoleInputCallback(ImGuiInputTextCallbackData* data);

  void DrawConsole();
  void DrawCommandReference();
  void DrawHostSettings();
  void DrawRendererCVars();

  // --- Host cvar helpers (rex::cvar registry, by name) ---
  // By-name access rather than REXCVAR_GET for the reason documented in
  // camera_mouselook_hook.cpp: a cvar's storage may live in a different linked
  // module than the one the console/config actually updates.
  bool CVarBool(const char* name);
  void SetCVarBool(const char* name, bool value);
  int CVarInt(const char* name);
  void SetCVarInt(const char* name, int value);
  double CVarDouble(const char* name);
  void SetCVarDouble(const char* name, double value);

  std::vector<std::string> log_lines_;
  std::vector<std::string> history_;
  int history_pos_ = -1;  // -1 == not browsing history
  char input_buf_[256] = {};
  bool scroll_log_to_bottom_ = false;
  bool refocus_input_ = false;
  char command_filter_[64] = {};
  char info_screen_text_[128] = "hello";
};

}  // namespace rex::ui
