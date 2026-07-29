#include "debug_overlay.h"

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Player pawn accessor (defined in camera_mouselook_hook.cpp).
extern "C" uint32_t Darkness_GetPlayerPawn();

// Debug-command dispatch (defined in camera_mouselook_hook.cpp). Buttons here
// can't call guest code directly -- ImGui draws off the guest CPU thread, and
// the guest call machinery needs a ThreadState bound to the calling thread --
// so we queue an opaque handler pointer and let the per-frame camera hook
// (which does run on the guest thread) invoke it against the live pawn. See
// the long comment on g_pending_debug_command for why.
// `arg` is the handler's second guest argument, built into an engine string on
// the guest thread. Every one of these handlers is registered with arity 1, so
// there is always a second argument -- most ignore it, but showinfoscreen
// dereferences and releases it. See the comment on DebugCommandFn.
extern "C" void Darkness_QueueDebugCommand(void* fn, const char* arg);
extern "C" void* Darkness_GetDebugCommandFn(int index);

// Queues a line for the game's own script console (CConsole::ExecuteString).
// Same threading constraint, same queue-and-drain solution -- see the guest
// console bridge comment in camera_mouselook_hook.cpp.
extern "C" void Darkness_QueueConsoleCommand(const char* text);

// Cheat support (camera_mouselook_hook.cpp). The player character is resolved
// on the guest thread each frame via the engine's own entity-ID lookup; these
// expose the result so the UI can show what it found instead of silently
// doing nothing.
extern "C" void Darkness_QueueNoclipOn(uint32_t character, int mode);
extern "C" uint32_t Darkness_GetPlayerEntityId();
extern "C" uint32_t Darkness_GetEntityContainer();
extern "C" int Darkness_GetCharacterCandidateCount();
extern "C" bool Darkness_GetCharacterCandidate(int index, uint32_t* ptr, uint32_t* vtable,
                                               uint32_t* id, uint32_t* gen, uint32_t* mode,
                                               uint32_t* flags, uint64_t* hits, bool* is_character);
extern "C" uint32_t Darkness_GetPlayerCharacter();
extern "C" uint32_t Darkness_GetPlayerMoveMode();
extern "C" bool Darkness_IsNoclipBlocked();
extern "C" bool Darkness_WasNoclipRefused();
extern "C" void Darkness_QueueGodmode();
extern "C" bool Darkness_IsGodmodeActive();
extern "C" bool Darkness_IsGodmodeAvailable();
extern "C" void Darkness_QueueToggleAimAssist();
extern "C" bool Darkness_IsAimAssistActive();
extern "C" void Darkness_QueueCycleCamera();
extern "C" bool Darkness_IsCycleCameraActive();
extern "C" void Darkness_QueueToggleNightVision();
extern "C" bool Darkness_IsNightVisionActive();
extern "C" void Darkness_QueueDemonArm(int level);
extern "C" void Darkness_QueueSkipCutscene();
extern "C" void Darkness_QueueDarknessLevel(uint32_t level_index);
extern "C" void Darkness_QueueKill();
extern "C" void Darkness_QueueGiveAll();
extern "C" void Darkness_ClearCharacterCandidates();

// Keep in sync with the kTable order in Darkness_GetDebugCommandFn.
enum DebugCommandIndex {
  kCmdToggleDebugCamera = 0,
  kCmdToggleDebugCamera2,
  kCmdShowDebugHud,
  kCmdToggleShowHud,
  kCmdShowInfoScreen,
  kCmdShaderDiff,
  kCmdShaderSpec,
  kCmdDarknessVision,
  kCmdCreepingDark,
  kCmdOtherworld,
  kCmdRetinaDots,
  kCmdDumpTextureUsage,
};

// Game cheat gate constants.
static constexpr uint32_t kCheatFlagOffset = 516;  // player + 0x204
static constexpr uint32_t kCheatFlagBit = 4;       // bit 2 (mask 4)

