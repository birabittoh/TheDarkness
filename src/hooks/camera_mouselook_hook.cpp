// Camera mouselook for The Darkness (direct look-velocity injection).
//
// Earlier approaches wrote into the analog-stick pipeline (sub_82793FE8) and
// could not be made to feel right -- that pipeline feeds a velocity-integration
// camera and no value shape at that layer works. This hook instead writes the
// mouse delta straight into the pawn's look-velocity *input* fields each frame,
// bypassing the deadzone/response-curve/latch entirely while keeping the
// engine's own rot-velocity easing + friction. See PlayerLookVelocityHook
// below.
//
// mnk_sensitivity is read via REXCVAR_QUERY (registry-by-name), not
// REXCVAR_GET/REXCVAR_DECLARE (direct storage_() call): mnk_input_driver.cpp
// -- and its REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, ...) -- ends up compiled
// into more than one linked module (this exe vs. rexruntimerd.dll), so each
// gets its own independent FLAGS_mnk_sensitivity_storage_() static behind
// the same registry name (see the duplicate-registration handling in
// rexglue-sdk's cvar.cpp RegisterFlag). REXCVAR_GET here would resolve to
// whichever copy this module linked, which is NOT necessarily the one
// SetFlagByName (console/config) actually updates after startup, so runtime
// sensitivity changes were silently ignored. Query() always goes through
// the single shared registry and its currently-registered getter, so it
// sees live changes regardless of which module's storage owns the value.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/ppc/stack.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

// Debug-overlay command dispatch.
//
// The debug overlay (src/debug_overlay.cpp) runs its ImGui draw callback off
// the guest CPU thread (it's driven by the presenter/UI thread), so it cannot
// safely call into recompiled guest code directly: rex::ppc::ImportFunction's
// auto-isolating call needs a live rex::runtime::ThreadState for *this*
// thread, which only exists on the guest thread that's actually executing
// PPC code. Calling it from the UI thread would either crash (no
// ThreadState) or -- if some other guest thread happened to have one bound
// to TLS -- corrupt that unrelated thread's call stack.
//
// Instead, the overlay just stashes the address of the guest handler it
// wants run into g_pending_debug_command (a single-slot queue -- these are
// all fire-and-forget toggles, never queued faster than one per frame), and
// PlayerLookVelocityHook -- which already runs on the guest thread once per
// frame -- drains and invokes it there via the same ImportFunction machinery
// REX_IMPORT generates.
// Every one of these handlers takes TWO guest arguments: (pawn, CString*).
//
// This was originally typed as uint32_t(uint32_t) on the belief that they were
// plain `(*(vtable+N))(pawn)` trampolines. They are not: the trampoline is
// `lwz r12,0(r3); lwz r11,N(r12); bctr`, which tail-calls with *whatever r4
// the caller had*. Every registrar in sub_823FF1D0 (sub_82400E58 /
// sub_82400F98 / sub_824010D8) stores arity 1 -- they differ only in the
// parameter's type descriptor -- so all of these commands are declared to take
// one argument.
//
// Calling them with one argument therefore leaves r4 as garbage, and whether
// that is survivable depends entirely on the handler. `toggledebugcamera`
// (loc_823FB188) overwrites r4 with a computed value before tail-calling, so
// it never noticed; `showinfoscreen` (sub_823FCA88) dereferences r4 as a
// refcounted engine string and releases it, which is why it crashed
// instantly. Passing a real string closes that hole for every handler,
// including the ones nobody has pressed yet.
using DebugCommandFn = rex::ppc::ImportFunction<uint32_t(uint32_t, uint32_t)>;

struct PendingDebugCommand {
  DebugCommandFn* fn;
  std::string arg;
};

static std::mutex g_debug_command_mutex;
static PendingDebugCommand g_pending_debug_command{nullptr, {}};

extern "C" void Darkness_QueueDebugCommand(void* fn, const char* arg) {
  std::lock_guard<std::mutex> lock(g_debug_command_mutex);
  g_pending_debug_command.fn = static_cast<DebugCommandFn*>(fn);
  g_pending_debug_command.arg = arg ? arg : "";
}

// Named debug/console commands from DEBUG_MENUS_REVERSE_ENGINEERING.md
// section 3. Calling the guest handler directly reproduces exactly what
// pressing the bound key would do, without going through the game's script
// console -- useful as a second, simpler path when a console name or argument
// shape is in doubt.
REX_IMPORT(sub_82401370, Darkness_ToggleDebugCamera, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_82401460, Darkness_ToggleDebugCamera2, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_824015B0, Darkness_ShowDebugHud, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_824015F0, Darkness_ToggleShowHud, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_824013C0, Darkness_ShowInfoScreen, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_82401500, Darkness_ShaderDiff, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_82401330, Darkness_ShaderSpec, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_82401350, Darkness_DarknessVision, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_82401440, Darkness_CreepingDark, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_82401540, Darkness_Otherworld, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_82401650, Darkness_RetinaDots, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_82401560, Darkness_DumpTextureUsage, uint32_t(uint32_t, uint32_t));

