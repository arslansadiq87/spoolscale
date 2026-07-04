#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HX711.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESPmDNS.h>
#include <esp_system.h>
#include <math.h>

#if __has_include("user_config.h")
#include "user_config.h"
#else
#include "user_config.example.h"
#endif

// -------------------- Config bindings --------------------
static const char *WIFI_SSID_VALUE = WIFI_SSID;
static const char *WIFI_PASSWORD_VALUE = WIFI_PASSWORD;
static const char *DEVICE_NAME_VALUE = DEVICE_NAME;
static const char *MDNS_HOSTNAME_VALUE = MDNS_HOSTNAME;
static const int HX711_DOUT_GPIO = HX711_DOUT_PIN;
static const int HX711_SCK_GPIO = HX711_SCK_PIN;
static const int I2C_SDA_PIN = OLED_SDA_PIN;
static const int I2C_SCL_PIN = OLED_SCL_PIN;
static const uint8_t OLED_ADDR = OLED_I2C_ADDRESS;
static const int TARE_BUTTON_GPIO = TARE_BUTTON_PIN;
static const int TOUCH_GPIO = TTP223_PIN;

// Defaults
static constexpr float DEFAULT_CALIBRATION_FACTOR = HX711_CALIBRATION_FACTOR;
static constexpr float CONFIG_FILTER_ALPHA = SCALE_FILTER_ALPHA;
static constexpr float CONFIG_WEIGHT_DEADBAND_KG = SCALE_WEIGHT_DEADBAND_KG;
static constexpr uint32_t CONFIG_SENSOR_POLL_MS = SCALE_SENSOR_POLL_MS;
static constexpr uint32_t CONFIG_OLED_REFRESH_MS = SCALE_OLED_REFRESH_MS;
static constexpr uint32_t CONFIG_WIFI_RETRY_MS = SCALE_WIFI_RETRY_MS;
static constexpr uint32_t CONFIG_WIFI_CONNECT_TIMEOUT_MS = SCALE_WIFI_CONNECT_TIMEOUT_MS;
static constexpr uint32_t CONFIG_OLED_WAKE_WINDOW_MS = SCALE_OLED_WAKE_WINDOW_MS;
static constexpr uint32_t CONFIG_BUTTON_DEBOUNCE_MS = SCALE_BUTTON_DEBOUNCE_MS;
static constexpr uint8_t CONFIG_TARE_SAMPLES = SCALE_TARE_SAMPLES;
static constexpr uint8_t CONFIG_CALIBRATION_SAMPLES = SCALE_CALIBRATION_SAMPLES;

// -------------------- Globals --------------------
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
HX711 scale;
WebServer server(80);
Preferences prefs;

float calibrationFactor = DEFAULT_CALIBRATION_FACTOR;
float filteredWeightKg = 0.0f;
long lastRaw = 0;
long tareOffset = 0;
String modeText = "BOOT";

bool displayReady = false;
bool hxReady = false;
bool oledAwake = true;
bool lastTouchState = false;
bool lastButtonPressed = false;
bool tareRequested = false;
bool tareInProgress = false;
bool calibrationRequested = false;
bool mdnsStarted = false;
bool wifiConnecting = false;
bool hasStoredTareOffset = false;

float calibrationKnownKg = 0.0f;
float pendingCalibrationFactor = 0.0f;

uint8_t tareSamplesCollected = 0;
int64_t tareAccumulator = 0;
uint8_t calibrationSamplesCollected = 0;
int64_t calibrationAccumulator = 0;

unsigned long lastSensorPollMs = 0;
unsigned long lastOledMs = 0;
unsigned long oledWakeUntilMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long wifiConnectStartedMs = 0;
unsigned long lastButtonChangeMs = 0;

String lastStatusMessage = "Booting";

