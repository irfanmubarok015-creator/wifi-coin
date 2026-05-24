#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <SPIFFS.h>
#include <base64.h>

#define CURRENT_VERSION "ESP32-WIFI-NO-LCD-1.0"

// =========================
// BASIC CONFIGURATION
// =========================

// WiFi that ESP32 will connect to. Usually your MikroTik WiFi/Hotspot backend WiFi.
String WIFI_SSID = "MikroTik-SSID";
String WIFI_PASSWORD = "";

// MikroTik router IP and login.
IPAddress MIKROTIK_IP(192, 168, 88, 1);
String MIKROTIK_USER = "admin";
String MIKROTIK_PASS = "";

// Admin web login for ESP32 dashboard.
String ADMIN_USER = "admin";
String ADMIN_PASS = "admin123";

// Vendor/brand name used in comments.
String VENDOR_NAME = "PisoWiFi";

// Voucher configuration.
String VOUCHER_PREFIX = "P";
String VOUCHER_PROFILE = "default";     // MikroTik hotspot user profile. Use "default" or your profile name.
bool VOUCHER_USE_PASSWORD = true;        // If true, voucher password = voucher code.

// Coin wait time after session started.
unsigned long MAX_WAIT_COIN_MS = 30000;  // 30 seconds

// Optional internet checking. Keep false for simpler/safer demo.
bool CHECK_INTERNET_CONNECTION = false;

// LED trigger type.
// true  = HIGH means ON
// false = LOW means ON, for active-low LED modules
bool LED_ACTIVE_HIGH = true;

// =========================
// PIN CONFIGURATION
// =========================

// Recommended safe pins for ESP32.
const int COIN_SIGNAL_PIN = 27;      // Coin pulse input through optocoupler
const int COIN_ENABLE_PIN = 26;      // Relay/MOSFET to enable coin acceptor
const int START_BUTTON_PIN = 25;     // Button to start manual coin purchase
const int LED_READY_PIN = 14;        // System ready / WiFi + MikroTik OK
const int LED_PROCESS_PIN = 12;      // Coin session/status LED

// =========================
// EEPROM ADDRESSES
// =========================

const int EEPROM_SIZE = 512;
const int LIFETIME_COIN_COUNT_ADDRESS = 0;
const int COIN_COUNT_ADDRESS = 8;
const int CUSTOMER_COUNT_ADDRESS = 16;

// =========================
// PROMO RATES
// =========================

struct PromoRate {
  String rateName;
  int price;          // coin amount
  int minutes;        // uptime minutes added
  int validity;       // comment validity minutes
  int dataLimitMB;    // 0 = unlimited
  String profileName;
};

// Edit this according to your coin/rate plan.
// Example: 1 coin = 10 minutes, 5 coins = 60 minutes, 10 coins = 150 minutes.
PromoRate rates[] = {
  {"1 Coin", 1, 10, 10, 0, ""},
  {"5 Coins", 5, 60, 60, 0, ""},
  {"10 Coins", 10, 150, 150, 0, ""}
};
const int ratesCount = sizeof(rates) / sizeof(rates[0]);

// =========================
// GLOBAL STATE
// =========================

WebServer server(80);
WiFiClient telnetClient;

volatile int coinPulseCount = 0;
volatile bool coinChanged = false;

bool networkConnected = false;
bool mikrotikConnected = false;
bool coinSlotActive = false;
bool acceptCoin = false;
bool manualVoucher = false;
bool isNewVoucher = false;
bool coinExpired = false;

int processCoin = 0;
int totalCoin = 0;
int timeToAddSeconds = 0;
int currentValidity = 0;
int currentDataLimit = 0;
String currentRateProfile = "";
String currentActiveVoucher = "";
String currentMacAddress = "";
String currentIpAddress = "";

unsigned long targetMillis = 0;
unsigned long lastBlinkMillis = 0;
bool blinkState = false;

String adminAuth = "";