extern "C" void* Darkness_GetDebugCommandFn(int index) {
  static DebugCommandFn* const kTable[] = {
      &Darkness_ToggleDebugCamera,  &Darkness_ToggleDebugCamera2, &Darkness_ShowDebugHud,
      &Darkness_ToggleShowHud,      &Darkness_ShowInfoScreen,     &Darkness_ShaderDiff,
      &Darkness_ShaderSpec,         &Darkness_DarknessVision,     &Darkness_CreepingDark,
      &Darkness_Otherworld,         &Darkness_RetinaDots,         &Darkness_DumpTextureUsage,
  };
  if (index < 0 || static_cast<size_t>(index) >= std::size(kTable))
    return nullptr;
  return kTable[index];
}

// ===========================================================================
// Guest console (CConsole::ExecuteString) bridge
// ===========================================================================
//
// The game's "console commands" are not a name->handler table the host can
// poke: every one of them (`noclip`, `godmode`, all the `xr_*` renderer
// cvars, `saveprofile`, ...) is registered as a *script function* into the
// engine's script VM, and the console runs a typed line by compiling it as a
// tiny script and executing its `main`. So the only way to invoke them
// generically is to hand the engine the same string a real console would.
//
// The entry point is sub_8277B388 -- a free function (no `this`) that pulls
// the console service out of the global service locator at 0x82A690F8 and
// forwards to CConsole::ExecuteString (sub_82782E28, identified by the
// "CConsole::ExecuteString" tag literal its callers pass). Reproducing the
// exact call sequence the game itself uses (e.g. sub_8229FA10, which runs
// "loadgame('checkpoint')" this way):
//
//   sub_821F86A8(&src, "text")      // engine-string ctor from a C string
//   sub_821F8918(&arg, &src)        // copy ctor (callee takes ownership)
//   sub_8277B388(&arg, "ConExecute", 0)   // compile + run; releases `arg`
//   sub_821F8AD0(&src)              // release our own copy
//
// Syntax is script-call syntax, not id Tech style: `noclip(1)`,
// `xr_lodscale(2.0)`, `loadgame('checkpoint')`, `saveprofile()`.
REX_IMPORT(sub_821F86A8, Darkness_StrFromCStr, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_821F8918, Darkness_StrCopy, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_8277B388, Darkness_ConExecuteStr, uint32_t(uint32_t, uint32_t, uint32_t));
REX_IMPORT(sub_821F8AD0, Darkness_StrRelease, uint32_t(uint32_t));

// Queue rather than direct call, for the same reason as g_pending_debug_command:
// the overlay's ImGui draw runs on the UI thread with no ThreadState bound, so
// it cannot enter guest code. The per-frame hook below drains this on the guest
// thread. Unlike the single-slot command queue this is a real FIFO -- a user
// can submit several lines (or hold Enter) between two hook invocations, and
// silently dropping console input would be worse than a one-frame delay.
static std::mutex g_console_queue_mutex;
static std::deque<std::string> g_console_queue;

extern "C" void Darkness_QueueConsoleCommand(const char* text) {
  if (!text || !*text)
    return;
  std::lock_guard<std::mutex> lock(g_console_queue_mutex);
  // Bound the queue so a stuck guest thread can't grow it without limit.
  if (g_console_queue.size() < 64)
    g_console_queue.emplace_back(text);
}

// ===========================================================================
// Cheats (noclip)
// ===========================================================================
//
// The `noclip` etc. names registered by sub_82401178 are named integer
// CONSTANTS (input action IDs), not commands -- invoking one just evaluates to
// its ID. The real implementation is reached by the input system dispatching
// that ID through a command packet into sub_82126828, which is a packet
// deserializer and not callable. But the functions it dispatches *to* are
// perfectly callable once you hold the right object:
//
//   sub_82133C58(character, mode)   toggles noclip.  mode 4 = noclip,
//                                   6 = noclip2 (the two call sites in
//                                   sub_82126828). It reads the current mode
//                                   from (character+0x1A4 >> 12) & 0xF and
//                                   switches back to 2 if already 4, so a
//                                   single call is a toggle. Bails out if
//                                   character+0x16C & 0x100000 is set.
//
// The catch was that this wants the *character*, a different object from the
// controller this hook captures. Resolving one from the other:
//
//   sub_8249D618(controller, entity_id) -> entity object
//
// is the engine's entity-ID lookup (two ID ranges split at +3956, arrays at
// +3940/+3952, validating the returned object's own ID at +0x170). That the
// controller is also the entity container is not an assumption -- it is what
// sub_824A6180 (the controller's own vtable+332 message send) does: it keeps
// r3 untouched across `bl sub_8249D618` and only moves the target ID into r4.
//
// The player's entity ID is controller+0x218, written by sub_823ED958 from the
// "PLAYEROBJ" config entry (and +0x21C is "GAMEOBJ"), defaulting to -1.
//
// So there is no ambiguity about which character we get, and no need to snoop
// pointers out of NPC code paths.
REX_IMPORT(sub_8249D618, Darkness_GetEntityById, uint32_t(uint32_t, uint32_t));
REX_IMPORT(sub_82133C58, Darkness_ToggleNoclip, uint32_t(uint32_t, uint32_t));

