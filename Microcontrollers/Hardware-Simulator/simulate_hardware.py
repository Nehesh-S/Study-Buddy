import time
import random
import csv
from datetime import datetime
import requests
import threading

ESP8266_URL = "http://localhost:8000/api/esp8266-sync"
ESP32_URL = "http://localhost:8000/api/predict"
LOG_FILE = "simulation_log.csv"

# Initialize CSV
with open(LOG_FILE, mode='w', newline='') as file:
    writer = csv.writer(file)
    writer.writerow(["Timestamp", "Device", "Status", "Details"])

def log_event(device, status, details):
    with open(LOG_FILE, mode='a', newline='') as file:
        writer = csv.writer(file)
        writer.writerow([datetime.now().strftime("%Y-%m-%d %H:%M:%S"), device, status, details])
    print(f"[{device}] {status}: {details}")

def simulate_esp8266():
    timer_seconds = 0
    timer_running = False
    
    BASE_HUMIDITY = 45.0
    BASE_TEMP = 22.0
    BASE_LDR = 600
    
    while True:
        button_pressed = False
        
        # Simulate button press to start the 5-min timer
        if not timer_running and random.random() < 0.5: # 50% chance every 5s to hit the button if idle
            timer_running = True
            timer_seconds = 300
            button_pressed = True
            
        if timer_running:
            timer_seconds -= 5
            if timer_seconds <= 0:
                timer_seconds = 0
                timer_running = False
                
        payload = {
            "humidity": round(random.gauss(BASE_HUMIDITY, 1.0), 1),
            "temperature": round(random.gauss(BASE_TEMP, 0.5), 1),
            "timer_value": timer_seconds,
            "ldr_value": int(random.gauss(BASE_LDR, 15)),
            "button_pressed": button_pressed
        }
        try:
            response = requests.post(ESP8266_URL, json=payload)
            log_event("ESP8266", response.status_code, response.json().get("current_state", "N/A"))
        except Exception as e:
            log_event("ESP8266", "Error", str(e))
            
        time.sleep(5)

import os
import glob

def simulate_esp32():
    cycle_count = 0
    current_image_idx = 0
    
    while True:
        try:
            images = sorted(glob.glob("mock_assets/*.jpg") + glob.glob("mock_assets/*.png"))
            
            image_data = b'\xff\xd8\xff\xe0\x00\x10JFIFdummy'
            
            if images:
                if cycle_count >= 5:
                    cycle_count = 0
                    current_image_idx = (current_image_idx + 1) % len(images)
                    
                selected_image = images[current_image_idx]
                with open(selected_image, 'rb') as f:
                    image_data = f.read()
                
                cycle_count += 1
                
            base_noise = random.gauss(40.0, 3.0)
            if random.random() < 0.3: # 30% chance for a loud peak
                base_noise += random.uniform(15.0, 30.0)
                
            headers = {
                "Content-Type": "image/jpeg",
                "Noise-Level": str(round(base_noise, 1))
            }
            
            # Send raw binary POST like the ESP32 does, instead of multipart form files
            response = requests.post(ESP32_URL, data=image_data, headers=headers)
            log_event("ESP32", response.status_code, response.json().get("current_state", "N/A"))
        except Exception as e:
            log_event("ESP32", "Error", str(e))
        
        time.sleep(5)

if __name__ == "__main__":
    print("Starting Hardware Simulation (ESP8266 & ESP32)...")
    print("Press Ctrl+C to stop.")
    
    t1 = threading.Thread(target=simulate_esp8266, daemon=True)
    t2 = threading.Thread(target=simulate_esp32, daemon=True)
    
    t1.start()
    t2.start()
    
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nSimulation stopped.")
