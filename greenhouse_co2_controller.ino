#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include "driver/gpio.h"
#include "esp_sleep.h"

//  Settings 
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";
const char* BOT_TOKEN = "";
const char* CHAT_ID   = "";
const char* PHONE     = "";

#define PIN_FAN    25   // ريلاي المروحة
#define PIN_VALVE  27   // ريلاي valve الـ CO2
#define RELAY_ON   HIGH // اقلبيهم لو الريلاي Active-LOW
#define RELAY_OFF  LOW

const int CO2_LOW = 800, CO2_HIGH = 1000;
const int CO2_HIGH_NIGHT = 1500, CO2_ALARM = 2000;
const float DAY_LUX = 1000;
const uint32_t LOOP_MS = 30000;
const uint32_t VALVE_MAX_MS = 120000, VALVE_REST_MS = 300000;
const uint32_t MAX_ACTIVE_MS = 30UL * 60 * 1000;
const uint64_t SLEEP_MIN = 30;
const int REPORT_EVERY = 2;   // تقرير كل دورتين = كل ساعة

RTC_DATA_ATTR uint32_t wakeCount = 0;
RTC_DATA_ATTR bool alarmOn = false;

void setFan(bool on)   { digitalWrite(PIN_FAN,   on ? RELAY_ON : RELAY_OFF); }
void setValve(bool on) { digitalWrite(PIN_VALVE, on ? RELAY_ON : RELAY_OFF); }

//  SCD41 (I2C 0x62) 
#define SCD_ADDR 0x62

uint8_t crc8(const uint8_t* d, int n) {
  uint8_t c = 0xFF;
  for (int i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (c << 1) ^ 0x31 : (c << 1);
  }
  return c;
}

void scdCmd(uint16_t cmd) {
  Wire.beginTransmission(SCD_ADDR);
  Wire.write(cmd >> 8); Wire.write(cmd & 0xFF);
  Wire.endTransmission();
}

void scdInit() {
  scdCmd(0x3F86); delay(500);            // stop periodic measurement
  Wire.beginTransmission(SCD_ADDR);      // اقفلي المعايرة التلقائية (ABC)
  Wire.write(0x24); Wire.write(0x16);
  Wire.write(0x00); Wire.write(0x00); Wire.write(0x81);
  Wire.endTransmission();
  delay(1);
}

int readCO2() {
  scdCmd(0x219D); delay(5200);           // single shot (5 ثواني)
  scdCmd(0xEC05); delay(2);              // read measurement
  if (Wire.requestFrom(SCD_ADDR, 9) != 9) return -1;
  uint8_t b[9];
  for (int i = 0; i < 9; i++) b[i] = Wire.read();
  for (int i = 0; i < 9; i += 3) if (crc8(b + i, 2) != b[i + 2]) return -1;
  return (b[0] << 8) | b[1];
}

//  BH1750 (I2C 0x23) 
float readLux() {
  Wire.beginTransmission(0x23); Wire.write(0x10);
  if (Wire.endTransmission()) return -1;
  delay(180);
  Wire.requestFrom(0x23, 2);
  if (Wire.available() < 2) return -1;
  uint16_t raw = (Wire.read() << 8) | Wire.read();
  return raw / 1.2;
}

//  Telegram 
bool wifiUp() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

bool sendTelegram(const String& msg) {
  if (!wifiUp()) return false;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h;
  h.begin(c, "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage");
  h.addHeader("Content-Type", "application/json");
  int code = h.POST("{\"chat_id\":\"" + String(CHAT_ID) + "\",\"text\":\"" + msg + "\"}");
  h.end();
  return code == 200;
}

//  SMS via A7670E (Serial1: RX=32, TX=33) 
bool waitFor(const char* expect, uint32_t to) {
  String r; uint32_t t = millis();
  while (millis() - t < to) {
    while (Serial1.available()) r += (char)Serial1.read();
    if (r.indexOf(expect) >= 0) return true;
    delay(10);
  }
  return false;
}

bool at(const String& cmd, const char* expect, uint32_t to = 2000) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  return waitFor(expect, to);
}

