#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

// =====================================================
// WIFI CONFIG
// =====================================================
const char* WIFI_SSID = "WIFI KOIN";
const char* WIFI_PASSWORD = ""; // kosong jika WiFi tanpa password

// =====================================================
// MIKROTIK REST API CONFIG
// =====================================================
const char* MIKROTIK_HOST = "http://125.15.15.1:8080";
const char* MIKROTIK_USER = "iotnet";
const char* MIKROTIK_PASS = "mikrotik";
const char* HOTSPOT_PROFILE = "default";

// =====================================================
// PIN CONFIG
// =====================================================
#define COIN_SIGNAL_PIN   14
#define LED_KUNING        22
#define LED_PUTIH         23
#define LED_BUILTIN_WIFI  2

// =====================================================
// SYSTEM CONFIG
// =====================================================
const unsigned long WAIT_COIN_TIMEOUT = 30000; // 30 detik
const unsigned long PULSE_GAP_TIME = 900;      // aman untuk pulse 20
const unsigned long DEBOUNCE_TIME = 25;        // anti noise

WebServer server(80);

// =====================================================
// COIN VARIABLES
// =====================================================
volatile int pulseCount = 0;
volatile bool pulseDetected = false;
volatile unsigned long interruptLastMillis = 0;
volatile unsigned long lastPulseMillis = 0;

bool sessionActive = false;
bool coinSlotActive = false;

String activeMac = "";
String sessionId = "";
String generatedVoucher = "";

int totalMoney = 0;
int totalMinutes = 0;

unsigned long lastCoinMillis = 0;

// =====================================================
// DEBUG VARIABLES
// =====================================================
int lastHttpCode = 0;
String lastMikrotikResponse = "";
String lastMikrotikUrl = "";
String lastMikrotikBody = "";

// =====================================================
// INTERRUPT COIN
// =====================================================
void IRAM_ATTR coinInserted() {
  unsigned long now = millis();

  if (now - interruptLastMillis > DEBOUNCE_TIME) {
    if (coinSlotActive) {
      pulseCount++;
      pulseDetected = true;
      lastPulseMillis = now;
    }

    interruptLastMillis = now;
  }
}

// =====================================================
// RATE CONFIG
// =====================================================
// Mapping final:
// pulse 2  = Rp500 silver
// pulse 10 = Rp500 kuning
// pulse 20 = Rp1000 silver
int getCoinValueFromPulse(int pulse) {
  if (pulse == 2) return 500;
  if (pulse == 10) return 500;
  if (pulse == 20) return 1000;

  return 0;
}

// Tarif:
// Rp500  = 30 menit
// Rp1000 = 1 jam
// Rp2000 = 6 jam
// Rp3000 = 12 jam
// Rp5000 = 24 jam
int calculateDurationMinutes(int money) {
  int minutes = 0;
  int remaining = money;

  while (remaining >= 5000) {
    minutes += 1440;
    remaining -= 5000;
  }

  while (remaining >= 3000) {
    minutes += 720;
    remaining -= 3000;
  }

  while (remaining >= 2000) {
    minutes += 360;
    remaining -= 2000;
  }

  while (remaining >= 1000) {
    minutes += 60;
    remaining -= 1000;
  }

  while (remaining >= 500) {
    minutes += 30;
    remaining -= 500;
  }

  return minutes;
}

// =====================================================
// RESPONSE HELPER
// =====================================================
void sendCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}

void sendJSON(String json) {
  sendCORS();
  server.send(200, "application/json", json);
}

String jsonEscape(String text) {
  text.replace("\\", "\\\\");
  text.replace("\"", "\\\"");
  text.replace("\n", "\\n");
  text.replace("\r", "");
  return text;
}

// =====================================================
// COIN SLOT LOGIC CONTROL
// =====================================================
// Catatan:
// Karena tidak ada COIN_SET_PIN / relay / MOSFET,
// coin acceptor tetap mendapat daya dari adaptor 12V.
// Fungsi ini hanya mengatur status pembacaan di ESP32
// dan indikator LED kuning.