namespace {

struct CommandDoc {
  const char* insert;  // text placed in the console input
  const char* desc;
};

// Curated from DEBUG_MENUS_REVERSE_ENGINEERING.md. These are script functions
// registered into the engine's script VM, so the syntax is `name(args)`.
//
// NOTE: the "cheat commands" from section 2 of that document are deliberately
// absent here -- they are NOT callable. See kActionIdConstants below.

// The names registered by sub_82401178 (noclip, godmode, giveall, kill, the
// darkness levels, ...) are named integer CONSTANTS, not commands. That
// registrar stores arity 0, return type "int32", the action ID at +0x10, and a
// vtable (off_82080CE4) whose only method is:
//
//   loc_82401208:  lwz r10, 0x10(r3)   ; the stored ID
//                  stw r10, 0(r11)     ; write it to the output slot
//                  blr
//
// So `noclip(1)` evaluates to the integer 32 and has no side effect at all --
// which is exactly what "the button does nothing" looked like. They exist so
// keybind/config scripts can name an input action symbolically; the actual
// behaviour fires when the input system dispatches that action ID through the
// message pipeline handled by sub_82126828. Two IDs corroborate the mapping:
// giveall = 35 = 0x23 (the "giveall path" sub-case) and selfsummon = 42 = 0x2A
// (the "Summon is cheat protected" sub-case).
//
// Triggering them therefore needs a synthesized message send, which is not
// wired up yet. Listed here as reference only, never as runnable text.
struct ActionIdDoc {
  const char* name;
  int id;
  const char* desc;
};

const ActionIdDoc kActionIdConstants[] = {
    {"noclip", 32, "Fly through walls"},
    {"physlog", 33, "Physics debug logging"},
    {"godmode", 34, "Invincibility"},
    {"giveall", 35, "Give all weapons and items"},
    {"noclip2", 36, "Alternate noclip mode"},
    {"weapon", 37, "Switch to a specific weapon"},
    {"nextweapon", 38, "Cycle to next weapon"},
    {"prevweapon", 39, "Cycle to previous weapon"},
    {"nextitem", 40, "Next inventory item"},
    {"previtem", 41, "Previous inventory item"},
    {"selfsummon", 42, "Summon a darkling at your position"},
    {"kill", 43, "Kill the player"},
    {"skipingamecutscene", 44, "Skip the current in-game cutscene"},
    {"enddialogue", 45, "End the current dialogue"},
    {"closeclientwindow", 46, "Close the client UI window"},
    {"togglenightvision", 47, "Toggle night vision"},
    {"cyclecamera", 48, "Cycle camera modes"},
    {"showcontroller", 49, "Show the controller input overlay"},
    {"toggleaimassistance", 50, "Toggle aim assist"},
    {"loadlastsave", 51, "Load the most recent save"},
    {"helpbutton", 52, "Help button"},
    {"givedarknesslevel1", 55, "Give darkness power level 1"},
    {"givedarknesslevel2", 56, "Give darkness power level 2"},
    {"givedarknesslevel3", 57, "Give darkness power level 3"},
    {"givedarknesslevel4", 58, "Give darkness power level 4"},
    {"givedarknesslevel5", 59, "Give darkness power level 5"},
    {"givedemonarmlevel1", 60, "Give demon arm level 1"},
    {"givedemonarmlevel2", 61, "Give demon arm level 2"},
};

const CommandDoc kDebugCommands[] = {
    {"toggledebugcamera()", "Toggle the free-fly debug camera"},
    {"toggledebugcamera2()", "Toggle the second debug camera mode"},
    {"showdebughud()", "Toggle the debug HUD overlay"},
    {"toggleshowhud()", "Toggle the entire HUD"},
    {"showinfoscreen('text')", "Set the info screen text (needs a string arg)"},
    {"shaderdiff()", "Shader diffuse debug visualization"},
    {"shaderspec()", "Shader specular debug visualization"},
    {"darknessvision()", "Toggle darkness vision mode"},
    {"creepingdark()", "Toggle the creeping dark effect"},
    {"otherworld()", "Toggle otherworld rendering"},
    {"visionsenabled()", "Toggle vision effects"},
    {"retinadots()", "Toggle retina dots debug rendering"},
    {"dumptextureusage()", "Dump texture memory usage to the log"},
    {"nvviewscale(1.0)", "Night-vision view scale factor"},
    {"toggleinvertmouse()", "Toggle mouse inversion"},
    {"clearinput()", "Clear the input state"},
    {"forcenoworld()", "Force the no-world state"},
    {"hurteffect()", "Trigger the hurt effect"},
    {"invmenu()", "Open the inventory menu"},
};

const CommandDoc kGameCommands[] = {
    {"saveprofile()", "Save the player profile"},
    {"loadgame('checkpoint')", "Load the last checkpoint"},
    {"loadgame('Quick')", "Load the quick save"},
    {"changemap('mapname', 4)", "Change to another map"},
    {"unlockall()", "Unlock all content"},
    {"setdifficulty(2)", "Set the game difficulty"},
    {"commitdebugdifficulty()", "Apply the DEBUG_DIFFICULTY_* cvars"},
    {"deleteallsavegames()", "Delete all save games"},
    {"richpresence()", "Refresh rich presence"},
    {"vsync(1)", "Guest vsync toggle (0 = off)"},
};

const CommandDoc kSystemCommands[] = {
    {"sys_showmemory()", "Display memory information"},
    {"sys_memoryreport()", "Print a memory report"},
    {"sys_memorytracking(1)", "Enable memory tracking"},
    {"sys_memoryshowallocs()", "Show memory allocations"},
    {"sys_memoryhideallocs()", "Hide memory allocations"},
    {"sys_perfcountsystem(1)", "Performance counter for the system"},
    {"sys_perfcountrender(1)", "Performance counter for rendering"},
    {"sys_tracerecordrender(1)", "Trace record for rendering"},
};

struct CommandGroup {
  const char* name;
  const CommandDoc* items;
  size_t count;
};

const CommandGroup kCommandGroups[] = {
    {"Debug / visualization", kDebugCommands, std::size(kDebugCommands)},
    {"Game", kGameCommands, std::size(kGameCommands)},
    {"System", kSystemCommands, std::size(kSystemCommands)},
};

// --- Renderer cvars -------------------------------------------------------
// These are script functions too (registered by sub_825EC690), so we can only
// *write* them from here -- there is no by-name read path back out of the
// script VM. The widgets below therefore track a host-side shadow of whatever
// this panel last sent, not the engine's actual current value; see the note
// rendered above them.

enum class CVarKind { Toggle, Int, Float };

struct RendererCVar {
  const char* name;
  CVarKind kind;
  float default_value;
  float min_value;
  float max_value;
  const char* desc;
};

const RendererCVar kRendererCVars[] = {
    // Visualization toggles
    {"xr_showbounding", CVarKind::Toggle, 0, 0, 1, "Show bounding boxes"},
    {"xr_showtiming", CVarKind::Toggle, 0, 0, 1, "Show render timing info"},
    {"xr_showvbtime", CVarKind::Toggle, 0, 0, 1, "Show vertex buffer timing"},
    {"xr_showfoginfo", CVarKind::Toggle, 0, 0, 1, "Show fog info"},
    {"xr_showvelocity", CVarKind::Toggle, 0, 0, 1, "Show velocity vectors"},
    {"xr_showportalfence", CVarKind::Toggle, 0, 0, 1, "Show portal fences"},
    {"xr_worldonly", CVarKind::Toggle, 0, 0, 1, "Render world geometry only"},
    {"xr_objectsonly", CVarKind::Toggle, 0, 0, 1, "Render objects only"},
    {"xr_portalsonly", CVarKind::Toggle, 0, 0, 1, "Render portals only"},
    {"xr_freeze", CVarKind::Toggle, 0, 0, 1, "Freeze the renderer"},
    // Feature toggles
    {"xr_flares", CVarKind::Toggle, 1, 0, 1, "Lens flares"},
    {"xr_dlight", CVarKind::Toggle, 1, 0, 1, "Dynamic lights"},
    {"xr_fastlight", CVarKind::Toggle, 0, 0, 1, "Fast lighting mode"},
    {"xr_particles", CVarKind::Toggle, 1, 0, 1, "Particles"},
    {"xr_sky", CVarKind::Toggle, 1, 0, 1, "Sky rendering"},
    {"xr_wallmarks", CVarKind::Toggle, 1, 0, 1, "Wall marks / bullet decals"},
    {"xr_shadowdecals", CVarKind::Toggle, 1, 0, 1, "Shadow decals"},
    {"xr_stencilshadows", CVarKind::Toggle, 1, 0, 1, "Stencil shadows"},
    {"xr_synconrender", CVarKind::Toggle, 0, 0, 1, "Sync on render"},
    {"xr_optimizedsurfaces", CVarKind::Toggle, 1, 0, 1, "Optimized surfaces"},
    // Tuning
    {"xr_lodoffset", CVarKind::Float, 0.0f, -8.0f, 8.0f, "LOD offset"},
    {"xr_lodscale", CVarKind::Float, 1.0f, 0.1f, 8.0f, "LOD scale factor"},
    {"xr_maxportals", CVarKind::Int, 8, 0, 64, "Max portal recursion"},
    {"xr_maxrecursion", CVarKind::Int, 4, 0, 32, "Max recursion depth"},
    {"xr_portaltexturesize", CVarKind::Int, 256, 32, 1024, "Portal texture size"},
    {"xr_shadowdecaltexturesize", CVarKind::Int, 256, 32, 1024, "Shadow decal texture size"},
    {"xr_shadowmapmode", CVarKind::Int, 0, 0, 4, "Shadow map mode"},
    {"r_picmip", CVarKind::Int, 0, 0, 4, "Texture quality mip level (higher = blurrier)"},
    {"xr_debugfontsize", CVarKind::Float, 1.0f, 0.25f, 4.0f, "Debug font size"},
    // Post-processing
    {"xr_ppglowscale", CVarKind::Float, 1.0f, 0.0f, 4.0f, "Glow scale"},
    {"xr_ppglowbias", CVarKind::Float, 0.0f, -1.0f, 1.0f, "Glow bias"},
    {"xr_ppglowgamma", CVarKind::Float, 1.0f, 0.1f, 4.0f, "Glow gamma"},
    {"xr_ppglowexp", CVarKind::Float, 1.0f, 0.1f, 8.0f, "Glow exponent"},
    {"xr_ppexposurescale", CVarKind::Float, 1.0f, 0.0f, 4.0f, "Exposure scale"},
    {"xr_ppexposureexp", CVarKind::Float, 1.0f, 0.1f, 8.0f, "Exposure exponent"},
    {"xr_ppexposurecontrast", CVarKind::Float, 1.0f, 0.0f, 4.0f, "Exposure contrast"},
    {"xr_ppexposuresaturation", CVarKind::Float, 1.0f, 0.0f, 4.0f, "Exposure saturation"},
    {"xr_ppexposureblacklevel", CVarKind::Float, 0.0f, 0.0f, 1.0f, "Exposure black level"},
    {"xr_pptoggledynamicexposure", CVarKind::Toggle, 1, 0, 1, "Dynamic exposure"},
    {"xr_pptoggleexposuredebug", CVarKind::Toggle, 0, 0, 1, "Exposure debug display"},
    {"xr_ppmbmaxradius", CVarKind::Float, 1.0f, 0.0f, 8.0f, "Motion blur max radius"},
    {"xr_cc_gamma", CVarKind::Float, 1.0f, 0.1f, 4.0f, "Color correction gamma"},
    {"xr_cc_blacklevel", CVarKind::Float, 0.0f, 0.0f, 1.0f, "Color correction black level"},
};

// Host-side shadow of what this panel last sent for each renderer cvar.
std::map<std::string, float>& RendererShadow() {
  static std::map<std::string, float> shadow;
  return shadow;
}

std::string FormatCall(const char* name, float value, bool as_int) {
  char buf[128];
  if (as_int) {
    snprintf(buf, sizeof(buf), "%s(%d)", name, static_cast<int>(value));
  } else {
    snprintf(buf, sizeof(buf), "%s(%.4f)", name, value);
  }
  return buf;
}

bool ContainsCaseInsensitive(const char* haystack, const char* needle) {
  if (!needle || !*needle)
    return true;
  std::string h(haystack), n(needle);
  std::transform(h.begin(), h.end(), h.begin(), ::tolower);
  std::transform(n.begin(), n.end(), n.begin(), ::tolower);
  return h.find(n) != std::string::npos;
}

}  // namespace

