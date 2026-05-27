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
    while True:
        payload = {
            "humidity": round(random.uniform(30.0, 60.0), 1),
            "timer_value": random.randint(0, 3600),
            "ldr_value": random.randint(100, 1024),
            "button_pressed": random.choice([True, False, False, False])
        }
        try:
            response = requests.post(ESP8266_URL, json=payload)
            log_event("ESP8266", response.status_code, response.json().get("current_state", "N/A"))
        except Exception as e:
            log_event("ESP8266", "Error", str(e))
        time.sleep(2)

def simulate_esp32():
    while True:
        try:
            # Create dummy bytes for image and audio mimicking the ESP32 sensors
            files = {
                'image': ('capture.jpg', b'\xff\xd8\xff\xe0\x00\x10JFIFdummy', 'image/jpeg'),
                'audio': ('mic.wav', b'RIFFdummyWAVEfmt dummy', 'audio/wav')
            }
            response = requests.post(ESP32_URL, files=files)
            log_event("ESP32", response.status_code, response.json().get("current_state", "N/A"))
        except Exception as e:
            log_event("ESP32", "Error", str(e))
        
        # The camera/mic might sample less frequently than the environmental sensors
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