String wifiStatusText(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "NO_SHIELD";
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

bool timeReached(unsigned long now, unsigned long target) {
  return static_cast<long>(now - target) >= 0;
}

void setStatus(const String &status) {
  lastStatusMessage = status;
  Serial.println(status);
}

void setOledPower(bool on) {
  if (!displayReady) return;
  if (oledAwake == on) return;
  oledAwake = on;
  display.ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
}

void wakeOledForWindow() {
  oledWakeUntilMs = millis() + CONFIG_OLED_WAKE_WINDOW_MS;
  setOledPower(true);
}

void saveCalibration(float value) {
  calibrationFactor = value;
  prefs.putFloat("cal", calibrationFactor);
  if (hxReady) {
    scale.set_scale(calibrationFactor);
  }
}

void saveTareOffset(long value) {
  tareOffset = value;
  hasStoredTareOffset = true;
  prefs.putLong("offset", tareOffset);
  if (hxReady) {
    scale.set_offset(tareOffset);
  }
}

void showLines(const String &l1, const String &l2 = "", const String &l3 = "") {
  if (!displayReady) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(l1);
  if (l2.length()) display.println(l2);
  if (l3.length()) display.println(l3);
  display.display();
}

String ipString() {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return "0.0.0.0";
}

String displayWeightValue(float weightKg, uint8_t decimalsKg = 3, uint8_t decimalsG = 0) {
  if (fabsf(weightKg) < 1.0f) {
    return String(weightKg * 1000.0f, static_cast<unsigned int>(decimalsG));
  }
  return String(weightKg, static_cast<unsigned int>(decimalsKg));
}

String displayWeightUnit(float weightKg) {
  return fabsf(weightKg) < 1.0f ? "g" : "kg";
}

bool wifiConfigured() {
  return WIFI_SSID_VALUE[0] != '\0';
}

void beginWifiConnect() {
  if (wifiConnecting) return;
  if (!wifiConfigured()) {
    modeText = "OFFLINE";
    setStatus("Wi-Fi disabled: set WIFI_SSID in include/user_config.h");
    return;
  }

  wifiConnecting = true;
  lastWifiAttemptMs = millis();
  wifiConnectStartedMs = lastWifiAttemptMs;
  setStatus("Wi-Fi: connecting to " + String(WIFI_SSID_VALUE));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID_VALUE, WIFI_PASSWORD_VALUE);
}

void updateWifi() {
  if (!wifiConfigured()) {
    modeText = "OFFLINE";
    return;
  }

  wl_status_t status = WiFi.status();
  unsigned long now = millis();

  if (status == WL_CONNECTED) {
    if (wifiConnecting) {
      wifiConnecting = false;
      modeText = "STA";
      setStatus("Wi-Fi connected: " + WiFi.localIP().toString());
    }

    if (!mdnsStarted) {
      mdnsStarted = MDNS.begin(MDNS_HOSTNAME_VALUE);
      if (mdnsStarted) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS ready: http://" + String(MDNS_HOSTNAME_VALUE) + ".local/");
      } else {
        Serial.println("mDNS start failed");
      }
    }
    return;
  }

  modeText = "OFFLINE";

  if (wifiConnecting && now - wifiConnectStartedMs >= CONFIG_WIFI_CONNECT_TIMEOUT_MS) {
    wifiConnecting = false;
    setStatus("Wi-Fi timeout: " + wifiStatusText(status));
  }

  if (!wifiConnecting && now - lastWifiAttemptMs >= CONFIG_WIFI_RETRY_MS) {
    beginWifiConnect();
  }
}

void initDisplay() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!displayReady) {
    Serial.println("OLED init failed");
    return;
  }

  showLines("Scale Boot", "Init...");
  wakeOledForWindow();
}

void initScale() {
  scale.begin(HX711_DOUT_GPIO, HX711_SCK_GPIO);
  hxReady = scale.wait_ready_timeout(1500);
  if (!hxReady) {
    setStatus("HX711 not ready, will retry in loop");
    return;
  }

  scale.set_scale(calibrationFactor);
  if (hasStoredTareOffset) {
    scale.set_offset(tareOffset);
    setStatus("HX711 ready with saved tare");
  } else {
    tareRequested = true;
    setStatus("HX711 ready, tare required");
  }
}

bool readScaleSample(long &sample) {
  if (!hxReady || !scale.is_ready()) return false;
  sample = scale.read();
  return true;
}

float rawToKg(long raw) {
  if (fabsf(calibrationFactor) < 0.000001f) return 0.0f;
  return static_cast<float>(raw - scale.get_offset()) / calibrationFactor;
}