void enableCoinSlot() {
  coinSlotActive = true;
  digitalWrite(LED_KUNING, HIGH);
  lastCoinMillis = millis();
}

void disableCoinSlot() {
  coinSlotActive = false;
  digitalWrite(LED_KUNING, LOW);
}

// =====================================================
// MIKROTIK REST API
// =====================================================
bool mikrotikGet(String endpoint, String &responseOut) {
  if (WiFi.status() != WL_CONNECTED) {
    responseOut = "WiFi ESP32 tidak terhubung";
    lastHttpCode = -1;
    lastMikrotikResponse = responseOut;
    return false;
  }

  HTTPClient http;
  String url = String(MIKROTIK_HOST) + endpoint;

  lastMikrotikUrl = url;
  lastMikrotikBody = "";

  Serial.println();
  Serial.println("==================================");
  Serial.println("MIKROTIK REST GET");
  Serial.println("URL  : " + url);

  http.begin(url);
  http.setAuthorization(MIKROTIK_USER, MIKROTIK_PASS);
  http.setTimeout(10000);
  http.useHTTP10(true);

  int httpCode = http.GET();
  String response = http.getString();

  lastHttpCode = httpCode;
  lastMikrotikResponse = response;

  Serial.println("HTTP CODE : " + String(httpCode));
  Serial.println("RESPONSE  : " + response);
  Serial.println("==================================");
  Serial.println();

  responseOut = response;
  http.end();

  return (httpCode >= 200 && httpCode < 300);
}

bool mikrotikPut(String endpoint, String jsonBody, String &responseOut) {
  if (WiFi.status() != WL_CONNECTED) {
    responseOut = "WiFi ESP32 tidak terhubung";
    lastHttpCode = -1;
    lastMikrotikResponse = responseOut;
    return false;
  }

  HTTPClient http;
  String url = String(MIKROTIK_HOST) + endpoint;

  lastMikrotikUrl = url;
  lastMikrotikBody = jsonBody;

  Serial.println();
  Serial.println("==================================");
  Serial.println("MIKROTIK REST PUT");
  Serial.println("URL  : " + url);
  Serial.println("BODY : " + jsonBody);

  http.begin(url);
  http.setAuthorization(MIKROTIK_USER, MIKROTIK_PASS);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  http.useHTTP10(true);

  int httpCode = http.PUT(jsonBody);
  String response = http.getString();

  lastHttpCode = httpCode;
  lastMikrotikResponse = response;

  Serial.println("HTTP CODE : " + String(httpCode));
  Serial.println("RESPONSE  : " + response);
  Serial.println("==================================");
  Serial.println();

  responseOut = response;
  http.end();

  return (httpCode >= 200 && httpCode < 300);
}

String makeVoucherCode() {
  String code = "WK";
  code += String(random(100000, 999999));
  code += String(millis() % 1000);
  return code;
}

bool createMikrotikVoucher(String voucher, int minutes, int money) {
  String response = "";

  String comment = "WIFI_COIN_Rp" + String(money) + "_" + String(minutes) + "menit";

  String body = "{";
  body += "\"name\":\"" + voucher + "\",";
  body += "\"password\":\"" + voucher + "\",";
  body += "\"profile\":\"" + String(HOTSPOT_PROFILE) + "\",";
  body += "\"limit-uptime\":\"" + String(minutes) + "m\",";
  body += "\"comment\":\"" + comment + "\"";
  body += "}";

  for (int i = 1; i <= 3; i++) {
    Serial.println("Percobaan membuat voucher ke-" + String(i));

    bool ok = mikrotikPut("/rest/ip/hotspot/user", body, response);

    if (ok) {
      return true;
    }

    delay(1000);
  }

  return false;
}

