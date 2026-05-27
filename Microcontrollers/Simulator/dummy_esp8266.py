import time
import random
import requests
import csv
import os

URL = "http://localhost:8000/api/esp8266-sync"
LOG_FILE = "simulation_log.csv"

def init_log_file():
    if not os.path.exists(LOG_FILE):
        with open(LOG_FILE, mode='w', newline='') as file:
            writer = csv.writer(file)
            writer.writerow([
                "Timestamp", 
                "Sent_Humidity", 
                "Sent_Timer", 
                "Sent_LDR", 
                "Sent_Button", 
                "HTTP_Status", 
                "Received_State"
            ])

def main():
    init_log_file()
    print("Starting ESP8266 Simulator...")
    
    while True:
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        humidity = round(random.uniform(30.0, 60.0), 1)
        timer_value = random.randint(0, 3600)
        ldr_value = random.randint(100, 1024)
        button_pressed = random.choice([True] + [False] * 9) # mostly False
        
        payload = {
            "humidity": humidity,
            "timer_value": timer_value,
            "ldr_value": ldr_value,
            "button_pressed": button_pressed
        }
        
        http_status = "Error"
        received_state = "Error"
        
        try:
            response = requests.post(URL, json=payload, timeout=5)
            http_status = str(response.status_code)
            if response.status_code == 200:
                data = response.json()
                received_state = data.get("current_state", "Unknown")
            else:
                received_state = "N/A"
        except requests.exceptions.RequestException as e:
            print(f"Connection error: {e}")
            
        with open(LOG_FILE, mode='a', newline='') as file:
            writer = csv.writer(file)
            writer.writerow([
                timestamp, 
                humidity, 
                timer_value, 
                ldr_value, 
                button_pressed, 
                http_status, 
                received_state
            ])
            
        print(f"[{timestamp}] Sent payload, received state: {received_state}")
        time.sleep(2)

if __name__ == "__main__":
    main()
