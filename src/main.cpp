#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEScan.h>

const char* apSSID = "ESP32-Monitor";
const char* apPASS = "12345678";

WebServer server(80);
BLEScan* pBLEScan = nullptr;

// Track recent serial activity to guess if we’re plugged into a PC
unsigned long lastSerialActivity = 0;

// ---------- Internal temperature (chip) helpers ----------
extern "C" uint8_t temprature_sens_read();  // provided by ESP-IDF

float readChipTemperatureC() {
  // Convert from raw sensor reading to approximate Celsius
  return (temprature_sens_read() - 32) / 1.8f;
}

// Track min/max and simple history of temperature (Celsius)
const int TEMP_HISTORY_SIZE = 40;
float tempHistory[TEMP_HISTORY_SIZE];
int   tempHistoryCount = 0;
bool  tempStatsInitialized = false;
float tempMinC = 0.0f;
float tempMaxC = 0.0f;

void recordTemperatureSample(float tC) {
  if (!tempStatsInitialized) {
    tempMinC = tempMaxC = tC;
    tempStatsInitialized = true;
  } else {
    if (tC < tempMinC) tempMinC = tC;
    if (tC > tempMaxC) tempMaxC = tC;
  }

  if (tempHistoryCount < TEMP_HISTORY_SIZE) {
    tempHistoryCount++;
  } else {
    // shift left, drop oldest
    for (int i = 1; i < TEMP_HISTORY_SIZE; ++i) {
      tempHistory[i - 1] = tempHistory[i];
    }
  }
  tempHistory[tempHistoryCount - 1] = tC;
}

// ---------- Helpers ----------

String formatBytes(size_t bytes) {
  const char* sizes[] = { "B", "KB", "MB" };
  int order = 0;
  double fBytes = bytes;

  while (fBytes >= 1024 && order < 2) {
    order++;
    fBytes = fBytes / 1024.0;
  }

  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.2f %s", fBytes, sizes[order]);
  return String(buffer);
}

String getUptimeString() {
  unsigned long ms = millis();
  unsigned long seconds = ms / 1000;
  unsigned long s = seconds % 60;
  unsigned long minutes = (seconds / 60) % 60;
  unsigned long hours = (seconds / 3600) % 24;
  unsigned long days = seconds / 86400;

  char buf[64];
  snprintf(buf, sizeof(buf), "%lu d %02lu:%02lu:%02lu", days, hours, minutes, s);
  return String(buf);
}

bool isSerialActiveRecently() {
  return (millis() - lastSerialActivity) < 10000UL; // 10 seconds
}

// ---------- Common HTML head + header/nav ----------