void updateWeight() {
  unsigned long now = millis();
  if (now - lastSensorPollMs < CONFIG_SENSOR_POLL_MS) return;
  lastSensorPollMs = now;

  if (!hxReady) {
    hxReady = scale.wait_ready_timeout(1);
    if (hxReady) {
      scale.set_scale(calibrationFactor);
      if (hasStoredTareOffset) {
        scale.set_offset(tareOffset);
        setStatus("HX711 recovered with saved tare");
      } else {
        tareRequested = true;
        setStatus("HX711 recovered, tare required");
      }
    }
    return;
  }

  long sample = 0;
  if (!readScaleSample(sample)) return;

  lastRaw = sample;

  if (tareInProgress) {
    tareAccumulator += sample;
    tareSamplesCollected++;
    if (tareSamplesCollected >= CONFIG_TARE_SAMPLES) {
      long newOffset = static_cast<long>(tareAccumulator / tareSamplesCollected);
      saveTareOffset(newOffset);
      tareInProgress = false;
      tareSamplesCollected = 0;
      tareAccumulator = 0;
      filteredWeightKg = 0.0f;
      setStatus("Tare complete");
    }
    return;
  }

  if (calibrationRequested) {
    calibrationAccumulator += sample;
    calibrationSamplesCollected++;
    if (calibrationSamplesCollected >= CONFIG_CALIBRATION_SAMPLES) {
      long averaged = static_cast<long>(calibrationAccumulator / calibrationSamplesCollected);
      long delta = averaged - scale.get_offset();
      calibrationRequested = false;
      calibrationSamplesCollected = 0;
      calibrationAccumulator = 0;

      if (delta == 0 || calibrationKnownKg <= 0.0f) {
        setStatus("Calibration failed: no load change");
      } else {
        pendingCalibrationFactor = static_cast<float>(delta) / calibrationKnownKg;
        saveCalibration(pendingCalibrationFactor);
        setStatus("Calibration updated: " + String(calibrationFactor, 5));
      }
    }
    return;
  }

  float weightKg = rawToKg(sample);
  if (fabsf(weightKg) < CONFIG_WEIGHT_DEADBAND_KG) {
    weightKg = 0.0f;
  }
  filteredWeightKg = CONFIG_FILTER_ALPHA * weightKg + (1.0f - CONFIG_FILTER_ALPHA) * filteredWeightKg;
}

void requestTare() {
  if (!hxReady) {
    setStatus("Tare ignored: HX711 not ready");
    return;
  }
  tareRequested = true;
  wakeOledForWindow();
}

void updateTareState() {
  if (!tareRequested || tareInProgress || calibrationRequested) return;
  tareRequested = false;
  tareInProgress = true;
  tareSamplesCollected = 0;
  tareAccumulator = 0;
  setStatus("Tare started");
}

void updateButtonInput() {
  if (TARE_BUTTON_GPIO < 0) return;

  bool pressed = digitalRead(TARE_BUTTON_GPIO) == LOW;
  unsigned long now = millis();

  if (pressed != lastButtonPressed && now - lastButtonChangeMs >= CONFIG_BUTTON_DEBOUNCE_MS) {
    lastButtonChangeMs = now;
    lastButtonPressed = pressed;
    if (pressed) {
      requestTare();
    }
  }
}

void updateTouchWake() {
  bool touchState = digitalRead(TOUCH_GPIO) == HIGH;
  bool touchRisingEdge = touchState && !lastTouchState;
  lastTouchState = touchState;

  if (touchRisingEdge) {
    wakeOledForWindow();
  }

  if (displayReady && timeReached(millis(), oledWakeUntilMs)) {
    setOledPower(false);
  }
}

String buildWeightJson() {
  String json;
  json.reserve(220);
  json += "{";
  json += "\"weight_kg\":" + String(filteredWeightKg, 4) + ",";
  json += "\"raw\":" + String(lastRaw) + ",";
  json += "\"calibration_factor\":" + String(calibrationFactor, 5) + ",";
  json += "\"ip\":\"" + ipString() + "\",";
  json += "\"mode\":\"" + modeText + "\",";
  json += "\"status\":\"" + lastStatusMessage + "\"";
  json += "}";
  return json;
}

void handleWeight() {
  server.send(200, "application/json", buildWeightJson());
}

void handleTare() {
  if (!hxReady) {
    server.send(503, "application/json", "{\"message\":\"HX711 not ready\"}");
    return;
  }

  if (tareInProgress) {
    server.send(202, "application/json", "{\"message\":\"Tare already running\"}");
    return;
  }

  requestTare();
  server.send(202, "application/json", "{\"message\":\"Tare scheduled\"}");
}

void handleSetFactor() {
  if (!server.hasArg("value")) {
    server.send(400, "application/json", "{\"message\":\"Missing value\"}");
    return;
  }

  float value = server.arg("value").toFloat();
  if (fabsf(value) < 0.000001f) {
    server.send(400, "application/json", "{\"message\":\"Invalid factor\"}");
    return;
  }

  saveCalibration(value);
  String msg = "{\"message\":\"Saved factor\",\"value\":" + String(calibrationFactor, 5) + "}";
  server.send(200, "application/json", msg);
}

