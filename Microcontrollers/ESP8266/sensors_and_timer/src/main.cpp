#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <Arduino_JSON.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_ADS1X15.h>
#include <DFRobotDFPlayerMini.h>
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

#define POT_PIN A0        //          — potentiometer wiper (ESP8266's own ADC)
#define ADS_LDR_CHANNEL 0 // ADS1115 AIN0 — LDR divider (16-bit analog over I2C)

// Suppress push button read via a spare ADS1115 channel (all ESP GPIOs are
// used). Button ties AIN1 to 3V3 when pressed, a pull-down holds it near 0 V
// otherwise; anything above the threshold counts as pressed.
#define ADS_BUTTON_CHANNEL 1
#define BUTTON_PRESS_THRESHOLD 10000  // ~1.25 V at GAIN_ONE (idle ~0, pressed ~26000)

// DFPlayer Mini (mini MP3 module) on the hardware UART. Escalation: if a
// distraction outlasts the blink/grace period, play a random track from the
// SD folder "01" (= your distracted sounds, files 001.mp3, 002.mp3, ...).
#define DFPLAYER_VOLUME 10         // 0..30 — moderate on purpose: high volume clips a
                                   // small speaker and spikes current (distortion/cutout)
#define DISTRACTED_FOLDER 1        // SD card folder "01"
#define NUM_DISTRACTED_TRACKS 11    // set to how many NNN.mp3 files are in folder 01
#define BLINK_CYCLES 2             // LED blinks this many distracted cycles, then audio fires
#define SUPPRESS_CYCLES 2          // a button press mutes the LED for this many cycles first
#define REPEAT_AUDIO_CYCLES 4      // after the first audio, re-play every this many distracted cycles

#define TOUCH1_PIN 13 //D7  — start focus timer / stop the cycle
#define TOUCH2_PIN 12 //D6  — cycle set mode (focus -> break -> idle)

// Traffic-light LEDs (active HIGH). GPIO15/D8 is a boot-strap pin, but an
// LED-to-GND holds it LOW at boot, which is the required state.
#define LED_RED_PIN 14    //D5
#define LED_YELLOW_PIN 16 //D0
#define LED_GREEN_PIN 15  //D8

// Alert LED (active HIGH) — blinks when the backend reports distracted/away.
// GPIO0/D3 is a boot-strap pin (must be HIGH at boot); an LED-to-GND is a light
// enough load that the pin's pull-up still boots normally.
#define LED_BLUE_PIN 0    //D3

// Fraction of a phase after which the yellow "about to switch" LED joins in.
#define LED_WARN_NUMERATOR 9
#define LED_WARN_DENOMINATOR 10

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
#define CYCLE_SECONDS (SYNC_INTERVAL / 1000UL)  // distracted seconds counted per distracted reply
#define SYNC_POLL_INTERVAL 50
#define WIFI_MONITOR_INTERVAL 500
#define ALERT_BLINK_INTERVAL 250  // blue LED toggles at this rate = ~2 Hz blink
#define SUPPRESS_POLL_INTERVAL 150  // how often the suppress button is sampled

// A touch must read HIGH for this many consecutive polls (~60 ms) to count.
// Rejects brief glitches — e.g. TTP223 output blips when WiFi transmit spikes
// the supply — that a raw single-sample edge would wrongly accept as a touch.
#define TOUCH_CONFIRM_SAMPLES 3

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
DHT dht(DHTPIN, DHTTYPE);
Adafruit_ADS1115 ads;   // external 16-bit ADC on the shared I2C bus (addr 0x48)
bool adsReady = false;  // set true once ads.begin() succeeds
DFRobotDFPlayerMini dfplayer;  // mini MP3 module on the hardware UART (Serial)
bool dfReady = false;

ReactESP app;  // ReactESP 2.x — v3 dropped ESP8266 support (requires FreeRTOS)

// Reused socket for the backend sync + its non-blocking response state.
WiFiClient syncClient;
enum SyncState { SYNC_IDLE, SYNC_WAITING };  // WAITING = request sent, reply pending
SyncState syncState = SYNC_IDLE;
unsigned long syncSentAt = 0;
String syncResponse;  // accumulates the full HTTP reply as bytes trickle in

// Set from the backend's current_state: true while "distracted"/"away" so the
// blue LED blinks; false on "working". Consumed by blinkAlertLed().
bool alertActive = false;

// Distracted-escalation ladder (per distracted episode during focus):
//   blink for BLINK_CYCLES cycles -> play audio. A button press inserts
//   SUPPRESS_CYCLES muted cycles and restarts the blink count, delaying audio.
// Any non-distracted reply resets the whole thing.
int suppressCyclesLeft = 0;      // muted cycles remaining (LED off)
int blinkCyclesDone = 0;         // blinking cycles counted so far this ladder
bool audioPlayed = false;        // first audio has fired this ladder (LED then solid on)
int postAudioCycles = 0;         // distracted cycles since the last audio nag
bool isDistracted = false;       // last reply was "distracted"
bool buttonWasPressed = false;   // for rising-edge detection on the ADS button