String commonHtmlHead(const String& pageTitle, const String& active) {
  String html;
  html += F(
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>"
  );
  html += pageTitle;
  html += F(
    "</title>"
    "<style>"
    "body{font-family:Arial,Helvetica,sans-serif;background:#050509;color:#f3f3f3;margin:0;padding:0;}"
    ".topbar{position:sticky;top:0;z-index:10;background:rgba(5,5,12,0.96);backdrop-filter:blur(10px);"
      "border-bottom:1px solid #222;padding:8px 14px;display:flex;align-items:center;justify-content:space-between;}"
    ".brand{font-weight:bold;font-size:1rem;display:flex;align-items:center;gap:8px;}"
    ".brand-badge{width:18px;height:18px;border-radius:4px;border:1px solid #888;position:relative;}"
    ".brand-badge::after{content:'';position:absolute;left:3px;right:3px;top:3px;bottom:3px;border-radius:2px;border:1px solid #888;}"
    ".brand-text{letter-spacing:0.05em;font-size:0.85rem;color:#ddd;}"
    ".chip-pill{font-size:0.7rem;padding:2px 8px;border-radius:999px;border:1px solid #444;background:#131325;color:#8ec5ff;margin-left:6px;}"
    ".nav-links{display:flex;gap:8px;font-size:0.8rem;flex-wrap:wrap;justify-content:flex-end;}"
    ".nav-link{display:inline-flex;align-items:center;gap:6px;padding:4px 8px;border-radius:999px;"
      "color:#aaa;text-decoration:none;border:1px solid transparent;}"
    ".nav-link:hover{background:#181828;border-color:#333;color:#fff;}"
    ".nav-link.active{background:#1b2140;border-color:#3b4a7a;color:#fff;}"
    ".icon{display:inline-block;width:14px;height:14px;border-radius:3px;border:1px solid #888;position:relative;}"
    ".icon-device::after{content:'';position:absolute;left:3px;right:3px;top:3px;bottom:3px;border-radius:2px;border:1px solid #888;}"
    ".icon-env::after{content:'';position:absolute;left:3px;right:3px;top:3px;bottom:3px;border-radius:50%;border:1px solid #888;}"
    ".icon-wifi::before{content:'';position:absolute;left:2px;right:2px;bottom:2px;border-radius:50% 50% 0 0;border:2px solid #888;border-bottom:0;}"
    ".icon-wifi::after{content:'';position:absolute;left:5px;right:5px;bottom:3px;border-radius:50% 50% 0 0;border:1px solid #888;border-bottom:0;}"
    ".icon-bt::before{content:'';position:absolute;left:6px;right:6px;top:2px;bottom:2px;border-left:1px solid #888;}"
    ".icon-bt::after{content:'';position:absolute;left:4px;right:4px;top:4px;bottom:4px;border-right:1px solid #888;border-top:1px solid #888;border-bottom:1px solid #888;clip-path:polygon(50% 0,100% 50%,50% 100%,0 50%);}"
    ".icon-crowd::after{content:'';position:absolute;left:3px;right:3px;top:3px;bottom:3px;border-radius:2px;border:1px solid #888;box-shadow:0 0 0 1px #888 inset;}"
    ".icon-rf::after{content:'';position:absolute;left:3px;right:3px;top:3px;bottom:3px;border-radius:50%;border:1px solid #888;border-top-style:dashed;}"
    ".container{max-width:900px;margin:16px auto;padding:16px;}"
    "h1{font-size:1.4rem;margin:6px 0 10px 0;}"
    "h2{font-size:1.05rem;margin-top:20px;margin-bottom:8px;border-bottom:1px solid #333;padding-bottom:4px;}"
    "table{width:100%;border-collapse:collapse;margin-bottom:4px;}"
    "td,th{padding:6px 4px;vertical-align:top;font-size:0.9rem;}"
    "td.label{color:#9a9a9a;width:40%;}"
    ".footer{margin-top:18px;font-size:0.78rem;color:#777;text-align:center;}"
    ".subtle{font-size:0.8rem;color:#888;margin-top:2px;}"
    ".card{background:#0a0c16;border-radius:10px;border:1px solid #1d2030;padding:10px 12px;margin:10px 0;}"
    ".badge{display:inline-block;padding:2px 6px;border-radius:999px;font-size:0.7rem;border:1px solid #444;color:#aaa;}"
    ".btn{display:inline-block;padding:6px 10px;border-radius:999px;border:1px solid #444;background:#121327;"
      "color:#eee;font-size:0.8rem;text-decoration:none;margin-right:6px;}"
    ".btn:hover{background:#1b1d3b;}"
    ".status-pill{display:inline-flex;align-items:center;gap:8px;padding:4px 8px;border-radius:999px;"
      "background:#111323;border:1px solid #26283b;font-size:0.8rem;margin-right:6px;margin-bottom:4px;}"
    ".status-dot{width:9px;height:9px;border-radius:50%;background:#555;box-shadow:0 0 6px rgba(0,0,0,0.8);}"
    ".ok{background:#00d46a;box-shadow:0 0 8px #00d46a;}"
    ".warn{background:#ffc107;box-shadow:0 0 8px #ffc107;}"
    ".bad{background:#ff5252;box-shadow:0 0 8px #ff5252;}"
    ".table-list{width:100%;border-collapse:collapse;margin-top:8px;}"
    ".table-list th,.table-list td{border-bottom:1px solid #222;font-size:0.85rem;}"
    ".table-list th{color:#bbb;font-weight:bold;text-align:left;}"
    ".temp-graph{margin-top:6px;height:80px;border-radius:6px;background:#070812;"
      "border:1px solid #202235;padding:4px 4px 2px 4px;display:flex;align-items:flex-end;gap:2px;}"
    ".temp-bar{flex:1;border-radius:2px 2px 0 0;background:linear-gradient(to top,#ff7043,#ffa726);}"
    ".temp-baseline{display:flex;justify-content:space-between;font-size:0.7rem;color:#777;margin-top:2px;}"
    ".heat-graph{margin-top:6px;height:70px;border-radius:6px;background:#070812;border:1px solid #202235;padding:4px;display:flex;align-items:flex-end;gap:3px;}"
    ".heat-bar{flex:1;border-radius:3px 3px 0 0;background:linear-gradient(to top,#3949ab,#8e24aa);}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='topbar'>"
      "<div class='brand'>"
        "<div class='brand-badge'></div>"
        "<div class='brand-text'>ESP32 MONITOR</div>"
        "<span class='chip-pill'>AP 192.168.4.1</span>"
      "</div>"
      "<nav class='nav-links'>"
  );

  auto navLink = [&](const char* id, const char* href, const char* iconClass, const char* label) {
    html += "<a class='nav-link";
    if (active == id) html += " active";
    html += "' href='";
    html += href;
    html += "'><span class='icon ";
    html += iconClass;
    html += "'></span><span>";
    html += label;
    html += "</span></a>";
  };

  navLink("device", "/device", "icon-device", "Device");
  navLink("environment", "/environment", "icon-env", "Environment");
  navLink("wifi", "/wifi", "icon-wifi", "Wi-Fi Scan");
  navLink("ble", "/ble", "icon-bt", "Bluetooth");
  navLink("crowd", "/crowd", "icon-crowd", "Crowd");
  navLink("rf", "/rf", "icon-rf", "Interference");

  html += F(
      "</nav>"
    "</div>"
    "<div class='container'>"
  );
  return html;
}

