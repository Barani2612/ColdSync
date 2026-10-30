#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_INA219.h>
#include <Adafruit_SHT31.h>
#include <math.h>

// Phone hotspot Wi-Fi credentials
const char* WIFI_SSID = "test123";
const char* WIFI_PASSWORD = "12345678";

// Wi-Fi connection timeout
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

#define DHT_PIN 15
#define DS18B20_PIN 4
#define INA219_SDA 21
#define INA219_SCL 22
#define VIBRATION_PIN 27
#define LDR_PIN 34
#define ACS712_PIN 35
#define DHT_TYPE DHT11
#define VIBRATION_ACTIVE_LEVEL HIGH

const float TEMP_MIN_SAFE = 2.0f;
const float TEMP_MAX_SAFE = 8.0f;
const int LDR_OPEN_THRESHOLD = 2300;
const int LDR_CLOSE_THRESHOLD = 2900;
const float ACS712_SENSITIVITY_MV_PER_A = 185.0f;
const float ADC_MV_APPROX = 3300.0f;
const int ADC_MAX = 4095;
const uint32_t SEND_INTERVAL_MS = 2000;

DHT dht(DHT_PIN, DHT_TYPE);
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);
Adafruit_INA219 ina219;
Adafruit_SHT31 sht31 = Adafruit_SHT31();