namespace rex::ui {

DarknessDebugOverlay::DarknessDebugOverlay(ImGuiDrawer* imgui_drawer)
    : ImGuiDialog(imgui_drawer) {
  REXLOG_INFO("Darkness debug overlay created");
  log_lines_.emplace_back("Darkness debug console. Type a script call, e.g. noclip(1).");
  log_lines_.emplace_back("Cheat commands need the cheat gate open (see Cheat Gate).");
}

DarknessDebugOverlay::~DarknessDebugOverlay() { REXLOG_INFO("Debug overlay destroyed"); }

uint8_t* DarknessDebugOverlay::GetMembase() {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory())
    return nullptr;
  return ks->memory()->virtual_membase();
}

uint32_t DarknessDebugOverlay::GetPawn() { return Darkness_GetPlayerPawn(); }

uint32_t DarknessDebugOverlay::GetPawnFlag() {
  uint8_t* base = GetMembase();
  uint32_t pawn = GetPawn();
  if (!base || !pawn)
    return 0;
  return rex::memory::load_and_swap<uint32_t>(base + pawn + kCheatFlagOffset);
}

void DarknessDebugOverlay::SetPawnFlag(uint32_t flags) {
  uint8_t* base = GetMembase();
  uint32_t pawn = GetPawn();
  if (!base || !pawn)
    return;
  rex::memory::store_and_swap<uint32_t>(base + pawn + kCheatFlagOffset, flags);
}