// ---------- Device page (/device) ----------

String buildDevicePage() {
  size_t heapSize   = ESP.getHeapSize();
  size_t freeHeap   = ESP.getFreeHeap();
  float  heapRatio  = heapSize ? (float)freeHeap / (float)heapSize : 0.0f;

  String heapClass = "ok";
  String heapText  = "Healthy";
  if (heapRatio < 0.3f) {
    heapClass = "bad"; heapText = "Low";
  } else if (heapRatio < 0.6f) {
    heapClass = "warn"; heapText = "Moderate";
  }

  bool serialActive = isSerialActiveRecently();

  String html = commonHtmlHead("ESP32 Device", "device");
  html += "<h1>Device</h1>";

  html += "<div class='card'>";
  html += "<div class='status-pill'><span class='status-dot ok'></span><span>Wi-Fi Access Point</span></div>";
  html += "<div class='status-pill'><span class='status-dot " + heapClass + "'></span><span>Heap: " + heapText + "</span></div>";
  html += "<div class='status-pill'><span class='status-dot " + String(serialActive ? "ok" : "warn") + "'></span>";
  html += "<span>Host: " + String(serialActive ? "Serial activity detected" : "No recent serial activity") + "</span></div>";
  html += "</div>";

  html += "<h2>Chip</h2><table>";
  html += "<tr><td class='label'>Model</td><td>" + String(ESP.getChipModel()) + "</td></tr>";
  html += "<tr><td class='label'>Revision</td><td>" + String(ESP.getChipRevision()) + "</td></tr>";
  html += "<tr><td class='label'>CPU Cores</td><td>" + String(ESP.getChipCores()) + "</td></tr>";
  html += "<tr><td class='label'>CPU Frequency</td><td>" + String(ESP.getCpuFreqMHz()) + " MHz</td></tr>";
  html += "<tr><td class='label'>SDK Version</td><td>" + String(ESP.getSdkVersion()) + "</td></tr>";
  html += "</table>";

  html += "<h2>Flash</h2><table>";
  html += "<tr><td class='label'>Flash Size</td><td>" + formatBytes(ESP.getFlashChipSize()) + "</td></tr>";
  html += "<tr><td class='label'>Flash Speed</td><td>" + String(ESP.getFlashChipSpeed() / 1000000) + " MHz</td></tr>";
  html += "</table>";

  html += "<h2>Memory</h2><table>";
  html += "<tr><td class='label'>Heap Size</td><td>" + formatBytes(heapSize) + "</td></tr>";
  html += "<tr><td class='label'>Free Heap</td><td>" + formatBytes(freeHeap) + "</td></tr>";
  html += "<tr><td class='label'>Min Free Heap</td><td>" + formatBytes(ESP.getMinFreeHeap()) + "</td></tr>";
  html += "<tr><td class='label'>Max Alloc Heap</td><td>" + formatBytes(ESP.getMaxAllocHeap()) + "</td></tr>";
  html += "<tr><td class='label'>PSRAM Size</td><td>" + formatBytes(ESP.getPsramSize()) + "</td></tr>";
  html += "<tr><td class='label'>Free PSRAM</td><td>" + formatBytes(ESP.getFreePsram()) + "</td></tr>";
  html += "</table>";

  html += "<h2>System</h2><table>";
  html += "<tr><td class='label'>Uptime</td><td>" + getUptimeString() + "</td></tr>";
  html += "<tr><td class='label'>Millis</td><td>" + String(millis()) + " ms</td></tr>";
  html += "<tr><td class='label'>Reset Reason CPU0</td><td>" + String(esp_reset_reason()) + "</td></tr>";
  html += "</table>";

  html += "<div class='footer'>ESP32 Monitor • Device view</div></div></body></html>";
  return html;
}

// ---------- Environment page (/environment) ----------