// Distracted-time stats. sessionDistractedSeconds accumulates the current focus
// session's distracted time (CYCLE_SECONDS per distracted reply);
// minDistractedSeconds is the least of that across completed focus sessions.
unsigned long sessionDistractedSeconds = 0;
unsigned long minDistractedSeconds = 0xFFFFFFFFUL;  // sentinel: no session finished yet

// Tracks WiFi connection state so we can log transitions without blocking.
wl_status_t lastWifiStatus = WL_IDLE_STATUS;

// Latest sensor readings, refreshed by the sensor-read event and consumed
// by the display and sync events.
float humidity = 0.0;
float temperature = 0.0;
int ldrValue = 0;  // raw 16-bit ADS1115 reading (higher = brighter)

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

// Focus pause (backend reports distracted/away). While paused we freeze the
// elapsed time at pausedElapsed; on resume phaseStartMillis is shifted forward
// so the countdown continues from exactly where it stopped.
bool timerPaused = false;
unsigned long pausedElapsed = 0;

// Touch confirmation state (TTP223 drives the line HIGH while touched).
int touch1Count = 0;
bool touch1Latched = false;
int touch2Count = 0;
bool touch2Latched = false;

bool touchDetected = false;  // sensor-1 touched since last sync (telemetry)

unsigned long focusDurationMs() { return (unsigned long)focusMinutes * 60UL * 1000UL; }
unsigned long breakDurationMs() { return (unsigned long)breakMinutes * 60UL * 1000UL; }

// Effective elapsed time in the running phase — frozen while paused.
unsigned long phaseElapsed() {
  return timerPaused ? pausedElapsed : (millis() - phaseStartMillis);
}

// Remaining whole seconds in the running phase, clamped so it never underflows.
unsigned long remainingSeconds(unsigned long durationMs) {
  unsigned long elapsed = phaseElapsed();
  if (elapsed >= durationMs) return 0;
  return (durationMs - elapsed) / 1000;
}

// Begin a fresh focus phase from the top, unpaused and assuming "working"
// until the backend says otherwise.
void startFocusPhase() {
  mode = MODE_FOCUS;
  phaseStartMillis = millis();
  timerPaused = false;
  alertActive = false;
  suppressCyclesLeft = 0;
  blinkCyclesDone = 0;
  audioPlayed = false;
  postAudioCycles = 0;
  isDistracted = false;
  sessionDistractedSeconds = 0;  // fresh distracted-time tally for this session
}

// Map the potentiometer to a whole number of minutes in [MIN, MAX].
int readPotMinutes() {
  int raw = analogRead(POT_PIN);
  return constrain(map(raw, 0, 1023, MIN_MINUTES, MAX_MINUTES), MIN_MINUTES, MAX_MINUTES);
}