// =====================================================
// API ENDPOINTS
// =====================================================
void handleRoot() {
  String html = "";
  html += "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>WiFi Coin ESP32</title>";
  html += "<style>";
  html += "body{font-family:Arial;background:#f4f7fb;padding:20px;text-align:center;}";
  html += ".card{background:white;padding:20px;border-radius:16px;box-shadow:0 8px 20px rgba(0,0,0,.08);max-width:480px;margin:auto;}";
  html += "h2{margin-top:0;color:#111827;}";
  html += "p{color:#374151;}";
  html += ".ok{color:#16a34a;font-weight:bold;}";
  html += "code{background:#eef2ff;padding:4px 8px;border-radius:6px;display:inline-block;margin:3px;}";
  html += "</style></head><body>";
  html += "<div class='card'>";
  html += "<h2>WiFi Coin ESP32</h2>";
  html += "<p class='ok'>Perangkat aktif</p>";
  html += "<p>IP ESP32: <b>" + WiFi.localIP().toString() + "</b></p>";
  html += "<p>MAC ESP32: <b>" + WiFi.macAddress() + "</b></p>";
  html += "<code>/health</code><br>";
  html += "<code>/status</code><br>";
  html += "<code>/start?mac=AA:BB:CC:DD:EE:FF</code><br>";
  html += "<code>/confirm</code><br>";
  html += "<code>/cancel</code><br>";
  html += "<code>/test-rest</code><br>";
  html += "<code>/test-voucher</code><br>";
  html += "<code>/debug</code>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleStart() {
  if (sessionActive) {
    sendJSON("{\"status\":false,\"message\":\"Alat sedang digunakan\"}");
    return;
  }

  String mac = server.arg("mac");

  if (mac == "") {
    sendJSON("{\"status\":false,\"message\":\"MAC Address kosong\"}");
    return;
  }

  activeMac = mac;
  sessionId = String(random(100000, 999999));
  generatedVoucher = "";

  totalMoney = 0;
  totalMinutes = 0;

  noInterrupts();
  pulseCount = 0;
  pulseDetected = false;
  interrupts();

  sessionActive = true;
  enableCoinSlot();

  String json = "{";
  json += "\"status\":true,";
  json += "\"message\":\"Silakan masukkan koin\",";
  json += "\"session\":\"" + sessionId + "\",";
  json += "\"mac\":\"" + activeMac + "\"";
  json += "}";

  sendJSON(json);
}

