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
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <cstdint>

// Direct look-velocity injection.
//
// sub_823F8AF0 is the player pawn's per-frame look-input -> rotation-velocity
// update (pawn virtual slot +0x1C4). r3 (a1) is the pawn. At its top it reads
// the two look-velocity *input* floats and multiplies each by 1/255
// (0.0039215689), then eases the pawn's *applied* rot-velocity
// (pawn+0x2294/0x2298) toward that input at dt=1/120 and calls the vtable
// integrator that advances facing:
//   lookvelocity_x = *(float*)(pawn + 0x227C)   (yaw input,   +/-255 range)
//   lookvelocity_y = *(float*)(pawn + 0x2280)   (pitch input, +/-255 range)
// (Offsets resolved from the pawn vtable accessor cluster at base 0x82081bf0:
//  0x823fd118 = set both, 0x823fd128 = set x -> 0x227C, 0x823fd130 = set y ->
//  0x2280.)
//
// By hooking sub_823F8AF0's ENTRY and overwriting those two fields with the
// mouse delta *before* they are consumed, we win over whatever the game's own
// analog-stick pipeline wrote earlier this frame -- so we bypass every layer
// that made mouselook sluggish/spinny (radial + linear deadzone, log response
// curve, sub_82793FE8's press/release latch) while keeping the engine's own
// rot-velocity easing + friction, which is what makes turning feel smooth.
//
// We write an *absolute* input value each frame (not an accumulation), so the
// raw hard-reset-per-poll delta (TryGetLookDelta) is exactly right here -- the
// opposite of the old stick-pipeline bypass, which fed a change-triggered
// press/release dispatch and needed a decaying accumulator. When the mouse
// is idle TryGetLookDelta still returns true (captured + focused) with (0, 0),
// so we write 0 and the engine eases the applied rot-velocity to a clean stop.
// It returns false only when MnK mode is off / unfocused / uncaptured, in which
// case we leave the field alone so a real controller still drives the pawn.
//
// Guest floats are big-endian; rex::memory::store_and_swap<float> writes them
// in guest byte order.

// [[midasm_hook]] address = 0x823F8AF0, name = "PlayerLookVelocityHook",
// registers = ["r3"], after_instruction = false
void PlayerLookVelocityHook(PPCRegister& r3) {
  auto* input = static_cast<rex::input::InputSystem*>(
      rex::Runtime::instance()->input_system());
  if (!input) {
    return;
  }
  auto* mnk = input->GetDriver<rex::input::mnk::MnkInputDriver>();
  if (!mnk) {
    return;
  }

  int32_t dx = 0, dy = 0;
  if (!mnk->TryGetLookDelta(&dx, &dy)) {
    // MnK inactive (off / unfocused / mouse not captured): don't stomp the
    // pawn's look-velocity input -- let the normal (controller) path drive it.
    return;
  }

  // Map raw per-poll mouse pixels into the game's +/-255 look-velocity input
  // range. kDeltaToInputScale is the coarse feel knob; fine-tune live via the
  // mnk_sensitivity console cvar (queried by name -- see the note on
  // REXCVAR_QUERY above).
  constexpr double kDeltaToInputScale = 8.0;
  double sensitivity = REXCVAR_QUERY(double, mnk_sensitivity);
  float out_x = static_cast<float>(
      std::clamp(dx * sensitivity * kDeltaToInputScale, -255.0, 255.0));
  float out_y = static_cast<float>(
      std::clamp(dy * sensitivity * kDeltaToInputScale, -255.0, 255.0));

  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  uint32_t pawn = r3.u32;
  rex::memory::store_and_swap<float>(base + pawn + 0x227C, out_x);  // yaw input
  rex::memory::store_and_swap<float>(base + pawn + 0x2280, out_y);  // pitch input

  static int debug_counter = 0;
  if ((out_x != 0.0f || out_y != 0.0f) && (debug_counter++ % 5) == 0) {
    REXLOG_INFO(
        "PlayerLookVelocityHook: pawn={:08X} dx={} dy={} sensitivity={} out_x={:.1f} out_y={:.1f}",
        pawn, dx, dy, sensitivity, out_x, out_y);
  }
}
