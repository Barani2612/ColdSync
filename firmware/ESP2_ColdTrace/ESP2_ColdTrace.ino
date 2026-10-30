#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TinyGPSPlus.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "RTClib.h"
#include <ArduinoJson.h>

#define SD_CS_PIN 5
#define BUZZER_PIN 13
// Six status LEDs: each LED requires its own 220-330 ohm series resistor.
#define LED_GREEN_1 25
#define LED_GREEN_2 26
#define LED_RED_1   32
#define LED_RED_2   33
#define LED_BLUE_1  27
#define LED_BLUE_2  14
#define GPS_RX 16
#define GPS_TX 17
#define I2C_SDA 21
#define I2C_SCL 22
#define WIFI_SSID "test123"
#define WIFI_PASSWORD "12345678"
#define WIFI_CONNECT_TIMEOUT_MS 12000UL
#define TEMP_MIN_SAFE 2.0f
#define TEMP_MAX_SAFE 8.0f
#define GPS_FIX_MAX_AGE_MS 5000UL
#define MOVING_THRESHOLD_KMPH 2.0f

WebServer server(80);
Adafruit_SSD1306 display(128,64,&Wire,-1);
RTC_DS3231 rtc;
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

// MUST match ESP1 exactly, field-for-field and in the same order.
typedef struct __attribute__((packed)) {
  uint32_t packetNumber;
  uint32_t uptimeMs;
  float ambientTempC;
  float humidityPct;          // DHT11 atmospheric RH
  float sht31HumidityPct;     // SHT31 internal RH
  bool sht31Valid;
  float coreTemp1C;
  float coreTemp2C;
  float inaBusVoltageV;
  float inaShuntVoltageMv;
  float inaCurrentMa;
  float inaPowerMw;
  float acsCurrentA;
  uint16_t ldrRaw;
  bool lidOpen;
  bool vibrationDetected;
  bool temperatureAlarm;
  bool dhtValid;
  bool ds1Valid;
  bool ds2Valid;
  bool inaValid;
} ColdTracePacket;
static_assert(sizeof(ColdTracePacket)==58,"ESP1/ESP2 packet must be 58 bytes");

ColdTracePacket incomingReadings={};
portMUX_TYPE packetMux=portMUX_INITIALIZER_UNLOCKED;
volatile unsigned long lastPacketTime=0;
volatile bool newPacketAvailable=false;
bool sdCardAvailable=false, rtcAvailable=false, oledAvailable=false, littleFSAvailable=false;
bool excursion=false, buzzerActive=false;
bool temperatureStable=false;
float stableTemps[30];
uint8_t stableCount=0, stableIndex=0;
unsigned long lastStabilitySample=0;
unsigned long lastDisplayUpdate=0,lastSerialUpdate=0,lastLogRetry=0;
uint8_t displayPage=0;
String latestTimestamp="RTC_UNAVAILABLE";

String getTimestamp(){
  if(!rtcAvailable) return "RTC_UNAVAILABLE";
  DateTime t=rtc.now(); char buf[24];
  snprintf(buf,sizeof(buf),"%04d-%02d-%02d %02d:%02d:%02d",t.year(),t.month(),t.day(),t.hour(),t.minute(),t.second());
  return String(buf);
}
bool packetIsFresh(){ unsigned long t=lastPacketTime; return t!=0&&(millis()-t<6000); }
bool gpsFixFresh(){ return gps.location.isValid()&&gps.location.age()<GPS_FIX_MAX_AGE_MS&&gps.speed.isValid()&&gps.speed.age()<GPS_FIX_MAX_AGE_MS; }
void evaluateAlarm(){
  bool fresh=packetIsFresh();
  bool tempBad=(incomingReadings.ds1Valid&&(incomingReadings.coreTemp1C<TEMP_MIN_SAFE||incomingReadings.coreTemp1C>TEMP_MAX_SAFE)) || (incomingReadings.ds2Valid&&(incomingReadings.coreTemp2C<TEMP_MIN_SAFE||incomingReadings.coreTemp2C>TEMP_MAX_SAFE));
  excursion=!fresh||tempBad||incomingReadings.temperatureAlarm||incomingReadings.vibrationDetected||incomingReadings.lidOpen;
  buzzerActive=excursion;
  digitalWrite(BUZZER_PIN,buzzerActive&&(millis()%1000<500)?HIGH:LOW);
}
float statusCoreTemperature() {
  bool v1 = incomingReadings.ds1Valid && isfinite(incomingReadings.coreTemp1C);
  bool v2 = incomingReadings.ds2Valid && isfinite(incomingReadings.coreTemp2C);
  if (v1 && v2) return (incomingReadings.coreTemp1C + incomingReadings.coreTemp2C) / 2.0f;
  if (v1) return incomingReadings.coreTemp1C;
  if (v2) return incomingReadings.coreTemp2C;
  return NAN;
}

