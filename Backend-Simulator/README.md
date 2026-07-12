# Backend Simulator

A tiny stand-in for the Study-Buddy backend so you can test the ESP8266 firmware
without running the real YOLO/FastAPI stack. **Stdlib only** — no `uv`, no pip.

It answers `POST /api/esp8266-sync` with the same shape as the real backend:

```json
{"status": "success", "current_state": "working"}
```

## Run

```bash
python simulate_backend.py
```

It listens on port **8000** and prints the IP to point the firmware at. In the
firmware (`Microcontrollers/ESP8266/sensors_and_timer/src/main.cpp`), set:

```cpp
const char* serverHost = "<the IP this script prints>";
const uint16_t serverPort = 8000;
```

Run it in a real terminal window (PowerShell/cmd) so it can read the keyboard —
not through a pipe.

## Controls (sticky — the state stays until you change it)

| Key   | `current_state` | Firmware effect (during FOCUS only) |
|-------|-----------------|-------------------------------------|
| SPACE | `distracted`    | focus timer pauses, blue LED blinks |
| ENTER | `away`          | same as distracted                  |
| `w`   | `working`       | focus timer resumes                 |
| `q`   | —               | quit                                |

The firmware only reacts to `current_state` while the **focus** timer is
running (break/idle ignore it), so start a focus session before testing.
Each incoming request is logged with the telemetry the firmware sent.