bool sendSMS(const String& msg) {
  bool ok = false;
  for (int i = 0; i < 5 && !ok; i++) ok = at("AT", "OK", 500);
  if (!ok) return false;
  at("ATE0", "OK"); at("AT+CMGF=1", "OK"); at("AT+CSCS=\"GSM\"", "OK");
  ok = false;
  for (int i = 0; i < 30 && !ok; i++) {
    ok = at("AT+CREG?", "0,1", 800) || at("AT+CREG?", "0,5", 800);
    if (!ok) delay(1000);
  }
  if (!ok) return false;
  if (!at("AT+CMGS=\"" + String(PHONE) + "\"", ">", 5000)) return false;
  Serial1.print(msg); Serial1.write(26);
  return waitFor("+CMGS", 20000);
}

// تليجرام أولاً، وSMS لو فشل أو لو التنبيه حرج
void notify(const String& msg, bool critical) {
  bool ok = sendTelegram(msg);
  if (!ok || critical) sendSMS(msg);
}

//  Main
void setup() {
  Serial.begin(115200);

  // فك تثبيت الـ pins من النوم
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_FAN);
  gpio_hold_dis((gpio_num_t)PIN_VALVE);

  pinMode(PIN_FAN, OUTPUT);
  pinMode(PIN_VALVE, OUTPUT);
  setFan(false); setValve(false);

  Wire.begin(21, 22);
  Serial1.begin(115200, SERIAL_8N1, 32, 33);
  scdInit();
  wakeCount++;

  bool fan = false, valve = false;
  uint8_t calm = 0, fails = 0;
  uint32_t start = millis(), valveOnAt = 0, valveLockUntil = 0;
  int co2 = -1; float lux = -1;

  while (true) {
    //  قياس 
    co2 = readCO2();
    if (co2 < 0) { delay(500); co2 = readCO2(); }
    if (co2 < 0) {
      if (++fails >= 3) {
        notify("ALERT: CO2 sensor not responding, valve closed", true);
        break;
      }
      delay(LOOP_MS);
      continue;
    }
    fails = 0;
    lux = readLux();
    bool day = lux >= DAY_LUX;
    uint32_t now = millis();

    //  المروحة (hysteresis 100 ppm) 
    int fanOn = day ? CO2_HIGH : CO2_HIGH_NIGHT;
    if (co2 > fanOn) fan = true;
    else if (co2 < fanOn - 100) fan = false;

    //  الـ valve (نهاراً فقط، بحد أقصى وراحة) 
    if (!valve && !fan && day && co2 < CO2_LOW && now > valveLockUntil) {
      valve = true; valveOnAt = now;
    } else if (valve && (fan || !day || co2 >= CO2_LOW + 50 || now - valveOnAt > VALVE_MAX_MS)) {
      if (now - valveOnAt > VALVE_MAX_MS) valveLockUntil = now + VALVE_REST_MS;
      valve = false;
    }
    setFan(fan);
    setValve(valve);

    //  تنبيه CO2 عالي (مرة عند الحدوث ومرة عند الرجوع) 
    if (co2 > CO2_ALARM && !alarmOn) {
      alarmOn = true;
      notify("ALARM: CO2 " + String(co2) + " ppm is too high", true);
    } else if (co2 <= CO2_HIGH && alarmOn) {
      alarmOn = false;
      notify("OK: CO2 back to normal, " + String(co2) + " ppm", false);
    }

    //  هل استقر؟ 
    bool settled = !fan && !valve && co2 <= fanOn && (co2 >= CO2_LOW || !day);
    if (settled) { if (++calm >= 2) break; } else calm = 0;

    if (millis() - start > MAX_ACTIVE_MS) {
      notify("WARNING: CO2 not stable after 30 min (" + String(co2) +
             " ppm). Check cylinder, regulator, fan", true);
      break;
    }
    delay(LOOP_MS);
  }

  //  تقرير دوري 
  if (co2 >= 0 && (wakeCount % REPORT_EVERY == 1)) {
    notify("Report: CO2 " + String(co2) + " ppm, light " + String((int)lux) +
           " lux, fan " + String(fan ? "ON" : "OFF") +
           ", valve " + String(valve ? "ON" : "OFF"), false);
  }

  //  كله OFF وتثبيت الحالة قبل النوم 
  setFan(false); setValve(false);
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  gpio_hold_en((gpio_num_t)PIN_FAN);
  gpio_hold_en((gpio_num_t)PIN_VALVE);
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_timer_wakeup(SLEEP_MIN * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
