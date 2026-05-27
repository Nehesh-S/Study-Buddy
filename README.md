# Study Buddy IoT System

The **Study Buddy** is an IoT-based productivity and focus monitoring system. It leverages microcontrollers, computer vision (mocked via YOLO), time-series databases, and dashboarding to track a user's study environment and distraction levels.

---

## 1. Setup & Execution

### Prerequisites
* [Docker Desktop](https://www.docker.com/products/docker-desktop/) and Docker Compose
* Python 3.10+ and [uv](https://github.com/astral-sh/uv) (for running the local simulator)

### 1. Start the Databases (Docker)
To keep things clean, we run InfluxDB and Grafana inside Docker containers:
```bash
cd Backend/Docker
docker-compose up -d influxdb grafana
```

### 2. Start the AI Backend (Native)
To squeeze maximum performance out of your Apple Silicon or local GPU for the heavy YOLOv8 Large model, run the backend natively outside of Docker:
```bash
cd Backend
uv run uvicorn src.main:app --host 0.0.0.0 --port 8000
```

### 3. Run the Hardware Simulator
If you don't have physical hardware, run the Python simulator to send mocked sensor data and camera images to the backend:
```bash
cd Microcontrollers/Hardware-Simulator
uv run simulate_hardware.py
```

### Stop / Reset Data
```bash
# Stop the system normally
docker-compose down

# Completely wipe all databases and dashboards to start fresh
docker-compose down -v
```

---

## 2. Environment & Credentials

Once the system is running, all services are mapped to your local host machine. 
*(If connecting from a microcontroller on the same Wi-Fi, replace `localhost` with your host machine's IP address, e.g., `192.168.1.50`)*

### Service Endpoints & Logins
| Service | Access URL | Username | Password / Token |
| :--- | :--- | :--- | :--- |
| **FastAPI Backend** | [http://localhost:8000](http://localhost:8000) | - | - |
| **Grafana Dashboard** | [http://localhost:3000](http://localhost:3000) | `admin` | `admin` |
| **InfluxDB Admin UI** | [http://localhost:8086](http://localhost:8086) | `admin` | `studentproject` |

### Internal System Tokens
If you are developing new backend services or modifying the database connection, use these internal credentials:
* **InfluxDB Organization:** `myorg`
* **InfluxDB Bucket:** `sensors`
* **InfluxDB Admin Token:** `studybuddy_admin_token_123`

---

## 3. Hardware API Reference

All hardware traffic flows into the backend via standard HTTP POST requests on port `8000`.

### Sensor Synchronization
* **Endpoint:** `POST http://<SERVER_IP>:8000/api/esp8266-sync`
* **Content-Type:** `application/json`
* **Description:** Ingests sensor data, writes it to InfluxDB, and returns the latest focus state.

**Request Payload:**
```json
{
  "humidity": 45.5,
  "timer_value": 3600,
  "ldr_value": 512,
  "button_pressed": false
}
```

**Response Payload:**
```json
{
  "status": "success",
  "current_state": "working"
}
```

### Camera & Microphone Inference (ESP32)
* **Endpoint:** `POST http://<SERVER_IP>:8000/api/predict`
* **Content-Type:** `multipart/form-data`
* **Description:** Receives an image and optional audio from the ESP32 to detect distraction.

**Request Payload:**
Send standard form-data file uploads under the keys:
* `image` (Required: `.jpg` / `.png`)
* `audio` (Optional: `.wav`)

**Response Payload:**
```json
{
  "status": "success",
  "current_state": "distracted"
}
```

---

## 4. Database Schema (InfluxDB)

Data is structured in InfluxDB automatically based on the API inputs.
* **Bucket:** `sensors`
* **Measurement (Table):** `environment`

| Field Name | Type | Description |
| :--- | :--- | :--- |
| `humidity` | `float` | Room humidity percentage (e.g., 45.5). |
| `timer_value` | `int` | Current timer duration in seconds. |
| `ldr_value` | `int` | Raw light-dependent resistor reading (0-1024). |
| `button_pressed`| `bool` | Whether the physical button is currently engaged. |