String buildEnvironmentPage() {
  int   hall      = hallRead();
  float tempC     = readChipTemperatureC();
  float tempF     = tempC * 9.0f / 5.0f + 32.0f;

  recordTemperatureSample(tempC);

  uint8_t channel = WiFi.channel();
  IPAddress apIP  = WiFi.softAPIP();
  int stations    = WiFi.softAPgetStationNum();

  size_t freeHeap = ESP.getFreeHeap();

  String html = commonHtmlHead("ESP32 Environment", "environment");
  html += "<h1>Environment</h1>";

  html += "<div class='card'>";
  html += "<div><span class='badge'>On-chip and RF environment</span></div>";
  html += "<div class='subtle'>Internal temperature and hall sensor measure the ESP32 die, not room air.</div>";
  html += "</div>";

  html += "<h2>Temperature</h2><table>";
  html += "<tr><td class='label'>Current</td><td>" + String(tempC, 1) + " °C / " + String(tempF, 1) + " °F</td></tr>";

  if (tempStatsInitialized) {
    html += "<tr><td class='label'>Min since boot</td><td>" + String(tempMinC, 1) + " °C</td></tr>";
    html += "<tr><td class='label'>Max since boot</td><td>" + String(tempMaxC, 1) + " °C</td></tr>";
  } else {
    html += "<tr><td class='label'>Min/Max</td><td>Collecting data...</td></tr>";
  }

  html += "<tr><td class='label'>Free Heap</td><td>" + formatBytes(freeHeap) + "</td></tr>";
  html += "</table>";

  html += "<div class='temp-graph'>";
  if (tempHistoryCount > 0) {
    for (int i = 0; i < tempHistoryCount; ++i) {
      float t = tempHistory[i];
      if (t < 0.0f)  t = 0.0f;
      if (t > 80.0f) t = 80.0f;
      int height = (int)((t / 80.0f) * 100.0f + 0.5f);

      html += "<div class='temp-bar' style=\"height:";
      html += String(height);
      html += "%;\"></div>";
    }
  }
  html += "</div>";
  html += "<div class='temp-baseline'><span>0 °C</span><span>80 °C</span></div>";

  html += "<h2>On-chip Sensor & Access Point</h2><table>";
  html += "<tr><td class='label'>Hall Sensor (raw)</td><td>" + String(hall) + "</td></tr>";
  html += "<tr><td class='label'>AP IP</td><td>" + apIP.toString() + "</td></tr>";
  html += "<tr><td class='label'>AP Channel</td><td>" + String(channel == 0 ? 1 : channel) + "</td></tr>";
  html += "<tr><td class='label'>Connected Stations</td><td>" + String(stations) + "</td></tr>";
  html += "</table>";

  html += "<div class='subtle'>Reload this page to update the temperature graph and stats.</div>";

  html += "<div class='footer'>ESP32 Monitor • Environment view</div></div></body></html>";
  return html;
}

// ---------- Wi-Fi scan page (/wifi) & AP detail ----------

String encTypeToString(int t) {
  switch (t) {
    case WIFI_AUTH_OPEN:          return "OPEN";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE:return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-PSK";
    default:                      return "UNKNOWN";
  }
}

String guessRouterVendor(const String& ssid, const String& bssidPrefix) {
  String s = ssid;
  s.toLowerCase();

  if (s.indexOf("tp-link") >= 0 || s.indexOf("tplink") >= 0) return "TP-Link (SSID guess)";
  if (s.indexOf("netgear") >= 0)   return "Netgear (SSID guess)";
  if (s.indexOf("linksys") >= 0)   return "Linksys (SSID guess)";
  if (s.indexOf("asus") >= 0)      return "ASUS (SSID guess)";
  if (s.indexOf("fritz") >= 0)     return "AVM FRITZ!Box (SSID guess)";
  if (s.indexOf("dlink") >= 0 || s.indexOf("d-link") >= 0) return "D-Link (SSID guess)";

  // Fallback: just show OUI prefix
  return "Unknown (OUI " + bssidPrefix + ")";
}

String buildWifiPage() {
  String html = commonHtmlHead("ESP32 Wi-Fi Scan", "wifi");
  html += "<h1>Wi-Fi Scan</h1>";

  html += "<div class='card'>";
  html += "<a class='btn' href='/wifi'>Scan Now</a>";
  html += "<span class='subtle'>Each time you open or refresh this page, a new scan is performed.</span>";
  html += "</div>";

  int n = WiFi.scanNetworks();
  if (n <= 0) {
    html += "<p>No networks found.</p>";
  } else {
    html += "<p>Found <span class='badge'>" + String(n) + " network(s)</span></p>";
    html += "<table class='table-list'><tr>"
            "<th>#</th><th>SSID</th><th>RSSI</th><th>Security</th><th>Ch</th><th>Details</th>"
            "</tr>";
    for (int i = 0; i < n; i++) {
      html += "<tr>";
      html += "<td>" + String(i + 1) + "</td>";
      html += "<td>" + WiFi.SSID(i) + "</td>";
      html += "<td>" + String(WiFi.RSSI(i)) + " dBm</td>";
      html += "<td>" + encTypeToString(WiFi.encryptionType(i)) + "</td>";
      html += "<td>" + String(WiFi.channel(i)) + "</td>";
      html += "<td><a class='btn' href='/wifi/ap?idx=" + String(i) + "'>View</a></td>";
      html += "</tr>";
    }
    html += "</table>";
  }

  html += "<div class='footer'>ESP32 Monitor • Wi-Fi scan view</div></div></body></html>";
  return html;
}

