from fastapi import FastAPI, UploadFile, File
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
from typing import Optional
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS
from ultralytics import YOLO
import random
import time
import os
import shutil

app = FastAPI()

STATIC_DIR = "static"
os.makedirs(STATIC_DIR, exist_ok=True)
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

model = YOLO("yolov8l.pt")
latest_yolo_state = "working"

class SensorData(BaseModel):
    humidity: float
    temperature: float
    timer_value: int
    ldr_value: int
    button_pressed: bool

@app.post("/api/predict")
async def predict(image: UploadFile = File(...), audio: Optional[UploadFile] = File(None)):
    global latest_yolo_state
    
    # Save the incoming image to process
    temp_path = f"temp_{image.filename}"
    with open(temp_path, "wb") as buffer:
        shutil.copyfileobj(image.file, buffer)
        
    try:
        # Run YOLO inference
        results = model(temp_path)
        
        # Basic state logic based on detected classes
        detections = [model.names[int(c)] for c in results[0].boxes.cls]
        
        if "cell phone" in detections:
            latest_yolo_state = "distracted"
        elif "person" in detections:
            latest_yolo_state = "working"
        else:
            latest_yolo_state = "distracted" # Empty desk
            
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
        
        point = Point("camera") \
            .field("image_url", image_url) \
            .field("state", latest_yolo_state) \
            .field("state_code", 1 if latest_yolo_state == "working" else 0)
            
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
    global latest_yolo_state
    
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
