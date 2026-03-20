// Signal K UDP broadcaster for HALMET
//
// Sends Signal K delta messages to the multicast group 239.2.5.26:4445
// every second.  Accumulates values published via PublishSKFloat/Bool
// and flushes a single delta per tick so clients receive one clean message.
//
// Falls back to directed broadcast (255.255.255.255:4445) when the AP-only
// interface is active (softAP IP != 0), so it also works before a station
// network is joined.

#include "halmet_sk_udp.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>

#include <vector>

#include "sensesp.h"
#include "sensesp_base_app.h"

using namespace sensesp;

namespace halmet {

// Signal K standard UDP multicast address and port
static const IPAddress kSKMulticast(239, 2, 5, 26);
static const uint16_t  kSKPort = 4445;

// Pending values — flushed once per second
struct SKEntry {
  String path;
  bool   is_float;
  float  fval;
  bool   bval;
};
static std::vector<SKEntry> pending_;
static WiFiUDP udp_;

static bool network_ready() {
  if (WiFi.status() == WL_CONNECTED) return true;
  // AP-only mode: softAP interface is up
  return WiFi.softAPIP() != IPAddress(0, 0, 0, 0);
}

// Build an ISO-8601 timestamp string (best-effort; uses millis if no NTP).
static String iso_timestamp() {
  struct tm ti;
  if (getLocalTime(&ti, 0)) {
    char buf[30];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &ti);
    return String(buf);
  }
  // No NTP yet — use uptime in a placeholder format
  uint32_t ms = millis();
  char buf[40];
  snprintf(buf, sizeof(buf), "1970-01-01T00:%02lu:%02lu.%03luZ",
           (ms / 60000UL) % 60, (ms / 1000UL) % 60, ms % 1000UL);
  return String(buf);
}

static void flush_delta() {
  if (pending_.empty()) return;
  if (!network_ready())  return;

  JsonDocument doc;
  doc["context"] = "vessels.self";

  JsonArray updates = doc["updates"].to<JsonArray>();
  JsonObject update  = updates.add<JsonObject>();

  JsonObject src = update["source"].to<JsonObject>();
  src["label"] = "HALMET";
  src["type"]  = "sensor";

  update["timestamp"] = iso_timestamp();

  JsonArray values = update["values"].to<JsonArray>();
  for (auto& e : pending_) {
    JsonObject v = values.add<JsonObject>();
    v["path"] = e.path;
    if (e.is_float) v["value"] = e.fval;
    else            v["value"] = e.bval;
  }
  pending_.clear();

  String payload;
  serializeJson(doc, payload);

  // Send to SK multicast group
  udp_.beginPacket(kSKMulticast, kSKPort);
  udp_.print(payload);
  udp_.endPacket();

  // Also send as directed broadcast so AP clients receive it before they
  // have a multicast-capable route (most mobile browsers/apps will get it
  // via broadcast even if multicast isn't set up on the AP).
  IPAddress bcast;
  if (WiFi.status() == WL_CONNECTED) {
    uint32_t ip   = (uint32_t)WiFi.localIP();
    uint32_t mask = (uint32_t)WiFi.subnetMask();
    bcast = IPAddress((ip & mask) | ~mask);
  } else {
    bcast = IPAddress(192, 168, 4, 255);  // AP subnet broadcast
  }
  udp_.beginPacket(bcast, kSKPort);
  udp_.print(payload);
  udp_.endPacket();

  Serial.printf("[SK-UDP] sent delta (%d values) → %s + %s:%d\n",
                (int)values.size(),
                kSKMulticast.toString().c_str(),
                bcast.toString().c_str(), kSKPort);
}

void SetupSKUDP() {
  udp_.begin(kSKPort);

  // Flush accumulated values every second
  event_loop()->onRepeat(1000, []() { flush_delta(); });

  Serial.printf("[SK-UDP] broadcaster ready — multicast %s:%d\n",
                kSKMulticast.toString().c_str(), kSKPort);
}

void PublishSKFloat(const String& path, float value) {
  // Update existing entry if path already queued, otherwise append
  for (auto& e : pending_) {
    if (e.path == path) { e.fval = value; return; }
  }
  pending_.push_back({path, true, value, false});
}

void PublishSKBool(const String& path, bool value) {
  for (auto& e : pending_) {
    if (e.path == path) { e.bval = value; return; }
  }
  pending_.push_back({path, false, 0.0f, value});
}

}  // namespace halmet
