// Mid-asm hook wiring raw mouse deltas directly into the game's own
// controller-poll function (sub_82794448), which every frame reads a
// per-player XINPUT_STATE-shaped struct off the stack and dispatches
// sThumbRX/sThumbRY into the game's ACTION_LOOKAXIS handling (camera turn).
//
// The hook fires right after the struct is filled by the XamInputGetState
// call at 0x82794464, before the game reads sThumbRX/sThumbRY out of it, so
// overwriting those two fields here rides mouse motion through the exact
// same deadzone/sensitivity/autoaim-assist code path a physical stick would
// use -- no separate emulated-controller layer, no extra frame of latency.

#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include <cstdint>

namespace {

// XINPUT_GAMEPAD-shaped struct sub_82794448 reads its poll result into, at
// r1+0x50 in guest stack terms (see var_40 in the disassembly).
struct GuestXInputGamepad {
  uint32_t packet_number;
  uint16_t buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
  int16_t thumb_lx;
  int16_t thumb_ly;
  int16_t thumb_rx;
  int16_t thumb_ry;
};
static_assert(sizeof(GuestXInputGamepad) == 16);

}  // namespace

// [[midasm_hook]] address = 0x82794464, name = "CameraMouseLookHook",
// registers = ["r1"], after_instruction = true
void CameraMouseLookHook(PPCRegister& r1) {
  auto* input = static_cast<rex::input::InputSystem*>(rex::Runtime::instance()->input_system());
  if (!input) {
    return;
  }
  auto* mnk = input->GetDriver<rex::input::mnk::MnkInputDriver>();
  if (!mnk) {
    return;
  }

  int16_t rx = 0, ry = 0;
  if (!mnk->TryGetLookStick(&rx, &ry)) {
    return;
  }
  // TryGetLookStick() reports the mouse's decayed stick value, which reads
  // as (0, 0) whenever the mouse hasn't moved recently -- indistinguishable
  // from "mouse centered". Since this hook runs after the real
  // XamInputGetState poll rather than replacing it, writing that zero back
  // would stomp a physical controller's genuine right-stick deflection every
  // frame the mouse sits idle. Only take over the guest's stick fields when
  // the mouse actually has look input to contribute.
  if (rx == 0 && ry == 0) {
    return;
  }

  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  auto* gamepad =
      reinterpret_cast<GuestXInputGamepad*>(base + r1.u32 + 0x50);

  // Guest struct is big-endian; the fields we touch are 16-bit halfwords, so
  // a byte-swap on write is required for correctness on little-endian hosts.
  auto to_be16 = [](int16_t v) -> int16_t {
    uint16_t u = static_cast<uint16_t>(v);
    return static_cast<int16_t>((u << 8) | (u >> 8));
  };

  gamepad->thumb_rx = to_be16(rx);
  gamepad->thumb_ry = to_be16(ry);
}