static constexpr uint32_t kControllerPlayerObjId = 0x218;
static constexpr uint32_t kCharacterFlagsOffset = 0x16C;
static constexpr uint32_t kCharacterNoclipBlockedMask = 0x100000;
static constexpr uint32_t kCharacterModeOffset = 0x1A4;

// Published for the overlay so it can show what was resolved rather than
// making the user guess why a button did nothing. Refreshed once per frame on
// the guest thread; read (racily, but they are plain word-sized snapshots) by
// the UI thread.
static std::atomic<uint32_t> g_player_entity_id{0xFFFFFFFF};
static std::atomic<uint32_t> g_player_character{0};
static std::atomic<uint32_t> g_player_move_mode{0};
static std::atomic<bool> g_noclip_blocked{false};
static std::atomic<bool> g_noclip_refused{false};

extern "C" uint32_t Darkness_GetPlayerEntityId() {
  return g_player_entity_id.load(std::memory_order_acquire);
}
extern "C" uint32_t Darkness_GetPlayerCharacter() {
  return g_player_character.load(std::memory_order_acquire);
}
extern "C" uint32_t Darkness_GetPlayerMoveMode() {
  return g_player_move_mode.load(std::memory_order_acquire);
}
extern "C" bool Darkness_IsNoclipBlocked() {
  return g_noclip_blocked.load(std::memory_order_acquire);
}
extern "C" bool Darkness_WasNoclipRefused() {
  return g_noclip_refused.load(std::memory_order_acquire);
}

// The entity container, observed rather than inferred.
//
// sub_824A6180 (the controller's vtable+332 message send) keeps r3 untouched
// across `bl sub_8249D618` and only loads the target ID into r4, which reads as
// "the controller is the container". Calling it that way returned
// 0xD6DF7A80 -- not a guest address -- so that reading is wrong somewhere, and
// sub_8249D618 does not fault on a bad container: its validity checks
// (result+368/370) just read whatever the bogus index landed on and hand back
// garbage, which then crashes in the caller.
//
// Rather than keep guessing which object owns the entity arrays, EntityLookupHook
// records the container the engine passes when it does its own lookups. That is
// authoritative by construction.
static std::atomic<uint32_t> g_entity_container{0};

extern "C" uint32_t Darkness_GetEntityContainer() {
  return g_entity_container.load(std::memory_order_acquire);
}

// [[midasm_hook]] address = 0x8249D618, name = "EntityLookupHook",
// registers = ["r3", "r4"], after_instruction = false
void EntityLookupHook(PPCRegister& r3, PPCRegister& r4) {
  uint32_t prev = g_entity_container.exchange(r3.u32, std::memory_order_acq_rel);
  if (prev != r3.u32) {
    // Log only on change: this is called constantly during normal play, and we
    // mainly want to know whether the container is stable and what it is.
    REXLOG_INFO("[entity] container = 0x{:08X} (was 0x{:08X}), id = {}", r3.u32, prev, r4.u32);
  }
}

// The player character, observed rather than derived.
//
// Deriving it via sub_8249D618 does not work, and fails in the worst possible
// way: with the wrong container it returns a pointer that satisfies both the
// engine's own validity checks (entity+0x368/0x370 non-zero) and an explicit
// "does entity+0x170 match the ID I asked for" test, and still explodes on
// first use -- sub_82132F10 died calling through *(r31+0x70), a garbage
// function pointer. There is no cheap predicate that separates a good pointer
// from that one.
//
// So take one the engine hands us. sub_82132F10 is the movement-mode switch
// that sub_82133C58 itself calls, so its r3 is a character the engine is
// actively using for precisely this operation -- a pointer that is valid by
// construction, whatever else is unclear.
//
// Identifying the *player* among them takes two tests, not one.
//
// entity+0x170 == PLAYEROBJ id (controller+0x218) is necessary but NOT
// sufficient: several objects share the player's entity id. Observed live with
// PLAYEROBJ = 2559, five objects carried that id -- the character itself
// (vt=0x82062030) and four attachments (vt=0x82073750). Filtering on the id
// alone therefore keeps reassigning between them, and handing an attachment to
// sub_82133C58 is what crashed: it is not a character, so the vtable dispatch
// inside sub_82132F10 calls through a field that is not a function pointer.
//
// The second test asks "is this class one that handles player command
// messages?" -- i.e. does its vtable hold sub_82126828 at +0x158. That offset
// is not assumed: it is confirmed against four independently observed vtables
// (0x820614D0->0x82061628, 0x82062030->0x82062188, 0x820626C8->0x82062820,
// 0x82091BC0->0x82091D18), and the attachment class 0x82073750 fails it.
//
// Reading the slot rather than whitelisting vtable addresses matters because
// the player is not always the same class -- the darkling sections swap it --
// so anything keyed to 0x82062030 specifically would silently stop working.
static constexpr uint32_t kCharacterEntityIdOffset = 0x170;
static constexpr uint32_t kCharacterEntityGenOffset = 0x172;
static constexpr uint32_t kCommandHandlerSlot = 0x158;
static constexpr uint32_t kCommandHandlerAddr = 0x82126828;