String buildWifiApDetailPage(int idx) {
  String html = commonHtmlHead("Wi-Fi AP Details", "wifi");
  html += "<h1>Wi-Fi Access Point</h1>";

  int n = WiFi.scanNetworks();
  if (idx < 0 || idx >= n) {
    html += "<p>AP index out of range. Try rescanning from the Wi-Fi Scan page.</p>";
    html += "<p><a class='btn' href='/wifi'>Back to Wi-Fi Scan</a></p>";
    html += "<div class='footer'>ESP32 Monitor • Wi-Fi AP detail</div></div></body></html>";
    return html;
  }

  String ssid  = WiFi.SSID(idx);
  int32_t rssi = WiFi.RSSI(idx);
  int32_t ch   = WiFi.channel(idx);
  String bssid = WiFi.BSSIDstr(idx);

  String bssidPrefix = bssid.substring(0, 8); // OUI
  String vendorGuess = guessRouterVendor(ssid, bssidPrefix);

  // Rough "channel load" guess: count how many APs share this channel
  int nSameChannel = 0;
  for (int i = 0; i < n; ++i) {
    if (WiFi.channel(i) == ch) nSameChannel++;
  }

  String loadText;
  String loadClass = "ok";
  if (nSameChannel <= 2) {
    loadText = "Light (few neighbors)";
  } else if (nSameChannel <= 5) {
    loadText = "Moderate (shared channel)";
    loadClass = "warn";
  } else {
    loadText = "Heavy (crowded channel)";
    loadClass = "bad";
  }

  html += "<div class='card'>";
  html += "<div class='status-pill'><span class='status-dot ok'></span><span>Beaconing</span></div>";
  html += "<div class='status-pill'><span class='status-dot " + loadClass + "'></span><span>Channel Load: " + loadText + "</span></div>";
  html += "</div>";

  html += "<h2>Basic Info</h2><table>";
  html += "<tr><td class='label'>SSID</td><td>" + ssid + "</td></tr>";
  html += "<tr><td class='label'>BSSID</td><td>" + bssid + "</td></tr>";
  html += "<tr><td class='label'>Channel</td><td>" + String(ch) + "</td></tr>";
  html += "<tr><td class='label'>RSSI</td><td>" + String(rssi) + " dBm</td></tr>";
  html += "<tr><td class='label'>Security</td><td>" + encTypeToString(WiFi.encryptionType(idx)) + "</td></tr>";
  html += "</table>";

  html += "<h2>Router Signature</h2><table>";
  html += "<tr><td class='label'>Vendor (heuristic)</td><td>" + vendorGuess + "</td></tr>";
  html += "<tr><td class='label'>OUI Prefix</td><td>" + bssidPrefix + "</td></tr>";
  html += "<tr><td class='label'>SSID Pattern</td><td>" + (ssid.length() ? ssid : String("(hidden or blank)")) + "</td></tr>";
  html += "</table>";

  html += "<div class='subtle'>"
          "This view heuristically fingerprints the AP from SSID and BSSID prefix. "
          "Channel load is estimated from how many APs share the same channel—"
          "not from real traffic counters."
          "</div>";

  html += "<p><a class='btn' href='/wifi'>Back to Wi-Fi Scan</a></p>";

  html += "<div class='footer'>ESP32 Monitor • Wi-Fi AP detail</div></div></body></html>";
  return html;
}

// ---------- Bluetooth (BLE) list & detail ----------

String classifyBleDeviceType(const String& name) {
  String n = name;
  n.toLowerCase();

  if (n.indexOf("iphone") >= 0 || n.indexOf("ipad") >= 0 || n.indexOf("ios") >= 0) return "Phone / iOS device (name guess)";
  if (n.indexOf("android") >= 0 || n.indexOf("pixel") >= 0 || n.indexOf("mi ") >= 0) return "Phone / Android device (name guess)";
  if (n.indexOf("watch") >= 0 || n.indexOf("wear") >= 0 || n.indexOf("fitbit") >= 0 || n.indexOf("garmin") >= 0) return "Watch / wearable (name guess)";
  if (n.indexOf("airpods") >= 0 || n.indexOf("buds") >= 0 || n.indexOf("ear") >= 0) return "Earbuds / audio (name guess)";
  if (n.indexOf("tv") >= 0 || n.indexOf("light") >= 0 || n.indexOf("bulb") >= 0 || n.indexOf("plug") >= 0) return "Smart home / appliance (name guess)";

  return "Unknown category (name-based guess)";
}

