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
#define LDRPIN A0

#define TOUCH_PIN 13 //D7

#define TIMER_PRESET (1UL * 60UL * 1000UL)  // 5 minutes in milliseconds

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_SDA 4  //D2
#define OLED_SCL 5  //D1

// Event cadences (milliseconds)
#define TOUCH_POLL_INTERVAL 20
#define SENSOR_READ_INTERVAL 2000
#define DISPLAY_INTERVAL 1000
#define SYNC_INTERVAL 5000

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
int ldrValue = 0;

// Timer state
bool timerRunning = false;
unsigned long timerStartMillis = 0;
bool touchDetected = false;
int lastTouchState = LOW;

// Remaining timer value (seconds), maintained by the display event and
// reported by the sync event.
unsigned long currentTimerSeconds = TIMER_PRESET / 1000;

void updateDisplay(unsigned long timerSeconds, bool running) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(running ? "TIMER RUNNING" : "TIMER IDLE");

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

// Watch the capacitive touch pad and start the timer on a fresh touch.
void checkTouch() {
  int touchState = digitalRead(TOUCH_PIN);
  if (touchState == HIGH && lastTouchState == LOW) {  // rising edge = new touch
    timerRunning = true;
    timerStartMillis = millis();
    touchDetected = true;
    Serial.println("Touch detected — timer started");
  }
  lastTouchState = touchState;
}

// Read the DHT22 and LDR. Keeps the last good values if the DHT read fails.
void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  ldrValue = analogRead(LDRPIN);

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
  Serial.print(" °C\t LDR: ");
  Serial.println(ldrValue);
}

// Advance the countdown and repaint the OLED once per second.
void refreshDisplay() {
  unsigned long remaining = TIMER_PRESET;
  if (timerRunning) {
    unsigned long elapsed = millis() - timerStartMillis;
    if (elapsed >= TIMER_PRESET) {
      remaining = 0;
      timerRunning = false;
      Serial.println("Timer finished");
    } else {
      remaining = TIMER_PRESET - elapsed;
    }
  }
  currentTimerSeconds = remaining / 1000;
  updateDisplay(currentTimerSeconds, timerRunning);
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

  pinMode(TOUCH_PIN, INPUT);  // TTP223 actively drives the line

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
  app.onRepeat(TOUCH_POLL_INTERVAL, checkTouch);
  app.onRepeat(SENSOR_READ_INTERVAL, readSensors);
  app.onRepeat(DISPLAY_INTERVAL, refreshDisplay);
  app.onRepeat(SYNC_INTERVAL, syncToServer);
  app.onRepeat(500, monitorWiFi);
}

void loop() {
  app.tick();
}