// True if `vtable` belongs to a class that handles player command messages,
// i.e. a real character rather than an attachment sharing its owner's id.
static bool IsCharacterVTable(uint8_t* base, uint32_t vtable) {
  // Guest image addresses only; anything else is not a vtable we can read.
  if (vtable < 0x82000000 || vtable >= 0x83000000)
    return false;
  return rex::memory::load_and_swap<uint32_t>(base + vtable + kCommandHandlerSlot) ==
         kCommandHandlerAddr;
}

// The auto-selected player character: id matches PLAYEROBJ *and* the class
// handles player commands.
static std::atomic<uint32_t> g_auto_player_character{0};

struct CharacterCandidate {
  uint32_t ptr = 0;
  uint32_t vtable = 0;
  uint16_t id = 0;
  uint16_t gen = 0;
  uint32_t mode = 0;
  uint32_t flags = 0;
  uint64_t hits = 0;
  bool is_character = false;  // vtable holds sub_82126828 at +0x158
};

static constexpr size_t kMaxCandidates = 24;
static std::mutex g_candidates_mutex;
static CharacterCandidate g_candidates[kMaxCandidates];
static size_t g_candidate_count = 0;

// [[midasm_hook]] address = 0x82132F10, name = "MovementModeHook",
// registers = ["r3"], after_instruction = false
void MovementModeHook(PPCRegister& r3) {
  uint32_t character = r3.u32;
  if (!character)
    return;

  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();

  uint32_t vtable = rex::memory::load_and_swap<uint32_t>(base + character);
  bool is_character = IsCharacterVTable(base, vtable);
  uint16_t id = rex::memory::load_and_swap<uint16_t>(base + character + kCharacterEntityIdOffset);

  // Auto-select: the player's entity id AND an actual character class. Both
  // conditions are required -- see the comment above IsCharacterVTable.
  uint32_t want_id = g_player_entity_id.load(std::memory_order_acquire);
  if (is_character && want_id != 0xFFFFFFFF && id == static_cast<uint16_t>(want_id)) {
    uint32_t prev = g_auto_player_character.exchange(character, std::memory_order_acq_rel);
    if (prev != character)
      REXLOG_INFO("[cheat] player character = 0x{:08X} (id {}, vt 0x{:08X})", character, id, vtable);
  }

  std::lock_guard<std::mutex> lock(g_candidates_mutex);
  for (size_t i = 0; i < g_candidate_count; ++i) {
    if (g_candidates[i].ptr == character) {
      ++g_candidates[i].hits;
      // Refresh the live fields so the picker shows current state.
      g_candidates[i].mode =
          (rex::memory::load_and_swap<uint32_t>(base + character + kCharacterModeOffset) >> 12) &
          0xF;
      g_candidates[i].flags =
          rex::memory::load_and_swap<uint32_t>(base + character + kCharacterFlagsOffset);
      return;
    }
  }
  if (g_candidate_count >= kMaxCandidates)
    return;

  CharacterCandidate& c = g_candidates[g_candidate_count++];
  c.ptr = character;
  c.vtable = vtable;
  c.id = id;
  c.gen = rex::memory::load_and_swap<uint16_t>(base + character + kCharacterEntityGenOffset);
  c.mode =
      (rex::memory::load_and_swap<uint32_t>(base + character + kCharacterModeOffset) >> 12) & 0xF;
  c.flags = rex::memory::load_and_swap<uint32_t>(base + character + kCharacterFlagsOffset);
  c.hits = 1;
  c.is_character = is_character;
  REXLOG_INFO(
      "[cheat] candidate 0x{:08X} vt=0x{:08X} id={} gen={} mode={} flags=0x{:08X} character={}",
      c.ptr, c.vtable, c.id, c.gen, c.mode, c.flags, c.is_character);
}

extern "C" int Darkness_GetCharacterCandidateCount() {
  std::lock_guard<std::mutex> lock(g_candidates_mutex);
  return static_cast<int>(g_candidate_count);
}

extern "C" bool Darkness_GetCharacterCandidate(int index, uint32_t* ptr, uint32_t* vtable,
                                               uint32_t* id, uint32_t* gen, uint32_t* mode,
                                               uint32_t* flags, uint64_t* hits,
                                               bool* is_character) {
  std::lock_guard<std::mutex> lock(g_candidates_mutex);
  if (index < 0 || static_cast<size_t>(index) >= g_candidate_count)
    return false;
  const CharacterCandidate& c = g_candidates[index];
  *ptr = c.ptr;
  *vtable = c.vtable;
  *id = c.id;
  *gen = c.gen;
  *mode = c.mode;
  *flags = c.flags;
  *hits = c.hits;
  *is_character = c.is_character;
  return true;
}

extern "C" void Darkness_ClearCharacterCandidates() {
  std::lock_guard<std::mutex> lock(g_candidates_mutex);
  g_candidate_count = 0;
}

