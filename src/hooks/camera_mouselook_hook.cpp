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
#include <chrono>
#include <cstdint>

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
