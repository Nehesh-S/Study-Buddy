#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <Arduino_JSON.h>

const char* ssid = "laptop";
const char* password = "0987654321";

const char* serverName = "http://192.168.137.90:8000/api/esp8266-sync";

// Pin configuration and sensor type
#define DHTPIN 2        // GPIO pin connected to DHT22
#define DHTTYPE DHT22   // Specify DHT22 sensor

#define LDRPIN A0

unsigned long lastTime = 0;
unsigned long timerDelay = 5000;

DHT dht(DHTPIN, DHTTYPE);

void setup() {
  Serial.begin(115200);   // Start serial communication for debugging
  dht.begin();          // Initialize the DHT sensor

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
  delay(2000);  // Delay for sensor stability (DHT22 polling rate)

  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();
  int ldr = analogRead(LDRPIN);
  // float Vout = float(ldr) * (3.3 / 1023.0);
  // float RLDR = (10000.0 * (3.3 - Vout)) / Vout;
  // int lux = 500 / (RLDR / 1000);

  //Send an HTTP POST request every 10 minutes
  if ((millis() - lastTime) > timerDelay) {
    //Check WiFi connection status
    if(WiFi.status()== WL_CONNECTED){
      WiFiClient client;
      HTTPClient http;
      
      // Your Domain name with URL path or IP address with path
      http.begin(client, serverName);
  
      // If you need Node-RED/server authentication, insert user and password below
      //http.setAuthorization("REPLACE_WITH_SERVER_USERNAME", "REPLACE_WITH_SERVER_PASSWORD");
  
      // Specify content-type header
      http.addHeader("Content-Type", "application/json");
       // Create JSON data
      // String jsonData = "{";
      // jsonData += "\"humidity\":" + String(humidity) + ",";
      // jsonData += "\"temperature\":" + String(temperature) + ",";
      // jsonData += "\"timer_value\":" + String(69) + ",";
      // jsonData += "\"ldr_value\":" + String(69) + ",";
      // jsonData += "\"button_pressed\":" + String(0) + ",";
      // jsonData += "}";      

      JSONVar doc;
      doc["humidity"] = humidity;
      doc["temperature"] = temperature;
      doc["timer_value"] = 69;
      doc["ldr_value"] = ldr;
      doc["button_pressed"] = true;

      String jsonBody = JSON.stringify(doc);


      // Send HTTP POST request
      int httpResponseCode = http.POST(jsonBody);
      
      // If you need an HTTP request with a content type: application/json, use the following:
      //http.addHeader("Content-Type", "application/json");
      //int httpResponseCode = http.POST("{\"api_key\":\"tPmAT5Ab3j7F9\",\"sensor\":\"BME280\",\"value1\":\"24.25\",\"value2\":\"49.54\",\"value3\":\"1005.14\"}");

      // If you need an HTTP request with a content type: text/plain
      //http.addHeader("Content-Type", "text/plain");
      //int httpResponseCode = http.POST("Hello, World!");
     
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
        
      // Free resources
      http.end();
    }
    else {
      Serial.println("WiFi Disconnected");
    }
    lastTime = millis();
  }
  

  // Error handling if sensor fails to provide data
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Error: Unable to read data from DHT sensor.");
    return;
  }

  // Print the results to the serial monitor
  Serial.print("Humidity: ");
  Serial.print(humidity);
  Serial.print(" %\t");
  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" °C");
  Serial.print(" %\t");
  Serial.print("LDR: ");
  Serial.println(ldr);
}