#pragma once
/**
 * WifiPortal.h - Interactive Wi-Fi Setup & Reset Portal for ESP32 Gas & Fire Monitor
 *
 * Capabilities:
 *  - Stores Wi-Fi credentials in persistent flash (Preferences namespace "wifi").
 *  - Prevents unwanted rollback to compile-time credentials when wiped.
 *  - Webserver portal (192.168.4.1 or device IP):
 *      1) Lists available 2.4 GHz networks with signal quality.
 *      2) Allows setting new Wi-Fi credentials with live test verification.
 *      3) Allows forgetting / resetting stored Wi-Fi credentials directly from the web UI.
 *  - Physical BOOT button (GPIO 0):
 *      - Hold 3 seconds  -> Open setup portal AP ("FireMonitor-Setup").
 *      - Hold 7+ seconds -> Factory erase Wi-Fi credentials from flash memory.
 *  - Serial commands (115200 baud):
 *      - "resetwifi" -> Erases credentials & starts portal.
 *      - "wifi"      -> Opens setup portal.
 *      - "status"    -> Prints current Wi-Fi connection and sensor info.
 *  - Remote cloud commands (via PWA /commands queue):
 *      - "reset_wifi"  -> Erases credentials & opens AP.
 *      - "start_portal"-> Opens setup portal.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

#if __has_include("FireApi.h")
#include "FireApi.h"
#endif

// ---------- Configuration Constants ----------
static const char* PORTAL_AP_SSID       = "FireMonitor-Setup";
static const char* PORTAL_AP_PASS       = "";                    // Open AP for easiest configuration (or set "fire1234")
static const int   PIN_PORTAL_BTN       = 0;                     // ESP32 BOOT button (GPIO 0)
static const unsigned long PORTAL_HOLD_MS       = 3000;          // Hold BOOT 3s to open portal
static const unsigned long PORTAL_RESET_HOLD_MS = 7000;          // Hold BOOT 7s to erase credentials
static const unsigned long PORTAL_FALLBACK_MS   = 120000;        // 2 min unreachable -> auto open portal
static const unsigned long PORTAL_TIMEOUT_MS    = 600000;        // 10 min idle -> auto close portal if online
static const unsigned long PROVISION_TIMEOUT_MS = 20000;         // 20s test timeout for new credentials

// ---------- State Definitions ----------
enum ProvState { PROV_IDLE = 0, PROV_CONNECTING = 1, PROV_OK = 2, PROV_FAILED = 3 };

static Preferences wifiPrefs;
static WebServer   portal(80);
static DNSServer   dnsServer;

static String wifiSsid, wifiPass;
static String pendingSsid, pendingPass;

static bool portalActive   = false;
static bool portalAutoStop = false;
static unsigned long portalStart = 0, portalStopAt = 0, provStart = 0;
static unsigned long btnDownSince = 0, offlineSince = 0, lastWifiTry = 0;
static volatile ProvState provState = PROV_IDLE;

volatile bool portalRequested   = false;
volatile bool wipeWifiRequested = false;

// Forward declaration
void wifiMgrResetCreds();
static void startPortal(bool autoStop);
static void stopPortal();

// ---------- Helpers ----------
static const char* wlStatusStr(wl_status_t s) {
  switch (s) {
    case WL_NO_SHIELD:        return "NO_SHIELD";
    case WL_IDLE_STATUS:      return "IDLE";
    case WL_NO_SSID_AVAIL:    return "NO_SSID_AVAIL (Network not found/out of range)";
    case WL_SCAN_COMPLETED:   return "SCAN_COMPLETED";
    case WL_CONNECTED:        return "CONNECTED";
    case WL_CONNECT_FAILED:   return "CONNECT_FAILED (Wrong password)";
    case WL_CONNECTION_LOST:  return "CONNECTION_LOST";
    case WL_DISCONNECTED:     return "DISCONNECTED (Associating/DHCP...)";
    default:                  return "UNKNOWN";
  }
}

static String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  o += "&amp;";  break;
      case '<':  o += "&lt;";   break;
      case '>':  o += "&gt;";   break;
      case '"':  o += "&quot;"; break;
      case '\'': o += "&#39;";  break;
      default:   o += c;
    }
  }
  return o;
}

static void loadWifiCreds() {
  wifiPrefs.begin("wifi", false);
  bool wasCleared = wifiPrefs.getBool("cleared", false);
  wifiSsid = wifiPrefs.getString("ssid", "");
  wifiPass = wifiPrefs.getString("pass", "");

#ifdef WIFI_SSID
  // Only fall back to compile-time header if user has NEVER wiped Wi-Fi
  if (!wasCleared && wifiSsid.isEmpty()) {
    wifiSsid = WIFI_SSID;
    wifiPass = WIFI_PASS;
  }
#endif
}

// ---------- Web Handlers ----------

static void handleRoot() {
  int n = WiFi.scanNetworks();
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  String page = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>FireLine &bull; Wi-Fi Configuration</title><style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;max-width:440px;margin:20px auto;padding:0 16px;background:#09090b;color:#f4f4f5}"
    ".card{background:#18181b;border-radius:20px;border:1px solid #27272a;padding:24px;box-shadow:0 10px 30px rgba(0,0,0,0.5)}"
    ".badge{display:inline-block;padding:4px 10px;border-radius:999px;font-size:11px;font-weight:800;letter-spacing:0.5px;text-transform:uppercase;margin-bottom:12px}"
    ".badge-green{background:#052e16;color:#4ade80;border:1px solid #166534}"
    ".badge-amber{background:#451a03;color:#fbbf24;border:1px solid #92400e}"
    ".badge-zinc{background:#27272a;color:#a1a1aa;border:1px solid #3f3f46}"
    "h2{margin:0 0 6px;font-size:20px;font-weight:800;color:#fff;display:flex;align-items:center;gap:8px}"
    "p.sub{font-size:13px;color:#a1a1aa;margin:0 0 20px;line-height:1.4}"
    "label{display:block;font-size:12px;font-weight:700;color:#d4d4d8;margin-bottom:6px}"
    "input,select{width:100%;padding:12px 14px;margin-bottom:14px;background:#09090b;border:1px solid #3f3f46;border-radius:12px;font-size:14px;color:#fff;box-sizing:border-box;outline:none}"
    "input:focus,select:focus{border-color:#e11d48}"
    ".btn-primary{width:100%;padding:13px;background:#e11d48;color:#fff;border:0;border-radius:12px;font-size:14px;font-weight:800;cursor:pointer;transition:background .15s}"
    ".btn-primary:hover{background:#be123c}"
    ".btn-danger{width:100%;padding:11px;background:transparent;color:#f43f5e;border:1px solid #e11d48;border-radius:12px;font-size:13px;font-weight:700;cursor:pointer;margin-top:8px}"
    ".btn-danger:hover{background:#4c0519}"
    ".divider{height:1px;background:#27272a;margin:22px 0 16px}"
    ".show-pw{display:flex;align-items:center;gap:8px;font-size:12px;color:#a1a1aa;margin:-6px 0 14px;cursor:pointer}"
    ".show-pw input{width:auto;margin:0}"
    "a{color:#e11d48;text-decoration:none;font-size:12px;font-weight:600}"
    "</style></head><body><div class='card'>"
    "<h2>🔥 FireLine Monitor</h2>"
    "<p class='sub'>ESP32 Hardware Wi-Fi Setup &amp; Network Provisioning</p>");

  // Status banner
  if (isConnected) {
    page += "<span class='badge badge-green'>&#10003; Connected to " + htmlEscape(WiFi.SSID()) + "</span>";
    page += "<p style='font-size:12px;color:#a1a1aa;margin:-4px 0 16px;'>IP Address: <b style='color:#fff'>" + WiFi.localIP().toString() + "</b> &bull; RSSI: " + String(WiFi.RSSI()) + " dBm</p>";
  } else if (wifiSsid.length() > 0) {
    page += "<span class='badge badge-amber'>&#9203; Saved: " + htmlEscape(wifiSsid) + " (Connecting...)</span>";
  } else {
    page += "<span class='badge badge-zinc'>&#9881; Setup Mode: No Wi-Fi Saved</span>";
  }

  // Network selection form
  page += F("<form method='POST' action='/save'>"
            "<label>Select 2.4 GHz Network</label><select name='ssid' id='netSel'>");

  for (int i = 0; i < n; i++) {
    String name = WiFi.SSID(i);
    if (name.isEmpty()) continue;
    String e = htmlEscape(name);
    int rssi = WiFi.RSSI(i);
    String signal = (rssi > -60) ? "Strong" : (rssi > -75 ? "Good" : "Fair");
    page += "<option value=\"" + e + "\">" + e + " (" + signal + ", " + String(rssi) + " dBm)</option>";
  }
  WiFi.scanDelete();

  page += F(
    "</select>"
    "<label>Or Hidden Network Name (SSID)</label>"
    "<input name='manual' maxlength='32' placeholder='Type manual SSID if hidden'>"
    "<label>Wi-Fi Password</label>"
    "<input name='pass' id='pwInput' type='password' maxlength='63' placeholder='Enter password (leave empty if open)'>"
    "<label class='show-pw'><input type='checkbox' onclick='var x=document.getElementById(\"pwInput\");x.type=this.checked?\"text\":\"password\";'> Show password</label>"
    "<button type='submit' class='btn-primary'>Save &amp; Connect to Wi-Fi</button>"
    "<div style='display:flex;justify-content:space-between;align-items:center;margin-top:12px;'>"
    "<a href='/'>&#8635; Rescan Networks</a>"
    "<span style='font-size:11px;color:#71717a;'>AP: FireMonitor-Setup</span>"
    "</div>"
    "</form>");

  // Danger zone: Reset Wi-Fi credentials
  if (wifiSsid.length() > 0 || isConnected) {
    page += F(
      "<div class='divider'></div>"
      "<label style='color:#f43f5e;'>Forget / Reset Network</label>"
      "<p style='font-size:12px;color:#71717a;margin:0 0 10px;'>Erase stored Wi-Fi credentials from ESP32 memory and restart captive portal.</p>"
      "<form method='POST' action='/reset' onsubmit='return confirm(\"Are you sure you want to forget saved Wi-Fi credentials? ESP32 will disconnect.\");'>"
      "<button type='submit' class='btn-danger'>Forget &amp; Reset Saved Wi-Fi</button>"
      "</form>");
  }

  page += F("</div></body></html>");
  portal.send(200, "text/html", page);
}

static void handleSave() {
  String s = portal.arg("manual");
  if (s.isEmpty()) s = portal.arg("ssid");
  String p = portal.arg("pass");
  s.trim();

  if (s.isEmpty() || s.length() > 32 || p.length() > 63 || (p.length() > 0 && p.length() < 8)) {
    portal.send(400, "text/plain", "Invalid network name or password. WPA/WPA2 passwords must be at least 8 characters.");
    return;
  }
  if (provState == PROV_CONNECTING) {
    portal.send(409, "text/plain", "Already testing connection, please wait.");
    return;
  }

  // Save credentials immediately to flash memory so they persist
  wifiSsid = s;
  wifiPass = p;
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("ssid", wifiSsid);
  wifiPrefs.putString("pass", wifiPass);
  wifiPrefs.putBool("cleared", false);

  provStart   = millis();
  provState   = PROV_CONNECTING;

  Serial.printf("\n[PORTAL] Saved network '%s' to flash. Transitioning to station mode...\n", wifiSsid.c_str());

  String resp = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved &bull; Connecting...</title><style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;max-width:440px;margin:24px auto;padding:0 16px;background:#09090b;color:#f4f4f5;text-align:center}"
    ".card{background:#18181b;border-radius:20px;border:1px solid #27272a;padding:28px;box-shadow:0 10px 30px rgba(0,0,0,0.5)}"
    ".spinner{width:36px;height:36px;border:3px solid #27272a;border-top-color:#e11d48;border-radius:50%;animation:spin 1s linear infinite;margin:0 auto 16px}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    "h2{margin:0 0 10px;font-size:20px;color:#4ade80}"
    "p{font-size:14px;color:#a1a1aa;margin:0 0 14px;line-height:1.5}"
    ".badge{display:inline-block;padding:6px 14px;background:#052e16;color:#4ade80;border:1px solid #166534;border-radius:999px;font-size:12px;font-weight:700;margin-bottom:14px}"
    "</style></head><body><div class='card'>"
    "<div class='spinner'></div>"
    "<h2>&#10004; Credentials Saved!</h2>"
    "<span class='badge'>Connecting to Wi-Fi</span>"
    "<p>ESP32 has saved <b>");
  resp += htmlEscape(wifiSsid);
  resp += F("</b> to flash memory and is now connecting.</p>"
            "<p style='font-size:13px;color:#d4d4d8;'>The setup Access Point will now close so the ESP32 can connect with full radio signal.<br><br>"
            "If the password is correct, the device will come online in the dashboard.<br>"
            "<span style='color:#71717a;font-size:12px;'>If connection fails, the setup AP will reopen automatically in 20s.</span></p>"
            "</div></body></html>");

  portal.send(200, "text/html", resp);
  portal.client().flush();
}

static void handleReset() {
  wifiMgrResetCreds();

  String html = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Wi-Fi Erased</title><style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;max-width:440px;margin:24px auto;padding:0 16px;background:#09090b;color:#f4f4f5;text-align:center}"
    ".card{background:#18181b;border-radius:20px;border:1px solid #27272a;padding:28px;box-shadow:0 10px 30px rgba(0,0,0,0.5)}"
    "h2{margin:0 0 10px;font-size:20px;color:#f43f5e}"
    "p{font-size:14px;color:#a1a1aa;margin:0 0 20px;line-height:1.5}"
    ".btn{display:inline-block;padding:12px 24px;background:#e11d48;color:#fff;border-radius:12px;text-decoration:none;font-weight:700;font-size:14px}"
    "</style></head><body><div class='card'>"
    "<h2>&#9888; Wi-Fi Credentials Erased</h2>"
    "<p>All saved network settings have been wiped from ESP32 memory.<br>The setup Access Point <b>FireMonitor-Setup</b> is now active.</p>"
    "<a href='/' class='btn'>Configure New Wi-Fi Network</a>"
    "</div></body></html>"
  );
  portal.send(200, "text/html", html);
}

static void handleStatus() {
  String safeSsid = (provState == PROV_OK) ? wifiSsid : pendingSsid;
  safeSsid.replace("\\", "\\\\");
  safeSsid.replace("\"", "\\\"");
  String json = "{\"state\":" + String((int)provState) + 
                ",\"ip\":\"" + WiFi.localIP().toString() + "\"" + 
                ",\"ssid\":\"" + safeSsid + "\"}";
  portal.send(200, "application/json", json);
}

// ---------- Portal Lifecycle ----------

static void startPortal(bool autoStop) {
  if (portalActive) {
    if (!autoStop) portalAutoStop = false;
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  if (strlen(PORTAL_AP_PASS) >= 8) {
    WiFi.softAP(PORTAL_AP_SSID, PORTAL_AP_PASS);
  } else {
    WiFi.softAP(PORTAL_AP_SSID); // Open AP without password for easiest initial connection
  }
  dnsServer.start(53, "*", WiFi.softAPIP());

  static bool routesSet = false;
  if (!routesSet) {
    portal.on("/",       HTTP_GET,  handleRoot);
    portal.on("/save",   HTTP_POST, handleSave);
    portal.on("/reset",  HTTP_POST, handleReset);
    portal.on("/reset",  HTTP_GET,  handleReset);
    portal.on("/status", HTTP_GET,  handleStatus);
    portal.on("/generate_204", HTTP_GET, []() {
      portal.sendHeader("Location", "http://192.168.4.1/", true);
      portal.send(302, "text/plain", "");
    });
    portal.on("/hotspot-detect.html", HTTP_GET, []() {
      portal.sendHeader("Location", "http://192.168.4.1/", true);
      portal.send(302, "text/plain", "");
    });
    portal.onNotFound([]() {
      portal.sendHeader("Location", "http://192.168.4.1/", true);
      portal.send(302, "text/plain", "");
    });
    routesSet = true;
  }
  portal.begin();

  portalActive   = true;
  portalAutoStop = autoStop;
  portalStart    = millis();
  portalStopAt   = 0;
  provState      = PROV_IDLE;
  Serial.printf("\n📡 [PORTAL OPEN] Connect to Wi-Fi '%s', then navigate to http://192.168.4.1\n", PORTAL_AP_SSID);
}

static void stopPortal() {
  if (!portalActive) return;
  portal.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalActive = false;
  Serial.println("📡 [PORTAL CLOSED] Setup portal AP turned off.");
}

static bool provApStopped = false;

static void portalTick(unsigned long now) {
  if (provState == PROV_CONNECTING) {
    if (!provApStopped && now - provStart >= 500) {
      provApStopped = true;
      Serial.printf("[WIFI] Shutting down setup AP to connect on router channel without radio conflicts...\n");
      stopPortal();
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
      Serial.printf("[WIFI] Associating with '%s'...\n", wifiSsid.c_str());
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }

    if (provApStopped) {
      static unsigned long lastLog = 0;
      if (now - lastLog >= 1500) {
        lastLog = now;
        int secs = (now - provStart) / 1000;
        Serial.printf("[WIFI] Connecting to '%s' (%ds/%ds)... Status: %s\n",
                      wifiSsid.c_str(), secs, (int)(PROVISION_TIMEOUT_MS / 1000),
                      wlStatusStr(WiFi.status()));
      }

      if (WiFi.status() == WL_CONNECTED) {
        provState = PROV_OK;
        provApStopped = false;
        Serial.printf("\n✅ [WI-FI CONNECTED] Successfully joined '%s'!\n", wifiSsid.c_str());
        Serial.printf("   Assigned IP: %s | RSSI: %d dBm\n\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return;
      }

      if (WiFi.status() == WL_CONNECT_FAILED && (now - provStart > 3000)) {
        provState = PROV_FAILED;
        provApStopped = false;
        Serial.println(F("\n❌ [WI-FI FAILED] Authentication failed! Password was rejected by the router."));
        Serial.println(F("📡 Reopening setup portal 'FireMonitor-Setup' at http://192.168.4.1..."));
        startPortal(false);
        return;
      }

      if (now - provStart > PROVISION_TIMEOUT_MS) {
        provState = PROV_FAILED;
        provApStopped = false;
        Serial.printf("\n❌ [WI-FI FAILED] Connection timed out after %ds. (Last status: %s)\n",
                      (int)(PROVISION_TIMEOUT_MS / 1000), wlStatusStr(WiFi.status()));
        Serial.println(F("📡 Reopening setup portal 'FireMonitor-Setup' at http://192.168.4.1..."));
        startPortal(false);
        return;
      }
    }
    return;
  }

  if (provState == PROV_OK && portalStopAt && now >= portalStopAt) {
    stopPortal();
    return;
  }

  const bool online = (WiFi.status() == WL_CONNECTED);
  if (portalAutoStop && online) {
    stopPortal();
    return;
  }
  if (online && now - portalStart > PORTAL_TIMEOUT_MS) {
    stopPortal();
    return;
  }
}

// ---------- Public Functions ----------

void wifiMgrSetCreds(const String& ssid, const String& pass) {
  wifiSsid = ssid;
  wifiPass = pass;
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("ssid", wifiSsid);
  wifiPrefs.putString("pass", wifiPass);
  wifiPrefs.putBool("cleared", false);
  Serial.printf("\n[WIFI] Saved credentials for '%s'. Reconnecting in station mode...\n", wifiSsid.c_str());
  provStart = millis();
  provState = PROV_CONNECTING;
  provApStopped = true;
  stopPortal();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

void wifiMgrResetCreds() {
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putBool("cleared", true);
  wifiPrefs.remove("ssid");
  wifiPrefs.remove("pass");
  wifiSsid = "";
  wifiPass = "";
  WiFi.disconnect();
  provState = PROV_IDLE;
  provApStopped = false;
  Serial.println(F("\n⚠️ [WI-FI RESET] Stored network credentials wiped from ESP32 flash memory."));
  startPortal(false);
}

void wifiMgrBegin() {
  pinMode(PIN_PORTAL_BTN, INPUT_PULLUP);
  loadWifiCreds();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  lastWifiTry = millis();

  if (wifiSsid.length()) {
    Serial.printf("Connecting to saved Wi-Fi '%s'...\n", wifiSsid.c_str());
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  } else {
    Serial.println("No saved Wi-Fi found. Starting setup portal...");
    startPortal(false);
  }
}

bool wifiPortalActive() {
  return portalActive;
}

bool wifiMgrService(unsigned long now) {
  // Physical BOOT button handler (GPIO 0)
  if (digitalRead(PIN_PORTAL_BTN) == LOW) {
    if (!btnDownSince) {
      btnDownSince = now;
    } else if (now - btnDownSince >= PORTAL_RESET_HOLD_MS) {
      // Prolonged hold (>= 7 seconds): Factory Wi-Fi reset
      btnDownSince = now + 10000; // prevent re-trigger
      wipeWifiRequested = true;
    } else if (now - btnDownSince >= PORTAL_HOLD_MS && !portalActive) {
      // Short hold (>= 3 seconds): Open AP portal
      portalRequested = true;
    }
  } else {
    btnDownSince = 0;
  }

  // Wipe requested from Web / Serial / Hardware Button / Cloud Command
  if (wipeWifiRequested) {
    wipeWifiRequested = false;
    wifiMgrResetCreds();
  }

  // Portal requested from Web / Serial / Hardware Button / Cloud Command
  if (portalRequested) {
    portalRequested = false;
    startPortal(false);
  }

  // Automatic offline fallback: If disconnected for > 2 min, open setup portal
  if (WiFi.status() == WL_CONNECTED) {
    offlineSince = 0;
  } else {
    if (!offlineSince) offlineSince = now;
    if (!portalActive && now - offlineSince >= PORTAL_FALLBACK_MS) {
      startPortal(true);
    }
  }

  // Serve portal requests if active
  if (portalActive) {
    dnsServer.processNextRequest();
    portal.handleClient();
  }

  // Always tick connection monitor & timers (even when AP was closed to connect)
  portalTick(now);

  // Periodic STA reconnection attempts (only when not actively provisioning)
  if (WiFi.status() != WL_CONNECTED && provState != PROV_CONNECTING &&
      wifiSsid.length() && now - lastWifiTry >= 10000) {
    lastWifiTry = now;
    WiFi.disconnect();
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }

  return WiFi.status() == WL_CONNECTED;
}