// Explicit target chosen by the operator, plus the mode to switch to.
static std::atomic<uint32_t> g_pending_cheat_target{0};
static std::atomic<int> g_pending_cheat{0};  // 0 = none, 4/6 = noclip mode

extern "C" void Darkness_QueueNoclipOn(uint32_t character, int mode) {
  g_pending_cheat_target.store(character, std::memory_order_release);
  g_pending_cheat.store(mode, std::memory_order_release);
}

// Godmode (action id 34, jump table case 2 in sub_82126828).
//
// Unlike noclip this needs no guest call at all -- the handler is a plain bit
// toggle:
//
//   r30 = *(character + 0x18C)          ; the character's stats/state block
//   if (character->vtable[0x144](character)) {   ; cheat-permission predicate
//     r11 = r30 + 0x2800
//     r10 = *r11 ^ 0x200                ; rlwinm tests bit 0x200, then either
//     if (*r11 != r10) *r11 = r10;      ; clears it or ORs it back in
//   }
//
// We skip the permission predicate (that is the thing being cheated) and do
// the write directly, which also means there is no guest code to crash in.
static constexpr uint32_t kCharacterStateBlockOffset = 0x18C;
static constexpr uint32_t kGodmodeFieldOffset = 0x2800;
static constexpr uint32_t kGodmodeMask = 0x200;

static std::atomic<bool> g_pending_godmode{false};
static std::atomic<bool> g_godmode_active{false};
static std::atomic<bool> g_godmode_available{false};

extern "C" void Darkness_QueueGodmode() {
  g_pending_godmode.store(true, std::memory_order_release);
}
extern "C" bool Darkness_IsGodmodeActive() {
  return g_godmode_active.load(std::memory_order_acquire);
}
extern "C" bool Darkness_IsGodmodeAvailable() {
  return g_godmode_available.load(std::memory_order_acquire);
}

// Returns the address of the godmode flag word, or 0 if it isn't reachable.
static uint32_t GodmodeFieldAddress(uint8_t* base, uint32_t character) {
  if (!character)
    return 0;
  uint32_t state =
      rex::memory::load_and_swap<uint32_t>(base + character + kCharacterStateBlockOffset);
  // Guest heap pointer sanity: null and image addresses are both wrong here.
  if (state < 0x10000 || state >= 0xFFFF0000)
    return 0;
  return state + kGodmodeFieldOffset;
}

// Resolves the player character from the controller and refreshes the snapshot
// the overlay reads. Guest thread only. Returns 0 if the lookup failed (no
// PLAYEROBJ configured yet, or the entity is not live).
static uint32_t ResolvePlayerCharacter(uint8_t* base, uint32_t controller) {
  uint32_t id =
      rex::memory::load_and_swap<uint32_t>(base + controller + kControllerPlayerObjId);
  g_player_entity_id.store(id, std::memory_order_release);
  if (id == 0xFFFFFFFF) {
    g_player_character.store(0, std::memory_order_release);
    return 0;
  }

  uint32_t character = g_auto_player_character.load(std::memory_order_acquire);
  g_player_character.store(character, std::memory_order_release);
  if (!character)
    return 0;

  uint32_t flags =
      rex::memory::load_and_swap<uint32_t>(base + character + kCharacterFlagsOffset);
  g_noclip_blocked.store((flags & kCharacterNoclipBlockedMask) != 0, std::memory_order_release);
  uint32_t mode_field =
      rex::memory::load_and_swap<uint32_t>(base + character + kCharacterModeOffset);
  g_player_move_mode.store((mode_field >> 12) & 0xF, std::memory_order_release);
  return character;
}

static void DrainCheatQueue(uint8_t* base, uint32_t controller) {
  uint32_t player = ResolvePlayerCharacter(base, controller);

  // --- godmode: direct bit toggle, no guest call ---
  uint32_t god_addr = GodmodeFieldAddress(base, player);
  g_godmode_available.store(god_addr != 0, std::memory_order_release);
  if (god_addr) {
    uint32_t word = rex::memory::load_and_swap<uint32_t>(base + god_addr);
    if (g_pending_godmode.exchange(false, std::memory_order_acq_rel)) {
      word ^= kGodmodeMask;
      rex::memory::store_and_swap<uint32_t>(base + god_addr, word);
      REXLOG_INFO("[cheat] godmode {} (0x{:08X} = 0x{:08X})",
                  (word & kGodmodeMask) ? "ON" : "OFF", god_addr, word);
    }
    g_godmode_active.store((word & kGodmodeMask) != 0, std::memory_order_release);
  } else {
    g_pending_godmode.store(false, std::memory_order_release);
  }

  // --- noclip ---
  int mode = g_pending_cheat.exchange(0, std::memory_order_acq_rel);
  if (!mode)
    return;
  uint32_t character = g_pending_cheat_target.exchange(0, std::memory_order_acq_rel);
  if (!character)
    return;

  uint32_t flags =
      rex::memory::load_and_swap<uint32_t>(base + character + kCharacterFlagsOffset);
  g_noclip_blocked.store((flags & kCharacterNoclipBlockedMask) != 0, std::memory_order_release);

  uint32_t before =
      (rex::memory::load_and_swap<uint32_t>(base + character + kCharacterModeOffset) >> 12) & 0xF;
  uint32_t ok = Darkness_ToggleNoclip(character, static_cast<uint32_t>(mode));
  uint32_t after =
      (rex::memory::load_and_swap<uint32_t>(base + character + kCharacterModeOffset) >> 12) & 0xF;

  // sub_82133C58 returns 0 when sub_82132F10 refused the switch -- the engine
  // prints "Unable to switch to/from noclip." in that case. Leaving noclip
  // needs somewhere legal to stand, so refusals are expected when you are
  // parked inside geometry; that is the game's rule, not a bug here.
  if (ok)
    REXLOG_INFO("[cheat] noclip: mode {} -> {}", before, after);
  else
    REXLOG_WARN("[cheat] noclip: engine refused the switch (mode {}, still {}) -- "
                "move somewhere with room to stand", before, after);
  g_noclip_refused.store(ok == 0, std::memory_order_release);
}

