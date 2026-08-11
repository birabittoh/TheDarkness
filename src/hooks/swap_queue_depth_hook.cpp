// Caps how many frames Direct3D lets the guest CPU queue ahead of the display.
//
// THE BUG THIS FIXES
// ------------------
// Every input in the game -- mouse look, movement, firing -- was evaluated
// roughly half a second before it appeared on screen. The delay was identical
// with `vsync` on and off, which rules out the SDK's vblank pacing: turning
// vsync off makes the guest simulate much faster but does not shorten the lag
// by a single frame.
//
// The cause is in the game's own statically-linked Xbox Direct3D 9 library, at
// the tail of D3DDevice::Swap (sub_82867620):
//
//   loc_82867CDC:
//     bl   sub_8285DEB8              ; "is the GPU still making progress?"
//     beq  loc_82867D00
//     lwz  r11, 0x40A0(r31)          ; swaps RETIRED   (device+16544)
//     lwz  r10, 0x4098(r31)          ; swaps SUBMITTED (device+16536)
//     subf r11, r11, r10
//     cmplwi cr6, r11, 0xF
//     bge  cr6, loc_82867CDC         ; spin while >= 15 swaps are in flight
//
// So D3D happily lets the CPU run **15 frames ahead** of the display before it
// applies any back-pressure at all; 15 frames at the game's ~30fps is ~500ms,
// which is exactly the lag observed. The matching pending-swap ring in the
// vblank ISR (sub_82866E58) is 16 slots wide (`& 0x78`, 8-byte entries), so 15
// is "fill the ring, leave one free" -- a deliberate constant, not a bug in the
// game.
//
// On real hardware this allowance is never reached: the retire counter at
// +16544 is bumped by the graphics interrupt callback (sub_8285D7B0 -> vblank
// path -> sub_82866E58), which pops a pending swap only once its target vblank
// count has arrived, and the GPU is the thing setting that pace. The console's
// CPU simply cannot outrun its GPU by 15 frames.
//
// Here it can. Guest CPU code is recompiled to native x86 and runs at host
// speed, while the GPU is emulated and is the slow end of the pipe, so the
// guest sprints away until it hits the D3D limit and then sits there --
// permanently saturated at ~15 queued frames. That is why the lag is a fixed
// wall-clock amount rather than a fixed number of frames, and why `vsync`
// changes the smoothness but not the latency: either way the queue is full.
//
// THE FIX
// -------
// Rewrite the comparison operand so the same spin loop trips at a sane depth.
// The hook sits on the `cmplwi cr6, r11, 0xF` itself and rewrites r11 to either
// 0xF (keep spinning) or 0 (proceed), which makes the untouched `cmplwi`/`bge`
// pair behave as if the constant were `d3d_max_queued_frames`. Patching r11
// rather than the instruction keeps every register the loop actually depends on
// (r31, r10, cr6 consumers past the branch) exactly as the game left it: r11 is
// reloaded from +0x40A0 on each iteration and is dead after the loop (the next
// use is `lbz r11, 0x2ABD(r31)` at 0x82867D10), so it is free to clobber.
//
// Note this whole block is conditional on device+0x56F8 & 4 (the presentation
// interval flag tested by `rlwinm. r11, r11, 0,29,29` at 0x82867CA8). With an
// immediate present interval the game does not throttle here at all, so this
// hook correctly does nothing in that case.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <cstdint>

REXCVAR_DEFINE_UINT32(d3d_max_queued_frames, 2, "GPU",
                      "Frames the guest CPU may queue ahead of the display -- directly, the "
                      "frames of input lag.\n"
                      "The game's Direct3D allows 15, which on a recompiled (native-speed) "
                      "guest saturates and shows up as ~500ms of lag. 2 keeps the GPU fed "
                      "without a visible delay; 1 is the lowest latency, at some cost in "
                      "throughput and jitter tolerance; 15 is the stock behaviour.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The stock immediate the untouched `cmplwi cr6, r11, 0xF` compares against.
// Feeding this value back keeps the loop spinning; feeding 0 exits it.
static constexpr uint32_t kStockQueueLimit = 0xF;

// [[midasm_hook]] address = 0x82867CF8, name = "SwapQueueDepthHook",
// registers = ["r11"], after_instruction = false
void SwapQueueDepthHook(PPCRegister& r11) {
  uint32_t limit = REXCVAR_GET(d3d_max_queued_frames);
  if (limit >= kStockQueueLimit) {
    // Stock behaviour: leave the real in-flight count alone.
    return;
  }
  // A limit of 0 would make the `>=` below always true and spin forever (the
  // guest can never have fewer than zero swaps in flight), so the floor is 1 --
  // "submit, then wait for it to retire before submitting the next."
  if (limit < 1) {
    limit = 1;
  }
  // r11 holds (swaps submitted - swaps retired) on entry.
  r11.u32 = (r11.u32 >= limit) ? kStockQueueLimit : 0;
}