// =========================
// INTERRUPT
// =========================

void IRAM_ATTR coinInserted() {
  if (coinSlotActive) {
    coinPulseCount++;
    coinChanged = true;
  }
}

// =========================
// HELPER: EEPROM
// =========================

void eeWriteLong(int pos, long val) {
  EEPROM.put(pos, val);
  EEPROM.commit();
}

long eeReadLong(int pos) {
  long val = 0;
  EEPROM.get(pos, val);
  if (val < 0) return 0;
  return val;
}

// =========================
// HELPER: LED
// =========================

int ledOnValue() {
  return LED_ACTIVE_HIGH ? HIGH : LOW;
}

int ledOffValue() {
  return LED_ACTIVE_HIGH ? LOW : HIGH;
}

void ledOn(int pin) {
  digitalWrite(pin, ledOnValue());
}

void ledOff(int pin) {
  digitalWrite(pin, ledOffValue());
}

void blinkLed(int pin, int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    ledOn(pin);
    delay(delayMs);
    ledOff(pin);
    delay(delayMs);
  }
}

void processLedHeartbeat() {
  if (millis() - lastBlinkMillis >= 500) {
    lastBlinkMillis = millis();
    blinkState = !blinkState;
    digitalWrite(LED_PROCESS_PIN, blinkState ? ledOnValue() : ledOffValue());
  }
}

// =========================
// HELPER: JSON
// =========================

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  return value;
}

void sendJson(String json) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.send(200, "application/json", json);
}

void sendPlain(String text) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", text);
}

// =========================
// AUTH
// =========================

bool isAuthorized() {
  String auth = server.header("Authorization");
  String expected = "Basic " + adminAuth;
  return auth == expected;
}

void handleNotAuthorized() {
  server.sendHeader("WWW-Authenticate", "Basic realm=\"ESP32 PisoWiFi Admin\"");
  server.send(401, "text/html", "<h3>Authentication required</h3>");
}

// =========================
// MIKROTIK TELNET
// =========================

bool waitForTelnetText(const String &needle, unsigned long timeoutMs) {
  unsigned long start = millis();
  String buffer = "";

  while (millis() - start < timeoutMs) {
    while (telnetClient.available()) {
      char c = telnetClient.read();
      buffer += c;
      if (buffer.indexOf(needle) >= 0) {
        return true;
      }
      if (buffer.length() > 300) {
        buffer.remove(0, 150);
      }
    }
    delay(10);
  }
  return false;
}

void telnetSendLine(const String &line) {
  telnetClient.print(line);
  telnetClient.print("\r\n");
}

bool connectMikroTikTelnet() {
  if (telnetClient.connected()) {
    telnetClient.stop();
    delay(200);
  }

  Serial.print("Connecting Telnet to MikroTik: ");
  Serial.println(MIKROTIK_IP);

  if (!telnetClient.connect(MIKROTIK_IP, 23)) {
    Serial.println("Telnet connection failed");
    return false;
  }

  // RouterOS Telnet usually asks Login: and Password:
  bool gotLogin = waitForTelnetText("Login", 5000) || waitForTelnetText("login", 2000);
  if (!gotLogin) {
    Serial.println("Login prompt not detected, trying to continue anyway");
  }

  telnetSendLine(MIKROTIK_USER);

  bool gotPassword = waitForTelnetText("Password", 5000) || waitForTelnetText("password", 2000);
  if (!gotPassword) {
    Serial.println("Password prompt not detected, trying to continue anyway");
  }

  telnetSendLine(MIKROTIK_PASS);

  bool gotPrompt = waitForTelnetText(">", 7000);
  if (gotPrompt) {
    Serial.println("MikroTik Telnet login success");
    return true;
  }

  Serial.println("MikroTik Telnet prompt not detected. Login may have failed.");
  return false;
}