// Runs one queued line. Must be called on the guest thread.
static void ExecuteConsoleLine(const std::string& line) {
  // Scratch lives on the guest stack: the two engine-string objects are 8
  // bytes each, but the ctor/copy-ctor take ownership of a heap payload, so
  // they must be released before this frame is popped -- which the sequence
  // below does. stack_guard restores r1 on scope exit; ImportFunction's
  // auto-isolating call builds its own frame at (our r1 - 0x70), so it never
  // overlaps the scratch.
  rex::ppc::stack_guard guard;

  uint32_t text_addr = rex::ppc::stack_push_string(line.c_str());
  uint32_t tag_addr = rex::ppc::stack_push_string("ConExecute");

  const uint8_t kZeroObj[8] = {};
  uint32_t src = rex::ppc::stack_push(kZeroObj, sizeof(kZeroObj));
  uint32_t arg = rex::ppc::stack_push(kZeroObj, sizeof(kZeroObj));

  Darkness_StrFromCStr(src, text_addr);
  Darkness_StrCopy(arg, src);
  Darkness_ConExecuteStr(arg, tag_addr, 0);  // takes ownership of `arg`
  Darkness_StrRelease(src);
}

// Invokes a debug handler as (pawn, CString*). Must be called on the guest
// thread.
//
// The callee takes ownership of the string -- sub_823FCA88 releases it -- so
// we hand it a fresh copy and release only our own, exactly as the console
// path does. Handlers that ignore r4 (toggledebugcamera clobbers it outright)
// leak that copy, which is a few bytes per button press and not worth
// tracking per-command ownership to avoid.
static void ExecuteDebugCommand(DebugCommandFn* fn, uint32_t pawn, const std::string& arg) {
  rex::ppc::stack_guard guard;

  uint32_t text_addr = rex::ppc::stack_push_string(arg.c_str());
  const uint8_t kZeroObj[8] = {};
  uint32_t src = rex::ppc::stack_push(kZeroObj, sizeof(kZeroObj));
  uint32_t owned = rex::ppc::stack_push(kZeroObj, sizeof(kZeroObj));

  Darkness_StrFromCStr(src, text_addr);
  Darkness_StrCopy(owned, src);
  (*fn)(pawn, owned);
  Darkness_StrRelease(src);
}

static void DrainConsoleQueue() {
  for (;;) {
    std::string line;
    {
      std::lock_guard<std::mutex> lock(g_console_queue_mutex);
      if (g_console_queue.empty())
        return;
      line = std::move(g_console_queue.front());
      g_console_queue.pop_front();
    }
    REXLOG_INFO("[console] {}", line);
    ExecuteConsoleLine(line);
  }
}