float estimateDistanceMeters(int rssi, int txPowerDbm) {
  // Simple path loss model: d = 10 ^ ((Tx - RSSI) / (10 * n))
  // n ~ 2.0 (free space) to 3.0 (indoor). We'll use 2.0 and clamp.
  float n = 2.0f;
  float ratioDb = (float)txPowerDbm - (float)rssi;
  float exponent = ratioDb / (10.0f * n);
  float d = powf(10.0f, exponent);
  if (d < 0.1f) d = 0.1f;
  if (d > 20.0f) d = 20.0f;
  return d;
}

String buildBlePage() {
  String html = commonHtmlHead("ESP32 Bluetooth Devices", "ble");
  html += "<h1>Bluetooth Low Energy Devices</h1>";

  html += "<div class='card'>";
  html += "<a class='btn' href='/ble'>Scan Now</a>";
  html += "<span class='subtle'>Short active scan for nearby BLE advertisers.</span>";
  html += "</div>";

  if (!pBLEScan) {
    html += "<p>BLE not initialized.</p>";
    html += "<div class='footer'>ESP32 Monitor • BLE view</div></div></body></html>";
    return html;
  }

  int scanTimeSeconds = 3;
  BLEScanResults results = pBLEScan->start(scanTimeSeconds, false);
  int count = results.getCount();

  if (count == 0) {
    html += "<p>No BLE devices found.</p>";
  } else {
    html += "<p>Found <span class='badge'>" + String(count) + " device(s)</span></p>";
    html += "<table class='table-list'><tr>"
            "<th>#</th><th>Name</th><th>Address</th><th>RSSI</th><th>Details</th>"
            "</tr>";
    for (int i = 0; i < count; i++) {
      BLEAdvertisedDevice dev = results.getDevice(i);
      String name = dev.getName().c_str();
      if (name.length() == 0) name = "(unnamed)";
      String addr = dev.getAddress().toString().c_str();
      int rssi = dev.getRSSI();

      html += "<tr>";
      html += "<td>" + String(i + 1) + "</td>";
      html += "<td>" + name + "</td>";
      html += "<td>" + addr + "</td>";
      html += "<td>" + String(rssi) + " dBm</td>";
      html += "<td><a class='btn' href='/ble/dev?addr=" + addr + "'>View</a></td>";
      html += "</tr>";
    }
    html += "</table>";
  }

  pBLEScan->clearResults();

  html += "<div class='footer'>ESP32 Monitor • BLE view</div></div></body></html>";
  return html;
}

String buildBleDetailPage(const String& addrQuery) {
  String html = commonHtmlHead("BLE Device Details", "ble");
  html += "<h1>BLE Device</h1>";

  if (!pBLEScan) {
    html += "<p>BLE not initialized.</p>";
    html += "<p><a class='btn' href='/ble'>Back to BLE List</a></p>";
    html += "<div class='footer'>ESP32 Monitor • BLE detail</div></div></body></html>";
    return html;
  }

  int scanTimeSeconds = 3;
  BLEScanResults results = pBLEScan->start(scanTimeSeconds, false);
  int count = results.getCount();

  BLEAdvertisedDevice* found = nullptr;
  BLEAdvertisedDevice tmp;

  for (int i = 0; i < count; i++) {
    BLEAdvertisedDevice dev = results.getDevice(i);
    String addr = dev.getAddress().toString().c_str();
    if (addr == addrQuery) {
      tmp = dev;
      found = &tmp;
      break;
    }
  }

  if (!found) {
    html += "<p>Device not found in this scan. It may have stopped advertising.</p>";
    html += "<p><a class='btn' href='/ble'>Back to BLE List</a></p>";
    html += "<div class='footer'>ESP32 Monitor • BLE detail</div></div></body></html>";
    pBLEScan->clearResults();
    return html;
  }

  String name = found->getName().c_str();
  if (name.length() == 0) name = "(unnamed)";
  String addr = found->getAddress().toString().c_str();
  int rssi = found->getRSSI();

  bool haveTxPower = found->haveTXPower();
  int txPowerDbm   = haveTxPower ? found->getTXPower() : -59; // -59 as common 1m ref
  float distance   = estimateDistanceMeters(rssi, txPowerDbm);

  String devType = classifyBleDeviceType(name);

  html += "<div class='card'>";
  html += "<div class='status-pill'><span class='status-dot ok'></span><span>Advertising seen in last scan</span></div>";
  html += "</div>";

  html += "<h2>Basic Info</h2><table>";
  html += "<tr><td class='label'>Name</td><td>" + name + "</td></tr>";
  html += "<tr><td class='label'>Address</td><td>" + addr + "</td></tr>";
  html += "<tr><td class='label'>RSSI</td><td>" + String(rssi) + " dBm</td></tr>";
  html += "<tr><td class='label'>Heuristic Type</td><td>" + devType + "</td></tr>";
  html += "</table>";

  html += "<h2>TX Power / Distance</h2><table>";
  if (haveTxPower) {
    html += "<tr><td class='label'>TX Power (advertised)</td><td>" + String(txPowerDbm) + " dBm</td></tr>";
  } else {
    html += "<tr><td class='label'>TX Power</td><td>Not advertised (using typical -59 dBm @ 1m)</td></tr>";
  }
  html += "<tr><td class='label'>Estimated Distance</td><td>~" + String(distance, 1) + " m (very approximate)</td></tr>";
  html += "</table>";

  // Manufacturer data, if present
  if (found->haveManufacturerData()) {
    std::string md = found->getManufacturerData();
    html += "<h2>Manufacturer Data</h2>";
    html += "<div class='card'><div class='subtle'>Raw manufacturer data (first bytes shown in hex):</div><code>";
    size_t showBytes = md.size() < 16 ? md.size() : 16;
    char buf[4];
    for (size_t i = 0; i < showBytes; ++i) {
      uint8_t b = (uint8_t)md[i];
      snprintf(buf, sizeof(buf), "%02X", b);
      html += buf;
      if (i + 1 < showBytes) html += " ";
    }
    if (md.size() > showBytes) html += " ...";
    html += "</code></div>";
  }

  html += "<div class='subtle'>"
          "Distance, device type, and activity are inferred from RSSI, TX power, and name. "
          "For more precise contact-tracing style analysis you’d track this device over time and "
          "analyze advertising intervals and RSSI trends."
          "</div>";

  html += "<p><a class='btn' href='/ble'>Back to BLE List</a></p>";

  html += "<div class='footer'>ESP32 Monitor • BLE detail</div></div></body></html>";

  pBLEScan->clearResults();
  return html;
}

