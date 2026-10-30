# ColdTrace RTC + portable backend fix

## What this patch changes
- ESP2 firmware adds `POST /api/rtc/set` and `GET /api/rtc` for the physical DS3231.
- Flask adds `POST /api/rtc/sync`. It takes the laptop/backend's local system time, sends it to ESP2, and reports success only when ESP2 reads the written time back.
- The live dashboard synchronizes the RTC when it opens and retries briefly; the RTC button can retry manually.
- The dashboard prefers the current ESP2 payload over an older SQLite row for live fields.
- Sensor telemetry continues to be stored if the optional scikit-learn model fails to load. The prediction is left unavailable rather than fabricated.
- The project archive deliberately excludes the old `.venv`, which is machine-specific and may contain incompatible compiled packages.

## Required firmware
Upload `firmware/ESP2_ColdTrace/ESP2_ColdTrace.ino` to the ESP2 hub. The sketch uses:
- Wi-Fi AP name: `Cold Chain Box`
- Password: `coldchain123`
- AP IP: `192.168.4.1`
- Data API: `http://192.168.4.1/api/data`

Do not upload only the ESP1 sketch. The RTC write route belongs on ESP2, where the DS3231 is connected.

## Libraries for ESP2
Install the libraries used by the sketch in Arduino IDE Library Manager, including RTClib by Adafruit, ArduinoJson 7, Adafruit SSD1306, Adafruit GFX Library, and TinyGPSPlus. ESP32 core provides WiFi, WebServer, ESP-NOW, LittleFS, SD, SPI, and Wire.

## Run on any Windows laptop
1. Power ESP1 and ESP2.
2. Connect the laptop to Wi-Fi `Cold Chain Box` (password `coldchain123`).
3. Confirm `http://192.168.4.1/api/data` returns JSON. `rtc_available` should be true.
4. Open a terminal in `SIH/backend` and run:

   ```powershell
   py -3.13 -m venv .venv
   .\.venv\Scripts\Activate.ps1
   python -m pip install --upgrade pip
   python -m pip install -r requirements.txt
   python app.py
   ```

   Python 3.12 or 3.13 is recommended for compatibility with scientific Python wheels. Do not copy a virtual environment from another computer.
5. Open `http://127.0.0.1:5000/login`. The dashboard should sync the RTC automatically. Default development accounts are `admin` / `admin123` and `viewer` / `viewer123`.
6. Check `http://127.0.0.1:5000/api/bridge-status`. If `connected` is false, inspect `last_error`. If RTC sync fails, open `http://192.168.4.1/api/rtc` and confirm that the ESP2 firmware was updated.

## Important time-source limitation
The DS3231 keeps time across power loss only while its backup cell is healthy and the clock has previously been set. When the backup is lost, the ESP32 cannot know the correct real-world time from power alone. This patch sets the physical RTC automatically when the dashboard/backend is started and connected to ESP2, using the laptop's system clock. For accurate timestamps, enable automatic date/time on the laptop. If the clock must recover before any laptop connects, an internet/NTP time source or another reliable boot-time source must be configured on ESP2.
