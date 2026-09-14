// Device info page: IP, mDNS, SSID, RSSI, SD status, free heap, firmware
// version, and a "hold 5s to reset Wi-Fi" gesture. DESIGN.md SS8.
#pragma once
#include <lvgl.h>

#include "../config_store.h"

namespace transit_app::ui {

lv_obj_t *createDeviceInfoScreen(const Config &cfg);
void refreshDeviceInfoScreen(lv_obj_t *screen);

}  // namespace transit_app::ui