// ---------- Crowd density page (/crowd) ----------

String describeCrowdLevel(float score) {
  if (score < 3.0f) return "Very quiet (almost empty)";
  if (score < 8.0f) return "Light activity";
  if (score < 16.0f) return "Moderate crowd";
  if (score < 30.0f) return "Busy environment";
  return "Highly crowded / RF noisy";
}

String buildCrowdPage() {
  String html = commonHtmlHead("ESP32 Crowd Density", "crowd");
  html += "<h1>Crowd Density</h1>";

  html += "<div class='card'>";
  html += "<a class='btn' href='/crowd'>Measure Now</a>";
  html += "<span class='subtle'>This is a heuristic based on Wi-Fi and BLE activity around the ESP32.</span>";
  html += "</div>";

  // Wi-Fi scan
  int wifiCount = WiFi.scanNetworks();

  // BLE scan
  int bleCount = 0;
  if (pBLEScan) {
    BLEScanResults res = pBLEScan->start(2, false);
    bleCount = res.getCount();
    pBLEScan->clearResults();
  }

  float crowdScore = wifiCount * 1.0f + bleCount * 0.5f;
  String crowdDesc = describeCrowdLevel(crowdScore);

  String crowdClass = "ok";
  if (crowdScore >= 16.0f) crowdClass = "warn";
  if (crowdScore >= 30.0f) crowdClass = "bad";

  html += "<div class='card'>";
  html += "<div class='status-pill'><span class='status-dot " + crowdClass + "'></span><span>" + crowdDesc + "</span></div>";
  html += "<div class='subtle'>Score ≈ Wi-Fi count × 1.0 + BLE count × 0.5</div>";
  html += "</div>";

  html += "<h2>Raw Counts</h2><table>";
  html += "<tr><td class='label'>Wi-Fi networks detected</td><td>" + String(wifiCount) + "</td></tr>";
  html += "<tr><td class='label'>BLE devices detected</td><td>" + String(bleCount) + "</td></tr>";
  html += "<tr><td class='label'>Crowd score</td><td>" + String(crowdScore, 1) + "</td></tr>";
  html += "</table>";

  html += "<div class='subtle'>"
          "This does not decode any payloads; it only counts how many radios are active nearby. "
          "For motion / presence detection, you’d take multiple measurements over time and look for changes."
          "</div>";

  html += "<div class='footer'>ESP32 Monitor • Crowd density view</div></div></body></html>";
  return html;
}

// ---------- RF Interference page (/rf) ----------

String describeRfLevel(float energy) {
  if (energy < 50.0f) return "Low RF energy";
  if (energy < 150.0f) return "Moderate RF energy";
  if (energy < 300.0f) return "High RF energy";
  return "Very high RF energy / noisy band";
}

