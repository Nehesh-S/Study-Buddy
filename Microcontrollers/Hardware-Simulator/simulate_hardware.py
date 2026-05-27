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
    current_timer = random.randint(60, 180)
    
    BASE_HUMIDITY = 45.0
    BASE_TEMP = 22.0
    BASE_LDR = 600
    
    while True:
        payload = {
            "humidity": round(random.gauss(BASE_HUMIDITY, 1.0), 1),
            "temperature": round(random.gauss(BASE_TEMP, 0.5), 1),
            "timer_value": current_timer,
            "ldr_value": int(random.gauss(BASE_LDR, 15)),
            "button_pressed": False
        }
        try:
            response = requests.post(ESP8266_URL, json=payload)
            log_event("ESP8266", response.status_code, response.json().get("current_state", "N/A"))
        except Exception as e:
            log_event("ESP8266", "Error", str(e))
            
        current_timer -= 2
        if current_timer <= 0:
            current_timer = random.randint(60, 180)
            
        time.sleep(2)

import os
import glob

def simulate_esp32():
    cycle_count = 0
    current_image_idx = 0
    
    while True:
        try:
            images = sorted(glob.glob("mock_assets/*.jpg") + glob.glob("mock_assets/*.png"))
            
            image_data = b'\xff\xd8\xff\xe0\x00\x10JFIFdummy'
            image_name = 'capture.jpg'
            
            if images:
                if cycle_count >= 5:
                    cycle_count = 0
                    current_image_idx = (current_image_idx + 1) % len(images)
                    
                selected_image = images[current_image_idx]
                image_name = os.path.basename(selected_image)
                with open(selected_image, 'rb') as f:
                    image_data = f.read()
                
                cycle_count += 1
                
            files = {
                'image': (image_name, image_data, 'image/jpeg'),
                'audio': ('mic.wav', b'RIFFdummyWAVEfmt dummy', 'audio/wav')
            }
            response = requests.post(ESP32_URL, files=files)
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