// Print a duration as MM:SS at the current cursor.
void printMMSS(unsigned long totalSeconds) {
  unsigned long m = totalSeconds / 60;
  unsigned long s = totalSeconds % 60;
  if (m < 10) display.print('0');
  display.print(m);
  display.print(':');
  if (s < 10) display.print('0');
  display.print(s);
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

  // Bottom line: distracted-time stat. During focus, this session's distracted
  // time; during break, the least distracted time across finished sessions.
  display.setTextSize(1);
  display.setCursor(0, 56);
  if (mode == MODE_FOCUS) {
    display.print("Distr ");
    printMMSS(sessionDistractedSeconds);
  } else if (mode == MODE_BREAK) {
    display.print("Best ");
    if (minDistractedSeconds == 0xFFFFFFFFUL) display.print("--:--");
    else printMMSS(minDistractedSeconds);
  }

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
    startFocusPhase();
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

// Drive the traffic-light LEDs from the current phase and its progress:
//   FOCUS  first 90% -> red;            last 10% -> red + yellow
//   BREAK  first 90% -> green;          last 10% -> green + yellow
//   IDLE / SET modes -> all off.
void updateLeds() {
  bool red = false, yellow = false, green = false;

  if (mode == MODE_FOCUS) {
    red = true;
    unsigned long warnAt = focusDurationMs() * LED_WARN_NUMERATOR / LED_WARN_DENOMINATOR;
    if (phaseElapsed() >= warnAt) yellow = true;
  } else if (mode == MODE_BREAK) {
    green = true;
    unsigned long warnAt = breakDurationMs() * LED_WARN_NUMERATOR / LED_WARN_DENOMINATOR;
    if (phaseElapsed() >= warnAt) yellow = true;
  }

  digitalWrite(LED_RED_PIN, red ? HIGH : LOW);
  digitalWrite(LED_YELLOW_PIN, yellow ? HIGH : LOW);
  digitalWrite(LED_GREEN_PIN, green ? HIGH : LOW);
}

// Advance the focus/break cycle, apply the pot in set mode, and repaint.
void updateUI() {
  // 1) Pause/resume the focus countdown from the backend's camera state. Only
  //    focus is affected; break/idle ignore it. Freezing elapsed here stops the
  //    phase from advancing below.
  bool shouldPause = (mode == MODE_FOCUS && alertActive);
  if (shouldPause && !timerPaused) {
    pausedElapsed = millis() - phaseStartMillis;
    timerPaused = true;
    Serial.println("Focus paused — distracted/away");
  } else if (!shouldPause && timerPaused) {
    phaseStartMillis = millis() - pausedElapsed;  // continue where we stopped
    timerPaused = false;
    Serial.println("Focus resumed — working");
  }

  // 2) Advance the running phases; when one ends, the other starts automatically.
  //    phaseElapsed() is frozen while paused, so a paused focus never expires.
  if (mode == MODE_FOCUS && phaseElapsed() >= focusDurationMs()) {
    if (sessionDistractedSeconds < minDistractedSeconds) {
      minDistractedSeconds = sessionDistractedSeconds;  // new best (least-distracted) session
    }
    mode = MODE_BREAK;
    phaseStartMillis = millis();
    Serial.println("Focus finished — break started");
  } else if (mode == MODE_BREAK && phaseElapsed() >= breakDurationMs()) {
    startFocusPhase();
    Serial.println("Break finished — focus started");
  }

  // 3) In set mode the value snaps to the potentiometer position.
  if (mode == MODE_SET_FOCUS) {
    focusMinutes = readPotMinutes();
  } else if (mode == MODE_SET_BREAK) {
    breakMinutes = readPotMinutes();
  }

  // 4) Decide what to show.
  const char* label;
  unsigned long showSeconds;
  switch (mode) {
    case MODE_FOCUS:
      label = timerPaused ? "FOCUS PAUSED" : "FOCUS";
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
  updateLeds();
}

// Read the DHT22 and the digital LDR. Keeps last good DHT values on a failed read.
void readSensors() {
  // DIAGNOSTIC: watch the heap trend. A steady decline every cycle = a leak
  // (prime suspect: the async HTTP path) and the eventual crash is heap starvation.
  Serial.print("Heap: ");
  Serial.print(ESP.getFreeHeap());

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (adsReady) {
    ldrValue = ads.readADC_SingleEnded(ADS_LDR_CHANNEL);  // 16-bit, ~few ms
  }

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
  Serial.print(" °C\t LDR: ");
  Serial.print(ldrValue);
  Serial.print(" (");
  Serial.print(ads.computeVolts(ldrValue), 3);
  Serial.println(" V)");
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
  syncResponse = "";
  syncResponse.reserve(256);
  touchDetected = false;  // request is on its way
}

// Play a random track from the distracted-sounds folder on the DFPlayer's SD.
void playDistractedAudio() {
  if (!dfReady) return;
  uint8_t track = random(1, NUM_DISTRACTED_TRACKS + 1);  // 1..NUM
  dfplayer.playFolder(DISTRACTED_FOLDER, track);
}

// Parse the reply body and act on current_state ("working"/"distracted"/"away").
// Only the focus phase cares about the camera state — break/idle ignore it.
void handleSyncResponse() {
  if (mode != MODE_FOCUS) return;

  // Grab the JSON object out of the raw reply (robust to headers / chunking).
  int start = syncResponse.indexOf('{');
  int end = syncResponse.lastIndexOf('}');
  if (start < 0 || end <= start) {
    Serial.println("Sync reply: no JSON body");
    return;
  }

  JSONVar doc = JSON.parse(syncResponse.substring(start, end + 1));
  if (JSON.typeof(doc) == "undefined" || !doc.hasOwnProperty("current_state")) {
    Serial.println("Sync reply: bad JSON / no current_state");
    return;
  }

  String state = (const char*)doc["current_state"];
  bool distracted = (state == "distracted");
  alertActive = distracted || (state == "away");  // drives the focus pause

  if (distracted) {
    isDistracted = true;
    sessionDistractedSeconds += CYCLE_SECONDS;  // count this distracted cycle
    if (suppressCyclesLeft > 0) {
      suppressCyclesLeft--;             // muted cycle: LED off, ladder frozen
    } else if (!audioPlayed) {
      blinkCyclesDone++;                // a blinking cycle
      if (blinkCyclesDone > BLINK_CYCLES) {
        playDistractedAudio();          // blinked long enough -> escalate to sound
        audioPlayed = true;
        postAudioCycles = 0;            // start the re-nag counter (LED now solid on)
      }
    } else {
      // Past the first audio: LED stays solid on; re-play periodically.
      postAudioCycles++;
      if (postAudioCycles >= REPEAT_AUDIO_CYCLES) {
        playDistractedAudio();
        postAudioCycles = 0;
      }
    }
  } else {
    // "working" or "away" — anything but distracted resets the ladder.
    isDistracted = false;
    suppressCyclesLeft = 0;
    blinkCyclesDone = 0;
    audioPlayed = false;
    postAudioCycles = 0;
  }

  Serial.print("current_state: ");
  Serial.print(state);
  Serial.println(alertActive ? "  -> ALERT" : "  -> working");
}

// Non-blocking reply handler: reads only bytes already available, so it never
// waits on the network. Accumulates the full reply, then parses it on close.
void pollSyncResponse() {
  if (syncState != SYNC_WAITING) return;

  while (syncClient.available()) {
    syncResponse += (char)syncClient.read();
  }

  // Peer closed (we sent "Connection: close") or we ran out of patience.
  bool closed = !syncClient.connected();
  bool timedOut = millis() - syncSentAt > RESPONSE_TIMEOUT_MS;
  if (closed || timedOut) {
    if (syncResponse.length() > 0) {
      handleSyncResponse();
    } else {
      Serial.println("Sync reply timed out (no data)");
    }
    syncClient.stop();
    syncState = SYNC_IDLE;
  }
}

// Drive the blue LED for the distracted ladder (while distracted, in focus, and
// not button-muted): blink during the pre-audio stage, then hold solid on after
// the audio fires as a steady distracted-status indicator.
void blinkAlertLed() {
  static bool on = false;
  bool active = isDistracted && mode == MODE_FOCUS && suppressCyclesLeft == 0;
  if (!active) {
    on = false;
    digitalWrite(LED_BLUE_PIN, LOW);
    return;
  }
  if (audioPlayed) {
    on = true;                              // solid on after escalation
    digitalWrite(LED_BLUE_PIN, HIGH);
  } else {
    on = !on;                               // blinking before escalation
    digitalWrite(LED_BLUE_PIN, on ? HIGH : LOW);
  }
}

// Poll the suppress push button (on a spare ADS1115 channel). A fresh press
// mutes the blue blink for the current alert window; the ~150 ms sample rate
// also debounces the mechanical contacts.
void checkSuppressButton() {
  if (!adsReady) return;
  bool pressed = ads.readADC_SingleEnded(ADS_BUTTON_CHANNEL) > BUTTON_PRESS_THRESHOLD;
  if (pressed && !buttonWasPressed) {  // rising edge = new press
    suppressCyclesLeft = SUPPRESS_CYCLES;  // mute N cycles...
    blinkCyclesDone = 0;                   // ...then blink N cycles again...
    audioPlayed = false;                   // ...before audio can fire
    Serial.println("Suppress button — mute, re-blink, then audio");
  }
  buttonWasPressed = pressed;
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
  // The hardware UART now drives the DFPlayer Mini (9600 baud, fixed), so the
  // USB serial monitor is no longer usable — debug prints still go out but are
  // ignored by the module (they contain no 0x7E frame-start byte).
  Serial.begin(9600);

  // Command-only link to the MP3 module (isACK=false: we don't read its replies).
  dfReady = dfplayer.begin(Serial, false, false);
  dfplayer.volume(DFPLAYER_VOLUME);
  randomSeed(micros());

  dht.begin();

  pinMode(TOUCH1_PIN, INPUT);  // TTP223 actively drives the line
  pinMode(TOUCH2_PIN, INPUT);

  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_BLUE_PIN, OUTPUT);
  digitalWrite(LED_RED_PIN, LOW);
  digitalWrite(LED_YELLOW_PIN, LOW);
  digitalWrite(LED_GREEN_PIN, LOW);
  digitalWrite(LED_BLUE_PIN, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
  } else {
    display.clearDisplay();
    display.display();
  }

  // External ADC for the LDR (shares the I2C bus; default address 0x48).
  if (ads.begin()) {
    ads.setGain(GAIN_ONE);  // ±4.096 V full scale — LDR node tops out at 3.3 V
    adsReady = true;
    Serial.println("ADS1115 ready");
  } else {
    Serial.println("ADS1115 not found (check wiring / ADDR->GND for 0x48)");
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
  app.onRepeat(ALERT_BLINK_INTERVAL, blinkAlertLed);
  app.onRepeat(SUPPRESS_POLL_INTERVAL, checkSuppressButton);
  app.onRepeat(WIFI_MONITOR_INTERVAL, monitorWiFi);
}

void loop() {
  app.tick();
}