// Direct look-velocity injection.
//
// sub_823F8AF0 is the player pawn's per-frame look-input -> rotation-velocity
// update (pawn virtual slot +0x1C4). r3 (a1) is the pawn. At its top it reads
// the two look-velocity *input* floats and multiplies each by 1/255
// (0.0039215689), then rate-limits the pawn's *applied* rot-velocity state
// (pawn+0x2274/0x2278) toward that input and calls the vtable integrator that
// advances facing:
//   lookvelocity_x = *(float*)(pawn + 0x227C)   (yaw input,   +/-255 range)
//   lookvelocity_y = *(float*)(pawn + 0x2280)   (pitch input, +/-255 range)
// (Offsets resolved from the pawn vtable accessor cluster at base 0x82081bf0:
//  0x823fd118 = set both, 0x823fd128 = set x -> 0x227C, 0x823fd130 = set y ->
//  0x2280.)
//
// The update is NOT a fixed-dt exponential ease (an earlier comment here
// claimed "eases toward that input at dt=1/120" -- that was wrong). A fresh
// decompile this session showed it (a) computes a real, measured dt from a
// vtable time-source (v17 = now - pawn+0x2298 stored timestamp, clamped), and
// (b) does a rate-limited "move towards": each axis steps its applied value
// toward (input * speed * frame_factor) at most accel_const*dt per call,
// snapping to the target on overshoot. So an instant full-deflection input
// still ramps up over multiple real ticks (the per-axis accel cap), and the
// *number* of ticks per second (our call rate) gates how fast it reaches the
// cap -- see the open call-rate question in CAMERA_HANDOFF_NEXT.md. The exact
// fsel clamp formula wasn't fully hand-traced; re-verify before relying on it.
//
// By hooking sub_823F8AF0's ENTRY and overwriting those two fields with the
// mouse delta *before* they are consumed, we win over whatever the game's own
// analog-stick pipeline wrote earlier this frame -- so we bypass every layer
// that made mouselook sluggish/spinny (radial + linear deadzone, log response
// curve, sub_82793FE8's press/release latch) while keeping the engine's own
// rot-velocity easing + friction, which is what makes turning feel smooth.
//
// We write an *absolute* input value each frame (not an accumulation), fed by
// the driver's continuously-decaying mouse accumulator (TryGetLookVelocity),
// not the old hard-drain net-vector-sum (TryGetLookDelta). The hard-drain
// summed every raw sample since the last read and zeroed on read, so a fast
// flick-and-correct inside one ~40ms hook window netted to ~0 and read as "no
// motion" (with minor asymmetric jitter on the other axis showing as bobbing).
// The decaying accumulator isn't bucketed into discrete read windows, so our
// low, irregular call rate can't alias it that way; it's also already
// time-aware, so no host-side dt-normalization is needed. TryGetLookVelocity
// returns true (with ~0) while idle-but-captured, so we ease to a clean stop;
// it returns false only when MnK is off / unfocused / uncaptured, in which
// case we leave the field alone so a real controller still drives the pawn.
//
// Guest floats are big-endian; rex::memory::store_and_swap<float> writes them
// in guest byte order.

// Player pawn pointer exported for the debug overlay.
static uint32_t g_player_pawn = 0;
extern "C" uint32_t Darkness_GetPlayerPawn() { return g_player_pawn; }