bool sendMikroTikCommand(String command) {
  if (!telnetClient.connected()) {
    if (!connectMikroTikTelnet()) {
      mikrotikConnected = false;
      ledOff(LED_READY_PIN);
      return false;
    }
  }

  Serial.print("MikroTik command: ");
  Serial.println(command);

  telnetSendLine(command);
  waitForTelnetText(">", 3000);
  return true;
}

// =========================
// WIFI
// =========================

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());

  Serial.print("Connecting WiFi to ");
  Serial.println(WIFI_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    blinkLed(LED_READY_PIN, 1, 100);
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    currentIpAddress = WiFi.localIP().toString();
    currentMacAddress = WiFi.macAddress();
    Serial.print("WiFi connected. IP: ");
    Serial.println(currentIpAddress);
    Serial.print("MAC: ");
    Serial.println(currentMacAddress);
    return true;
  }

  Serial.println("WiFi failed");
  return false;
}

// =========================
// COIN / VOUCHER LOGIC
// =========================

String generateVoucher() {
  randomSeed(esp_random());
  int randomNumber = random(1000, 9999);
  return VOUCHER_PREFIX + String(randomNumber);
}

void activateCoinSlot() {
  processCoin = 0;
  acceptCoin = true;
  coinSlotActive = true;
  coinExpired = false;
  targetMillis = millis() + MAX_WAIT_COIN_MS;

  digitalWrite(COIN_ENABLE_PIN, HIGH);
  ledOn(LED_PROCESS_PIN);

  Serial.println("Coin slot activated");
}

void disableCoinSlot() {
  coinSlotActive = false;
  acceptCoin = false;

  digitalWrite(COIN_ENABLE_PIN, LOW);
  ledOff(LED_PROCESS_PIN);

  Serial.println("Coin slot disabled");
}

void resetTransaction() {
  currentActiveVoucher = "";
  processCoin = 0;
  totalCoin = 0;
  timeToAddSeconds = 0;
  currentValidity = 0;
  currentDataLimit = 0;
  currentRateProfile = "";
  manualVoucher = false;
  isNewVoucher = false;
}

int calculateAddTime() {
  int totalTimeMinutes = 0;
  currentValidity = 0;
  currentDataLimit = 0;
  currentRateProfile = "";

  int remainingCoin = totalCoin;
  int highestPrice = 0;

  while (remainingCoin > 0) {
    int candidatePrice = 0;
    int candidateIndex = -1;

    for (int i = 0; i < ratesCount; i++) {
      if (rates[i].price <= remainingCoin && rates[i].price > candidatePrice) {
        candidatePrice = rates[i].price;
        candidateIndex = i;
      }
    }

    if (candidateIndex < 0) break;

    currentValidity += rates[candidateIndex].validity;
    currentDataLimit += rates[candidateIndex].dataLimitMB;
    totalTimeMinutes += rates[candidateIndex].minutes;

    if (rates[candidateIndex].price > highestPrice) {
      highestPrice = rates[candidateIndex].price;
      currentRateProfile = rates[candidateIndex].profileName;
    }

    remainingCoin -= rates[candidateIndex].price;
  }

  return totalTimeMinutes * 60;
}

void updateStatistic() {
  long lifetime = eeReadLong(LIFETIME_COIN_COUNT_ADDRESS);
  long coinCount = eeReadLong(COIN_COUNT_ADDRESS);
  long customerCount = eeReadLong(CUSTOMER_COUNT_ADDRESS);

  lifetime += totalCoin;
  coinCount += totalCoin;
  customerCount++;

  eeWriteLong(LIFETIME_COIN_COUNT_ADDRESS, lifetime);
  eeWriteLong(COIN_COUNT_ADDRESS, coinCount);
  eeWriteLong(CUSTOMER_COUNT_ADDRESS, customerCount);
}

bool registerNewVoucher(String voucher) {
  String command = "/ip hotspot user add name=\"" + voucher + "\" limit-uptime=0 comment=0";

  if (VOUCHER_USE_PASSWORD) {
    command += " password=\"" + voucher + "\"";
  }

  if (VOUCHER_PROFILE != "" && VOUCHER_PROFILE != "default") {
    command += " profile=\"" + VOUCHER_PROFILE + "\"";
  }

  return sendMikroTikCommand(command);
}