void updateTemperatureStability(unsigned long now) {
  if (now - lastStabilitySample < 2000) return;
  lastStabilitySample = now;
  float t = statusCoreTemperature();
  if (!packetIsFresh() || !isfinite(t) || t < TEMP_MIN_SAFE || t > TEMP_MAX_SAFE) {
    stableCount = 0; stableIndex = 0; temperatureStable = false; return;
  }
  stableTemps[stableIndex] = t;
  stableIndex = (stableIndex + 1) % 30;
  if (stableCount < 30) stableCount++;
  if (stableCount < 30) { temperatureStable = false; return; }
  float lo = stableTemps[0], hi = stableTemps[0];
  for (uint8_t i=1; i<30; i++) { if (stableTemps[i] < lo) lo=stableTemps[i]; if (stableTemps[i] > hi) hi=stableTemps[i]; }
  temperatureStable = (hi - lo) <= 0.5f; // stable if 60-second range is <= 0.5 C
}

void updateStatusLEDs() {
  const bool blink = ((millis() / 500) % 2) == 0;
  const float t = statusCoreTemperature();
  const bool inRange = packetIsFresh() && isfinite(t) && t >= TEMP_MIN_SAFE && t <= TEMP_MAX_SAFE;
  bool redOn = excursion && blink;
  bool greenOn = !excursion && inRange && blink;
  bool blueOn = !excursion && inRange && temperatureStable && blink;
  // Blue may blink alongside green when the in-range temperature is stable.
  digitalWrite(LED_RED_1, redOn); digitalWrite(LED_RED_2, redOn);
  digitalWrite(LED_GREEN_1, greenOn); digitalWrite(LED_GREEN_2, greenOn);
  digitalWrite(LED_BLUE_1, blueOn); digitalWrite(LED_BLUE_2, blueOn);
}

