// HALMET live-data web interface
//
// Serves a simple label/value grid dashboard on port 8080, powered by a
// dedicated ESP-IDF HTTP server that lives alongside SensESP's own server
// (port 80).  SensESP already provides:
//  - WiFi management with soft-AP fallback (SSID "halmet", pw "thisisfine")
//  - Captive-portal WiFi configuration at http://192.168.4.1/wifi
//  - mDNS hostname resolution (halmet.local)
//  - Automatic WiFi reconnection
//
// This file adds:
//   /data         – live sensor dashboard (2 s refresh)
//   /api/data     – JSON sensor data
//   /debug        – full system diagnostics page (1 s refresh)
//   /api/debug    – JSON diagnostics dump

#include "halmet_web.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <Wire.h>
#include <elapsedMillis.h>
#include <esp_chip_info.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <rom/rtc.h>

#include <memory>
#include <utility>
#include <vector>

#include "halmet_const.h"
#include "sensesp.h"
#include "sensesp/net/http_server.h"
#include "sensesp/net/networking.h"
#include "sensesp_app.h"

using namespace sensesp;

// ---------------------------------------------------------------------------
// HTTPServer subclass with a unique ctrl_port.
//
// The ESP-IDF httpd component uses a loopback UDP socket ("ctrl socket") for
// internal task signalling.  Every active httpd instance must bind a distinct
// ctrl_port.  The default is 32768.  SensESP's own port-80 server takes 32768,
// so our port-8080 server uses 32769.
// ---------------------------------------------------------------------------
class HalmetHTTPServer : public HTTPServer {
 public:
  HalmetHTTPServer(int port, const String& config_path)
      : HTTPServer(port, config_path) {
    config_.ctrl_port = 32769;  // avoid clash with SensESP's server (32768)
  }
};

