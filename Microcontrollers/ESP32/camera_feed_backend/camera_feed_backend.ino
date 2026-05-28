/*
  XIAO ESP32S3 Sense Camera with Microphone Demo
  xiao-camera-mic-demo.ino
  Tests onboard Camera, MEMS Microphone, and MicroSD Card
  Takes a picture and a 10-second recording when Touch Switch is pressed
  Saves to MicroSD card in JPG & WAV format
  
  DroneBot Workshop 2023
  https://dronebotworkshop.com
*/

// Include required libraries
#include <esp_camera.h>
#include <WiFi.h>
#include <HTTPClient.h>

const char* ssid = "laptop";
const char* password = "0987654321";

// Server endpoint
const char* serverUrl = "http://192.168.137.90:8000/api/predict";

// Define camera model & pinout
#define CAMERA_MODEL_XIAO_ESP32S3  // Has PSRAM
#include "camera_pins.h"

// Camera status variable
bool camera_status = false;

// Camera Parameters for setup
void CameraParameters() {
  // Define camera parameters
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
}

void setup() {
  // Start Serial Monitor, wait until port is ready
  Serial.begin(115200);
  
  // Connect WiFi
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");

  // Define Camera Parameters and Initialize
  CameraParameters();

  // Camera is good, set status
  camera_status = true;
  Serial.println("Camera OK!");
}

void loop() {
  Serial.println("START");
  // Make sure the camera and MicroSD are ready
  if (camera_status) {
    char imageFileName[32];

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Failed to get camera frame buffer");
      return;
    }
    Serial.printf("Image captured: %d bytes\n", fb->len);
    
    // Send image via HTTP POST
    if (WiFi.status() == WL_CONNECTED) {

      HTTPClient http;

      http.begin(serverUrl);

      // JPEG content type
      http.addHeader("Content-Type", "image/jpeg");
      http.addHeader("Noise-Level", String(69));

      // POST binary image buffer
      int httpResponseCode = http.POST(fb->buf, fb->len);

      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);

      if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println(response);
      } else {
        Serial.print("Error: ");
        Serial.println(http.errorToString(httpResponseCode));
      }

      http.end();

    } else {
      Serial.println("WiFi disconnected");
    }

    // Release image buffer
    esp_camera_fb_return(fb);
  }
    delay(5000);
}