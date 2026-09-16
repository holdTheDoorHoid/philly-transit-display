// LVGL's last-resort assert path (lv_conf.h LV_ASSERT_HANDLER).
//
// LV_USE_ASSERT_MALLOC is on, so every LVGL allocation site that checks its result lands here when
// the 36 KB pool (LV_MEM_SIZE) cannot fit one more widget. The default handler is a bare abort(),
// which on this board is a backtrace nobody is watching and a reboot with no explanation.
//
// What it CANNOT be is "carry on". LVGL 9.5 is not allocation-failure-safe: lv_obj_class.c's
// create path does
//     parent->spec_attr->children = lv_realloc(...);
//     parent->spec_attr->children[child_cnt - 1] = obj;
// with no check on the realloc, so returning from this handler (or compiling the assert out) turns
// an exhausted pool into a NULL dereference a few instructions later instead of a clean stop. The
// real defence is ui.cpp's rule that only one page is ever resident and that no build starts
// unless the pool has room for it; this handler is what happens if that rule is ever wrong.
//
// So: say precisely what ran out, flush it, and stop. A reboot returns the device to the arrivals
// page with its config intact, which is the best outcome available once LVGL's pool is gone.
//
// And NAME it on the way out. Until 2026-09-16 this path called esp_restart() without a restart
// note, so a pool exhaustion was indistinguishable in /api/state.last_restart from a deliberate
// reboot or an OTA - the owner saw ESP_RST_SW and nothing else. That matters most for exactly the
// failure this handler catches: an arrivals page whose config does not fit reproduces the same
// crash on every boot, so what the owner has is a boot loop, and the note is the only thing that
// says which loop it is. The two figures recorded are the pool's free size and its high-water
// mark, which together say whether it was exhausted or merely fragmented.
#include <Arduino.h>
#include <esp_system.h>
#include <lvgl.h>

#include "../net_poller.h"  // noteSelfHealRestart(): RTC-backed, allocation-free, cannot throw

extern "C" void transitLvglAssertFailed(void) {
  // The caller's address, not its name: LV_ASSERT_HANDLER expands at ~400 sites inside LVGL and
  // passing __func__ would give each of their functions a static name string (+1,256 B of flash,
  // measured). `xtensa-esp-elf-addr2line -e .pio/build/<env>/firmware.elf <pc>` names it instead.
  void *pc = __builtin_return_address(0);
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  Serial.printf("[lvmem] LVGL assert at pc=%p: pool total=%u free=%u used_pct=%u frag_pct=%u max_used=%u\n",
                pc, (unsigned)m.total_size, (unsigned)m.free_size,
                (unsigned)m.used_pct, (unsigned)m.frag_pct, (unsigned)m.max_used);
  Serial.println("[lvmem] out of LVGL pool with no safe way to continue - restarting (DESIGN.md SS8)");
  transit_app::noteSelfHealRestart(transit_app::SelfHeal::LvglPool, (uint32_t)m.free_size, (uint32_t)m.max_used);
  Serial.flush();
  delay(50);  // the UART FIFO drains at 115200 before the reset takes the chip
  esp_restart();
}