namespace halmet {

// ---------------------------------------------------------------------------
// Ordered sensor-data store
// ---------------------------------------------------------------------------

static std::vector<std::pair<String, String>> web_data_;

void UpdateWebDataValue(const String& label, const String& value) {
  for (auto& kv : web_data_) {
    if (kv.first == label) {
      kv.second = value;
      return;
    }
  }
  web_data_.push_back({label, value});
}

void UpdateWebDataValue(const String& label, float value, int decimals) {
  UpdateWebDataValue(label, String(value, decimals));
}

void UpdateWebDataValue(const String& label, bool value) {
  UpdateWebDataValue(label, value ? String("ON") : String("OFF"));
}

// ---------------------------------------------------------------------------
// Embedded HTML dashboard
// ---------------------------------------------------------------------------

static const char kDataHtml[] = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HALMET Live Data</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
       background:#0d1117;color:#c9d1d9;padding:20px}
  header{margin-bottom:20px}
  h1{color:#58a6ff;font-size:1.5em;margin-bottom:4px}
  .sub{color:#8b949e;font-size:.82em}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;
       background:#3fb950;margin-right:6px}
  .grid{display:grid;
        grid-template-columns:repeat(auto-fill,minmax(160px,1fr));
        gap:12px}
  .card{background:#161b22;border:1px solid #30363d;border-radius:8px;
        padding:14px 16px}
  .lbl{font-size:.7em;color:#8b949e;text-transform:uppercase;
       letter-spacing:.8px;margin-bottom:6px}
  .val{font-size:1.6em;font-weight:600;color:#58a6ff;
       white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .footer{margin-top:14px;font-size:.75em;color:#484f58}
  .footer a{color:#58a6ff;text-decoration:none}
  .footer a:hover{text-decoration:underline}
</style>
</head>
<body>
<header>
  <h1>HALMET Live Data</h1>
  <div class="sub"><span class="dot"></span>Updates every 2 seconds</div>
</header>
<div class="grid" id="g"></div>
<div class="footer" id="ts"></div>
<div class="footer" style="margin-top:8px">
  <a href="/debug">&#x1F527; Debug page</a>
  &nbsp;&middot;&nbsp;
  <a href="/wifi">&#x1F4F6; WiFi setup</a>
</div>
<script>
async function refresh(){
  try{
    const r=await fetch('/api/data');
    const j=await r.json();
    document.getElementById('g').innerHTML=
      j.data.map(d=>
        '<div class="card"><div class="lbl">'+d.label+
        '</div><div class="val">'+d.value+'</div></div>'
      ).join('');
    document.getElementById('ts').textContent=
      'Last updated \u00b7 '+new Date().toLocaleTimeString();
  }catch(e){
    document.getElementById('ts').textContent='Waiting for data\u2026';
  }
}
refresh();
setInterval(refresh,2000);
</script>
</body>
</html>
)";

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

static esp_err_t handle_data_html(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr(req, kDataHtml);
  return ESP_OK;
}

static esp_err_t handle_data_json(httpd_req_t* req) {
  JsonDocument doc;
  JsonArray arr = doc["data"].to<JsonArray>();
  for (const auto& kv : web_data_) {
    JsonObject obj = arr.add<JsonObject>();
    obj["label"] = kv.first;
    obj["value"] = kv.second;
  }
  String json;
  serializeJson(doc, json);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_sendstr(req, json.c_str());
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Debug page – HTML
// ---------------------------------------------------------------------------

static const char kDebugHtml[] = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HALMET Debug</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Courier New',monospace;background:#0d1117;color:#c9d1d9;padding:16px 20px}
header{display:flex;align-items:baseline;gap:16px;margin-bottom:16px}
h1{color:#58a6ff;font-size:1.3em}
#status{font-size:.75em;color:#8b949e}
.blink{animation:blink 1s step-start infinite}
@keyframes blink{50%{opacity:0}}
/* Section blocks */
.section{margin-bottom:18px}
.section-title{font-size:.65em;font-weight:700;color:#f0883e;text-transform:uppercase;
  letter-spacing:1.2px;border-bottom:1px solid #21262d;padding-bottom:4px;margin-bottom:8px}
table{width:100%;border-collapse:collapse}
tr:hover td{background:#161b22}
td{padding:3px 8px;font-size:.82em;border-bottom:1px solid #21262d;vertical-align:top}
td:first-child{color:#8b949e;width:44%;padding-right:16px;white-space:nowrap}
td:last-child{color:#e6edf3;font-weight:500;word-break:break-all}
.ok{color:#3fb950}.warn{color:#f0883e}.err{color:#f85149}
.bar-wrap{background:#21262d;border-radius:4px;height:8px;width:100%;margin-top:3px}
.bar{height:8px;border-radius:4px;background:#58a6ff;transition:width .4s}
.pin-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:6px}
.pin{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:7px 10px;font-size:.78em}
.pin .pname{color:#8b949e;font-size:.75em;text-transform:uppercase;letter-spacing:.8px}
.pin .pval{margin-top:2px;font-weight:700;font-size:1.05em}
.hi{color:#3fb950}.lo{color:#484f58}.an{color:#58a6ff}.out{color:#f0883e}
.sg{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-bottom:18px}
@media(max-width:600px){.sg{grid-template-columns:repeat(2,1fr)}}
.sc{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:10px 12px}
.sn{font-size:.65em;color:#f0883e;text-transform:uppercase;letter-spacing:1px;margin-bottom:4px}
.sv{font-size:1.15em;font-weight:700;color:#58a6ff}
.sv.al-on{color:#f85149}.sv.al-off{color:#3fb950}.sv.na{color:#484f58}
footer{margin-top:16px;font-size:.7em;color:#484f58;border-top:1px solid #21262d;padding-top:8px}
a{color:#58a6ff;text-decoration:none}a:hover{text-decoration:underline}
</style>
</head>
<body>
<header><h1>&#x1F41E; HALMET Debug</h1><span id="status">Loading&hellip;</span></header>
<div id="sensors"></div>
<div id="body"></div>
<footer>
  <a href="/data">Live Sensor Dashboard</a> &nbsp;|&nbsp;
  <a href="/">SensESP Config UI (port 80)</a> &nbsp;|&nbsp;
  <a href="/api/debug" target="_blank">Raw JSON</a>
</footer>
<script>
function cls(v,lo,hi){return v<lo?'err':v<hi?'warn':'ok';}
function pct(used,total){return total>0?Math.round(100*used/total):0;}
function bar(p){
  const c=p>85?'#f85149':p>65?'#f0883e':'#58a6ff';
  return '<div class="bar-wrap"><div class="bar" style="width:'+p+'%;background:'+c+'"></div></div>';
}
function row(k,v,cls_){
  return '<tr><td>'+k+'</td><td class="'+(cls_||'')+'">'+v+'</td></tr>';
}
function section(title,rows){
  return '<div class="section"><div class="section-title">'+title+'</div><table>'+rows+'</table></div>';
}
function build(d){
  let h='';
  // ----- System -----
  const heapPct=pct(d.heap.total-d.heap.free,d.heap.total);
  const psramPct=d.psram.total>0?pct(d.psram.total-d.psram.free,d.psram.total):-1;
  let sys=
    row('Hostname','<b>'+d.system.hostname+'</b>')+
    row('Uptime',d.system.uptime_s+' s &nbsp;<small style="color:#484f58">('+d.system.uptime_human+')</small>')+
    row('Reset Reason',d.system.reset_reason)+
    row('SDK Version',d.system.idf_version)+
    row('Arduino Build',d.system.build_date+' '+d.system.build_time);
  h+=section('System',sys);
  // ----- CPU -----
  let cpu=
    row('Model',d.cpu.model)+
    row('Revision','rev '+d.cpu.revision)+
    row('Cores',d.cpu.cores)+
    row('Frequency',d.cpu.freq_mhz+' MHz')+
    row('Features',d.cpu.features.join(', '))+
    row('Temperature',d.cpu.temp_c!==null?d.cpu.temp_c.toFixed(1)+' &deg;C':'n/a',
        d.cpu.temp_c!==null?(d.cpu.temp_c>70?'err':d.cpu.temp_c>55?'warn':'ok'):'');
  h+=section('CPU',cpu);
  // ----- Memory -----
  let mem=
    row('Heap Free',d.heap.free.toLocaleString()+' / '+d.heap.total.toLocaleString()+' bytes')+
    row('',bar(heapPct)+' '+heapPct+'% used',cls(100-heapPct,20,40))+
    row('Heap Min Free Ever',d.heap.min_free.toLocaleString()+' bytes')+
    row('Heap Largest Block',d.heap.largest_block.toLocaleString()+' bytes');
  if(psramPct>=0)
    mem+=row('PSRAM Free',d.psram.free.toLocaleString()+' / '+d.psram.total.toLocaleString()+' bytes')+
         row('',bar(psramPct)+' '+psramPct+'% used');
  h+=section('Memory',mem);
  // ----- Flash -----
  let fl=
    row('Chip Size',d.flash.chip_size.toLocaleString()+' bytes')+
    row('Speed',d.flash.speed_mhz+' MHz')+
    row('Mode',d.flash.mode)+
    row('Sketch Size',d.flash.sketch_size.toLocaleString()+' bytes')+
    row('Sketch Free',d.flash.sketch_free.toLocaleString()+' bytes')+
    row('Partition',d.flash.partition_label+' (type '+d.flash.partition_type+'/'+d.flash.partition_subtype+')');
  h+=section('Flash',fl);
  // ----- WiFi -----
  const wifiUp=d.wifi.status==='Connected';
  let wf=row('Status','<b class="'+(wifiUp?'ok':'err')+'">'+d.wifi.status+'</b>');
  if(wifiUp){
    wf+=row('SSID',d.wifi.ssid)+
        row('BSSID',d.wifi.bssid)+
        row('Channel',d.wifi.channel)+
        row('RSSI',d.wifi.rssi+' dBm',d.wifi.rssi>-60?'ok':d.wifi.rssi>-75?'warn':'err')+
        row('IP Address',d.wifi.ip)+
        row('Subnet Mask',d.wifi.subnet)+
        row('Gateway',d.wifi.gateway)+
        row('DNS',d.wifi.dns)+
        row('MAC (Station)',d.wifi.mac_sta)+
        row('TX Power',d.wifi.tx_power+' dBm')+
        row('Auto-Reconnect',d.wifi.auto_reconnect?'Yes':'No');
  } else {
    wf+=row('MAC (Station)',d.wifi.mac_sta);
  }
  if(d.wifi.ap_active){
    wf+=row('Soft-AP SSID','<span class="warn">'+d.wifi.ap_ssid+'</span>')+
        row('Soft-AP IP',d.wifi.ap_ip)+
        row('Soft-AP MAC',d.wifi.mac_ap)+
        row('Soft-AP Clients',d.wifi.ap_clients);
  }
  h+=section('WiFi',wf);
  // ----- Bluetooth -----
  h+=section('Bluetooth',row('BT Controller','Disabled in firmware (Classic+BLE not started)'));
  // ----- mDNS -----
  h+=section('mDNS',row('Hostname',d.system.hostname+'.local')+row('Advertised','http on :80, http on :8080'));
  // ----- I2C Bus -----
  let i2c=row('SDA Pin','GPIO '+d.i2c.sda)+row('SCL Pin','GPIO '+d.i2c.scl);
  d.i2c.found.forEach(a=>{ i2c+=row('Device @','0x'+a.toString(16).toUpperCase()); });
  if(d.i2c.found.length===0) i2c+=row('Scan Result','<span class="warn">No devices detected</span>');
  h+=section('I2C Bus',i2c);
  // ----- GPIO Pins -----
  h+='<div class="section"><div class="section-title">GPIO Pins</div><div class="pin-grid">';
  d.gpio.forEach(p=>{
    let cls_='',vstr='';
    if(p.mode==='ANALOG'){ cls_='an'; vstr=p.adc_raw+' raw / '+p.voltage_mv+' mV'; }
    else if(p.mode==='OUTPUT'){ cls_='out'; vstr=p.level?'HIGH':'LOW'; }
    else if(p.mode==='INPUT'||p.mode==='INPUT_PULLUP'||p.mode==='INPUT_PULLDOWN'){
      cls_=p.level?'hi':'lo'; vstr=(p.level?'HIGH':'LOW')+(p.mode!=='INPUT'?' ('+p.mode+')':'');
    } else { cls_=''; vstr=p.mode; }
    h+='<div class="pin"><div class="pname">GPIO '+p.pin+(p.label?' &middot; '+p.label:'')+'</div>'+
       '<div class="pval '+cls_+'">'+vstr+'</div></div>';
  });
  h+='</div></div>';
  // ----- N2K -----
  let n2k=row('CAN TX Pin','GPIO '+d.n2k.tx_pin)+
          row('CAN RX Pin','GPIO '+d.n2k.rx_pin)+
          row('Time Since RX',d.n2k.ms_since_rx+' ms')+
          row('Time Since TX',d.n2k.ms_since_tx+' ms');
  h+=section('NMEA 2000 / CAN',n2k);
  // ----- Event Loop -----
  let ev=row('Queue Size',d.event_loop.queue_size)+
         row('Tick Count',d.event_loop.tick_count.toLocaleString());
  h+=section('SensESP Event Loop',ev);
  return h;
}
async function refresh(){
  try{
    const r=await fetch('/api/debug?t='+Date.now());
    const d=await r.json();
    document.getElementById('body').innerHTML=build(d);
    document.getElementById('status').innerHTML=
      '<span class="blink ok">&#x25CF;</span> Updated '+new Date().toLocaleTimeString();
  }catch(e){
    document.getElementById('status').textContent='Error: '+e;
  }
}
const SLOTS=[
  {k:'Tank A1',    n:'A1 \u00b7 Tank'},
  {k:'Voltage A2', n:'A2 \u00b7 Voltage'},
  {k:'Voltage A3', n:'A3 \u00b7 Voltage'},
  {k:'Voltage A4', n:'A4 \u00b7 Voltage'},
  {k:'RPM D1',     n:'D1 \u00b7 Tacho'},
  {k:'Alarm D2',   n:'D2 \u00b7 Alarm'},
  {k:'Alarm D3',   n:'D3 \u00b7 Alarm'},
  {k:'Alarm D4',   n:'D4 \u00b7 Alarm'},
];
async function refreshSensors(){
  try{
    const r=await fetch('/api/data?t='+Date.now());
    const j=await r.json();
    const m={};
    j.data.forEach(d=>{m[d.label]=d.value;});
    let h='<div class="section"><div class="section-title">Sensor Readings</div><div class="sg">';
    SLOTS.forEach(s=>{
      const v=m[s.k]||'\u2014';
      const na=v==='\u2014';
      const isAlarm=s.k.indexOf('Alarm')>=0;
      let c='sv'+(na?' na':isAlarm?(v==='ON'?' al-on':' al-off'):'');
      h+='<div class="sc"><div class="sn">'+s.n+'</div><div class="'+c+'">'+v+'</div></div>';
    });
    h+='</div></div>';
    document.getElementById('sensors').innerHTML=h;
  }catch(e){}
}
refreshSensors();
setInterval(refreshSensors,1000);
refresh();
setInterval(refresh,1000);
</script>
</body>
</html>
)";

// ---------------------------------------------------------------------------
// Debug – helpers
// ---------------------------------------------------------------------------

static const char* reset_reason_str(int reason) {
  switch (reason) {
    case 1:  return "Power-on";
    case 2:  return "External pin";
    case 3:  return "Software";
    case 4:  return "Unknown/watchdog";
    case 5:  return "Deep-sleep wakeup";
    case 6:  return "SDIO";
    case 7:  return "TG0 watchdog";
    case 8:  return "TG1 watchdog";
    case 9:  return "RTC watchdog";
    case 10: return "Intrusion reset";
    case 11: return "Timer group reset";
    case 12: return "Software CPU reset";
    case 13: return "RTC watchdog CPU";
    case 14: return "Reset by PRO CPU";
    case 15: return "Brown-out";
    case 16: return "RTC watchdog (all)";
    default: return "Unknown";
  }
}

static String uptime_human(uint32_t s) {
  char buf[40];
  uint32_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  snprintf(buf, sizeof(buf), "%dd %02dh %02dm %02ds", d, h, m, (int)(s % 60));
  return String(buf);
}

static float cpu_temperature() {
  // The internal temperature sensor API was removed in Arduino ESP32 v3.x.
  // Return sentinel so the debug page shows "n/a".
  return -273.0f;
}

// Scan I2C bus and return found addresses.
// i2c and n2k timers live in global scope in main.cpp — declare them outside
// this namespace so the linker resolves them correctly.
} // namespace halmet

extern TwoWire* i2c;
extern elapsedMillis n2k_time_since_rx;
extern elapsedMillis n2k_time_since_tx;

namespace halmet {

static std::vector<uint8_t> scan_i2c() {
  std::vector<uint8_t> found;
  if (!i2c) return found;
  for (uint8_t addr = 1; addr < 127; addr++) {
    i2c->beginTransmission(addr);
    if (i2c->endTransmission() == 0) {
      found.push_back(addr);
    }
  }
  return found;
}

// N2K timing exposed from main.cpp
// (extern declarations now live above, outside the halmet namespace)

// ---------------------------------------------------------------------------
// Debug – JSON handler
// ---------------------------------------------------------------------------

// GPIO table: {pin, label, is_analog, is_output}
// Covers every HALMET-relevant pin.
struct GpioEntry {
  int pin;
  const char* label;
  bool is_analog;
  bool is_output;
};

static const GpioEntry kGpioPins[] = {
  // I2C
  {21, "SDA",      false, false},
  {22, "SCL",      false, false},
  // CAN / N2K
  {18, "CAN_RX",   false, false},
  {19, "CAN_TX",   false, true},
  // Digital inputs
  {23, "D1_TACHO", false, false},
  {25, "D2_ALARM", false, false},
  {27, "D3_ALARM", false, false},
  {26, "D4",       false, false},
  // Test output
  {33, "TEST_OUT", false, true},
  // OLED (I2C – shown for completeness)
  // ADS1115 is I2C; raw analog pins reported via float values from the IC
};

static esp_err_t handle_debug_json(httpd_req_t* req) {
  JsonDocument doc;

  // ----- System -----
  {
    uint32_t up_s = millis() / 1000;
    auto sys = doc["system"].to<JsonObject>();
    sys["hostname"]    = WiFi.getHostname();
    sys["uptime_s"]    = up_s;
    sys["uptime_human"] = uptime_human(up_s);
    sys["reset_reason"] = reset_reason_str(rtc_get_reset_reason(0));
    sys["idf_version"]  = esp_get_idf_version();
    sys["build_date"]   = __DATE__;
    sys["build_time"]   = __TIME__;
  }

  // ----- CPU -----
  {
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    auto cpu = doc["cpu"].to<JsonObject>();
    cpu["model"]    = "ESP32";
    cpu["revision"] = (int)ci.revision;
    cpu["cores"]    = (int)ci.cores;
    cpu["freq_mhz"] = getCpuFrequencyMhz();
    auto feats = cpu["features"].to<JsonArray>();
    if (ci.features & CHIP_FEATURE_WIFI_BGN) feats.add("WiFi 802.11b/g/n");
    if (ci.features & CHIP_FEATURE_BT)       feats.add("Bluetooth Classic");
    if (ci.features & CHIP_FEATURE_BLE)      feats.add("BLE");
    if (ci.features & CHIP_FEATURE_EMB_FLASH) feats.add("Embedded Flash");
    // Temperature: >-273 means valid
    float t = cpu_temperature();
    if (t > -273.0f) cpu["temp_c"] = t;
    else             cpu["temp_c"] = nullptr;
  }

  // ----- Memory -----
  {
    auto heap = doc["heap"].to<JsonObject>();
    heap["total"]         = (int)ESP.getHeapSize();
    heap["free"]          = (int)ESP.getFreeHeap();
    heap["min_free"]      = (int)ESP.getMinFreeHeap();
    heap["largest_block"] = (int)ESP.getMaxAllocHeap();

    auto psram = doc["psram"].to<JsonObject>();
    psram["total"] = (int)ESP.getPsramSize();
    psram["free"]  = (int)ESP.getFreePsram();
  }

  // ----- Flash -----
  {
    auto fl = doc["flash"].to<JsonObject>();
    fl["chip_size"]   = (int)ESP.getFlashChipSize();
    fl["speed_mhz"]   = (int)(ESP.getFlashChipSpeed() / 1000000);
    fl["mode"]        = (int)ESP.getFlashChipMode();  // 0=QIO,1=QOUT,2=DIO,3=DOUT
    fl["sketch_size"] = (int)ESP.getSketchSize();
    fl["sketch_free"] = (int)ESP.getFreeSketchSpace();
    const esp_partition_t* p = esp_ota_get_running_partition();
    if (p) {
      fl["partition_label"]    = p->label;
      fl["partition_type"]     = (int)p->type;
      fl["partition_subtype"]  = (int)p->subtype;
    }
  }

  // ----- WiFi -----
  {
    auto wf = doc["wifi"].to<JsonObject>();
    bool connected = (WiFi.status() == WL_CONNECTED);
    wf["status"]         = connected ? "Connected" : "Disconnected";
    wf["mac_sta"]        = WiFi.macAddress();
    wf["auto_reconnect"] = WiFi.getAutoReconnect();
    if (connected) {
      wf["ssid"]    = WiFi.SSID();
      wf["bssid"]   = WiFi.BSSIDstr();
      wf["channel"] = (int)WiFi.channel();
      wf["rssi"]    = (int)WiFi.RSSI();
      wf["ip"]      = WiFi.localIP().toString();
      wf["subnet"]  = WiFi.subnetMask().toString();
      wf["gateway"] = WiFi.gatewayIP().toString();
      wf["dns"]     = WiFi.dnsIP().toString();
      wf["tx_power"] = (int)WiFi.getTxPower();
    }
    bool ap_active = (WiFi.softAPIP() != IPAddress(0, 0, 0, 0));
    wf["ap_active"] = ap_active;
    wf["mac_ap"]    = WiFi.softAPmacAddress();
    if (ap_active) {
      wf["ap_ssid"]    = WiFi.softAPSSID();
      wf["ap_ip"]      = WiFi.softAPIP().toString();
      wf["ap_clients"] = (int)WiFi.softAPgetStationNum();
    }
  }

  // ----- I2C -----
  {
    auto i2c_obj = doc["i2c"].to<JsonObject>();
    i2c_obj["sda"] = 21;
    i2c_obj["scl"] = 22;
    auto found = i2c_obj["found"].to<JsonArray>();
    // Only scan occasionally – scan is slow and blocks the bus.
    // We cache the last result and refresh every ~10 s.
    static unsigned long last_scan_ms = 0;
    static std::vector<uint8_t> cached_addrs;
    if (millis() - last_scan_ms > 10000 || last_scan_ms == 0) {
      cached_addrs = scan_i2c();
      last_scan_ms = millis();
    }
    for (uint8_t a : cached_addrs) found.add((int)a);
  }

  // ----- GPIO -----
  {
    auto gpio_arr = doc["gpio"].to<JsonArray>();
    for (const auto& g : kGpioPins) {
      auto p = gpio_arr.add<JsonObject>();
      p["pin"]   = g.pin;
      p["label"] = g.label;
      if (g.is_output) {
        p["mode"]  = "OUTPUT";
        p["level"] = (int)digitalRead(g.pin);
      } else if (g.is_analog) {
        p["mode"]       = "ANALOG";
        int raw         = analogRead(g.pin);
        p["adc_raw"]    = raw;
        p["voltage_mv"] = (int)(raw * 3300 / 4095);
      } else {
        p["mode"]  = "INPUT";
        p["level"] = (int)digitalRead(g.pin);
      }
    }
  }

  // ----- N2K -----
  {
    auto n2k = doc["n2k"].to<JsonObject>();
    n2k["tx_pin"]     = (int)sensesp::kCANTxPin;
    n2k["rx_pin"]     = (int)sensesp::kCANRxPin;
    n2k["ms_since_rx"] = (uint32_t)n2k_time_since_rx;
    n2k["ms_since_tx"] = (uint32_t)n2k_time_since_tx;
  }

  // ----- Event loop -----
  {
    auto ev = doc["event_loop"].to<JsonObject>();
    auto el = sensesp::SensESPBaseApp::get_event_loop();
    if (el) {
      ev["queue_size"] = (int)el->getEventQueueSize();
      ev["tick_count"] = (uint64_t)el->getTickCount();
    }
  }

  String json;
  serializeJson(doc, json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr(req, json.c_str());
  return ESP_OK;
}

static esp_err_t handle_debug_html(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr(req, kDebugHtml);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Server lifetime
// ---------------------------------------------------------------------------

// Must outlive setup() so the server is not destroyed.
static std::shared_ptr<HTTPServer> data_server_;

// ---------------------------------------------------------------------------
// WiFi setup page
// ---------------------------------------------------------------------------

static const char kWifiHtml[] = R"WIFI(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HALMET WiFi Setup</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
       background:#0d1117;color:#c9d1d9;padding:20px;max-width:480px;margin:0 auto}
  h1{color:#58a6ff;font-size:1.4em;margin-bottom:4px}
  .sub{color:#8b949e;font-size:.82em;margin-bottom:24px}
  label{display:block;font-size:.8em;color:#8b949e;text-transform:uppercase;
        letter-spacing:.6px;margin-bottom:5px}
  input{width:100%;background:#161b22;border:1px solid #30363d;border-radius:6px;
        color:#c9d1d9;padding:9px 12px;font-size:.95em;outline:none;
        transition:border .15s}
  input:focus{border-color:#58a6ff}
  .pw-wrap{position:relative}
  .pw-wrap input{padding-right:44px}
  .eye{position:absolute;right:10px;top:50%;transform:translateY(-50%);
       background:none;border:none;cursor:pointer;color:#8b949e;font-size:1.1em;
       padding:4px}
  .eye:hover{color:#c9d1d9}
  .field{margin-bottom:16px}
  .net-list{margin-bottom:20px;max-height:220px;overflow-y:auto;
            border:1px solid #30363d;border-radius:6px}
  .net{display:flex;align-items:center;justify-content:space-between;
       padding:10px 14px;cursor:pointer;border-bottom:1px solid #21262d;
       transition:background .12s}
  .net:last-child{border-bottom:none}
  .net:hover{background:#161b22}
  .net.active{background:#1c2a3a;border-left:3px solid #58a6ff}
  .ssid{font-size:.92em}
  .sig{font-size:.75em;color:#8b949e}
  .btn{width:100%;padding:10px;border:none;border-radius:6px;font-size:.95em;
       font-weight:600;cursor:pointer;margin-bottom:10px;transition:opacity .15s}
  .btn:active{opacity:.75}
  .btn-scan{background:#21262d;color:#c9d1d9;border:1px solid #30363d}
  .btn-save{background:#238636;color:#fff}
  .btn-save:disabled{background:#1b4a2a;color:#484f58;cursor:not-allowed}
  .msg{padding:10px 14px;border-radius:6px;font-size:.86em;margin-bottom:14px;display:none}
  .msg.ok{background:#1b4a2a;color:#3fb950;border:1px solid #238636;display:block}
  .msg.err{background:#3d1010;color:#f85149;border:1px solid #6e1a1a;display:block}
  .msg.info{background:#161b22;color:#8b949e;border:1px solid #30363d;display:block}
  .lock{font-size:.65em;margin-left:4px;opacity:.6}
  .back{display:inline-block;margin-top:18px;color:#58a6ff;font-size:.82em;text-decoration:none}
  .back:hover{text-decoration:underline}
  .security{font-size:.7em;color:#8b949e;margin-top:4px}
</style>
</head>
<body>
<h1>WiFi Setup</h1>
<div class="sub">Select a network or enter SSID manually</div>

<div id="msg" class="msg"></div>

<button class="btn btn-scan" onclick="scan()">&#x1F50D; Scan for networks</button>
<div class="net-list" id="nets" style="display:none"></div>

<div class="field">
  <label>Network name (SSID)</label>
  <input id="ssid" type="text" placeholder="Enter or select above" autocomplete="off" autocorrect="off" spellcheck="false">
</div>
<div class="field">
  <label>Password</label>
  <div class="pw-wrap">
    <input id="pw" type="password" placeholder="Leave empty for open networks" autocomplete="new-password">
    <button class="eye" id="eyebtn" onclick="togglePw()" title="Show/hide password" type="button">&#x1F441;</button>
  </div>
  <div class="security" id="sec-note"></div>
</div>

<button class="btn btn-save" id="savebtn" onclick="save()">&#x2713; Save &amp; Connect</button>
<br>
<a href="/data" class="back">&#x2190; Back to dashboard</a>

<script>
let scanning=false;
function setMsg(txt,cls){
  const m=document.getElementById('msg');
  m.className='msg '+cls; m.textContent=txt;
}
function clearMsg(){
  const m=document.getElementById('msg');
  m.className='msg'; m.textContent='';
}
function sigBars(rssi){
  const pct=Math.min(100,Math.max(0,2*(rssi+100)));
  if(pct>70)return'▂▄▆█';
  if(pct>45)return'▂▄▆·';
  if(pct>20)return'▂▄··';
  return'▂···';
}
function encIcon(enc){
  return enc===7?'':'<span class="lock">&#x1F512;</span>';
}
async function scan(){
  if(scanning)return;
  scanning=true;
  const b=document.querySelector('.btn-scan');
  b.textContent='Scanning…'; b.disabled=true;
  clearMsg();
  try{
    const r=await fetch('/api/wifi/scan');
    const d=await r.json();
    const list=document.getElementById('nets');
    if(!d.networks||d.networks.length===0){
      setMsg('No networks found. Try scanning again.','info');
      list.style.display='none';
    } else {
      list.innerHTML=d.networks.map(n=>
        `<div class="net" onclick="pick(this,'${n.ssid.replace(/'/g,"\\'")}',${n.enc})" data-ssid="${n.ssid}">
          <span class="ssid">${n.ssid}${encIcon(n.enc)}</span>
          <span class="sig">${sigBars(n.rssi)} ${n.rssi} dBm</span>
        </div>`
      ).join('');
      list.style.display='block';
    }
  }catch(e){
    setMsg('Scan failed: '+e,'err');
  }
  b.textContent='&#x1F50D; Scan for networks'; b.disabled=false;
  scanning=false;
}
function pick(el,ssid,enc){
  document.querySelectorAll('.net').forEach(n=>n.classList.remove('active'));
  el.classList.add('active');
  document.getElementById('ssid').value=ssid;
  document.getElementById('pw').focus();
  const note=document.getElementById('sec-note');
  note.textContent=enc===7?'Open network — no password needed':'WPA/WPA2 encrypted';
}
function togglePw(){
  const f=document.getElementById('pw');
  const b=document.getElementById('eyebtn');
  if(f.type==='password'){f.type='text';b.title='Hide password';}
  else{f.type='password';b.title='Show password';}
}
async function save(){
  const ssid=document.getElementById('ssid').value.trim();
  const pw=document.getElementById('pw').value;
  if(!ssid){setMsg('Please enter a network name.','err');return;}
  const btn=document.getElementById('savebtn');
  btn.disabled=true; btn.textContent='Saving…';
  clearMsg();
  try{
    const r=await fetch('/api/wifi/save',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid,password:pw})
    });
    const d=await r.json();
    if(d.success){
      setMsg('Saved! Device is connecting to "'+ssid+'" and will reboot shortly.','ok');
      btn.textContent='&#x2713; Saved';
    } else {
      setMsg('Error: '+(d.error||'unknown'),'err');
      btn.disabled=false; btn.textContent='&#x2713; Save & Connect';
    }
  }catch(e){
    setMsg('Request failed: '+e,'err');
    btn.disabled=false; btn.textContent='&#x2713; Save & Connect';
  }
}
// auto-scan on load
scan();
</script>
</body>
</html>
)WIFI";

static esp_err_t handle_wifi_html(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr(req, kWifiHtml);
  return ESP_OK;
}

// GET /api/wifi/scan — trigger a scan and return results
static esp_err_t handle_wifi_scan(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");

  // WiFi.scanNetworks blocks for ~2-3 s; run it synchronously in this handler
  int n = WiFi.scanNetworks(false, false);

  JsonDocument doc;
  JsonArray nets = doc["networks"].to<JsonArray>();

  if (n > 0) {
    // Sort by RSSI descending, deduplicate SSIDs
    // Build list, skip hidden or empty SSIDs
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      // Skip duplicate SSIDs (keep strongest signal)
      bool dup = false;
      for (JsonObject existing : nets) {
        if (existing["ssid"].as<String>() == ssid) { dup = true; break; }
      }
      if (dup) continue;
      JsonObject net = nets.add<JsonObject>();
      net["ssid"] = ssid;
      net["rssi"] = WiFi.RSSI(i);
      net["enc"]  = (int)WiFi.encryptionType(i);  // WIFI_AUTH_OPEN = 0, WPA2 = 3, etc; 7 = open in older enum
    }
    // Sort by rssi descending (simple selection sort on small list)
    int sz = nets.size();
    for (int i = 0; i < sz - 1; i++) {
      int best = i;
      for (int j = i + 1; j < sz; j++) {
        if (nets[j]["rssi"].as<int>() > nets[best]["rssi"].as<int>()) best = j;
      }
      if (best != i) {
        // swap
        String ss = nets[i]["ssid"].as<String>(); int rs = nets[i]["rssi"]; int en = nets[i]["enc"];
        nets[i]["ssid"] = nets[best]["ssid"].as<String>(); nets[i]["rssi"] = nets[best]["rssi"].as<int>(); nets[i]["enc"] = nets[best]["enc"].as<int>();
        nets[best]["ssid"] = ss; nets[best]["rssi"] = rs; nets[best]["enc"] = en;
      }
    }
  }
  WiFi.scanDelete();

  String out;
  serializeJson(doc, out);
  httpd_resp_sendstr(req, out.c_str());
  return ESP_OK;
}

// POST /api/wifi/save — persist credentials to SensESP Networking and reboot
static esp_err_t handle_wifi_save(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");

  // Read POST body (max 256 bytes)
  char buf[256] = {};
  int total = 0, ret;
  while (total < (int)sizeof(buf) - 1) {
    ret = httpd_req_recv(req, buf + total, sizeof(buf) - 1 - total);
    if (ret <= 0) break;
    total += ret;
  }
  buf[total] = '\0';

  JsonDocument body;
  if (deserializeJson(body, buf) != DeserializationError::Ok ||
      !body["ssid"].is<String>()) {
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid JSON\"}");
    return ESP_OK;
  }

  String ssid = body["ssid"].as<String>();
  String password = body["password"] | "";

  // Build config JSON in the format Networking::from_json() expects
  // and call save() on the Networking object.
  auto app = std::static_pointer_cast<SensESPApp>(SensESPBaseApp::get());
  if (!app) {
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"App not ready\"}");
    return ESP_OK;
  }

  auto& networking = app->get_networking();
  if (!networking) {
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Networking not ready\"}");
    return ESP_OK;
  }

  // Build the config JSON matching Networking's from_json format
  JsonDocument cfg;
  // Keep AP enabled so AP mode still available after failed connect
  cfg["apSettings"]["enabled"]             = true;
  cfg["apSettings"]["ssid"]                = SensESPBaseApp::get_hostname();
  cfg["apSettings"]["password"]            = "thisisfine";
  cfg["apSettings"]["channel"]             = 9;
  cfg["apSettings"]["hidden"]              = false;
  cfg["apSettings"]["captive_portal_enabled"] = true;

  cfg["clientSettings"]["enabled"]         = true;
  JsonArray clients = cfg["clientSettings"]["settings"].to<JsonArray>();
  JsonObject client = clients.add<JsonObject>();
  client["ssid"]     = ssid;
  client["password"] = password;
  client["use_dhcp"] = true;

  JsonObject cfgObj = cfg.as<JsonObject>();
  networking->from_json(cfgObj);
  networking->save();

  Serial.printf("[WiFi] Saved credentials for \"%s\" — rebooting in 1s\n",
                ssid.c_str());

  httpd_resp_sendstr(req, "{\"success\":true}");

  // Reboot after response is sent so the client gets the reply
  event_loop()->onDelay(1000, []() { esp_restart(); });

  return ESP_OK;
}



void SetupWebDataDisplay() {
  // Create a separate HTTP server instance on port 8080.
  // SensESP's own web UI continues to run on port 80.
  data_server_ =
      std::make_shared<HalmetHTTPServer>(8080, "/system/halmet_data_server");

  // Register the dashboard HTML handler
  auto html_handler = std::make_shared<HTTPRequestHandler>(
      1 << HTTP_GET, "/data", handle_data_html);
  data_server_->add_handler(html_handler);

  // Register the JSON data API handler
  auto json_handler = std::make_shared<HTTPRequestHandler>(
      1 << HTTP_GET, "/api/data", handle_data_json);
  data_server_->add_handler(json_handler);

  // Register the debug page handlers
  auto debug_html_handler = std::make_shared<HTTPRequestHandler>(
      1 << HTTP_GET, "/debug", handle_debug_html);
  data_server_->add_handler(debug_html_handler);

  auto debug_json_handler = std::make_shared<HTTPRequestHandler>(
      1 << HTTP_GET, "/api/debug", handle_debug_json);
  data_server_->add_handler(debug_json_handler);

  // Register the WiFi setup page handlers
  auto wifi_html_handler = std::make_shared<HTTPRequestHandler>(
      1 << HTTP_GET, "/wifi", handle_wifi_html);
  data_server_->add_handler(wifi_html_handler);

  auto wifi_scan_handler = std::make_shared<HTTPRequestHandler>(
      1 << HTTP_GET, "/api/wifi/scan", handle_wifi_scan);
  data_server_->add_handler(wifi_scan_handler);

  auto wifi_save_handler = std::make_shared<HTTPRequestHandler>(
      1 << HTTP_POST, "/api/wifi/save", handle_wifi_save);
  data_server_->add_handler(wifi_save_handler);

  // Advertise services via mDNS once WiFi is connected, and re-register
  // whenever the connection is re-established (e.g. after a drop).
  //
  // Two named _http._tcp instances are published so that mDNS browsers and
  // e-ink displays can discover them by name and navigate directly to the
  // correct path via the "path" TXT record:
  //
  //   "HALMET Live Data"  → _http._tcp  port 8080  path=/data
  //   "HALMET Debug"      → _http._tcp  port 8080  path=/debug
  //
  // The SensESP config UI is already published on port 80 by SensESP itself.
  static IPAddress last_ip;
  event_loop()->onRepeat(5000, []() {
    if (WiFi.status() != WL_CONNECTED) {
      last_ip = IPAddress(0, 0, 0, 0);
      return;
    }
    IPAddress current_ip = WiFi.localIP();
    if (current_ip == last_ip) return;  // already registered for this IP
    last_ip = current_ip;

    // Live-data dashboard
    MDNS.addService("http", "tcp", 8080);
    MDNS.addServiceTxt("http", "tcp", "name",    "HALMET Live Data");
    MDNS.addServiceTxt("http", "tcp", "path",    "/data");
    MDNS.addServiceTxt("http", "tcp", "board",   "HALMET");
    MDNS.addServiceTxt("http", "tcp", "version", "1.0");

    // Debug page — second instance on the same port with a different path.
    // Some mDNS stacks only expose one record per service type; the TXT
    // record above therefore points to /data.  Clients that parse TXT
    // "path" will land on the dashboard; navigating to /debug manually
    // (or via the dashboard footer link) reaches the debug page.
    MDNS.addServiceTxt("http", "tcp", "debug_path", "/debug");

    ESP_LOGI("halmet_web",
             "mDNS: registered _http._tcp on :8080 as 'HALMET Live Data' "
             "(path=/data) at %s",
             current_ip.toString().c_str());
  });

  // Log URLs to serial once the network (STA or AP) is ready.
  event_loop()->onRepeat(2000, []() {
    static bool logged_sta = false;
    static bool logged_ap  = false;

    if (!logged_sta && WiFi.status() == WL_CONNECTED) {
      logged_sta = true;
      Serial.printf(
          "\n[WiFi] Connected to \"%s\"  IP: %s\n"
          "[HALMET] Live sensors: http://%s:8080/data\n"
          "[HALMET] Debug page:   http://%s:8080/debug\n"
          "[HALMET] SensESP UI:   http://%s/\n"
          "[HALMET] mDNS:         http://halmet.local:8080/\n\n",
          WiFi.SSID().c_str(),
          WiFi.localIP().toString().c_str(),
          WiFi.localIP().toString().c_str(),
          WiFi.localIP().toString().c_str(),
          WiFi.localIP().toString().c_str());
    }

    wifi_mode_t mode = WiFi.getMode();
    if (!logged_ap && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
        && WiFi.softAPIP() != IPAddress(0,0,0,0)) {
      logged_ap = true;
      Serial.printf(
          "\n[WiFi] AP mode active — SSID: \"%s\"  IP: %s\n"
          "[HALMET] WiFi config:   http://192.168.4.1/\n"
          "[HALMET] Dashboard:     http://192.168.4.1:8080/data\n\n",
          WiFi.softAPSSID().c_str(),
          WiFi.softAPIP().toString().c_str());
    }
  });
}

}  // namespace halmet
