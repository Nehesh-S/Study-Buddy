from fastapi import FastAPI, UploadFile, File
from pydantic import BaseModel
from typing import Optional
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS
import random

app = FastAPI()

latest_yolo_state = "working"

class SensorData(BaseModel):
    humidity: float
    timer_value: int
    ldr_value: int
    button_pressed: bool

@app.post("/api/predict")
async def predict(image: UploadFile = File(...), audio: Optional[UploadFile] = File(None)):
    global latest_yolo_state
    # Dummy logic: randomly updates state
    latest_yolo_state = random.choice(["distracted", "working"])
    return {"status": "success", "current_state": latest_yolo_state}

@app.post("/api/esp8266-sync")
async def esp8266_sync(data: SensorData):
    global latest_yolo_state
    
    # Connect to InfluxDB container
    client = InfluxDBClient(
        url="http://influxdb:8086",
        token="studybuddy_admin_token_123",
        org="myorg"
    )
    write_api = client.write_api(write_options=SYNCHRONOUS)
    
    # Write sensor data to 'environment' measurement
    point = Point("environment") \
        .field("humidity", data.humidity) \
        .field("timer_value", data.timer_value) \
        .field("ldr_value", data.ldr_value) \
        .field("button_pressed", data.button_pressed)
        
    write_api.write(bucket="sensors", org="myorg", record=point)
    client.close()
    
    return {"status": "success", "current_state": latest_yolo_state}