void handleKnownCalibration() {
  if (!server.hasArg("known_kg")) {
    server.send(400, "application/json", "{\"message\":\"Missing known_kg\"}");
    return;
  }

  float known = server.arg("known_kg").toFloat();
  if (known <= 0.0f) {
    server.send(400, "application/json", "{\"message\":\"known_kg must be > 0\"}");
    return;
  }

  if (!hxReady) {
    server.send(503, "application/json", "{\"message\":\"HX711 not ready\"}");
    return;
  }

  if (tareInProgress || calibrationRequested) {
    server.send(409, "application/json", "{\"message\":\"Scale busy\"}");
    return;
  }

  calibrationKnownKg = known;
  calibrationRequested = true;
  calibrationSamplesCollected = 0;
  calibrationAccumulator = 0;
  setStatus("Calibration started");
  server.send(202, "application/json", "{\"message\":\"Calibration scheduled\"}");
}

String indexHtml() {
  String html;
  html.reserve(5200);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" + String(DEVICE_NAME_VALUE) + "</title><style>";
  html += ":root{--bg:#eff3f6;--panel:#ffffff;--ink:#13212f;--muted:#51606f;--accent:#10283f;--ok:#0a7f3f;--err:#b00020;}";
  html += "body{font-family:Verdana,sans-serif;margin:0;padding:20px;background:linear-gradient(180deg,#f7fafc,#e9eef2);color:var(--ink);}";
  html += ".card{max-width:760px;margin:auto;background:var(--panel);border-radius:14px;padding:18px;box-shadow:0 10px 30px rgba(0,0,0,.08);}";
  html += "h1{margin:0 0 10px;font-size:1.4rem;}h3{margin:18px 0 8px;}";
  html += ".w{font-size:2.3rem;font-weight:700;margin:8px 0 2px;}";
  html += ".meta{color:var(--muted);font-size:.92rem;margin-bottom:10px;}";
  html += ".row{display:flex;gap:10px;flex-wrap:wrap;margin:10px 0;}";
  html += "button,input{padding:10px;border:1px solid #b7c1cb;border-radius:8px;font-size:1rem;}";
  html += "button{background:var(--accent);color:#fff;border:none;cursor:pointer;}button:hover{background:#1a3e61;}";
  html += "form{display:flex;gap:8px;flex-wrap:wrap;margin:10px 0;}input{background:#f8fafc;}";
  html += ".ok{color:var(--ok);} .err{color:var(--err);} .pill{display:inline-block;padding:5px 10px;border-radius:999px;background:#e7edf3;font-size:.85rem;}";
  html += "</style></head><body><div class='card'>";
  html += "<h1>" + String(DEVICE_NAME_VALUE) + "</h1>";
  html += "<div class='w'><span id='w'>0</span> <span id='unit'>g</span></div>";
  html += "<div class='meta'>Raw: <span id='raw'>0</span> | Factor: <span id='factor'>0</span></div>";
  html += "<div class='row'><span class='pill'>Mode: <span id='mode'>BOOT</span></span><span class='pill'>IP: <span id='ip'>0.0.0.0</span></span></div>";
  html += "<div class='meta'>Status: <span id='status'>Starting</span></div>";
  html += "<button onclick='tare()'>Tare (Zero)</button>";
  html += "<h3>Set Calibration Factor</h3>";
  html += "<form onsubmit='setFactor(event)'><input id='factorInput' type='number' step='0.001' required><button type='submit'>Save Factor</button></form>";
  html += "<h3>Calibrate From Known Weight</h3>";
  html += "<form onsubmit='knownCal(event)'><input id='knownInput' type='number' step='0.001' min='0.001' required placeholder='Known kg'><button type='submit'>Calibrate</button></form>";
  html += "<div id='msg'></div>";
  html += "<p class='meta'>Process: tare empty scale, place known weight, then calibrate. Actions run in background.</p>";
  html += "</div><script>";
  html += "const msg=document.getElementById('msg');";
  html += "function say(t,ok=true){msg.textContent=t;msg.className=ok?'ok':'err';}";
  html += "async function parse(r){try{return await r.json();}catch{return {message:'Invalid response'};}}";
  html += "async function poll(){try{const r=await fetch('/api/weight');const j=await r.json();";
  html += "const kg=(j.weight_kg!=null)?j.weight_kg:0;const useG=Math.abs(kg)<1;w.textContent=useG?(kg*1000).toFixed(0):kg.toFixed(3);unit.textContent=useG?'g':'kg';raw.textContent=(j.raw!=null)?j.raw:0;factor.textContent=((j.calibration_factor!=null)?j.calibration_factor:0).toFixed(3);";
  html += "mode.textContent=(j.mode!=null)?j.mode:'-';ip.textContent=(j.ip!=null)?j.ip:'0.0.0.0';status.textContent=(j.status!=null)?j.status:'-';";
  html += "}catch(e){say('Connection error',false);}}";
  html += "async function tare(){const r=await fetch('/api/tare',{method:'POST'});const j=await parse(r);say(j.message||'Tare request sent',r.ok);}";
  html += "async function setFactor(e){e.preventDefault();const v=factorInput.value;";
  html += "const r=await fetch('/api/calibration/factor',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'value='+encodeURIComponent(v)});";
  html += "const j=await parse(r);say(j.message||'Factor updated',r.ok);}";
  html += "async function knownCal(e){e.preventDefault();const v=knownInput.value;";
  html += "const r=await fetch('/api/calibration/known',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'known_kg='+encodeURIComponent(v)});";
  html += "const j=await parse(r);say(j.message||'Calibration request sent',r.ok);}";
  html += "setInterval(poll,500);poll();";
  html += "</script></body></html>";
  return html;
}

