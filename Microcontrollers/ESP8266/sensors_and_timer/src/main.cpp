#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <Arduino_JSON.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

unsigned long lastTime = 0;
unsigned long timerDelay = 5000;

bool timerRunning = false;
unsigned long timerStartMillis = 0;
bool touchDetected = false;

int lastTouchState = LOW;

DHT dht(DHTPIN, DHTTYPE);

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

void setup() {
  Serial.begin(115200);
  dht.begin();

  pinMode(TOUCH_PIN, INPUT);  // TTP223 actively drives the line, no pull-up needed

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
  } else {
    display.clearDisplay();
    display.display();
  }

  WiFi.begin(ssid, password);
  Serial.println("Connecting");
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to WiFi network with IP Address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // Capacitive touch read (TTP223 outputs HIGH while touched)
  int touchState = digitalRead(TOUCH_PIN);
  if (touchState == HIGH && lastTouchState == LOW) {  // rising edge = new touch
    timerRunning = true;
    timerStartMillis = millis();
    touchDetected = true;
    Serial.println("Touch detected — timer started");
  }
  lastTouchState = touchState;

  // Calculate remaining timer seconds
  unsigned long timerRemaining = TIMER_PRESET;
  if (timerRunning) {
    unsigned long elapsed = millis() - timerStartMillis;
    if (elapsed >= TIMER_PRESET) {
      timerRemaining = 0;
      timerRunning = false;
      Serial.println("Timer finished");
    } else {
      timerRemaining = TIMER_PRESET - elapsed;
    }
  }
  unsigned long timerSeconds = timerRemaining / 1000;

  updateDisplay(timerSeconds, timerRunning);

  delay(2000);  // DHT22 polling rate

  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();
  int ldr = analogRead(LDRPIN);
  // float Vout = float(ldr) * (3.3 / 1023.0);
  // float RLDR = (10000.0 * (3.3 - Vout)) / Vout;
  // int lux = 500 / (RLDR / 1000);

  if ((millis() - lastTime) > timerDelay) {
    if (WiFi.status() == WL_CONNECTED) {
      WiFiClient client;
      HTTPClient http;

      http.begin(client, serverName);
      http.addHeader("Content-Type", "application/json");

      JSONVar doc;
      doc["humidity"] = humidity;
      doc["temperature"] = temperature;
      doc["timer_value"] = (int)timerSeconds;
      doc["ldr_value"] = ldr;
      doc["button_pressed"] = touchDetected;

      String jsonBody = JSON.stringify(doc);

      int httpResponseCode = http.POST(jsonBody);
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);

      http.end();

      touchDetected = false;  // reset after each send
    } else {
      Serial.println("WiFi Disconnected");
    }
    lastTime = millis();
  }

  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Error: Unable to read data from DHT sensor.");
    return;
  }

  Serial.print("Humidity: ");
  Serial.print(humidity);
  Serial.print(" %\t Temperature: ");
  Serial.print(temperature);
  Serial.print(" °C\t LDR: ");
  Serial.print(ldr);
  Serial.print("\t Timer: ");
  Serial.print(timerSeconds);
  Serial.println("s");
}