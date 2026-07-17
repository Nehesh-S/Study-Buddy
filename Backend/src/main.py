from fastapi import FastAPI, Request
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS
from ultralytics import YOLO
import random
import time
import os

app = FastAPI()

STATIC_DIR = "static"
os.makedirs(STATIC_DIR, exist_ok=True)
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

model = YOLO("yolov8l.pt")
latest_yolo_state = "working"
global_timer_value = 0
current_streak = 0.0
last_predict_time = time.time()
timer_last_changed = time.time()
last_timer_value = -1

class SensorData(BaseModel):
    humidity: float
    temperature: float
    timer_value: int
    ldr_value: int
    button_pressed: bool

@app.post("/api/predict")
async def predict(request: Request):
    global latest_yolo_state, global_timer_value, current_streak, last_predict_time, timer_last_changed

    now = time.time()
    delta = now - last_predict_time
    last_predict_time = now

    # 1. Grab the decibel value from the headers (defaults to 0 if missing)
    noise_str = request.headers.get("Noise-Level", "0")
    noise_db = float(noise_str)
    
    print(f"Received noise level: {noise_db} dB")

    # 1. Read the raw binary bytes from the POST body
    image_data = await request.body()
    
    if not image_data:
        return {"status": "error", "message": "No image data received"}
    
    # 2. Save the incoming raw bytes to process
    # Using a timestamp ensures unique temp files if requests overlap
    temp_path = f"temp_{int(time.time())}.jpg"
    with open(temp_path, "wb") as buffer:
        buffer.write(image_data)
        
    try:
        # Run YOLO inference
        results = model(temp_path)
        
        # Basic state logic based on detected classes
        detections = [model.names[int(c)] for c in results[0].boxes.cls]
        person_count = detections.count("person")
        
        if "cell phone" in detections:
            latest_yolo_state = "distracted"
        elif person_count > 1:
            latest_yolo_state = "distracted"
        elif person_count == 1:
            latest_yolo_state = "working"
        else:
            latest_yolo_state = "away" # No person detected
            
        # The simulator sends data every 5 seconds, real hardware might be faster.
        # If timer hasn't changed in 7 seconds, assume it is paused.
        is_timer_running = (global_timer_value > 0) and ((time.time() - timer_last_changed) < 7.0)

        if is_timer_running:
            if latest_yolo_state == "working":
                current_streak += delta
            else:
                current_streak = 0.0
        else:
            if latest_yolo_state != "working":
                latest_yolo_state = f"{latest_yolo_state} (break)"
            else:
                latest_yolo_state = "working (break)"
            
        # Save the YOLO annotated image
        output_filename = "latest_detection.jpg"
        output_path = os.path.join(STATIC_DIR, output_filename)
        results[0].save(filename=output_path)
        
        # Connect to InfluxDB to save the Image URL
        client = InfluxDBClient(
            url="http://localhost:8086",
            token="studybuddy_admin_token_123",
            org="myorg"
        )
        write_api = client.write_api(write_options=SYNCHRONOUS)
        
        # Attach a timestamp to the URL to bypass Grafana's caching
        ts = int(time.time())
        image_url = f"http://localhost:8000/static/{output_filename}?ts={ts}"
        
        if latest_yolo_state == "working":
            state_int = 1
        elif latest_yolo_state == "distracted":
            state_int = 0
        elif latest_yolo_state == "away":
            state_int = 2
        elif latest_yolo_state == "working (break)":
            state_int = 3
        elif latest_yolo_state == "distracted (break)":
            state_int = 4
        elif latest_yolo_state == "away (break)":
            state_int = 5
        else:
            state_int = 6
        
        point = Point("camera") \
            .field("image_url", image_url) \
            .field("state", latest_yolo_state) \
            .field("state_code", state_int) \
            .field("noise_db", noise_db) \
            .field("current_streak", int(current_streak))
            
        write_api.write(bucket="sensors", org="myorg", record=point)
        client.close()
        
    except Exception as e:
        print(f"YOLO Error: {e}")
        return {"status": "error", "message": str(e)}
    finally:
        if os.path.exists(temp_path):
            os.remove(temp_path)
            
    return {"status": "success", "current_state": latest_yolo_state}

@app.post("/api/esp8266-sync")
async def esp8266_sync(data: SensorData):
    global latest_yolo_state, global_timer_value, timer_last_changed, last_timer_value
    
    if data.timer_value != last_timer_value:
        timer_last_changed = time.time()
        last_timer_value = data.timer_value
        
    global_timer_value = data.timer_value
    
    client = InfluxDBClient(
        url="http://localhost:8086",
        token="studybuddy_admin_token_123",
        org="myorg"
    )
    write_api = client.write_api(write_options=SYNCHRONOUS)
    
    point = Point("environment") \
        .field("humidity", data.humidity) \
        .field("temperature", data.temperature) \
        .field("timer_value", data.timer_value) \
        .field("ldr_value", data.ldr_value) \
        .field("button_pressed", data.button_pressed)
        
    write_api.write(bucket="sensors", org="myorg", record=point)
    client.close()
    
    return {"status": "success", "current_state": latest_yolo_state}