// Read a game memory uint32 at pawn + offset.
uint32_t DarknessDebugOverlay::ReadPawnU32(uint32_t offset) {
  uint8_t* base = GetMembase();
  uint32_t pawn = GetPawn();
  if (!base || !pawn)
    return 0;
  return rex::memory::load_and_swap<uint32_t>(base + pawn + offset);
}

// Write a game memory uint32 at pawn + offset.
void DarknessDebugOverlay::WritePawnU32(uint32_t offset, uint32_t value) {
  uint8_t* base = GetMembase();
  uint32_t pawn = GetPawn();
  if (!base || !pawn)
    return;
  rex::memory::store_and_swap<uint32_t>(base + pawn + offset, value);
}

// Read a game memory float at pawn + offset.
float DarknessDebugOverlay::ReadPawnFloat(uint32_t offset) {
  uint8_t* base = GetMembase();
  uint32_t pawn = GetPawn();
  if (!base || !pawn)
    return 0.0f;
  return rex::memory::load_and_swap<float>(base + pawn + offset);
}

// ---------------------------------------------------------------------------
// Host cvar helpers
// ---------------------------------------------------------------------------

bool DarknessDebugOverlay::CVarBool(const char* name) {
  std::string v = rex::cvar::GetFlagByName(name);
  return v == "true" || v == "1";
}

void DarknessDebugOverlay::SetCVarBool(const char* name, bool value) {
  rex::cvar::SetFlagByName(name, value ? "true" : "false", /*persist=*/true);
}

int DarknessDebugOverlay::CVarInt(const char* name) {
  std::string v = rex::cvar::GetFlagByName(name);
  return v.empty() ? 0 : std::atoi(v.c_str());
}

void DarknessDebugOverlay::SetCVarInt(const char* name, int value) {
  rex::cvar::SetFlagByName(name, std::to_string(value), /*persist=*/true);
}

double DarknessDebugOverlay::CVarDouble(const char* name) {
  std::string v = rex::cvar::GetFlagByName(name);
  return v.empty() ? 0.0 : std::atof(v.c_str());
}

void DarknessDebugOverlay::SetCVarDouble(const char* name, double value) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.6f", value);
  rex::cvar::SetFlagByName(name, buf, /*persist=*/true);
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

void DarknessDebugOverlay::LogLine(const std::string& text) {
  log_lines_.push_back(text);
  // Keep the buffer bounded; this window can stay open for a whole session.
  if (log_lines_.size() > 512)
    log_lines_.erase(log_lines_.begin(), log_lines_.begin() + 128);
  scroll_log_to_bottom_ = true;
}

void DarknessDebugOverlay::SubmitConsoleLine(const std::string& line) {
  if (line.empty())
    return;
  LogLine("> " + line);
  Darkness_QueueConsoleCommand(line.c_str());

  // Dedupe consecutive repeats so holding Enter doesn't flood the history.
  if (history_.empty() || history_.back() != line)
    history_.push_back(line);
  if (history_.size() > 128)
    history_.erase(history_.begin());
  history_pos_ = -1;
}

int DarknessDebugOverlay::ConsoleInputCallback(ImGuiInputTextCallbackData* data) {
  auto* self = static_cast<DarknessDebugOverlay*>(data->UserData);
  if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || self->history_.empty())
    return 0;

  const int count = static_cast<int>(self->history_.size());
  if (data->EventKey == ImGuiKey_UpArrow) {
    if (self->history_pos_ == -1)
      self->history_pos_ = count - 1;
    else if (self->history_pos_ > 0)
      self->history_pos_--;
  } else if (data->EventKey == ImGuiKey_DownArrow) {
    if (self->history_pos_ != -1 && ++self->history_pos_ >= count)
      self->history_pos_ = -1;
  }

  const char* replacement = self->history_pos_ >= 0 ? self->history_[self->history_pos_].c_str() : "";
  data->DeleteChars(0, data->BufTextLen);
  data->InsertChars(0, replacement);
  return 0;
}