// MUST match ESP2 field-for-field and in exactly the same order.
typedef struct __attribute__((packed)) {
  uint32_t packetNumber;
  uint32_t uptimeMs;
  float ambientTempC;
  float humidityPct;          // DHT11 atmospheric humidity
  float sht31HumidityPct;     // SHT31 internal compartment humidity
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
static_assert(sizeof(ColdTracePacket) == 58, "ESP1 packet must be 58 bytes; keep ESP2 structure identical");

// Broadcast. For production, replace with ESP2's STA MAC address.
uint8_t receiverAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
esp_now_peer_info_t peerInfo = {};
ColdTracePacket packet = {};
bool inaReady=false, sht31Ready=false, espNowReady=false, lidOpenState=false;
float acsZeroMv=1650.0f;
uint32_t packetCounter=0, lastSendMs=0;

int readFilteredADC(int pin, int samples=30) {
  uint32_t total=0;
  for(int i=0;i<samples;i++){ total += analogRead(pin); delayMicroseconds(100); }
  return total/samples;
}
float readADCMillivoltsApprox(int pin) {
  return ((float)readFilteredADC(pin)/ADC_MAX)*ADC_MV_APPROX;
}
void calibrateACS712() {
  Serial.println("[ACS712] Keep measured load OFF during zero calibration.");
  float total=0;
  for(int i=0;i<60;i++){ total += readADCMillivoltsApprox(ACS712_PIN); delay(10); }
  acsZeroMv=total/60.0f;
  Serial.printf("[ACS712] Baseline: %.1f mV\n", acsZeroMv);
}
void OnDataSent(const wifi_tx_info_t *txInfo, esp_now_send_status_t status) {
  Serial.printf("[ESP-NOW] Delivery status: %s\n", status==ESP_NOW_SEND_SUCCESS?"SUCCESS":"FAILED");
}
bool initializeESPNow() {
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setAutoReconnect(true); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  Serial.printf("[ESP1] Connecting to hotspot '%s'",WIFI_SSID);
  unsigned long start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<WIFI_CONNECT_TIMEOUT_MS){delay(300);Serial.print(".");}
  Serial.println();
  if(WiFi.status()==WL_CONNECTED) Serial.printf("[ESP1] Wi-Fi connected; IP=%s channel=%u\n",WiFi.localIP().toString().c_str(),(unsigned)WiFi.channel());
  else Serial.println("[ESP1] Hotspot not available; ESP-NOW will attempt offline operation on the current channel.");
  Serial.print("[ESP1] STA MAC: "); Serial.println(WiFi.macAddress());
  if(esp_now_init()!=ESP_OK){ Serial.println("[ERROR] ESP-NOW init failed"); return false; }
  esp_now_register_send_cb(OnDataSent);
  memcpy(peerInfo.peer_addr, receiverAddress, 6);
  peerInfo.channel=0; // Use the current radio channel; both ESPs join the same hotspot.
  peerInfo.encrypt=false;
  if(esp_now_add_peer(&peerInfo)!=ESP_OK){ Serial.println("[ERROR] Could not add ESP-NOW peer"); return false; }
  return true;
}
void readAllSensors() {
  packet.dhtValid=false; packet.ds1Valid=false; packet.ds2Valid=false;
  packet.sht31Valid=false; packet.inaValid=inaReady;
  float h=dht.readHumidity(), at=dht.readTemperature();
  if(!isnan(h)&&!isnan(at)&&h>=0&&h<=100){ packet.humidityPct=h; packet.ambientTempC=at; packet.dhtValid=true; }
  if(sht31Ready){ float sh=sht31.readHumidity(); if(!isnan(sh)&&sh>=0&&sh<=100){packet.sht31HumidityPct=sh;packet.sht31Valid=true;} }
  ds18b20.requestTemperatures();
  float t1=ds18b20.getTempCByIndex(0), t2=ds18b20.getTempCByIndex(1);
  if(t1!=DEVICE_DISCONNECTED_C&&t1>=-55&&t1<=125){packet.coreTemp1C=t1;packet.ds1Valid=true;}
  if(t2!=DEVICE_DISCONNECTED_C&&t2>=-55&&t2<=125){packet.coreTemp2C=t2;packet.ds2Valid=true;}
  if(inaReady){ packet.inaBusVoltageV=ina219.getBusVoltage_V(); packet.inaShuntVoltageMv=ina219.getShuntVoltage_mV(); packet.inaCurrentMa=ina219.getCurrent_mA(); packet.inaPowerMw=ina219.getPower_mW(); }
  float deltaMv=readADCMillivoltsApprox(ACS712_PIN)-acsZeroMv;
  float amps=deltaMv/ACS712_SENSITIVITY_MV_PER_A; if(fabsf(amps)<0.04f) amps=0; packet.acsCurrentA=amps;
  packet.ldrRaw=readFilteredADC(LDR_PIN);
  if(packet.ldrRaw<LDR_OPEN_THRESHOLD) lidOpenState=true; else if(packet.ldrRaw>LDR_CLOSE_THRESHOLD) lidOpenState=false;
  packet.lidOpen=lidOpenState;
  packet.vibrationDetected=(digitalRead(VIBRATION_PIN)==VIBRATION_ACTIVE_LEVEL);
  bool t1Bad=packet.ds1Valid&&(packet.coreTemp1C<TEMP_MIN_SAFE||packet.coreTemp1C>TEMP_MAX_SAFE);
  bool t2Bad=packet.ds2Valid&&(packet.coreTemp2C<TEMP_MIN_SAFE||packet.coreTemp2C>TEMP_MAX_SAFE);
  packet.temperatureAlarm=t1Bad||t2Bad;
  packet.uptimeMs=millis();
}
void printSensorReport() {
  Serial.println("\n========== COLDTRACE ESP1 SENSOR REPORT ==========");
  Serial.printf("DHT11 valid: %s | Atmospheric temp: %.2f C | Atmospheric RH: %.2f %%\n",packet.dhtValid?"YES":"NO",packet.ambientTempC,packet.humidityPct);
  Serial.printf("SHT31 valid: %s | Internal RH: %.2f %%\n",packet.sht31Valid?"YES":"NO",packet.sht31HumidityPct);
  Serial.printf("DS18B20 #1: %.2f C [%s] | #2: %.2f C [%s]\n",packet.coreTemp1C,packet.ds1Valid?"VALID":"INVALID",packet.coreTemp2C,packet.ds2Valid?"VALID":"INVALID");
  Serial.printf("INA219: %s | Bus %.3f V | Current %.3f mA | Power %.3f mW\n",packet.inaValid?"VALID":"INVALID",packet.inaBusVoltageV,packet.inaCurrentMa,packet.inaPowerMw);
  Serial.printf("ACS712: %.3f A | LDR: %u | Lid: %s | Vibration: %s\n",packet.acsCurrentA,packet.ldrRaw,packet.lidOpen?"OPEN":"CLOSED",packet.vibrationDetected?"YES":"NO");
  Serial.printf("Temperature alarm: %s | ESP-NOW: %s | Packet: %lu | Size: %u bytes\n",packet.temperatureAlarm?"ACTIVE":"NORMAL",espNowReady?"READY":"NO",(unsigned long)packet.packetNumber,(unsigned)sizeof(packet));
}
void sendTelemetry() {
  packet.packetNumber=++packetCounter;
  esp_err_t result=esp_now_send(receiverAddress,(const uint8_t*)&packet,sizeof(packet));
  if(result!=ESP_OK) Serial.printf("[ESP-NOW] Queue error: %d\n",(int)result);
}
void setup() {
  Serial.begin(115200); delay(1000);
  pinMode(VIBRATION_PIN,INPUT); analogReadResolution(12); analogSetAttenuation(ADC_11db);
  dht.begin(); ds18b20.begin();
  Serial.printf("[DS18B20] Devices found: %d\n",ds18b20.getDeviceCount());
  Wire.begin(INA219_SDA,INA219_SCL);
  inaReady=ina219.begin(); Serial.printf("[INA219] %s\n",inaReady?"Initialized":"NOT DETECTED");
  sht31Ready=sht31.begin(0x44); Serial.printf("[SHT31] %s at I2C address 0x44\n",sht31Ready?"Initialized":"NOT DETECTED");
  calibrateACS712(); espNowReady=initializeESPNow();
  Serial.println("[ColdTrace] ESP1 ready; Serial Monitor 115200 baud.");
}
void loop() {
  if(millis()-lastSendMs>=SEND_INTERVAL_MS){ lastSendMs=millis(); readAllSensors(); printSensorReport(); if(espNowReady) sendTelemetry(); }
}