// [[midasm_hook]] address = 0x823F8AF0, name = "PlayerLookVelocityHook",
// registers = ["r3"], after_instruction = false
void PlayerLookVelocityHook(PPCRegister& r3) {
  g_player_pawn = r3.u32;

  // Drain any debug-overlay command queued from the UI thread (see the
  // g_pending_debug_command comment above) now that we're safely on the
  // guest thread with a valid pawn.
  {
    PendingDebugCommand cmd{nullptr, {}};
    {
      std::lock_guard<std::mutex> lock(g_debug_command_mutex);
      std::swap(cmd, g_pending_debug_command);
    }
    if (cmd.fn)
      ExecuteDebugCommand(cmd.fn, r3.u32, cmd.arg);
  }

  // Same deal for free-form console lines typed into the overlay.
  DrainConsoleQueue();

  // Resolve the player character (for the overlay's status readout) and run
  // any queued cheat against it.
  DrainCheatQueue(rex::system::kernel_state()->memory()->virtual_membase(), r3.u32);

  auto* input = static_cast<rex::input::InputSystem*>(
      rex::Runtime::instance()->input_system());
  if (!input) {
    return;
  }
  auto* mnk = input->GetDriver<rex::input::mnk::MnkInputDriver>();
  if (!mnk) {
    return;
  }

  double vx = 0.0, vy = 0.0;
  if (!mnk->TryGetLookVelocity(&vx, &vy)) {
    // MnK inactive (off / unfocused / mouse not captured): don't stomp the
    // pawn's look-velocity input -- let the normal (controller) path drive it.
    return;
  }

  // vx/vy come from the driver's *continuously decaying* mouse accumulator
  // (mouse_dx_/mouse_dy_, 50ms half-life), not a hard-drain net-vector-sum.
  // This is deliberate: the old TryGetLookDelta summed every raw sample since
  // the previous read and zeroed on read, so a fast flick-and-correct inside
  // one ~40ms hook window netted to ~0 -- indistinguishable from "no motion"
  // -- while minor asymmetric jitter on the other axis didn't cancel and read
  // as visible bobbing. The decaying accumulator is never bucketed into
  // discrete read windows, so polling it at our low, irregular call rate
  // doesn't alias that way. It's also already time-aware (decay uses real
  // elapsed time), so no host-side dt-normalization is needed here.
  //
  // TryGetLookVelocity returns true (with a value near zero) on every poll as
  // long as MnK is enabled + focused + captured, not just when the mouse is
  // actually being moved. Writing that near-zero unconditionally would stomp
  // the pawn's look-velocity fields every frame mnk_mode is on, permanently
  // overriding whatever the analog-stick pipeline wrote -- i.e. enabling
  // mouselook would silently kill stick look. Instead, only take over the
  // fields while there's recent motion (accumulator above the deadzone),
  // write the decaying value while active (giving natural deceleration as it
  // rings down), then release the fields entirely so the stick can drive.
  constexpr auto kMouseIdleReleaseWindow = std::chrono::milliseconds(200);
  static auto last_motion_time = std::chrono::steady_clock::time_point::min();
  static bool releasing = true;

  auto now = std::chrono::steady_clock::now();

  // Deadzone in raw accumulator units: rejects hand tremor / sensor noise so
  // it can't fake "motion" that keeps the hook latched active, and so a couple
  // of stray pixels on the axis you're not moving don't show up as bobbing.
  constexpr double kMouseDeadzone = 0.5;
  if (std::abs(vx) < kMouseDeadzone)
    vx = 0.0;
  if (std::abs(vy) < kMouseDeadzone)
    vy = 0.0;

  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  uint32_t pawn = r3.u32;

  // --- engine turn-acceleration ramp bypass (kills the input lag) ---
  // sub_823F8AF0 does not apply our look input directly: it treats it as a
  // *target* turn rate and rate-limits the pawn's *applied* turn rate toward
  // that target at accel*dt per frame (per-axis max-accel constants at
  // pawn+0x2290 yaw / pawn+0x2294 pitch, confirmed by disassembly), then
  // integrates facing from the applied rate. That accel cap is the ramp-up /
  // ramp-down lag that makes mouselook feel like a smoothed joystick.
  //
  // While we are actively driving look from the mouse, inflate those accel
  // constants to a huge value so the engine's own "move towards" snaps the
  // applied rate to the target in a single frame -- no ramp, instant response.
  // The engine clamps on overshoot (fsel), so an oversized step is stable: it
  // lands exactly on target, never past it. We save the originals and restore
  // them the moment we hand control back to the stick pipeline, so a real
  // controller keeps its intended ramp. Keyed on the pawn pointer so a pawn
  // swap can never restore stale values into the wrong object.
  constexpr uint32_t kAccelYawOff = 0x2290;
  constexpr uint32_t kAccelPitchOff = 0x2294;
  constexpr float kSnapAccel = 1.0e9f;
  static uint32_t accel_saved_pawn = 0;
  static float accel_saved_x = 0.0f;
  static float accel_saved_y = 0.0f;
  auto restore_accel = [&]() {
    if (accel_saved_pawn) {
      rex::memory::store_and_swap<float>(base + accel_saved_pawn + kAccelYawOff, accel_saved_x);
      rex::memory::store_and_swap<float>(base + accel_saved_pawn + kAccelPitchOff, accel_saved_y);
      accel_saved_pawn = 0;
    }
  };
  auto inflate_accel = [&]() {
    if (accel_saved_pawn != pawn) {
      restore_accel();  // pawn changed under us -- put the previous one back first
      accel_saved_x = rex::memory::load_and_swap<float>(base + pawn + kAccelYawOff);
      accel_saved_y = rex::memory::load_and_swap<float>(base + pawn + kAccelPitchOff);
      accel_saved_pawn = pawn;
    }
    rex::memory::store_and_swap<float>(base + pawn + kAccelYawOff, kSnapAccel);
    rex::memory::store_and_swap<float>(base + pawn + kAccelPitchOff, kSnapAccel);
  };

  bool has_motion = (vx != 0.0 || vy != 0.0);
  if (has_motion) {
    last_motion_time = now;
    releasing = false;
  } else if (!releasing) {
    if (now - last_motion_time >= kMouseIdleReleaseWindow) {
      releasing = true;
    }
  } else {
    // Already released control back to the stick pipeline: restore the pawn's
    // original turn-accel and leave the fields alone.
    restore_accel();
    return;
  }

  // Map the (decayed) accumulator into the game's +/-255 look-velocity input
  // range, PROPORTIONALLY -- the whole point is that a small mouse move gives a
  // small deflection and a big move a big one. The earlier value (90) pinned
  // the input at full +/-255 on almost any motion (mouse_dx_ is tens of pixels
  // after even a light flick; *90 clamps instantly), which read as a digital
  // "stick fully pushed in the direction moved" -- same turn rate regardless of
  // mouse speed. The SDK's own stick path (GetState) maps
  // mouse_dx_ * sensitivity * 200 into the +/-32767 int16 stick range; this
  // field is +/-255 (128x smaller), so the equivalent proportional scale is
  // 200 * (255/32767) ~= 1.6. kDeltaToInputScale is the coarse feel knob;
  // fine-tune live via the mnk_sensitivity console cvar (queried by name -- see
  // the note on REXCVAR_QUERY above). vx/vy are already 0 within the
  // idle-release window when there's no motion, so this cleanly eases to a stop.
  constexpr double kDeltaToInputScale = 1.6;
  double sensitivity = REXCVAR_QUERY(double, mnk_sensitivity);
  double scale = sensitivity * kDeltaToInputScale;
  float out_x = static_cast<float>(std::clamp(vx * scale, -255.0, 255.0));
  float out_y = static_cast<float>(std::clamp(vy * scale, -255.0, 255.0));

  // Snap the engine's turn-accel ramp so this input takes effect immediately
  // (see the accel-bypass note above). Also applies while easing to zero within
  // the release window, so the camera stops crisply instead of gliding.
  inflate_accel();
  rex::memory::store_and_swap<float>(base + pawn + 0x227C, out_x);  // yaw input
  rex::memory::store_and_swap<float>(base + pawn + 0x2280, out_y);  // pitch input
}