String buildRfPage() {
  String html = commonHtmlHead("ESP32 RF Interference", "rf");
  html += "<h1>2.4 GHz Interference</h1>";

  html += "<div class='card'>";
  html += "<a class='btn' href='/rf'>Measure Now</a>";
  html += "<span class='subtle'>Heuristic “noise” estimate based on Wi-Fi beacons and signal strengths.</span>";
  html += "</div>";

  int n = WiFi.scanNetworks();
  if (n <= 0) {
    html += "<p>No Wi-Fi networks detected. RF environment seems very quiet.</p>";
    html += "<div class='footer'>ESP32 Monitor • RF interference view</div></div></body></html>";
    return html;
  }

  // Compute rough "RF energy" score: sum of (100 + RSSI) across all networks
  float totalEnergy = 0.0f;
  int maxChannel = 14;
  int channelCounts[15];
  for (int i = 0; i <= maxChannel; ++i) channelCounts[i] = 0;

  for (int i = 0; i < n; ++i) {
    int rssi = WiFi.RSSI(i);           // typically negative
    totalEnergy += max(0, 100 + rssi); // stronger signals contribute more
    int ch = WiFi.channel(i);
    if (ch >= 1 && ch <= maxChannel) channelCounts[ch]++;
  }

  String rfDesc = describeRfLevel(totalEnergy);
  String rfClass = "ok";
  if (totalEnergy >= 150.0f) rfClass = "warn";
  if (totalEnergy >= 300.0f) rfClass = "bad";

  html += "<div class='card'>";
  html += "<div class='status-pill'><span class='status-dot " + rfClass + "'></span><span>" + rfDesc + "</span></div>";
  html += "<div class='subtle'>Energy score from nearby Wi-Fi beacons (higher = noisier band).</div>";
  html += "</div>";

  html += "<h2>Per-channel congestion</h2>";
  html += "<div class='heat-graph'>";
  for (int ch = 1; ch <= 13; ++ch) {
    int count = channelCounts[ch];
    int height = count * 15;
    if (height > 100) height = 100;
    html += "<div class='heat-bar' style=\"height:";
    html += String(height);
    html += "%;\"></div>";
  }
  html += "</div>";
  html += "<div class='temp-baseline'><span>Ch 1</span><span>Ch 13</span></div>";

  html += "<h2>Summary</h2><table>";
  html += "<tr><td class='label'>Wi-Fi networks detected</td><td>" + String(n) + "</td></tr>";
  html += "<tr><td class='label'>RF energy score</td><td>" + String(totalEnergy, 1) + "</td></tr>";
  html += "</table>";

  html += "<div class='subtle'>"
          "This does not measure true noise floor; it infers RF activity from visible Wi-Fi beacons. "
          "Strong spikes over time may correlate with things like microwaves or other 2.4 GHz sources."
          "</div>";

  html += "<div class='footer'>ESP32 Monitor • RF interference view</div></div></body></html>";
  return html;
}

// ---------- HTTP handlers ----------

void handleRoot() {
  // Redirect root to /device
  server.sendHeader("Location", String("/device"), true);
  server.send(302, "text/plain", "");
}

void handleDevice() {
  server.send(200, "text/html", buildDevicePage());
}

void handleEnvironment() {
  server.send(200, "text/html", buildEnvironmentPage());
}

void handleWifi() {
  server.send(200, "text/html", buildWifiPage());
}

void handleWifiApDetail() {
  if (!server.hasArg("idx")) {
    server.send(400, "text/plain", "Missing idx parameter");
    return;
  }
  int idx = server.arg("idx").toInt();
  server.send(200, "text/html", buildWifiApDetailPage(idx));
}

void handleBle() {
  server.send(200, "text/html", buildBlePage());
}

void handleBleDetail() {
  if (!server.hasArg("addr")) {
    server.send(400, "text/plain", "Missing addr parameter");
    return;
  }
  String addr = server.arg("addr");
  server.send(200, "text/html", buildBleDetailPage(addr));
}

void handleCrowd() {
  server.send(200, "text/html", buildCrowdPage());
}

void handleRf() {
  server.send(200, "text/html", buildRfPage());
}

void handleNotFound() {
  String message = "Not found\n\n";
  message += "URI: " + server.uri() + "\n";
  server.send(404, "text/plain", message);
}

// ---------- Setup & loop ----------

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Starting ESP32 Monitor AP...");

  // Wi-Fi: AP + STA so we can scan while running AP
  WiFi.mode(WIFI_AP_STA);
  bool apOk = WiFi.softAP(apSSID, apPASS);
  if (apOk) {
    Serial.print("AP started. SSID: ");
    Serial.println(apSSID);
    Serial.print("IP address: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Failed to start AP!");
  }

  // BLE init
  BLEDevice::init("ESP32-Monitor");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(80);

  // Routes
  server.on("/",            handleRoot);
  server.on("/device",      handleDevice);
  server.on("/environment", handleEnvironment);
  server.on("/wifi",        handleWifi);
  server.on("/wifi/ap",     handleWifiApDetail);
  server.on("/ble",         handleBle);
  server.on("/ble/dev",     handleBleDetail);
  server.on("/crowd",       handleCrowd);
  server.on("/rf",          handleRf);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("HTTP server started");
}

void loop() {
  // Track serial activity to infer host type
  while (Serial.available() > 0) {
    (void)Serial.read();
    lastSerialActivity = millis();
  }

  server.handleClient();
}
