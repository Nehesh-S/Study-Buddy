# Study Buddy IoT System

The **Study Buddy** is an IoT-based productivity and focus monitoring system. It leverages microcontrollers, computer vision (mocked via YOLO), time-series databases, and dashboarding to track a user's study environment and distraction levels.

---

## 1. Setup & Execution

If you are new to Docker or backend development, follow these step-by-step instructions to get the system running.

### Prerequisites

1. **Install Docker Desktop**: This is required to run our database and dashboard containers. 
   - Download it from [Docker's official website](https://www.docker.com/products/docker-desktop/).
   - Install and launch the application. Ensure the Docker engine is running (you should see a green whale icon in your system tray or menu bar).
2. **Install Python & uv**: 
   - Ensure you have **Python 3.10+** installed.
   - Install [uv](https://docs.astral.sh/uv/getting-started/installation/) (an extremely fast Python package installer and resolver).

### Step 1: Create and Start the Database Containers (Docker)

To keep our system clean and avoid installing databases directly on your machine, we run InfluxDB (for data storage) and Grafana (for dashboards) inside Docker containers.

Open your terminal and navigate to the Docker folder:
```bash
cd Backend/Docker
```

Run the following command to download the necessary images and start the containers in the background (`-d` means detached mode):
```bash
docker-compose up -d influxdb grafana
```
*(Note: The first time you run this, it may take a few minutes to download the container images).*

### Step 2: Start the AI Inference Backend (Native)

Open a **new** terminal window (or PowerShell/Command Prompt on Windows), navigate to the Backend folder, and start the server using `uv`:
```bash
cd Backend
uv run uvicorn src.main:app --host 0.0.0.0 --port 8000
```
*(`uv run` will automatically install any missing dependencies defined in the project before starting the server).*

> [!NOTE]
> **Windows Users:** The commands above work exactly the same in PowerShell or Command Prompt. 
> 
> **Running without a GPU:** If your computer (Windows, Mac, or Linux) does not have a dedicated GPU (or lacks the proper CUDA drivers), the YOLOv8 model will automatically fall back to running on your CPU. The system will still work perfectly fine, but processing each image will take longer (higher latency) and consume more CPU resources.

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
* **Content-Type:** `image/jpeg` (or raw binary)
* **Description:** Receives raw image bytes via the POST body and ambient noise via HTTP headers.

**Request Headers:**
* `Noise-Level` (Optional): The ambient noise in decibels (e.g., "45.5")

**Request Payload:**
Send the raw binary bytes of the `.jpg` image directly in the request body.

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
