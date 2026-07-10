#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <Arduino_JSON.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ReactESP.h>

using namespace reactesp;

// HTTP sync uses a raw WiFiClient (stable lwIP sockets), NOT the async
// ESPAsyncTCP stack — that stack null-derefs in its error handler on the ESP8266
// when a connection fails and hard-crashes the chip. We block only on connect()
// (bounded by CONNECT_TIMEOUT_MS); sending the request and reading the reply are
// done without blocking the loop (see syncToServer / pollSyncResponse).
#define CONNECT_TIMEOUT_MS 1000   // max blocking time while establishing the TCP connection
#define RESPONSE_TIMEOUT_MS 3000  // give up waiting for the reply after this (non-blocking)

const char* ssid = "laptop";
const char* password = "0987654321";

// Backend split into host/port/path so we can drive a raw socket ourselves.
const char* serverHost = "192.168.137.201";
const uint16_t serverPort = 8000;
const char* serverPath = "/api/esp8266-sync";

#define DHTPIN 2      //D4
#define DHTTYPE DHT22

#define LDR_PIN 14    //D5  — digital light/dark (HIGH = bright, LOW = dark)
#define POT_PIN A0    //     — potentiometer wiper (only ADC on the ESP8266)

#define TOUCH1_PIN 13 //D7  — start focus timer / stop the cycle
#define TOUCH2_PIN 12 //D6  — cycle set mode (focus -> break -> idle)

// Timer configuration
#define MIN_MINUTES 1
#define MAX_MINUTES 10
#define DEFAULT_MINUTES 1

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_SDA 4  //D2
#define OLED_SCL 5  //D1

// Event cadences (milliseconds)
#define TOUCH_POLL_INTERVAL 20
#define UI_INTERVAL 200
#define SENSOR_READ_INTERVAL 2000
#define SYNC_INTERVAL 5000
#define SYNC_POLL_INTERVAL 50
#define WIFI_MONITOR_INTERVAL 500

// A touch must read HIGH for this many consecutive polls (~60 ms) to count.
// Rejects brief glitches — e.g. TTP223 output blips when WiFi transmit spikes
// the supply — that a raw single-sample edge would wrongly accept as a touch.
#define TOUCH_CONFIRM_SAMPLES 3

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
DHT dht(DHTPIN, DHTTYPE);

ReactESP app;  // ReactESP 2.x — v3 dropped ESP8266 support (requires FreeRTOS)

// Reused socket for the backend sync + its non-blocking response state.
WiFiClient syncClient;
enum SyncState { SYNC_IDLE, SYNC_WAITING };  // WAITING = request sent, reply pending
SyncState syncState = SYNC_IDLE;
unsigned long syncSentAt = 0;
String syncStatusLine;  // accumulates the HTTP status line as bytes trickle in

// Tracks WiFi connection state so we can log transitions without blocking.
wl_status_t lastWifiStatus = WL_IDLE_STATUS;

// Latest sensor readings, refreshed by the sensor-read event and consumed
// by the display and sync events.
float humidity = 0.0;
float temperature = 0.0;
int ldrValue = 0;  // 1 = bright, 0 = dark

// --- Study Buddy timer state ----------------------------------------------

enum Mode {
  MODE_IDLE,       // stopped; shows the configured focus time
  MODE_FOCUS,      // focus timer counting down
  MODE_BREAK,      // break timer counting down
  MODE_SET_FOCUS,  // pot sets the focus duration
  MODE_SET_BREAK   // pot sets the break duration
};

Mode mode = MODE_IDLE;

int focusMinutes = DEFAULT_MINUTES;
int breakMinutes = DEFAULT_MINUTES;

unsigned long phaseStartMillis = 0;              // when the running phase began
unsigned long currentTimerSeconds = DEFAULT_MINUTES * 60UL;  // shown + synced value

// Touch confirmation state (TTP223 drives the line HIGH while touched).
int touch1Count = 0;
bool touch1Latched = false;
int touch2Count = 0;
bool touch2Latched = false;

bool touchDetected = false;  // sensor-1 touched since last sync (telemetry)

unsigned long focusDurationMs() { return (unsigned long)focusMinutes * 60UL * 1000UL; }
unsigned long breakDurationMs() { return (unsigned long)breakMinutes * 60UL * 1000UL; }

// Remaining whole seconds in the running phase, clamped so it never underflows.
unsigned long remainingSeconds(unsigned long durationMs) {
  unsigned long elapsed = millis() - phaseStartMillis;
  if (elapsed >= durationMs) return 0;
  return (durationMs - elapsed) / 1000;
}

// Map the potentiometer to a whole number of minutes in [MIN, MAX].
int readPotMinutes() {
  int raw = analogRead(POT_PIN);
  return constrain(map(raw, 0, 1023, MIN_MINUTES, MAX_MINUTES), MIN_MINUTES, MAX_MINUTES);
}