bool addTimeToVoucher(String voucher, int secondsToAdd) {
  int minutesToAdd = secondsToAdd / 60;

  String command = ":global lpt; :global nlu; ";
  command += ":set lpt [/ip hotspot user get \"" + voucher + "\" limit-uptime]; ";
  command += ":set nlu [($lpt+" + String(minutesToAdd) + "m)]; ";
  command += "/ip hotspot user set limit-uptime=$nlu comment=\"";
  command += String(currentValidity) + "m," + String(totalCoin) + "," + VENDOR_NAME + "\" ";

  if (currentRateProfile != "") {
    command += "profile=\"" + currentRateProfile + "\" ";
  }

  command += "\"" + voucher + "\"";

  bool ok = sendMikroTikCommand(command);

  if (ok && currentDataLimit > 0) {
    String dataCommand = ":global dtl; :global tdtl; ";
    dataCommand += ":set dtl [/ip hotspot user get \"" + voucher + "\" limit-bytes-total]; ";
    dataCommand += ":if ($dtl>0) do={ :set tdtl ($dtl+" + String(currentDataLimit) + "*1048576) } else={ :set tdtl (" + String(currentDataLimit) + "*1048576) }; ";
    dataCommand += "/ip hotspot user set limit-bytes-total=$tdtl \"" + voucher + "\"";
    ok = sendMikroTikCommand(dataCommand);
  }

  return ok;
}

bool finalizeTransaction() {
  disableCoinSlot();

  if (totalCoin <= 0) {
    Serial.println("No coin inserted, transaction cancelled");
    resetTransaction();
    return false;
  }

  timeToAddSeconds = calculateAddTime();
  if (timeToAddSeconds <= 0) {
    Serial.println("Coin amount does not match any rate");
    resetTransaction();
    return false;
  }

  if (currentActiveVoucher == "") {
    currentActiveVoucher = generateVoucher();
    isNewVoucher = true;
  }

  bool ok = true;
  if (isNewVoucher) {
    ok = registerNewVoucher(currentActiveVoucher);
  }

  if (ok) {
    ok = addTimeToVoucher(currentActiveVoucher, timeToAddSeconds);
  }

  if (ok) {
    updateStatistic();
    Serial.println("Transaction success");
    blinkLed(LED_PROCESS_PIN, 3, 150);
    ledOn(LED_READY_PIN);
  } else {
    Serial.println("Transaction failed when sending MikroTik command");
    blinkLed(LED_PROCESS_PIN, 8, 100);
  }

  return ok;
}

void startManualVoucherSession() {
  if (!networkConnected || WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot start session: WiFi not connected");
    blinkLed(LED_READY_PIN, 5, 150);
    return;
  }

  if (!mikrotikConnected) {
    mikrotikConnected = connectMikroTikTelnet();
    if (!mikrotikConnected) {
      blinkLed(LED_READY_PIN, 8, 100);
      return;
    }
  }

  resetTransaction();
  manualVoucher = true;
  isNewVoucher = true;
  currentActiveVoucher = generateVoucher();
  activateCoinSlot();

  Serial.print("Manual voucher session started. Voucher: ");
  Serial.println(currentActiveVoucher);
}

// =========================
// WEB HANDLERS
// =========================