void handleStatus() {
  unsigned long remain = 0;

  if (sessionActive && coinSlotActive) {
    unsigned long now = millis();

    if (now < lastCoinMillis + WAIT_COIN_TIMEOUT) {
      remain = (lastCoinMillis + WAIT_COIN_TIMEOUT - now) / 1000;
    }
  }

  int currentPulse = 0;

  noInterrupts();
  currentPulse = pulseCount;
  interrupts();

  String json = "{";
  json += "\"status\":true,";
  json += "\"sessionActive\":" + String(sessionActive ? "true" : "false") + ",";
  json += "\"coinSlotActive\":" + String(coinSlotActive ? "true" : "false") + ",";
  json += "\"currentPulse\":" + String(currentPulse) + ",";
  json += "\"money\":" + String(totalMoney) + ",";
  json += "\"minutes\":" + String(totalMinutes) + ",";
  json += "\"remainSeconds\":" + String(remain) + ",";
  json += "\"voucher\":\"" + generatedVoucher + "\",";
  json += "\"mac\":\"" + activeMac + "\",";
  json += "\"wifi\":\"" + String(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") + "\"";
  json += "}";

  sendJSON(json);
}

void handleConfirm() {
  if (!sessionActive) {
    sendJSON("{\"status\":false,\"message\":\"Tidak ada sesi aktif\"}");
    return;
  }

  disableCoinSlot();

  if (totalMoney <= 0 || totalMinutes <= 0) {
    sessionActive = false;
    activeMac = "";
    sessionId = "";
    generatedVoucher = "";

    sendJSON("{\"status\":false,\"message\":\"Belum ada koin masuk\"}");
    return;
  }

  generatedVoucher = makeVoucherCode();

  bool ok = createMikrotikVoucher(generatedVoucher, totalMinutes, totalMoney);

  if (ok) {
    String json = "{";
    json += "\"status\":true,";
    json += "\"message\":\"Voucher berhasil dibuat\",";
    json += "\"voucher\":\"" + generatedVoucher + "\",";
    json += "\"username\":\"" + generatedVoucher + "\",";
    json += "\"password\":\"" + generatedVoucher + "\",";
    json += "\"money\":" + String(totalMoney) + ",";
    json += "\"minutes\":" + String(totalMinutes);
    json += "}";

    sessionActive = false;
    activeMac = "";
    sessionId = "";
    totalMoney = 0;
    totalMinutes = 0;

    noInterrupts();
    pulseCount = 0;
    pulseDetected = false;
    interrupts();

    sendJSON(json);
  } else {
    String json = "{";
    json += "\"status\":false,";
    json += "\"message\":\"Gagal membuat voucher di Mikrotik\",";
    json += "\"httpCode\":" + String(lastHttpCode) + ",";
    json += "\"mikrotikResponse\":\"" + jsonEscape(lastMikrotikResponse) + "\"";
    json += "}";

    sendJSON(json);
  }
}

void handleCancel() {
  disableCoinSlot();

  sessionActive = false;
  activeMac = "";
  sessionId = "";
  generatedVoucher = "";
  totalMoney = 0;
  totalMinutes = 0;

  noInterrupts();
  pulseCount = 0;
  pulseDetected = false;
  interrupts();

  sendJSON("{\"status\":true,\"message\":\"Transaksi dibatalkan\"}");
}

void handleHealth() {
  String json = "{";
  json += "\"status\":true,";
  json += "\"message\":\"ESP32 WiFi Coin aktif\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"mac\":\"" + WiFi.macAddress() + "\"";
  json += "}";

  sendJSON(json);
}

void handleTestRest() {
  String response = "";
  bool ok = mikrotikGet("/rest/system/resource", response);

  String json = "{";
  json += "\"status\":" + String(ok ? "true" : "false") + ",";
  json += "\"message\":\"Test REST API Mikrotik\",";
  json += "\"httpCode\":" + String(lastHttpCode) + ",";
  json += "\"response\":\"" + jsonEscape(response) + "\"";
  json += "}";

  sendJSON(json);
}

void handleTestVoucher() {
  String voucher = makeVoucherCode();
  bool ok = createMikrotikVoucher(voucher, 30, 500);

  String json = "{";
  json += "\"status\":" + String(ok ? "true" : "false") + ",";
  json += "\"message\":\"Test membuat voucher 30 menit\",";
  json += "\"voucher\":\"" + voucher + "\",";
  json += "\"httpCode\":" + String(lastHttpCode) + ",";
  json += "\"mikrotikResponse\":\"" + jsonEscape(lastMikrotikResponse) + "\"";
  json += "}";

  sendJSON(json);
}

void handleDebug() {
  int currentPulse = 0;

  noInterrupts();
  currentPulse = pulseCount;
  interrupts();

  String json = "{";
  json += "\"lastHttpCode\":" + String(lastHttpCode) + ",";
  json += "\"lastMikrotikUrl\":\"" + jsonEscape(lastMikrotikUrl) + "\",";
  json += "\"lastMikrotikBody\":\"" + jsonEscape(lastMikrotikBody) + "\",";
  json += "\"lastMikrotikResponse\":\"" + jsonEscape(lastMikrotikResponse) + "\",";
  json += "\"currentPulse\":" + String(currentPulse) + ",";
  json += "\"totalMoney\":" + String(totalMoney) + ",";
  json += "\"totalMinutes\":" + String(totalMinutes) + ",";
  json += "\"sessionActive\":" + String(sessionActive ? "true" : "false") + ",";
  json += "\"coinSlotActive\":" + String(coinSlotActive ? "true" : "false");
  json += "}";

  sendJSON(json);
}

// =====================================================
// WIFI CONNECT
// =====================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Menghubungkan ke WiFi: ");
  Serial.println(WIFI_SSID);

  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_BUILTIN_WIFI, LOW);
    delay(300);
    digitalWrite(LED_BUILTIN_WIFI, HIGH);
    delay(300);
    Serial.print(".");
  }

  digitalWrite(LED_BUILTIN_WIFI, HIGH);

  Serial.println();
  Serial.println("WiFi Terhubung");
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("IP ESP32: ");
  Serial.println(WiFi.localIP());
  Serial.print("MAC ESP32: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);

  pinMode(COIN_SIGNAL_PIN, INPUT_PULLUP);
  pinMode(LED_KUNING, OUTPUT);
  pinMode(LED_PUTIH, OUTPUT);
  pinMode(LED_BUILTIN_WIFI, OUTPUT);

  digitalWrite(LED_KUNING, LOW);
  digitalWrite(LED_BUILTIN_WIFI, LOW);

  // LED putih menyala saat ESP32 hidup
  digitalWrite(LED_PUTIH, HIGH);

  attachInterrupt(digitalPinToInterrupt(COIN_SIGNAL_PIN), coinInserted, FALLING);

  randomSeed(analogRead(0));

  connectWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/start", HTTP_GET, handleStart);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/confirm", HTTP_GET, handleConfirm);
  server.on("/cancel", HTTP_GET, handleCancel);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/test-rest", HTTP_GET, handleTestRest);
  server.on("/test-voucher", HTTP_GET, handleTestVoucher);
  server.on("/debug", HTTP_GET, handleDebug);

  server.onNotFound([]() {
    sendCORS();
    server.send(404, "application/json", "{\"status\":false,\"message\":\"Endpoint tidak ditemukan\"}");
  });

  server.begin();

  Serial.println("WebServer ESP32 aktif");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  server.handleClient();

  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_BUILTIN_WIFI, HIGH);
  } else {
    digitalWrite(LED_BUILTIN_WIFI, LOW);
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(1000);
    return;
  }

  // Proses kumpulan pulse dari coin acceptor
  if (pulseDetected && (now - lastPulseMillis > PULSE_GAP_TIME)) {
    noInterrupts();
    int pulses = pulseCount;
    pulseCount = 0;
    pulseDetected = false;
    interrupts();

    int coinValue = getCoinValueFromPulse(pulses);

    Serial.println("==================================");
    Serial.println("KOIN / PULSE TERBACA");
    Serial.println("Pulse: " + String(pulses));

    if (coinValue > 0) {
      totalMoney += coinValue;
      totalMinutes = calculateDurationMinutes(totalMoney);
      lastCoinMillis = millis();

      Serial.println("Nominal: Rp" + String(coinValue));
      Serial.println("Total uang: Rp" + String(totalMoney));
      Serial.println("Durasi: " + String(totalMinutes) + " menit");

      digitalWrite(LED_KUNING, LOW);
      delay(80);
      digitalWrite(LED_KUNING, HIGH);
    } else {
      Serial.println("Pulse tidak dikenal: " + String(pulses));
      Serial.println("Mapping valid: 2=500, 10=500, 20=1000");
    }

    Serial.println("==================================");
  }

  // Timeout 30 detik
  if (sessionActive && coinSlotActive) {
    if (now - lastCoinMillis >= WAIT_COIN_TIMEOUT) {
      Serial.println("Timeout 30 detik, sesi insert coin dihentikan");

      disableCoinSlot();

      sessionActive = false;
      activeMac = "";
      sessionId = "";
      generatedVoucher = "";
      totalMoney = 0;
      totalMinutes = 0;

      noInterrupts();
      pulseCount = 0;
      pulseDetected = false;
      interrupts();
    }
  }
}