void DarknessDebugOverlay::DrawConsole() {
  ImGui::TextWrapped(
      "Lines go to the game's own script console (CConsole::ExecuteString). "
      "Syntax is a script call: name(args), e.g. noclip(1) or xr_lodscale(2.0).");
  ImGui::TextDisabled("Output goes to the game log, not here -- this pane echoes input only.");

  const float input_height = ImGui::GetFrameHeightWithSpacing();
  if (ImGui::BeginChild("##conlog", ImVec2(0, -input_height - 4.0f), true,
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    for (const auto& line : log_lines_) {
      if (!line.empty() && line[0] == '>')
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", line.c_str());
      else
        ImGui::TextUnformatted(line.c_str());
    }
    if (scroll_log_to_bottom_) {
      ImGui::SetScrollHereY(1.0f);
      scroll_log_to_bottom_ = false;
    }
  }
  ImGui::EndChild();

  ImGui::SetNextItemWidth(-70.0f);
  const ImGuiInputTextFlags flags =
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
  bool submitted = ImGui::InputText("##coninput", input_buf_, sizeof(input_buf_), flags,
                                    &DarknessDebugOverlay::ConsoleInputCallback, this);
  if (refocus_input_) {
    ImGui::SetKeyboardFocusHere(-1);
    refocus_input_ = false;
  }
  ImGui::SameLine();
  if (ImGui::Button("Send", ImVec2(-1, 0)))
    submitted = true;

  if (submitted) {
    SubmitConsoleLine(input_buf_);
    input_buf_[0] = '\0';
    refocus_input_ = true;
  }
}

void DarknessDebugOverlay::DrawCommandReference() {
  ImGui::SetNextItemWidth(-60.0f);
  ImGui::InputTextWithHint("##cmdfilter", "filter...", command_filter_, sizeof(command_filter_));
  ImGui::SameLine();
  if (ImGui::Button("Clear##cmdfilter"))
    command_filter_[0] = '\0';
  ImGui::TextDisabled("Click a command to put it in the console input.");

  for (const auto& group : kCommandGroups) {
    // Only show a group header if something in it survives the filter.
    bool any = false;
    for (size_t i = 0; i < group.count; ++i) {
      if (ContainsCaseInsensitive(group.items[i].insert, command_filter_) ||
          ContainsCaseInsensitive(group.items[i].desc, command_filter_)) {
        any = true;
        break;
      }
    }
    if (!any)
      continue;

    if (!ImGui::TreeNodeEx(group.name, ImGuiTreeNodeFlags_DefaultOpen))
      continue;
    for (size_t i = 0; i < group.count; ++i) {
      const CommandDoc& doc = group.items[i];
      if (!ContainsCaseInsensitive(doc.insert, command_filter_) &&
          !ContainsCaseInsensitive(doc.desc, command_filter_))
        continue;
      if (ImGui::Selectable(doc.insert)) {
        snprintf(input_buf_, sizeof(input_buf_), "%s", doc.insert);
        refocus_input_ = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", doc.desc);
      ImGui::SameLine(260.0f);
      ImGui::TextDisabled("%s", doc.desc);
    }
    ImGui::TreePop();
  }

  // Reference only -- deliberately not clickable, since putting these in the
  // console input would suggest they run.
  if (ImGui::TreeNodeEx("Input action IDs (NOT callable)")) {
    ImGui::TextWrapped(
        "These names resolve to an integer and nothing else. They identify "
        "input actions for keybind scripts; the behaviour fires when the "
        "input system dispatches the ID, not when the name is invoked.");
    for (const auto& a : kActionIdConstants) {
      if (!ContainsCaseInsensitive(a.name, command_filter_) &&
          !ContainsCaseInsensitive(a.desc, command_filter_))
        continue;
      ImGui::TextDisabled("%-22s = %-3d  %s", a.name, a.id, a.desc);
    }
    ImGui::TreePop();
  }
}

// ---------------------------------------------------------------------------
// Host (emulator) settings
// ---------------------------------------------------------------------------

void DarknessDebugOverlay::DrawHostSettings() {
  ImGui::TextDisabled("These are host rex cvars, saved to the config on change.");

  // --- Frame rate ---
  // The game has no framerate cap of its own; it is paced by the emulated
  // vblank. The SDK's vsync worker (graphics_system.cpp) marks a vblank every
  // 1/video_mode_refresh_rate second while `vsync` is true, and every 1ms
  // (i.e. effectively uncapped, ~1000Hz) while it is false. So `vsync` is the
  // FPS cap, and it is the one knob here that takes effect immediately.
  ImGui::SeparatorText("Frame rate");
  bool vsync = CVarBool("vsync");
  if (ImGui::Checkbox("vsync (guest vblank pacing)", &vsync))
    SetCVarBool("vsync", vsync);
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "On: the guest gets a vblank at the refresh rate below -- this is the FPS cap.\n"
        "Off: vblanks fire every 1ms, effectively uncapping the guest.\n"
        "Takes effect immediately. Game logic is tied to this, so uncapping\n"
        "can run systems faster than the engine expects.");
  }

  double refresh = CVarDouble("video_mode_refresh_rate");
  float refresh_f = static_cast<float>(refresh);
  if (ImGui::SliderFloat("refresh rate (Hz)", &refresh_f, 24.0f, 240.0f, "%.0f"))
    SetCVarDouble("video_mode_refresh_rate", refresh_f);
  ImGui::TextDisabled("Requires a restart: the vsync worker samples this once at GPU init.");

  bool tearing = CVarBool("d3d12_allow_variable_refresh_rate_and_tearing");
  if (ImGui::Checkbox("allow VRR / tearing (D3D12)", &tearing))
    SetCVarBool("d3d12_allow_variable_refresh_rate_and_tearing", tearing);

  // --- Resolution / image quality ---
  ImGui::SeparatorText("Resolution & image quality");
  int res_scale_x = CVarInt("draw_resolution_scale_x");
  if (ImGui::SliderInt("draw scale X", &res_scale_x, 1, 3))
    SetCVarInt("draw_resolution_scale_x", res_scale_x);
  int res_scale_y = CVarInt("draw_resolution_scale_y");
  if (ImGui::SliderInt("draw scale Y", &res_scale_y, 1, 3))
    SetCVarInt("draw_resolution_scale_y", res_scale_y);
  ImGui::TextDisabled("Internal render resolution multiplier. Restart to apply.");

  int aniso = CVarInt("anisotropic_override");
  if (ImGui::SliderInt("anisotropic override", &aniso, -1, 16))
    SetCVarInt("anisotropic_override", aniso);
  ImGui::TextDisabled("-1 keeps the game's own setting.");

  ImGui::SeparatorText("Window");
  bool fullscreen = CVarBool("fullscreen");
  if (ImGui::Checkbox("fullscreen", &fullscreen))
    SetCVarBool("fullscreen", fullscreen);
  bool letterbox = CVarBool("present_letterbox");
  if (ImGui::Checkbox("letterbox", &letterbox))
    SetCVarBool("present_letterbox", letterbox);

  ImGui::SeparatorText("Input & audio");
  double sens = CVarDouble("mnk_sensitivity");
  float sens_f = static_cast<float>(sens);
  if (ImGui::SliderFloat("mouse sensitivity", &sens_f, 0.05f, 5.0f, "%.2f"))
    SetCVarDouble("mnk_sensitivity", sens_f);
  bool capture = CVarBool("mnk_capture_mouse");
  if (ImGui::Checkbox("capture mouse", &capture))
    SetCVarBool("mnk_capture_mouse", capture);
  bool mute = CVarBool("audio_mute");
  if (ImGui::Checkbox("mute audio", &mute))
    SetCVarBool("audio_mute", mute);

  ImGui::SeparatorText("Dev");
  bool shader_dump = CVarBool("shader_dump_enabled");
  if (ImGui::Checkbox("dump shaders", &shader_dump))
    SetCVarBool("shader_dump_enabled", shader_dump);
  bool tex_dump = CVarBool("texture_dump_enabled");
  if (ImGui::Checkbox("dump textures", &tex_dump))
    SetCVarBool("texture_dump_enabled", tex_dump);
  bool gpu_markers = CVarBool("gpu_debug_markers");
  if (ImGui::Checkbox("GPU debug markers", &gpu_markers))
    SetCVarBool("gpu_debug_markers", gpu_markers);
}