void handleRoot() {
  String html = "";
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32 PisoWiFi</title>";
  html += "<style>body{font-family:Arial;margin:24px;background:#f5f5f5} .card{background:#fff;padding:18px;border-radius:12px;box-shadow:0 2px 10px #0001;margin-bottom:14px} button{padding:12px 16px;border:0;border-radius:10px;background:#111;color:white;font-weight:bold} code{background:#eee;padding:2px 5px;border-radius:4px}</style>";
  html += "</head><body>";
  html += "<div class='card'><h2>ESP32 PisoWiFi</h2><p>Status perangkat dan transaksi koin.</p>";
  html += "<p><b>IP:</b> " + currentIpAddress + "</p>";
  html += "<p><b>MAC:</b> " + currentMacAddress + "</p>";
  html += "<p><b>Voucher aktif:</b> <code>" + currentActiveVoucher + "</code></p>";
  html += "<p><b>Total coin:</b> " + String(totalCoin) + "</p>";
  html += "<p><b>Coin slot:</b> " + String(coinSlotActive ? "AKTIF" : "MATI") + "</p>";
  html += "<p><a href='/api/status'>Lihat JSON Status</a></p>";
  html += "</div>";
  html += "<div class='card'><button onclick=\"fetch('/api/start').then(r=>r.text()).then(alert)\">Mulai Insert Coin</button> ";
  html += "<button onclick=\"fetch('/api/finalize').then(r=>r.text()).then(alert)\">Selesaikan Transaksi</button></div>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleStatus() {
  long lifetime = eeReadLong(LIFETIME_COIN_COUNT_ADDRESS);
  long coinCount = eeReadLong(COIN_COUNT_ADDRESS);
  long customerCount = eeReadLong(CUSTOMER_COUNT_ADDRESS);

  String json = "{";
  json += "\"status\":true,";
  json += "\"version\":\"" + String(CURRENT_VERSION) + "\",";
  json += "\"wifi\":\"" + String(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") + "\",";
  json += "\"mikrotik\":\"" + String(mikrotikConnected ? "connected" : "disconnected") + "\",";
  json += "\"ip\":\"" + jsonEscape(currentIpAddress) + "\",";
  json += "\"mac\":\"" + jsonEscape(currentMacAddress) + "\",";
  json += "\"coinSlotActive\":" + String(coinSlotActive ? "true" : "false") + ",";
  json += "\"voucher\":\"" + jsonEscape(currentActiveVoucher) + "\",";
  json += "\"totalCoin\":" + String(totalCoin) + ",";
  json += "\"timeToAddSeconds\":" + String(timeToAddSeconds) + ",";
  json += "\"lifetimeCoin\":" + String(lifetime) + ",";
  json += "\"coinCount\":" + String(coinCount) + ",";
  json += "\"customerCount\":" + String(customerCount);
  json += "}";

  sendJson(json);
}

void handleStart() {
  startManualVoucherSession();
  sendPlain("Coin session started. Voucher: " + currentActiveVoucher);
}

void handleFinalize() {
  bool ok = finalizeTransaction();
  String voucher = currentActiveVoucher;
  int paid = totalCoin;
  int seconds = timeToAddSeconds;

  resetTransaction();

  if (ok) {
    sendPlain("Success. Voucher: " + voucher + " | Coin: " + String(paid) + " | Time: " + String(seconds / 60) + " minutes");
  } else {
    sendPlain("Failed or cancelled. Please check Serial Monitor and MikroTik connection.");
  }
}

void handleTopUp() {
  // Optional endpoint, compatible with web/mobile portal style.
  // /topUp?voucher=ABC123
  String voucher = server.arg("voucher");

  if (!networkConnected || WiFi.status() != WL_CONNECTED) {
    sendJson("{\"status\":false,\"errorCode\":\"wifi.not.connected\"}");
    return;
  }

  if (!mikrotikConnected) {
    mikrotikConnected = connectMikroTikTelnet();
    if (!mikrotikConnected) {
      sendJson("{\"status\":false,\"errorCode\":\"mikrotik.not.connected\"}");
      return;
    }
  }

  if (currentActiveVoucher != "" && voucher != currentActiveVoucher) {
    sendJson("{\"status\":false,\"errorCode\":\"coinslot.busy\"}");
    return;
  }

  resetTransaction();

  if (voucher == "") {
    currentActiveVoucher = generateVoucher();
    isNewVoucher = true;
  } else {
    currentActiveVoucher = voucher;
    isNewVoucher = false;
  }

  activateCoinSlot();

  sendJson("{\"status\":true,\"voucher\":\"" + jsonEscape(currentActiveVoucher) + "\"}");
}

void handleCheckCoin() {
  if (currentActiveVoucher == "") {
    sendJson("{\"status\":false,\"errorCode\":\"no.active.voucher\"}");
    return;
  }

  timeToAddSeconds = calculateAddTime();
  long remain = targetMillis > millis() ? targetMillis - millis() : 0;

  String json = "{";
  json += "\"status\":true,";
  json += "\"voucher\":\"" + jsonEscape(currentActiveVoucher) + "\",";
  json += "\"totalCoin\":" + String(totalCoin) + ",";
  json += "\"timeAdded\":" + String(timeToAddSeconds) + ",";
  json += "\"remainTime\":" + String(remain) + ",";
  json += "\"coinSlotActive\":" + String(coinSlotActive ? "true" : "false");
  json += "}";

  sendJson(json);
}

void handleUseVoucher() {
  if (currentActiveVoucher == "") {
    sendJson("{\"status\":false,\"errorCode\":\"no.active.voucher\"}");
    return;
  }

  bool ok = finalizeTransaction();
  String voucher = currentActiveVoucher;
  int paid = totalCoin;
  int seconds = timeToAddSeconds;

  resetTransaction();

  if (ok) {
    String json = "{";
    json += "\"status\":true,";
    json += "\"voucher\":\"" + jsonEscape(voucher) + "\",";
    json += "\"totalCoin\":" + String(paid) + ",";
    json += "\"timeAdded\":" + String(seconds);
    json += "}";
    sendJson(json);
  } else {
    sendJson("{\"status\":false,\"errorCode\":\"transaction.failed\"}");
  }
}

void handleAdminDashboard() {
  if (!isAuthorized()) {
    handleNotAuthorized();
    return;
  }
  handleStatus();
}

void handleResetStats() {
  if (!isAuthorized()) {
    handleNotAuthorized();
    return;
  }

  eeWriteLong(LIFETIME_COIN_COUNT_ADDRESS, 0);
  eeWriteLong(COIN_COUNT_ADDRESS, 0);
  eeWriteLong(CUSTOMER_COUNT_ADDRESS, 0);
  sendPlain("Statistics reset");
}

void handleTestInsertCoin() {
  if (!isAuthorized()) {
    handleNotAuthorized();
    return;
  }

  int coin = server.arg("coin").toInt();
  if (coin <= 0) coin = 1;

  if (coinSlotActive) {
    totalCoin += coin;
    timeToAddSeconds = calculateAddTime();
    blinkLed(LED_PROCESS_PIN, 1, 80);
    sendPlain("Added test coin: " + String(coin));
  } else {
    sendPlain("Coin slot is not active");
  }
}

void handleRates() {
  String json = "[";
  for (int i = 0; i < ratesCount; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"name\":\"" + jsonEscape(rates[i].rateName) + "\",";
    json += "\"price\":" + String(rates[i].price) + ",";
    json += "\"minutes\":" + String(rates[i].minutes) + ",";
    json += "\"validity\":" + String(rates[i].validity) + ",";
    json += "\"dataLimitMB\":" + String(rates[i].dataLimitMB);
    json += "}";
  }
  json += "]";
  sendJson(json);
}

void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.send(204);
}

