// Device info page (DESIGN.md SS8): the header strip with the firmware version, a Network panel
// (Wi-Fi bars, SSID, mDNS URL, IP, RSSI), a panel named after the device (web PIN in the big
// font, SD status and write health, free heap, uptime), a Data sources panel where the height
// allows it (SEPTA / weather / Indego / alerts: status word and age; the SEPTA line falls back
// into Network where it does not) and the "hold 5 s to reset Wi-Fi" button along the bottom.
// Built when tapped to and deleted when tapped away from (ui.cpp).
#pragma once
#include <lvgl.h>

#include "../config_store.h"

namespace transit_app::ui {

lv_obj_t *createDeviceInfoScreen(const Config &cfg);
void refreshDeviceInfoScreen(lv_obj_t *screen);

}  // namespace transit_app::ui
