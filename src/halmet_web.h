#ifndef HALMET_SRC_HALMET_WEB_H_
#define HALMET_SRC_HALMET_WEB_H_

#include <Arduino.h>

namespace halmet {

/**
 * @brief Update a sensor reading shown in the web data grid.
 *
 * Call this from sensor callbacks to keep the live display current.
 * If the label already exists its value is replaced; otherwise a new
 * row is appended to the grid.
 */
void UpdateWebDataValue(const String& label, const String& value);
void UpdateWebDataValue(const String& label, float value, int decimals = 2);
void UpdateWebDataValue(const String& label, bool value);

/**
 * @brief Start the HALMET data-display HTTP server on port 8080.
 *
 * Call once in setup() after the SensESP app has been created.
 *
 * Endpoints:
 *   GET /data      – HTML dashboard (auto-refreshes every 2 s)
 *   GET /api/data  – JSON array of {label, value} objects
 *
 * Access via:
 *   http://halmet.local:8080/data
 *   http://<ip>:8080/data
 *
 * WiFi / AP mode  (handled automatically by SensESP):
 *   When no WiFi network is configured the device creates a soft-AP
 *   named "halmet" (password: "thisisfine").  Use the captive portal
 *   at http://192.168.4.1/wifi to add a network credential.
 *
 * Reconnection:
 *   SensESP reconnects automatically on disconnect.  Call
 *   ->enable_wifi_watchdog() on the SensESPAppBuilder to restart
 *   the device after 3 minutes of continuous disconnection.
 */
void SetupWebDataDisplay();

}  // namespace halmet

#endif  // HALMET_SRC_HALMET_WEB_H_