void updateDisplay(const char* label, unsigned long timerSeconds) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(label);

  display.setTextSize(3);
  display.setCursor(10, 24);
  unsigned long mins = timerSeconds / 60;
  unsigned long secs = timerSeconds % 60;
  if (mins < 10) display.print("0");
  display.print(mins);
  display.print(":");
  if (secs < 10) display.print("0");
  display.print(secs);

  display.display();
}

// --- Events ---------------------------------------------------------------

// Confirmed rising-edge: returns true once per press, after the line has been
// HIGH for TOUCH_CONFIRM_SAMPLES consecutive polls. A single-sample glitch
// never reaches the threshold, so it is ignored.
bool touchPressed(uint8_t pin, int& stableCount, bool& latched) {
  if (digitalRead(pin) == HIGH) {
    if (stableCount < TOUCH_CONFIRM_SAMPLES) stableCount++;
    if (stableCount >= TOUCH_CONFIRM_SAMPLES && !latched) {
      latched = true;  // fire once; stays latched until released
      return true;
    }
  } else {
    stableCount = 0;
    latched = false;  // released — arm for the next press
  }
  return false;
}

// Touch 1: start the focus cycle from idle, or stop it back to idle.
void checkTouch1() {
  if (!touchPressed(TOUCH1_PIN, touch1Count, touch1Latched)) return;

  touchDetected = true;
  if (mode == MODE_IDLE) {
    mode = MODE_FOCUS;
    phaseStartMillis = millis();
    Serial.println("Touch 1 — focus timer started");
  } else if (mode == MODE_FOCUS || mode == MODE_BREAK) {
    mode = MODE_IDLE;  // stop the cycle and return to idle
    Serial.println("Touch 1 — cycle stopped, idle");
  }
  // In set mode Touch 1 is ignored; use Touch 2 to finish setting.
}

// Touch 2: cycle through the set-mode screens (focus -> break -> idle).
void checkTouch2() {
  if (!touchPressed(TOUCH2_PIN, touch2Count, touch2Latched)) return;

  if (mode == MODE_SET_FOCUS) {
    mode = MODE_SET_BREAK;
    Serial.println("Touch 2 — setting BREAK timer");
  } else if (mode == MODE_SET_BREAK) {
    mode = MODE_IDLE;
    Serial.print("Touch 2 — saved. Focus=");
    Serial.print(focusMinutes);
    Serial.print("min Break=");
    Serial.print(breakMinutes);
    Serial.println("min");
  } else if (mode == MODE_IDLE) {  // set mode is only reachable from idle
    mode = MODE_SET_FOCUS;
    Serial.println("Touch 2 — setting FOCUS timer");
  } else {  // FOCUS or BREAK running: Touch 2 is locked out, only Touch 1 resets
    Serial.println("Touch 2 ignored — timer running");
  }
}

// Advance the focus/break cycle, apply the pot in set mode, and repaint.
void updateUI() {
  // 1) Advance the running phases; when one ends, the other starts automatically.
  if (mode == MODE_FOCUS && millis() - phaseStartMillis >= focusDurationMs()) {
    mode = MODE_BREAK;
    phaseStartMillis = millis();
    Serial.println("Focus finished — break started");
  } else if (mode == MODE_BREAK && millis() - phaseStartMillis >= breakDurationMs()) {
    mode = MODE_FOCUS;
    phaseStartMillis = millis();
    Serial.println("Break finished — focus started");
  }

  // 2) In set mode the value snaps to the potentiometer position.
  if (mode == MODE_SET_FOCUS) {
    focusMinutes = readPotMinutes();
  } else if (mode == MODE_SET_BREAK) {
    breakMinutes = readPotMinutes();
  }

  // 3) Decide what to show.
  const char* label;
  unsigned long showSeconds;
  switch (mode) {
    case MODE_FOCUS:
      label = "FOCUS";
      showSeconds = remainingSeconds(focusDurationMs());
      break;
    case MODE_BREAK:
      label = "BREAK";
      showSeconds = remainingSeconds(breakDurationMs());
      break;
    case MODE_SET_FOCUS:
      label = "SET FOCUS";
      showSeconds = focusMinutes * 60UL;
      break;
    case MODE_SET_BREAK:
      label = "SET BREAK";
      showSeconds = breakMinutes * 60UL;
      break;
    case MODE_IDLE:
    default:
      label = "READY";
      showSeconds = focusMinutes * 60UL;
      break;
  }

  currentTimerSeconds = showSeconds;
  updateDisplay(label, showSeconds);
}

