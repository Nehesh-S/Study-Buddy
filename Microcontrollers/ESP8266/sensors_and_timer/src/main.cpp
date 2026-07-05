#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <Arduino_JSON.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ReactESP.h>
#include <AsyncHTTPRequest_Generic.h>  // pulls in ESPAsyncTCP on ESP8266

using namespace reactesp;

const char* ssid = "laptop";
const char* password = "0987654321";

const char* serverName = "http://192.168.137.201:8000/api/esp8266-sync";

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
#define WIFI_MONITOR_INTERVAL 500

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
DHT dht(DHTPIN, DHTTYPE);

ReactESP app;  // ReactESP 2.x — v3 dropped ESP8266 support (requires FreeRTOS)
AsyncHTTPRequest request;

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

// Touch edge-detection (TTP223 drives the line HIGH while touched).
int lastTouch1State = LOW;
int lastTouch2State = LOW;

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

// Touch 1: start the focus cycle from idle, or stop it back to idle.
void checkTouch1() {
  int s = digitalRead(TOUCH1_PIN);
  if (s == HIGH && lastTouch1State == LOW) {  // rising edge = new touch
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
  lastTouch1State = s;
}

// Touch 2: cycle through the set-mode screens (focus -> break -> idle).
void checkTouch2() {
  int s = digitalRead(TOUCH2_PIN);
  if (s == HIGH && lastTouch2State == LOW) {  // rising edge = new touch
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
    } else {  // from idle or a running phase, enter set mode
      mode = MODE_SET_FOCUS;
      Serial.println("Touch 2 — setting FOCUS timer");
    }
  }
  lastTouch2State = s;
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
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  ldrValue = digitalRead(LDR_PIN);  // HIGH = bright, LOW = dark

  if (isnan(h) || isnan(t)) {
    Serial.println("Error: Unable to read data from DHT sensor.");
    return;
  }
  humidity = h;
  temperature = t;

  Serial.print("Humidity: ");
  Serial.print(humidity);
  Serial.print(" %\t Temperature: ");
  Serial.print(temperature);
  Serial.print(" °C\t Light: ");
  Serial.println(ldrValue ? "bright" : "dark");
}

// Fires as the async request progresses; readyState 4 means the exchange is done.
void onRequestComplete(void* optParm, AsyncHTTPRequest* req, int readyState) {
  if (readyState != 4) return;  // only care about the completed state
  Serial.print("HTTP Response code: ");
  Serial.println(req->responseHTTPcode());
}

// Push the latest readings to the backend. Non-blocking: send() dispatches the
// request and returns immediately; the result arrives in onRequestComplete().
void syncToServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
    return;
  }

  // readyState 0 = idle, 4 = done. Anything else means a request is still in
  // flight, so skip this cycle rather than stomp on it.
  if (request.readyState() != 0 && request.readyState() != 4) {
    Serial.println("Previous request still in flight — skipping sync");
    return;
  }

  JSONVar doc;
  doc["humidity"] = humidity;
  doc["temperature"] = temperature;
  doc["timer_value"] = (int)currentTimerSeconds;
  doc["ldr_value"] = ldrValue;
  doc["button_pressed"] = touchDetected;

  String jsonBody = JSON.stringify(doc);

  if (request.open("POST", serverName)) {
    request.setReqHeader("Content-Type", "application/json");
    request.send(jsonBody);
    touchDetected = false;  // reset once the payload is dispatched
  } else {
    Serial.println("Failed to open async request");
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

  // Wire up the async HTTP request once; the callback handles every response.
  request.setDebug(false);
  request.onReadyStateChange(onRequestComplete);

  // Register the asynchronous events.
  app.onRepeat(TOUCH_POLL_INTERVAL, checkTouch1);
  app.onRepeat(TOUCH_POLL_INTERVAL, checkTouch2);
  app.onRepeat(UI_INTERVAL, updateUI);
  app.onRepeat(SENSOR_READ_INTERVAL, readSensors);
  app.onRepeat(SYNC_INTERVAL, syncToServer);
  app.onRepeat(WIFI_MONITOR_INTERVAL, monitorWiFi);
}

void loop() {
  app.tick();
}
