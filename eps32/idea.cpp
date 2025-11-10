/*
  ESP32 Wi‑Fi Onboarding (AP → STA) + 1‑minute Heartbeat Calls
  ----------------------------------------------------------------
  Features
  - On first boot (no creds) or failed STA connect → starts AP “ESP32_Setup” with captive portal.
  - Web UI at http://192.168.4.1/ to enter: WiFi SSID, WiFi Password, API Endpoint URL.
  - Validates WiFi credentials immediately (tries to connect). Only saves if validation succeeds.
  - After success → stores to NVS (Preferences), reboots, and connects in STA mode.
  - In STA mode → calls the API endpoint every 60 seconds (GET) with basic device info.
  - Long‑press Reset Button (>30s) to factory reset (clears stored creds and API URL) and reboot to AP mode.

  Notes
  - Uses only Arduino‑ESP32 core libs: WiFi, WebServer, DNSServer, Preferences, HTTPClient.
  - HTTPS endpoints are supported with insecure cert validation for simplicity (set to true below if needed).
  - Adjust pins and timings as needed. Default reset button pin uses INPUT_PULLUP (active‑LOW).

  Tested with: ESP32‑DevKitC, Arduino‑ESP32 core 2.x
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ====== User Config ======
static const char* AP_SSID = "ESP32_Setup";         // AP SSID in setup mode
static const char* AP_PASS = "";                    // empty: open AP; set a password if desired
static const byte   DNS_PORT = 53;                   // captive portal DNS

static const int RESET_BTN_PIN = 0;                  // Use a button wired to GND; INPUT_PULLUP
static const unsigned long FACTORY_HOLD_MS = 30000;  // 30s hold for factory reset

static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000; // 20s try to connect
static const unsigned long WIFI_RETRY_TOTAL_MS = 30000;      // 30s retries on normal boot

static const unsigned long HEARTBEAT_INTERVAL_MS = 60000;    // 60s API heartbeat

// Allow insecure HTTPS for ease of use (set false to require HTTP only)
static const bool ALLOW_INSECURE_HTTPS = true;

// ====== Globals ======
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

String g_ssid, g_pass, g_api;
unsigned long lastHeartbeat = 0;

// ====== Utility: URL‑encode (basic) ======
String urlEncode(const String &src) {
  String out;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < src.length(); ++i) {
    char c = src[i];
    if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

// ====== Factory Reset Check ======
bool checkFactoryResetHold() {
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  if (digitalRead(RESET_BTN_PIN) == LOW) {
    unsigned long t0 = millis();
    while (digitalRead(RESET_BTN_PIN) == LOW) {
      if (millis() - t0 >= FACTORY_HOLD_MS) {
        return true; // held long enough
      }
      delay(10);
    }
  }
  return false;
}

void factoryReset() {
  prefs.begin("net", false);
  prefs.clear();
  prefs.end();
  delay(100);
  ESP.restart();
}

// ====== Captive Portal Helpers ======
bool isIp(const String &str) {
  for (size_t i = 0; i < str.length(); i++) {
    char c = str[i];
    if (c != '.' && (c < '0' || c > '9')) return false;
  }
  return true;
}

String toStringIp(IPAddress ip) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

void handleCaptivePortal() {
  if (!isIp(server.hostHeader())) {
    server.sendHeader("Location", String("http://") + toStringIp(server.client().localIP()), true);
    server.send(302, "text/plain", "");
  }
}

// ====== AP Mode (Setup) ======
String htmlPage() {
  int n = WiFi.scanNetworks();
  String options;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "&quot;");
    options += "<option value=\"" + ssid + "\">" + ssid + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }

  String page = R"HTML(
  <!doctype html>
  <html><head>
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>ESP32 Setup</title>
    <style>
      body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:0;padding:24px;background:#f6f7fb;color:#111}
      .card{max-width:560px;margin:auto;background:#fff;border-radius:16px;box-shadow:0 6px 24px rgba(0,0,0,.08);padding:20px}
      h1{font-size:22px;margin:0 0 12px}
      label{display:block;margin:12px 0 6px;font-weight:600}
      input,select{width:100%;padding:10px;border:1px solid #ddd;border-radius:10px;font-size:14px}
      button{margin-top:16px;padding:12px 16px;border:0;border-radius:12px;font-weight:700}
      .primary{background:#111;color:#fff}
      .muted{color:#666;font-size:12px;margin-top:8px}
    </style>
  </head><body>
    <div class="card">
      <h1>ESP32 Wi‑Fi Setup</h1>
      <form method="POST" action="/save">
        <label>Wi‑Fi SSID</label>
        <select name="ssid">
          <option value="">— Select from scan —</option>
          )HTML";
  page += options;
  page += R"HTML(
        </select>
        <label>…or enter SSID manually</label>
        <input name="ssid_manual" placeholder="MyWiFi" />
        <label>Wi‑Fi Password</label>
        <input name="pass" type="password" placeholder="Password" />
        <label>API Endpoint URL</label>
        <input name="api" type="text" placeholder="https://example.com/heartbeat" />
        <button class="primary" type="submit">Save & Connect</button>
      </form>
      <p class="muted">Tip: Hold the device button for 30 seconds to factory reset.</p>
    </div>
  </body></html>
  )HTML";
  return page;
}

void startAPMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, (strlen(AP_PASS) ? AP_PASS : nullptr));
  delay(100);

  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(DNS_PORT, "*", apIP); // captive portal

  server.onNotFound([](){
    handleCaptivePortal();
    server.send(200, "text/html", htmlPage());
  });
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", htmlPage()); });

  server.on("/save", HTTP_POST, [](){
    String ssid = server.arg("ssid");
    String ssidManual = server.arg("ssid_manual");
    String pass = server.arg("pass");
    String api  = server.arg("api");
    if (ssidManual.length()) ssid = ssidManual;

    if (ssid.length() == 0 || api.length() == 0) {
      server.send(400, "text/plain", "SSID and API URL are required");
      return;
    }

    // Try connect to validate
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    delay(200);
    WiFi.setHostname("esp32-device");
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      // Save creds only on success
      prefs.begin("net", false);
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      prefs.putString("api",  api);
      prefs.end();

      String msg = String("<html><body><h2>Connected!</h2><p>IP: ") + WiFi.localIP().toString() + "</p><p>Rebooting…</p></body></html>";
      server.send(200, "text/html", msg);
      delay(800);
      ESP.restart();
    } else {
      server.send(200, "text/html", "<html><body><h2>Failed to connect.</h2><p>Please go back and check SSID/password.</p></body></html>");
      // Return to AP mode
      WiFi.mode(WIFI_AP);
    }
  });

  server.begin();
  Serial.println("\n[AP] Setup portal running at http://" + toStringIp(apIP));
}

// ====== STA Mode (Normal) ======
bool connectSTA(const String &ssid, const String &pass, unsigned long totalTimeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.setHostname("esp32-device");
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < totalTimeoutMs) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

void loadPrefs() {
  prefs.begin("net", true);
  g_ssid = prefs.getString("ssid", "");
  g_pass = prefs.getString("pass", "");
  g_api  = prefs.getString("api",  "");
  prefs.end();
}

// ====== Heartbeat ======
void sendHeartbeat() {
  if (g_api.length() == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  String url = g_api;
  // Append basic params (GET)
  String q = String("?device=ESP32&ip=") + urlEncode(WiFi.localIP().toString()) +
             "&rssi=" + String(WiFi.RSSI()) + "&uptime_ms=" + String(millis());
  if (!url.endsWith("/") && url.lastIndexOf('/') < 8) {
    // leave as is (handles full paths). Nothing special here.
  }

  // Choose client based on scheme
  HTTPClient http;
  bool ok = false;
  if (url.startsWith("https://")) {
    WiFiClientSecure *client = new WiFiClientSecure;
    if (ALLOW_INSECURE_HTTPS) client->setInsecure();
    if (http.begin(*client, url + q)) {
      int code = http.GET();
      Serial.printf("[HB] GET %s → %d\n", (url + q).c_str(), code);
      ok = (code > 0);
      http.end();
    }
    delete client;
  } else {
    WiFiClient client;
    if (http.begin(client, url + q)) {
      int code = http.GET();
      Serial.printf("[HB] GET %s → %d\n", (url + q).c_str(), code);
      ok = (code > 0);
      http.end();
    }
  }

  if (!ok) {
    Serial.println("[HB] Request failed");
  }
}

// ====== Setup / Loop ======
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nBooting…");

  if (checkFactoryResetHold()) {
    Serial.println("Factory reset requested — clearing NVS and rebooting.");
    factoryReset();
  }

  loadPrefs();

  if (g_ssid.length() == 0 || g_api.length() == 0) {
    Serial.println("No credentials/API found → entering AP setup mode.");
    startAPMode();
    return;
  }

  Serial.printf("Connecting to SSID: %s\n", g_ssid.c_str());
  if (connectSTA(g_ssid, g_pass, WIFI_RETRY_TOTAL_MS)) {
    Serial.print("Connected. IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("STA connect failed → entering AP setup mode.");
    startAPMode();
  }
}

void loop() {
  // If in AP mode, run captive portal services
  if (WiFi.getMode() & WIFI_AP) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  // In STA mode, send heartbeat every minute
  unsigned long now = millis();
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    sendHeartbeat();
  }
}
