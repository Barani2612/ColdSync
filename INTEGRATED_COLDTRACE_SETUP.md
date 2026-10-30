# ColdTrace integrated ESP2 / offline-first setup

## What was updated
- `firmware/ESP2/ESP2.ino`: ESP-NOW Core 3.x receiver using the exact 53-byte `ColdTracePacket` from `firmware/ESP1/ESP1.ino`; validates packet length; records every received packet to CSV and a durable JSONL pending queue; OLED rotating pages; GPS/RTC state; local JSON API; retrying HTTP upload and ACK-gated queue removal.
- `firmware/ESP2/data/index.html`: standalone live dashboard hosted by ESP2 LittleFS. It displays the complete sensor payload and remains available with no internet.
- `backend/app.py` + `backend/database.py`: backward-compatible database migration, full telemetry storage, event-ID deduplication, telemetry JSON endpoints, complete CSV export, and `/telemetry` full remote dashboard.
- Existing backend database is migrated additively when the server starts; do not delete `backend/data/coldchain.db`.

## 1. ESP1 / ESP2 packet compatibility
Use the ESP1 sketch already in `firmware/ESP1/ESP1.ino` and the new ESP2 sketch. Both packet definitions must remain byte-for-byte identical in field order and type. ESP2 expects `sizeof(ColdTracePacket) == 53` bytes.

## 2. ESP2 Arduino libraries
Install using Arduino IDE Library Manager:
- Adafruit SSD1306
- Adafruit GFX Library
- RTClib by Adafruit
- TinyGPSPlus
- ArduinoJson (v6 or v7)
ESP32 Arduino core 3.x provides WiFi, ESP-NOW, WebServer, LittleFS, SPI, SD, HTTPClient and Wire.

## 3. Set the network configuration before upload
Edit the top of `firmware/ESP2/ESP2.ino`:
- `WIFI_SSID` / `WIFI_PASSWORD`: laptop hotspot or router 2.4 GHz network; leave blank for offline-only testing.
- `CLOUD_URL`: `http://<LAPTOP_LAN_IP>:5000/api/data`, e.g. `http://192.168.1.100:5000/api/data`. Use the actual laptop IP, not localhost.
- Keep `API_KEY` equal to `API_KEY` in `backend/app.py`.
- `WIFI_CHANNEL` defaults to 1. If you use the router for internet, configure its 2.4 GHz band to fixed channel 1. ESP-NOW sender ESP1 and receiver ESP2 must be on the same channel. A router on another channel will break the ESP-NOW link while ESP2 is connected to it.
- Set `BUZZER_PIN` to the actual wire: GPIO13 in the pasted sketch or GPIO25 if that is the physical connection.

## 4. Upload dashboard files to ESP2 LittleFS
Keep `firmware/ESP2/data/index.html` in the ESP2 sketch's `data` directory. Install the LittleFS filesystem uploader appropriate for your Arduino IDE/ESP32 core, select the ESP2 board and port, upload the sketch, then run the ESP32 LittleFS data upload action. A normal sketch upload does not upload LittleFS files automatically.

## 5. Run the laptop backend
From `SIH/backend` in a terminal:

```bash
python -m venv .venv
# Windows: .venv\\Scripts\\activate
# macOS/Linux: source .venv/bin/activate
pip install -r requirements.txt
python app.py
```

Allow inbound TCP port 5000 on the laptop firewall for the local network. Check `http://127.0.0.1:5000/login` on the laptop. The existing development seed users are `admin` / `admin123` and `viewer` / `viewer123`; change them before any real deployment. Use the laptop's LAN IP in `CLOUD_URL`.

## 6. Use the offline dashboard
1. Power ESP2 and ESP1. Open Serial Monitor at 115200 baud.
2. Connect phone/laptop to Wi-Fi `ColdChain-BOX01`, password `coldchain123`.
3. Open `http://192.168.4.1`.
4. Verify that packet number and all sensor values update. The SD card should contain `/telemetry.csv` and `/pending.jsonl`.
5. With no internet/router, records remain in `/pending.jsonl`. When configured Wi-Fi and backend become reachable, ESP2 sends the oldest row and removes it only after HTTP 200/201 acknowledgement. Backend event IDs make retries idempotent.

## 7. Cloud dashboard
Once the backend is reachable, sign in at `http://<LAPTOP_LAN_IP>:5000/login`, then visit `/telemetry` for the full cloud telemetry dashboard. Existing `/admin` and `/user` dashboards remain available for the original project views. Full raw records are available from `/api/telemetry/latest` and `/api/telemetry/history`.

## 8. Cloud deployment later
Deploy the Flask backend and SQLite database to a host that supports persistent storage, HTTPS and background/long-running web services. Update `CLOUD_URL` to the hosted HTTPS endpoint. Do not use a free host that sleeps or erases its filesystem for production monitoring; if it sleeps, ESP2 will keep the queue locally and retry when the service wakes. Protect the API key and replace development passwords/secret key before public deployment.

## Reliability notes
- The RTC is set to firmware compile time only if it reports lost power. Set the DS3231 to the correct local time before relying on timestamps.
- If the microSD fails to mount, ESP2 reports the error. Do not treat the system as safely logging until SD is repaired.
- GPS coordinates are only valid after a satellite fix; zero coordinates in the CSV mean no current fix.
- The SD CSV is the primary log. The JSONL queue is the cloud retry queue. Do not remove either file while the device is operating.
- Test with a spare SD card first and verify that `/telemetry.csv` grows and records remain in `/pending.jsonl` when the backend is offline.