// Read the DHT22 and the digital LDR. Keeps last good DHT values on a failed read.
void readSensors() {
  // DIAGNOSTIC: watch the heap trend. A steady decline every cycle = a leak
  // (prime suspect: the async HTTP path) and the eventual crash is heap starvation.
  Serial.print("Heap: ");
  Serial.print(ESP.getFreeHeap());

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  ldrValue = digitalRead(LDR_PIN);  // HIGH = bright, LOW = dark

  if (isnan(h) || isnan(t)) {
    Serial.println("\tError: Unable to read data from DHT sensor.");
    return;
  }
  humidity = h;
  temperature = t;

  Serial.print("\t Humidity: ");
  Serial.print(humidity);
  Serial.print(" %\t Temperature: ");
  Serial.print(temperature);
  Serial.print(" °C\t Light: ");
  Serial.println(ldrValue ? "bright" : "dark");
}

// Open the connection and fire off the POST, then hand the reply to
// pollSyncResponse(). Only connect() may block (bounded by CONNECT_TIMEOUT_MS);
// writing the request goes into the socket buffer and returns immediately.
void syncToServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
    return;
  }

  // A reply from the previous sync is still pending — let the poller finish it.
  if (syncState != SYNC_IDLE) {
    Serial.println("Previous sync still awaiting reply — skipping");
    return;
  }

  JSONVar doc;
  doc["humidity"] = humidity;
  doc["temperature"] = temperature;
  doc["timer_value"] = (int)currentTimerSeconds;
  doc["ldr_value"] = ldrValue;
  doc["button_pressed"] = touchDetected;

  String jsonBody = JSON.stringify(doc);

  // Blocking connect — acceptable, and bounded by the timeout.
  syncClient.setTimeout(CONNECT_TIMEOUT_MS);
  if (!syncClient.connect(serverHost, serverPort)) {
    Serial.println("Sync connect failed");
    syncClient.stop();
    return;  // keep touchDetected set so the next cycle retries
  }

  // Send the request without waiting for the response.
  syncClient.print(F("POST "));
  syncClient.print(serverPath);
  syncClient.print(F(" HTTP/1.1\r\nHost: "));
  syncClient.print(serverHost);
  syncClient.print(':');
  syncClient.print(serverPort);
  syncClient.print(F("\r\nContent-Type: application/json\r\nContent-Length: "));
  syncClient.print(jsonBody.length());
  syncClient.print(F("\r\nConnection: close\r\n\r\n"));
  syncClient.print(jsonBody);

  syncState = SYNC_WAITING;
  syncSentAt = millis();
  syncStatusLine = "";
  touchDetected = false;  // request is on its way
}

// Non-blocking reply handler: reads only bytes already available, so it never
// waits on the network. Grabs the HTTP status line, then closes the socket.
void pollSyncResponse() {
  if (syncState != SYNC_WAITING) return;

  while (syncClient.available()) {
    char c = syncClient.read();
    if (c == '\n') {  // end of the status line ("HTTP/1.1 200 OK")
      Serial.print("HTTP status: ");
      Serial.println(syncStatusLine);
      syncClient.stop();
      syncState = SYNC_IDLE;
      return;
    }
    if (c != '\r') syncStatusLine += c;
  }

  // No (complete) reply in time, or the peer dropped: close and move on.
  if (!syncClient.connected() || millis() - syncSentAt > RESPONSE_TIMEOUT_MS) {
    Serial.println("Sync reply timed out / connection closed");
    syncClient.stop();
    syncState = SYNC_IDLE;
  }
}

// Log WiFi connect/disconnect transitions without blocking startup.
void monitorWiFi() {
  wl_status_t status = WiFi.status();
  if (status == lastWifiStatus) return;

  if (status == WL_CONNECTED) {
    Serial.print("Connected to WiFi network with IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected...");
  }
  lastWifiStatus = status;
}

// --------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // DIAGNOSTIC: why did we just (re)boot? "Exception"/"Software Watchdog" point
  // at a code fault; "External System"/"Power on" is a normal/manual reset.
  Serial.print("\n\nReset reason: ");
  Serial.println(ESP.getResetReason());

  dht.begin();

  pinMode(TOUCH1_PIN, INPUT);  // TTP223 actively drives the line
  pinMode(TOUCH2_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);     // divider node drives the pin; no pull-up

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
  } else {
    display.clearDisplay();
    display.display();
  }

  // Kick off the WiFi connection but don't block on it; monitorWiFi() reports
  // the outcome and the SDK keeps retrying in the background.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi in background...");

  // Register the event-loop tasks.
  app.onRepeat(TOUCH_POLL_INTERVAL, checkTouch1);
  app.onRepeat(TOUCH_POLL_INTERVAL, checkTouch2);
  app.onRepeat(UI_INTERVAL, updateUI);
  app.onRepeat(SENSOR_READ_INTERVAL, readSensors);
  app.onRepeat(SYNC_INTERVAL, syncToServer);
  app.onRepeat(SYNC_POLL_INTERVAL, pollSyncResponse);
  app.onRepeat(WIFI_MONITOR_INTERVAL, monitorWiFi);
}

void loop() {
  app.tick();
}