// ---------------------------------------------------------------------------
// Renderer cvars (write-only, via the script console)
// ---------------------------------------------------------------------------

void DarknessDebugOverlay::DrawRendererCVars() {
  ImGui::TextWrapped(
      "Engine renderer cvars. These are script functions, and the script VM has "
      "no by-name read path, so these controls are WRITE-ONLY: each one shows "
      "the last value this panel sent, not the engine's actual current value.");

  auto& shadow = RendererShadow();
  for (const auto& cv : kRendererCVars) {
    if (!ContainsCaseInsensitive(cv.name, command_filter_) &&
        !ContainsCaseInsensitive(cv.desc, command_filter_))
      continue;

    auto it = shadow.find(cv.name);
    if (it == shadow.end())
      it = shadow.emplace(cv.name, cv.default_value).first;
    float& value = it->second;

    ImGui::PushID(cv.name);
    switch (cv.kind) {
      case CVarKind::Toggle: {
        bool on = value != 0.0f;
        if (ImGui::Checkbox(cv.name, &on)) {
          value = on ? 1.0f : 0.0f;
          SubmitConsoleLine(FormatCall(cv.name, value, /*as_int=*/true));
        }
        break;
      }
      case CVarKind::Int: {
        int v = static_cast<int>(value);
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderInt(cv.name, &v, static_cast<int>(cv.min_value),
                             static_cast<int>(cv.max_value))) {
          value = static_cast<float>(v);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
          SubmitConsoleLine(FormatCall(cv.name, value, /*as_int=*/true));
        break;
      }
      case CVarKind::Float: {
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat(cv.name, &value, cv.min_value, cv.max_value, "%.3f");
        if (ImGui::IsItemDeactivatedAfterEdit())
          SubmitConsoleLine(FormatCall(cv.name, value, /*as_int=*/false));
        break;
      }
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", cv.desc);
    ImGui::PopID();
  }
}

// ---------------------------------------------------------------------------

void DarknessDebugOverlay::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 500.0f, 10.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(480.0f, 700.0f), ImGuiCond_FirstUseEver);

  if (!ImGui::Begin("The Darkness Debug##debug", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  uint32_t pawn = GetPawn();
  bool has_pawn = (pawn != 0);
  ImGui::Text("Pawn: 0x%08X", pawn);
  if (!has_pawn) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "(no pawn yet)");
  }

  if (!ImGui::BeginTabBar("##debugtabs")) {
    ImGui::End();
    return;
  }

  // --- Console ---
  if (ImGui::BeginTabItem("Console")) {
    if (!has_pawn) {
      ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
                         "Queued lines only run once the game is in-world.");
    }
    DrawConsole();
    ImGui::EndTabItem();
  }

  // --- Commands ---
  if (ImGui::BeginTabItem("Commands")) {
    DrawCommandReference();
    ImGui::EndTabItem();
  }

  // Fires a directly-callable guest handler (see DebugCommandIndex above):
  // queues it for the per-frame guest-thread hook to invoke against the live
  // pawn. Kept alongside the console path because it skips the script compile
  // and name lookup, so it works even if a name or argument shape is wrong in
  // the console tables.
  auto direct_button = [&](const char* label, DebugCommandIndex index, const char* arg = "") {
    ImGui::BeginDisabled(!has_pawn);
    if (ImGui::Button(label, ImVec2(-1, 0)))
      Darkness_QueueDebugCommand(Darkness_GetDebugCommandFn(index), arg);
    ImGui::EndDisabled();
  };

  // Sends a console line; disabled until there's a pawn, since the script VM
  // and the global service locator it lives behind aren't up before then.
  auto console_button = [&](const char* label, const char* command) {
    ImGui::BeginDisabled(!has_pawn);
    if (ImGui::Button(label, ImVec2(-1, 0)))
      SubmitConsoleLine(command);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", command);
  };

  // --- Quick actions ---
  if (ImGui::BeginTabItem("Quick")) {
    ImGui::TextDisabled("Direct handlers:");
    ImGui::Columns(2, nullptr, false);
    direct_button("Toggle Debug Cam", kCmdToggleDebugCamera);
    ImGui::NextColumn();
    direct_button("Debug Camera 2", kCmdToggleDebugCamera2);
    ImGui::NextColumn();
    direct_button("Toggle HUD", kCmdToggleShowHud);
    ImGui::NextColumn();
    direct_button("Dump Textures", kCmdDumpTextureUsage);
    ImGui::NextColumn();
    direct_button("Shader Diff", kCmdShaderDiff);
    ImGui::NextColumn();
    direct_button("Shader Spec", kCmdShaderSpec);
    ImGui::NextColumn();
    direct_button("Darkness Vision", kCmdDarknessVision);
    ImGui::NextColumn();
    direct_button("Creeping Dark", kCmdCreepingDark);
    ImGui::NextColumn();
    direct_button("Otherworld", kCmdOtherworld);
    ImGui::NextColumn();
    direct_button("Retina Dots", kCmdRetinaDots);
    ImGui::NextColumn();
    ImGui::Columns(1);

    // showdebughud (sub_823FDD28) runs fine but has no visible effect: it just
    // flips a flag by sending game messages 4418/4419 through the pawn's
    // dispatcher. Whatever consumes that flag evidently has nothing to draw
    // here. Kept separate so its label can say so rather than looking broken.
    direct_button("Toggle Debug HUD", kCmdShowDebugHud);
    ImGui::TextDisabled("^ runs, but toggles a flag with no visible output.");

    // showinfoscreen is the one handler that actually reads its string
    // argument -- it copies the text into a 252-byte buffer on the pawn.
    // Empirically the string is the text itself, not a lookup key: any
    // non-empty value brings the screen up (as a full-screen grey panel) and
    // an empty one dismisses it. Why it renders grey rather than showing the
    // text is not established, so "Hide" is offered explicitly instead of
    // leaving the only exit to be guessed.
    ImGui::Separator();
    ImGui::TextDisabled("Info screen text (any text shows it, empty hides it):");
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputText("##infotext", info_screen_text_, sizeof(info_screen_text_));
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_pawn);
    if (ImGui::Button("Show##info", ImVec2(-1, 0)))
      Darkness_QueueDebugCommand(Darkness_GetDebugCommandFn(kCmdShowInfoScreen),
                                 info_screen_text_);
    if (ImGui::Button("Hide info screen", ImVec2(-1, 0)))
      Darkness_QueueDebugCommand(Darkness_GetDebugCommandFn(kCmdShowInfoScreen), "");
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextDisabled("Via the script console:");
    console_button("Unlock All", "unlockall()");
    console_button("Save Profile", "saveprofile()");
    console_button("Load Checkpoint", "loadgame('checkpoint')");

    // Cheats. Only noclip is traced so far; it calls sub_82133C58 on the
    // player character, resolved through the engine's entity-ID lookup rather
    // than through the (uncallable) command-packet path.
    ImGui::Separator();
    ImGui::SeparatorText("Cheats");

    uint32_t entity_id = Darkness_GetPlayerEntityId();
    uint32_t container = Darkness_GetEntityContainer();

    if (entity_id == 0xFFFFFFFF)
      ImGui::TextDisabled("PLAYEROBJ id: (unset)");
    else
      ImGui::Text("PLAYEROBJ id: %u (0x%X)", entity_id, entity_id);
    if (container)
      ImGui::Text("Entity container: 0x%08X", container);
    else
      ImGui::TextDisabled("Entity container: not observed yet");

      ImGui::Separator();
      ImGui::TextDisabled("Memory-toggle cheats (no guest call, safe):");
      ImGui::Columns(2, nullptr, false);
      bool aim = Darkness_IsAimAssistActive();
      if (ImGui::Checkbox("Aim Assist", &aim))
        Darkness_QueueToggleAimAssist();
      ImGui::NextColumn();
      bool cam = Darkness_IsCycleCameraActive();
      if (ImGui::Checkbox("Cycle Camera", &cam))
        Darkness_QueueCycleCamera();
      ImGui::NextColumn();
      bool nv = Darkness_IsNightVisionActive();
      if (ImGui::Checkbox("Night Vision", &nv))
        Darkness_QueueToggleNightVision();
      ImGui::NextColumn();
      if (ImGui::Button("Demon Arm L1", ImVec2(-1, 0)))
        Darkness_QueueDemonArm(1);
      ImGui::NextColumn();
      if (ImGui::Button("Demon Arm L2", ImVec2(-1, 0)))
        Darkness_QueueDemonArm(2);
      ImGui::NextColumn();
      if (ImGui::Button("Skip Cutscene", ImVec2(-1, 0)))
        Darkness_QueueSkipCutscene();
      ImGui::NextColumn();
      if (ImGui::Button("Kill", ImVec2(-1, 0)))
        Darkness_QueueKill();
      ImGui::NextColumn();
      if (ImGui::Button("Give All", ImVec2(-1, 0)))
        Darkness_QueueGiveAll();
      ImGui::Columns(1);

      ImGui::Separator();
      ImGui::TextDisabled("Darkness levels:");
      ImGui::Columns(5, nullptr, false);
      for (uint32_t dl = 0; dl < 5; ++dl) {
        char label[32];
        snprintf(label, sizeof(label), "L%d", dl + 1);
        if (ImGui::Button(label, ImVec2(-1, 0)))
          Darkness_QueueDarknessLevel(dl);
        ImGui::NextColumn();
      }
      ImGui::Columns(1);

      uint32_t player = Darkness_GetPlayerCharacter();
      ImGui::Separator();
      if (player) {
      uint32_t move_mode = Darkness_GetPlayerMoveMode();
      ImGui::Text("Player character: 0x%08X", player);
      ImGui::Text("Move mode: %u %s", move_mode, move_mode == 4 ? "(NOCLIP)" : "");
      if (Darkness_IsNoclipBlocked())
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Noclip blocked (flags & 0x100000)");

      if (ImGui::Button("Toggle NoClip", ImVec2(-1, 0)))
        Darkness_QueueNoclipOn(player, 4);
      if (ImGui::Button("Toggle NoClip2", ImVec2(-1, 0)))
        Darkness_QueueNoclipOn(player, 6);

      // sub_82133C58 is a toggle (mode 4 -> back to 2), but sub_82132F10 can
      // refuse -- leaving noclip needs somewhere legal to stand. The engine
      // prints "Unable to switch from noclip." when that happens, which is
      // easy to miss, so say it here too.
      if (Darkness_WasNoclipRefused()) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
                           "Engine refused the last switch.");
        ImGui::TextWrapped(
            "Leaving noclip needs room to stand -- fly somewhere open, clear of "
            "walls and floors, and toggle again.");
      }

      ImGui::Separator();
      bool god_available = Darkness_IsGodmodeAvailable();
      bool god = Darkness_IsGodmodeActive();
      ImGui::BeginDisabled(!god_available);
      if (ImGui::Checkbox("God mode", &god))
        Darkness_QueueGodmode();
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::TextDisabled("(bit 0x200 in state+0x2800)");
    } else {
      ImGui::TextWrapped(
          "Player character not identified yet. It is picked up when the engine "
          "switches your movement mode -- walk, crouch, jump or land.");
    }

    // Kept as a fallback and as the evidence behind the auto-pick. Rows where
    // "chr" is blank are not characters -- their class lacks the player-command
    // handler at vtable+0x158 -- and those are exactly the attachments that
    // share the player's entity id and crash sub_82133C58.
    ImGui::Separator();
    if (ImGui::CollapsingHeader("All observed objects")) {
      int count = Darkness_GetCharacterCandidateCount();
      ImGui::Text("Objects seen: %d", count);
      ImGui::SameLine();
      if (ImGui::Button("Clear list"))
        Darkness_ClearCharacterCandidates();

      if (ImGui::BeginTable("##chars", 7,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("object");
        ImGui::TableSetupColumn("id");
        ImGui::TableSetupColumn("gen");
        ImGui::TableSetupColumn("mode");
        ImGui::TableSetupColumn("hits");
        ImGui::TableSetupColumn("chr");
        ImGui::TableSetupColumn("noclip");
        ImGui::TableHeadersRow();

        for (int i = 0; i < count; ++i) {
          uint32_t ptr = 0, vtable = 0, id = 0, gen = 0, mode = 0, flags = 0;
          uint64_t hits = 0;
          bool is_character = false;
          if (!Darkness_GetCharacterCandidate(i, &ptr, &vtable, &id, &gen, &mode, &flags, &hits,
                                              &is_character))
            continue;

          ImGui::TableNextRow();
          ImGui::PushID(i);

          ImGui::TableNextColumn();
          if (ptr == player)
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1), "0x%08X", ptr);
          else
            ImGui::Text("0x%08X", ptr);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("vtable 0x%08X\nflags 0x%08X", vtable, flags);

          ImGui::TableNextColumn();
          ImGui::Text("%u", id);
          ImGui::TableNextColumn();
          ImGui::Text("%u", gen);

          ImGui::TableNextColumn();
          if (mode == 4)
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1), "4 (NOCLIP)");
          else
            ImGui::Text("%u", mode);

          ImGui::TableNextColumn();
          ImGui::Text("%llu", static_cast<unsigned long long>(hits));

          ImGui::TableNextColumn();
          ImGui::TextUnformatted(is_character ? "yes" : "");

          ImGui::TableNextColumn();
          // sub_82133C58 bails out entirely when this bit is set, and a
          // non-character has no business being passed to it at all.
          bool blocked = (flags & 0x100000) != 0 || !is_character;
          ImGui::BeginDisabled(blocked);
          if (ImGui::SmallButton("toggle"))
            Darkness_QueueNoclipOn(ptr, 4);
          ImGui::SameLine();
          if (ImGui::SmallButton("alt"))
            Darkness_QueueNoclipOn(ptr, 6);
          ImGui::EndDisabled();
          if (blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(is_character
                                  ? "flags & 0x100000 set -- sub_82133C58 refuses this one"
                                  : "not a character (no command handler at vtable+0x158)");
          }

          ImGui::PopID();
        }
        ImGui::EndTable();
      }
    }

    ImGui::TextWrapped(
        "Memory-toggled: aim assist, cycle camera, night vision, demon arm (L1/L2), darkness L1-L5. "
        "Guest-call: noclip, noclip2, skip cutscene, kill. "
        "Giveall is still pending.");
    ImGui::EndTabItem();
  }

  // --- Renderer ---
  if (ImGui::BeginTabItem("Renderer")) {
    ImGui::SetNextItemWidth(-60.0f);
    ImGui::InputTextWithHint("##rfilter", "filter...", command_filter_, sizeof(command_filter_));
    ImGui::SameLine();
    if (ImGui::Button("Clear##rfilter"))
      command_filter_[0] = '\0';
    ImGui::BeginDisabled(!has_pawn);
    if (ImGui::BeginChild("##rcvars", ImVec2(0, 0), true))
      DrawRendererCVars();
    ImGui::EndChild();
    ImGui::EndDisabled();
    ImGui::EndTabItem();
  }

  // --- Host settings ---
  if (ImGui::BeginTabItem("Settings")) {
    if (ImGui::BeginChild("##hostsettings", ImVec2(0, 0), false))
      DrawHostSettings();
    ImGui::EndChild();
    ImGui::EndTabItem();
  }

  // --- Player state ---
  if (ImGui::BeginTabItem("Player")) {
    if (ImGui::CollapsingHeader("Cheat Gate", ImGuiTreeNodeFlags_DefaultOpen)) {
      if (has_pawn) {
        uint32_t flags = GetPawnFlag();
        bool bit_set = (flags & kCheatFlagBit) != 0;
        ImGui::Text("Flags: 0x%08X", flags);
        ImGui::Text("Status: %s", bit_set ? "CHEATS BLOCKED" : "CHEATS OPEN");

        if (bit_set) {
          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
          if (ImGui::Button("Enable Cheats (Clear Bit)", ImVec2(-1, 0)))
            SetPawnFlag(flags & ~kCheatFlagBit);
          ImGui::PopStyleColor();
        } else {
          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
          if (ImGui::Button("Disable Cheats (Set Bit)", ImVec2(-1, 0)))
            SetPawnFlag(flags | kCheatFlagBit);
          ImGui::PopStyleColor();
        }
        ImGui::TextWrapped(
            "This bit gates whether the cheat commands were registered at all. "
            "It is read when the command table is built (sub_823FF1D0), so "
            "flipping it now may not retroactively register them.");
      } else {
        ImGui::TextDisabled("Wait for game to start...");
      }
    }

    if (has_pawn && ImGui::CollapsingHeader("State", ImGuiTreeNodeFlags_DefaultOpen)) {
      // The old "NoClip (direct poke)" checkbox lived here. It was wrong on
      // both counts: the noclip flag is at 0x16C, not 692, and it is on the
      // *character*, not this object. Use the Quick tab's Toggle NoClip.
      float yaw = ReadPawnFloat(0x227C);
      float pitch = ReadPawnFloat(0x2280);
      ImGui::Text("Look Vel: %.1f / %.1f", yaw, pitch);

      float accel_yaw = ReadPawnFloat(0x2290);
      float accel_pitch = ReadPawnFloat(0x2294);
      ImGui::Text("Turn accel: %.1f / %.1f", accel_yaw, accel_pitch);
    }
    ImGui::EndTabItem();
  }

  ImGui::EndTabBar();
  ImGui::End();
}

}  // namespace rex::ui