void setupServer() {
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", indexHtml()); });
  server.on("/api/weight", HTTP_GET, handleWeight);
  server.on("/api/tare", HTTP_POST, handleTare);
  server.on("/api/calibration/factor", HTTP_POST, handleSetFactor);
  server.on("/api/calibration/known", HTTP_POST, handleKnownCalibration);

  server.on("/api/ha/state", HTTP_GET, []() {
    String json;
    json.reserve(180);
    json += "{";
    json += "\"state\":" + String(filteredWeightKg, 4) + ",";
    json += "\"unit_of_measurement\":\"kg\",";
    json += "\"attributes\":{\"raw\":" + String(lastRaw) + ",\"calibration_factor\":" + String(calibrationFactor, 5) + "}";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/ha/config", HTTP_GET, []() {
    server.send(200, "application/json",
                "{\"name\":\"" + String(DEVICE_NAME_VALUE) + "\",\"state_topic\":\"scale/state\",\"unit_of_measurement\":\"kg\","
                "\"device_class\":\"weight\",\"unique_id\":\"esp32_scale_1\",\"icon\":\"mdi:scale\"}");
  });

  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.print("Reset reason: ");
  Serial.println(static_cast<int>(esp_reset_reason()));

  if (TARE_BUTTON_GPIO >= 0) {
    pinMode(TARE_BUTTON_GPIO, INPUT_PULLUP);
  }
  pinMode(TOUCH_GPIO, INPUT);

  initDisplay();

  prefs.begin("scale", false);
  calibrationFactor = prefs.getFloat("cal", DEFAULT_CALIBRATION_FACTOR);
  hasStoredTareOffset = prefs.isKey("offset");
  if (hasStoredTareOffset) {
    tareOffset = prefs.getLong("offset", 0);
  }

  if (wifiConfigured()) {
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
  } else {
    setStatus("Wi-Fi disabled: set WIFI_SSID in include/user_config.h");
  }

  initScale();
  setupServer();
  lastWifiAttemptMs = millis() - CONFIG_WIFI_RETRY_MS;

  showLines("Scale Ready", wifiConfigured() ? "Wi-Fi connecting" : "Wi-Fi disabled", "Open web UI");
  Serial.println("=== " + String(DEVICE_NAME_VALUE) + " Ready ===");
  Serial.print("Calibration factor: ");
  Serial.println(calibrationFactor, 5);
}

void loop() {
  server.handleClient();
  updateWifi();
  updateTouchWake();
  updateButtonInput();
  updateTareState();
  updateWeight();

  if (displayReady && millis() - lastOledMs >= CONFIG_OLED_REFRESH_MS) {
    lastOledMs = millis();
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    if (OLED_HEIGHT <= 32) {
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print(modeText);
      display.print(" ");
      display.println(ipString());

      display.setTextSize(2);
      display.setCursor(0, 13);
      display.print(displayWeightValue(filteredWeightKg));
      display.println(displayWeightUnit(filteredWeightKg));
    } else {
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("Mode:");
      display.print(modeText);
      display.print(" IP:");
      display.println(ipString());

      display.setTextSize(2);
      display.setCursor(0, 20);
      display.print(displayWeightValue(filteredWeightKg));
      display.println(displayWeightUnit(filteredWeightKg));

      display.setTextSize(1);
      display.setCursor(0, 52);
      display.print("Status:");
      display.print(lastStatusMessage.substring(0, 18));
    }
    display.display();
  }
}
