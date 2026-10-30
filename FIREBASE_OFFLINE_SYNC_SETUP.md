# ColdTrace Firebase + hotspot setup

## Network architecture
- ESP1 and ESP2 both join the 2.4 GHz hotspot `test123` (password `12345678`).
- Both ESP32 boards use ESP-NOW on the current Wi-Fi channel (`peerInfo.channel = 0` on ESP1).
- ESP2 serves `/api/data`, `/api/network`, `/api/rtc`, `/api/rtc/set`, and `/api/telemetry.csv` at its hotspot-assigned IP.
- The laptop joins the same hotspot and runs Flask. Flask polls the live API and syncs every previously-unsynced row from ESP2's MicroSD CSV to Firebase Realtime Database.
- If internet/Firebase is unavailable, the SD CSV remains the source of truth. Flask retries cloud sync every 20 seconds. The Firebase key is deterministic, so retries overwrite the same record instead of creating duplicates.

## 1. Firebase setup
1. Open https://console.firebase.google.com/ and create/select your ColdTrace project.
2. In Project settings, register a Web app if needed and copy its Web API key.
3. Under Build -> Authentication -> Sign-in method, enable Email/Password.
4. Create a dedicated user for this demo under Authentication -> Users. Do not use a personal Google password.
5. Under Build -> Realtime Database, create the database and copy the exact database URL.
6. Set rules so only authenticated users can read/write. For a simple authenticated demo, rules may be:

```json
{
  ".read": "auth != null",
  ".write": "auth != null"
}
```

These rules allow any authenticated user in this Firebase project to access this database; for production, restrict access further. Do not enable public read/write.

## 2. Configure backend
1. Copy `backend/.env.example` to `backend/.env`.
2. Fill in `FIREBASE_API_KEY`, `FIREBASE_DATABASE_URL`, `FIREBASE_EMAIL`, and `FIREBASE_PASSWORD`.
3. Set `COLDTRACE_ESP2_API_URL` to the ESP2 IP printed in Arduino Serial Monitor, e.g. `http://192.168.43.123/api/data`. The IP can change when the phone hotspot restarts.
4. Install dependencies from `backend`:

```powershell
py -3.13 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
python app.py
```

Use Python 3.12 instead if Python 3.13 is not installed.

## 3. Upload firmware
1. Upload `firmware/ESP1_ColdTrace/ESP1_ColdTrace.ino` to ESP1.
2. Upload `firmware/ESP2_ColdTrace/ESP2_ColdTrace.ino` to ESP2.
3. ESP2 requires the same libraries already used by this project: ArduinoJson, RTClib, TinyGPSPlus, Adafruit GFX, and Adafruit SSD1306.
4. In each Serial Monitor (115200 baud), verify both devices connect to `test123` and report the same Wi-Fi channel. ESP2 prints its assigned IP.
5. Test `http://ESP2_IP/api/data`, `http://ESP2_IP/api/network`, and `http://ESP2_IP/api/telemetry.csv` from the laptop browser.

## 4. Demo test
1. Keep both ESP32s and the laptop connected to `test123`. Start Flask.
2. Open `http://127.0.0.1:5000/login` for the local dashboard.
3. Open `http://127.0.0.1:5000/api/cloud-status` to see Firebase sync status.
4. Turn off hotspot internet (leave local hotspot Wi-Fi active if the phone supports it). Confirm `telemetry.csv` continues growing on SD. Cloud sync should report an error but must not stop local logging.
5. Restore internet. Within the configured sync interval, the backend should upload the previously-unsynced CSV rows. Check Firebase Realtime Database -> `telemetry`.

## Important limitations
- The laptop/backend must be running for this version's cloud synchronization to run. ESP2 continues writing to SD while offline; the backend reads and uploads the full CSV when reachable.
- Some phone hotspots isolate clients. If laptop cannot open `http://ESP2_IP/api/data`, check hotspot client isolation or use a router/hotspot that allows clients to communicate.
- Both ESP32s must be on the same 2.4 GHz channel. If ESP-NOW stops after hotspot connection, confirm both serial logs show the same channel.
- This is a demo-oriented sync implementation. For long-running deployment, switch from repeatedly downloading the full CSV to an offset/cursor-based SD queue and use stricter Firebase per-device rules.
- Keep `backend/.env` private. Firebase Web API keys are project identifiers, not sufficient protection by themselves; Firebase Authentication and database rules provide access control.