void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/start", HTTP_GET, handleStart);
  server.on("/api/finalize", HTTP_GET, handleFinalize);
  server.on("/api/rates", HTTP_GET, handleRates);

  // Compatibility endpoints.
  server.on("/topUp", HTTP_GET, handleTopUp);
  server.on("/checkCoin", HTTP_GET, handleCheckCoin);
  server.on("/useVoucher", HTTP_GET, handleUseVoucher);

  // Admin endpoints.
  server.on("/admin/api/dashboard", HTTP_GET, handleAdminDashboard);
  server.on("/admin/api/resetStatistic", HTTP_GET, handleResetStats);
  server.on("/testInsertCoin", HTTP_GET, handleTestInsertCoin);

  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      handleOptions();
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });
}

// =========================
// SETUP & LOOP
// =========================

void setup() {
  Serial.begin(115200);
  delay(1000);

  EEPROM.begin(EEPROM_SIZE);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed, continuing without filesystem");
  }

  pinMode(COIN_SIGNAL_PIN, INPUT_PULLUP);
  pinMode(COIN_ENABLE_PIN, OUTPUT);
  pinMode(START_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_READY_PIN, OUTPUT);
  pinMode(LED_PROCESS_PIN, OUTPUT);

  digitalWrite(COIN_ENABLE_PIN, LOW);
  ledOff(LED_READY_PIN);
  ledOff(LED_PROCESS_PIN);

  adminAuth = base64::encode(ADMIN_USER + ":" + ADMIN_PASS);

  attachInterrupt(digitalPinToInterrupt(COIN_SIGNAL_PIN), coinInserted, RISING);

  networkConnected = connectWiFi();
  if (networkConnected) {
    mikrotikConnected = connectMikroTikTelnet();
    if (mikrotikConnected) {
      ledOn(LED_READY_PIN);
    } else {
      blinkLed(LED_READY_PIN, 6, 100);
    }
  }

  setupRoutes();
  server.begin();

  Serial.println("ESP32 PisoWiFi No LCD server started");
  Serial.print("Open: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    networkConnected = false;
    ledOff(LED_READY_PIN);
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 10000) {
      lastReconnect = millis();
      networkConnected = connectWiFi();
      if (networkConnected) {
        mikrotikConnected = connectMikroTikTelnet();
        if (mikrotikConnected) ledOn(LED_READY_PIN);
      }
    }
  }

  // Button starts manual voucher session.
  static bool lastButtonState = HIGH;
  bool buttonState = digitalRead(START_BUTTON_PIN);
  if (lastButtonState == HIGH && buttonState == LOW) {
    delay(50); // debounce
    if (digitalRead(START_BUTTON_PIN) == LOW) {
      if (!coinSlotActive) {
        startManualVoucherSession();
      } else {
        // If pressed while active, finalize immediately.
        bool ok = finalizeTransaction();
        Serial.println(ok ? "Manual finalized" : "Manual cancelled/failed");
        resetTransaction();
      }
    }
  }
  lastButtonState = buttonState;

  // Handle incoming coin pulse.
  if (coinChanged) {
    noInterrupts();
    int pulses = coinPulseCount;
    coinPulseCount = 0;
    coinChanged = false;
    interrupts();

    if (pulses > 0 && coinSlotActive) {
      processCoin = pulses;
      totalCoin += processCoin;
      timeToAddSeconds = calculateAddTime();

      Serial.print("Coin pulses: ");
      Serial.print(pulses);
      Serial.print(" | Total coin: ");
      Serial.print(totalCoin);
      Serial.print(" | Time: ");
      Serial.print(timeToAddSeconds / 60);
      Serial.println(" minutes");

      blinkLed(LED_PROCESS_PIN, 1, 80);
      ledOn(LED_PROCESS_PIN);

      // Extend wait time after every coin insert.
      targetMillis = millis() + MAX_WAIT_COIN_MS;
    }
  }

  // Auto finalize when wait time expired.
  if (coinSlotActive && acceptCoin && millis() > targetMillis) {
    Serial.println("Coin wait expired, auto finalizing transaction");
    bool ok = finalizeTransaction();
    Serial.println(ok ? "Auto finalize success" : "Auto finalize cancelled/failed");
    String lastVoucher = currentActiveVoucher;
    resetTransaction();
  }

  if (coinSlotActive) {
    processLedHeartbeat();
  }
}