void createCsvHeader(){
  if(!sdCardAvailable||SD.exists("/telemetry.csv")) return;
  File f=SD.open("/telemetry.csv",FILE_WRITE); if(!f){Serial.println("[SD] Could not create telemetry.csv");return;}
  f.println("Timestamp,PacketNumber,ESP1UptimeMs,AmbientTempC,DHT11AmbientHumidityPct,SHT31InternalHumidityPct,SHT31Valid,CoreTemp1C,CoreTemp2C,INA219BusVoltageV,INA219ShuntVoltageMv,INA219CurrentMa,INA219PowerMw,ACS712CurrentA,LDRRaw,LidOpen,Vibration,TemperatureAlarm,DHTValid,DS1Valid,DS2Valid,INAValid,GPSValid,Latitude,Longitude,SpeedKmph,Satellites,Moving,Excursion"); f.close();
}
void logPacketToSD(const ColdTracePacket &p){
  if(!sdCardAvailable)return;
  File f=SD.open("/telemetry.csv",FILE_APPEND); if(!f){Serial.println("[SD] append failed");sdCardAvailable=false;return;}
  bool fix=gpsFixFresh(); double lat=fix?gps.location.lat():0.0, lng=fix?gps.location.lng():0.0; double speed=fix?gps.speed.kmph():0.0; bool moving=fix&&speed>MOVING_THRESHOLD_KMPH;
  f.printf("%s,%lu,%lu,%.3f,%.3f,%.3f,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.6f,%.6f,%.2f,%lu,%u,%u\n",
    getTimestamp().c_str(),(unsigned long)p.packetNumber,(unsigned long)p.uptimeMs,p.ambientTempC,p.humidityPct,p.sht31HumidityPct,p.sht31Valid?1:0,p.coreTemp1C,p.coreTemp2C,p.inaBusVoltageV,p.inaShuntVoltageMv,p.inaCurrentMa,p.inaPowerMw,p.acsCurrentA,(unsigned)p.ldrRaw,p.lidOpen?1:0,p.vibrationDetected?1:0,p.temperatureAlarm?1:0,p.dhtValid?1:0,p.ds1Valid?1:0,p.ds2Valid?1:0,p.inaValid?1:0,fix?1:0,lat,lng,speed,(unsigned long)gps.satellites.value(),moving?1:0,excursion?1:0);
  f.flush();f.close();
}
void OnDataRecv(const esp_now_recv_info *info,const uint8_t *data,int len){
  if(len!=(int)sizeof(ColdTracePacket)){Serial.printf("[ESP-NOW] Rejected packet: %d bytes; expected %u\n",len,(unsigned)sizeof(ColdTracePacket));return;}
  portENTER_CRITICAL(&packetMux); memcpy(&incomingReadings,data,sizeof(incomingReadings)); lastPacketTime=millis();newPacketAvailable=true;portEXIT_CRITICAL(&packetMux);
}
void printTelemetryReport(){
  Serial.println("\n================ COLDTRACE LIVE TELEMETRY ================");
  Serial.printf("ESP-NOW: %s | Packet: %lu | Age: %lu ms\n",packetIsFresh()?"RECEIVING":"NO RECENT DATA",(unsigned long)incomingReadings.packetNumber,lastPacketTime?(unsigned long)(millis()-lastPacketTime):0UL);
  Serial.printf("RTC timestamp: %s | RTC: %s\n",latestTimestamp.c_str(),rtcAvailable?"READY":"ERROR");
  Serial.printf("Core 1: %.2f C [%s] | Core 2: %.2f C [%s]\n",incomingReadings.coreTemp1C,incomingReadings.ds1Valid?"VALID":"INVALID",incomingReadings.coreTemp2C,incomingReadings.ds2Valid?"VALID":"INVALID");
  Serial.printf("DHT11 atmospheric: %.2f C, %.2f %% RH [%s]\n",incomingReadings.ambientTempC,incomingReadings.humidityPct,incomingReadings.dhtValid?"VALID":"INVALID");
  Serial.printf("SHT31 internal RH: %.2f %% [%s]\n",incomingReadings.sht31HumidityPct,incomingReadings.sht31Valid?"VALID":"INVALID");
  Serial.printf("INA219: %.3f V, %.3f mA, %.3f mW [%s] | ACS712: %.3f A\n",incomingReadings.inaBusVoltageV,incomingReadings.inaCurrentMa,incomingReadings.inaPowerMw,incomingReadings.inaValid?"VALID":"INVALID",incomingReadings.acsCurrentA);
  Serial.printf("LDR: %u | Lid: %s | Vibration: %s\n",(unsigned)incomingReadings.ldrRaw,incomingReadings.lidOpen?"OPEN":"CLOSED",incomingReadings.vibrationDetected?"DETECTED":"NORMAL");
  if(gpsFixFresh()) Serial.printf("GPS: %.6f, %.6f | Speed: %.2f km/h | Satellites: %lu | %s\n",gps.location.lat(),gps.location.lng(),gps.speed.kmph(),(unsigned long)gps.satellites.value(),gps.speed.kmph()>MOVING_THRESHOLD_KMPH?"MOVING":"STATIONARY"); else Serial.printf("GPS: WAITING FOR FRESH FIX | Satellites: %lu\n",(unsigned long)gps.satellites.value());
  Serial.printf("Excursion: %s | Buzzer: %s | SD: %s | OLED: %s\n",excursion?"ALERT":"NORMAL",buzzerActive?"ACTIVE":"OFF",sdCardAvailable?"READY":"ERROR",oledAvailable?"READY":"ERROR");
  Serial.println("==========================================================");
}
void updateOLED(){
  if(!oledAvailable)return; display.clearDisplay();display.setTextColor(SSD1306_WHITE);display.setTextSize(1);
  if(displayPage==0){display.setCursor(0,0);display.println("COLDTRACE | LIVE");display.printf("Core 1: %.2f C\n",incomingReadings.coreTemp1C);display.printf("Core 2: %.2f C\n",incomingReadings.coreTemp2C);display.printf("DHT RH: %.1f%%\n",incomingReadings.humidityPct);display.printf("SHT RH: %.1f%%\n",incomingReadings.sht31HumidityPct);display.print(excursion?"!!! ALERT !!!":"STATUS: NORMAL");}
  else if(displayPage==1){display.setCursor(0,0);display.println("GPS TRACKING");if(gpsFixFresh()){display.printf("Lat: %.4f\n",gps.location.lat());display.printf("Lon: %.4f\n",gps.location.lng());display.printf("Speed: %.1f km/h\n",gps.speed.kmph());display.println(gps.speed.kmph()>MOVING_THRESHOLD_KMPH?"MOVING":"STATIONARY");}else display.println("WAITING FOR FIX");display.printf("Sats: %lu",(unsigned long)gps.satellites.value());}
  else{display.setCursor(0,0);display.println("HUB STATUS");display.printf("RTC: %s\n",rtcAvailable?"OK":"ERROR");display.printf("SD: %s\n",sdCardAvailable?"OK":"ERROR");display.printf("Link: %s\n",packetIsFresh()?"OK":"LOST");display.printf("Lid: %s\n",incomingReadings.lidOpen?"OPEN":"CLOSED");display.printf("Vibration: %s\n",incomingReadings.vibrationDetected?"YES":"NO");}
  display.display();displayPage=(displayPage+1)%3;
}
void configureWebServer(){
  server.on("/api/data",HTTP_GET,[](){
    JsonDocument doc;
    doc["device"]="ColdTrace-BOX01"; doc["timestamp"]=latestTimestamp;
    if(rtcAvailable){DateTime t=rtc.now();char iso[25];snprintf(iso,sizeof(iso),"%04d-%02d-%02dT%02d:%02d:%02d",t.year(),t.month(),t.day(),t.hour(),t.minute(),t.second());doc["rtc_timestamp"]=iso;}else doc["rtc_timestamp"]="";
    doc["rtc_available"]=rtcAvailable;doc["temperature_stable"]=temperatureStable;doc["led_status"]=excursion?"RED_EXCURSION":(temperatureStable?"GREEN_BLUE_STABLE":"GREEN_IN_RANGE");doc["packet_number"]=incomingReadings.packetNumber;doc["uptime_ms"]=incomingReadings.uptimeMs;doc["connected"]=packetIsFresh();doc["packet_age_ms"]=lastPacketTime?(long)(millis()-lastPacketTime):-1;
    doc["core1"]=incomingReadings.coreTemp1C;doc["core2"]=incomingReadings.coreTemp2C;doc["ambient"]=incomingReadings.ambientTempC;
    doc["hum"]=incomingReadings.humidityPct;doc["dht_humidity_pct"]=incomingReadings.humidityPct;doc["ambient_humidity_pct"]=incomingReadings.humidityPct;doc["internal_humidity_pct"]=incomingReadings.sht31HumidityPct;doc["sht31_humidity_pct"]=incomingReadings.sht31HumidityPct;
    doc["dht_valid"]=incomingReadings.dhtValid;doc["sht31_valid"]=incomingReadings.sht31Valid;doc["ds1_valid"]=incomingReadings.ds1Valid;doc["ds2_valid"]=incomingReadings.ds2Valid;
    doc["ina_bus_voltage_v"]=incomingReadings.inaBusVoltageV;doc["ina_shunt_voltage_mv"]=incomingReadings.inaShuntVoltageMv;doc["ina_current_ma"]=incomingReadings.inaCurrentMa;doc["power_mw"]=incomingReadings.inaPowerMw;doc["acs_current_a"]=incomingReadings.acsCurrentA;doc["ina_valid"]=incomingReadings.inaValid;
    doc["ldr"]=incomingReadings.ldrRaw;doc["lid_open"]=incomingReadings.lidOpen;doc["vib"]=incomingReadings.vibrationDetected;doc["temperature_alarm"]=incomingReadings.temperatureAlarm;doc["excursion"]=excursion;
    bool fix=gpsFixFresh();double lat=fix?gps.location.lat():0.0,lng=fix?gps.location.lng():0.0,speed=fix?gps.speed.kmph():0.0;bool moving=fix&&speed>MOVING_THRESHOLD_KMPH;
    doc["gps_valid"]=fix;doc["lat"]=lat;doc["lng"]=lng;doc["latitude"]=lat;doc["longitude"]=lng;doc["gps_speed_kmph"]=speed;doc["moving"]=moving;doc["movement_status"]=!fix?"GPS signal unavailable":(moving?"MOVING":"STATIONARY");doc["gps_age_ms"]=gps.location.age();doc["satellites"]=gps.satellites.value();
    doc["sd_available"]=sdCardAvailable;doc["oled_available"]=oledAvailable;doc["buzzer_active"]=buzzerActive;
    String output;serializeJson(doc,output);server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",output);
  });
  // Set the physical DS3231 clock from the trusted laptop/backend local time.
  // The backend only reports success after this endpoint reads the written time back.
  server.on("/api/rtc/set",HTTP_POST,[](){
    if(!rtcAvailable){server.send(503,"application/json","{\"ok\":false,\"error\":\"DS3231 not available\"}");return;}
    JsonDocument doc;
    DeserializationError err=deserializeJson(doc,server.arg("plain"));
    if(err){server.send(400,"application/json","{\"ok\":false,\"error\":\"Invalid JSON\"}");return;}
    int year=doc["year"]|0, month=doc["month"]|0, day=doc["day"]|0;
    int hour=doc["hour"]|-1, minute=doc["minute"]|-1, second=doc["second"]|-1;
    if(year<2024||year>2099||month<1||month>12||day<1||day>31||hour<0||hour>23||minute<0||minute>59||second<0||second>59){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"Date/time fields out of range\"}");return;
    }
    rtc.adjust(DateTime(year,month,day,hour,minute,second));
    DateTime check=rtc.now();
    char iso[25];snprintf(iso,sizeof(iso),"%04d-%02d-%02dT%02d:%02d:%02d",check.year(),check.month(),check.day(),check.hour(),check.minute(),check.second());
    bool verified=check.year()==year&&check.month()==month&&check.day()==day&&check.hour()==hour&&check.minute()==minute&&abs(check.second()-second)<=1;
    JsonDocument reply;reply["ok"]=verified;reply["rtc_timestamp"]=iso;reply["verified"]=verified;
    if(!verified)reply["error"]="RTC read-back did not match requested time";
    String output;serializeJson(reply,output);server.send(verified?200:500,"application/json",output);
  });
  server.on("/api/rtc",HTTP_GET,[](){
    JsonDocument doc;doc["rtc_available"]=rtcAvailable;
    if(rtcAvailable){DateTime t=rtc.now();char iso[25];snprintf(iso,sizeof(iso),"%04d-%02d-%02dT%02d:%02d:%02d",t.year(),t.month(),t.day(),t.hour(),t.minute(),t.second());doc["rtc_timestamp"]=iso;}
    String output;serializeJson(doc,output);server.send(200,"application/json",output);
  });
  server.on("/api/telemetry.csv",HTTP_GET,[](){
    if(!sdCardAvailable || !SD.exists("/telemetry.csv")) { server.send(503,"application/json","{\"ok\":false,\"error\":\"SD telemetry.csv unavailable\"}"); return; }
    File f=SD.open("/telemetry.csv",FILE_READ);
    if(!f) { server.send(500,"application/json","{\"ok\":false,\"error\":\"Could not open telemetry.csv\"}"); return; }
    server.sendHeader("Cache-Control","no-store");
    server.streamFile(f,"text/csv");
    f.close();
  });
  server.on("/api/network",HTTP_GET,[](){
    JsonDocument doc; doc["wifi_connected"]=(WiFi.status()==WL_CONNECTED); doc["ip"]=WiFi.localIP().toString(); doc["ssid"]=WiFi.SSID(); doc["channel"]=WiFi.channel(); doc["esp_now_receiving"]=packetIsFresh();
    String output; serializeJson(doc,output); server.send(200,"application/json",output);
  });
  server.on("/api/health",HTTP_GET,[](){JsonDocument doc;doc["status"]="running";doc["esp_now_receiving"]=packetIsFresh();doc["sd_available"]=sdCardAvailable;doc["rtc_available"]=rtcAvailable;doc["gps_valid"]=gpsFixFresh();String output;serializeJson(doc,output);server.send(200,"application/json",output);});
  server.serveStatic("/",LittleFS,"/");server.begin();
}
void setup(){
  Serial.begin(115200);delay(500);Serial.println("\nStarting ColdTrace ESP2 Hub...");Serial.printf("Expected ESP-NOW packet: %u bytes\n",(unsigned)sizeof(ColdTracePacket));
  pinMode(BUZZER_PIN,OUTPUT);digitalWrite(BUZZER_PIN,LOW); pinMode(LED_GREEN_1,OUTPUT);pinMode(LED_GREEN_2,OUTPUT);pinMode(LED_RED_1,OUTPUT);pinMode(LED_RED_2,OUTPUT);pinMode(LED_BLUE_1,OUTPUT);pinMode(LED_BLUE_2,OUTPUT);digitalWrite(LED_GREEN_1,LOW);digitalWrite(LED_GREEN_2,LOW);digitalWrite(LED_RED_1,LOW);digitalWrite(LED_RED_2,LOW);digitalWrite(LED_BLUE_1,LOW);digitalWrite(LED_BLUE_2,LOW);Wire.begin(I2C_SDA,I2C_SCL);
  oledAvailable=display.begin(SSD1306_SWITCHCAPVCC,0x3C);if(oledAvailable){display.clearDisplay();display.setTextColor(SSD1306_WHITE);display.setTextSize(1);display.setCursor(0,0);display.println("COLDTRACE");display.println("Starting hub...");display.display();}else Serial.println("[OLED] Not detected");
  rtcAvailable=rtc.begin();if(rtcAvailable){if(rtc.lostPower()){Serial.println("[RTC] Lost-power flag set; setting compile time as fallback.");rtc.adjust(DateTime(F(__DATE__),F(__TIME__)));}Serial.printf("[RTC] %s\n",getTimestamp().c_str());}else Serial.println("[RTC] DS3231 not detected");
  gpsSerial.begin(9600,SERIAL_8N1,GPS_RX,GPS_TX);Serial.println("[GPS] UART2 started at 9600 baud");
  SPI.begin();sdCardAvailable=SD.begin(SD_CS_PIN,SPI);if(sdCardAvailable){createCsvHeader();Serial.println("[SD] Ready");}else Serial.println("[SD] Mount failed");
  // false prevents silently formatting/reinitializing an existing LittleFS volume.
  littleFSAvailable=LittleFS.begin(false);if(!littleFSAvailable)Serial.println("[LittleFS] Mount failed (no formatting performed)");
  // Connect ESP2 to the same 2.4 GHz phone hotspot as the laptop and ESP1.
  // ESP-NOW must share the AP's current channel, so initialize ESP-NOW after Wi-Fi setup.
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setAutoReconnect(true); WiFi.begin(WIFI_SSID,WIFI_PASSWORD); if (WiFi.status() == WL_CONNECTED) {
  Serial.print("ESP2 WiFi IP: ");
  Serial.println(WiFi.localIP());
} else {
  Serial.println("ESP2 WiFi: NOT CONNECTED");
}

  Serial.printf("[Wi-Fi] Connecting to hotspot '%s'",WIFI_SSID);
  unsigned long wifiStart=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-wifiStart<WIFI_CONNECT_TIMEOUT_MS){delay(300);Serial.print(".");}
  Serial.println();
  if(WiFi.status()==WL_CONNECTED) Serial.printf("[Wi-Fi] Connected. ESP2 IP: %s | Channel: %u\n",WiFi.localIP().toString().c_str(),(unsigned)WiFi.channel());
  else Serial.println("[Wi-Fi] Hotspot not available. Continuing offline; local API will be unavailable until a network connection is made.");
  if(esp_now_init()!=ESP_OK){Serial.println("[ESP-NOW] Initialization failed");return;}if(esp_now_register_recv_cb(OnDataRecv)!=ESP_OK)Serial.println("[ESP-NOW] Callback registration failed");else Serial.println("[ESP-NOW] Receiver ready");
  configureWebServer();Serial.println("[WEB] API started; use the ESP2 IP printed above, then /api/data or /api/telemetry.csv");
}
void loop(){
  server.handleClient();while(gpsSerial.available()>0)gps.encode(gpsSerial.read());
  ColdTracePacket snap;bool ready=false;portENTER_CRITICAL(&packetMux);if(newPacketAvailable){memcpy(&snap,&incomingReadings,sizeof(snap));newPacketAvailable=false;ready=true;}portEXIT_CRITICAL(&packetMux);
  evaluateAlarm();updateTemperatureStability(millis());updateStatusLEDs();latestTimestamp=getTimestamp();if(ready)logPacketToSD(snap);
  unsigned long now=millis();if(!sdCardAvailable&&now-lastLogRetry>=10000){lastLogRetry=now;if(SD.begin(SD_CS_PIN,SPI)){sdCardAvailable=true;createCsvHeader();Serial.println("[SD] Recovered");}}
  if(now-lastSerialUpdate>=2000){lastSerialUpdate=now;printTelemetryReport();}if(now-lastDisplayUpdate>=2000){lastDisplayUpdate=now;updateOLED();}delay(2);
}
