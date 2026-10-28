// =============================================================================
// IoT Supercap - Firmware ESP32-C3 Super Mini (versi API sendiri)
//
// Alur per bangun:
//   1. Baca tegangan supercap (GPIO0 via divider 100k+100k)
//   2. Jika di bawah MIN_WIFI_VOLTAGE -> langsung tidur (tanpa WiFi)
//   3. Hitung status penjadwalan (ARIMA AR(1)) dan durasi tidur
//   4. Konek WiFi (jalur cepat: AP + channel + BSSID terakhir)
//   5. SATU POST HTTPS ke /api/ingest, baca balasan {"ok","cmd","sleep_s"}
//   6. Jalankan perintah (restart / sleep), matikan WiFi, deep sleep
//
// Dibuang dari versi lama: Blynk, OpenWeatherMap, Google Sheets, sensor tanah.
// Tidak butuh library tambahan (hanya WiFi, HTTPClient, WiFiClientSecure bawaan).
//
// Hardware (sesuai BOM): ESP32-C3 Super Mini, CN3791, 2x supercap 10F seri,
// HT7333, KA75330 di pad RST, divider 100k+100k dari Vsys ke GPIO0.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "secrets.h"  // Salin dari secrets.example.h, isi, dan JANGAN di-commit

// ------------------------------ TIPE DATA ------------------------------------
struct Reply {
  bool ok = false;
  int httpCode = 0;
  String cmd = "none";
  uint32_t sleepS = 0;
  String newSsid = "";
  String newPass = "";
};

// ------------------------------ PENGATURAN -----------------------------------
#define DEBUG_SERIAL 1       // 1 = cetak log ke Serial. Set 0 untuk pemakaian lapangan
#define LED_ENABLED 1        // 1 = kedip singkat sebagai indikator (LED GPIO8, active-LOW)
#define USE_INSECURE_TLS 1   // 1 = tanpa verifikasi sertifikat (hanya untuk uji). 0 = pakai ROOT_CA

static const int PIN_VCAP = 0;   // GPIO0 (ADC1_CH0): titik tengah divider Vsys
static const int PIN_LED = 8;    // LED onboard Super Mini, ACTIVE-LOW

// Pembacaan tegangan
static const float VCAP_DIVIDER = 2.15f;  // Kalibrasi kompensasi sag impedansi divider 100k
static const float VCAP_CAL = 1.00f;     // Kalibrasi tambahan: Vmultimeter / Vterbaca
static const int ADC_SAMPLES = 8;

// Penjadwalan (sama seperti firmware lama)
static const float MIN_WIFI_VOLTAGE = 3.60f;  // Batas cutoff WiFi (3.60V aman & ramah pagi/mendung)
static const uint32_t SLEEP_SAFE_S = 900;     // 15 menit
static const uint32_t SLEEP_MEDIUM_S = 1500;  // 25 menit
static const uint32_t SLEEP_CRITICAL_S = 2400;  // 40 menit
static const float ARIMA_CONST = 4.3387f;
static const float ARIMA_AR1 = 0.8297f;

// Jaringan
static const wifi_power_t WIFI_TX_POWER = WIFI_POWER_2dBm;  // Daya pancar Rendah
static const uint32_t FAST_CONNECT_MS = 4000;   // Jalur cepat (BSSID tersimpan)
static const uint32_t SLOW_CONNECT_MS = 8000;   // Per AP, jalur biasa
static const uint32_t HTTP_TIMEOUT_MS = 6000;

// Pengaman: kalau program macet, paksa tidur setelah ini
static const uint32_t FAILSAFE_MS = 45000;

// ------------------------------ LOG & LED ------------------------------------
#if DEBUG_SERIAL
#define LOG(...) Serial.printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

static void ledPulse(uint8_t times, uint16_t ms) {
#if LED_ENABLED
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(PIN_LED, LOW);   // LOW = nyala
    delay(ms);
    digitalWrite(PIN_LED, HIGH);  // HIGH = mati
    if (i + 1 < times) delay(ms);
  }
#endif
}

// ------------------------------ STATE RTC ------------------------------------
// Bertahan melewati deep sleep. Hilang saat daya habis (dan mungkin saat reset software).
RTC_DATA_ATTR uint32_t rtcBootCount = 0;
RTC_DATA_ATTR bool rtcHasBssid = false;
RTC_DATA_ATTR uint8_t rtcApIndex = 0;
RTC_DATA_ATTR uint8_t rtcChannel = 0;
RTC_DATA_ATTR uint8_t rtcBssid[6] = {0};

static volatile uint32_t g_plannedSleepS = SLEEP_CRITICAL_S;

// ------------------------------ DEEP SLEEP -----------------------------------
static void goToSleep(uint32_t seconds) {
  if (seconds < 60) seconds = 60;
  if (seconds > 86400) seconds = 86400;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  LOG("[SISTEM] Deep sleep %lu detik\n", (unsigned long)seconds);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

static void failsafeCallback(void*) {
  esp_sleep_enable_timer_wakeup((uint64_t)g_plannedSleepS * 1000000ULL);
  esp_deep_sleep_start();
}

static void armFailsafe() {
  esp_timer_handle_t handle;
  esp_timer_create_args_t args = {};
  args.callback = &failsafeCallback;
  args.name = "failsafe";
  if (esp_timer_create(&args, &handle) == ESP_OK) {
    esp_timer_start_once(handle, (uint64_t)FAILSAFE_MS * 1000ULL);
  }
}

// ------------------------------ SENSOR & LOGIKA ------------------------------
static float readVcap() {
  analogSetPinAttenuation(PIN_VCAP, ADC_11db);
  // Buang 1 sampel awal untuk mengisi kapasitor sampling ADC agar stabil
  analogReadMilliVolts(PIN_VCAP);
  delay(1);

  uint32_t sumMv = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sumMv += analogReadMilliVolts(PIN_VCAP);
    delayMicroseconds(200);
  }
  float mv = (float)sumMv / ADC_SAMPLES;
  return (mv / 1000.0f) * VCAP_DIVIDER * VCAP_CAL;
}

static float arimaPredict(float v) {
  return ARIMA_CONST + ARIMA_AR1 * (v - ARIMA_CONST);
}

static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw_reset";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
  }
}

// ------------------------------ WIFI -----------------------------------------
static bool waitConnected(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(20);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void saveApInfo(uint8_t index) {
  rtcApIndex = index;
  rtcChannel = (uint8_t)WiFi.channel();
  memcpy(rtcBssid, WiFi.BSSID(), 6);
  rtcHasBssid = true;
}

static bool connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setTxPower(WIFI_TX_POWER);

  // Baca konfigurasi Wi-Fi dinamis dari Preferences NVS (jika pernah diset dari web)
  Preferences prefs;
  prefs.begin("iot_wifi", true);
  String dynSsid = prefs.getString("ssid", "");
  String dynPass = prefs.getString("pass", "");
  prefs.end();

  // 1) Jalur cepat: AP, channel, dan BSSID terakhir yang berhasil (tanpa scan)
  if (rtcHasBssid) {
    String fastSsid = (dynSsid.length() > 0 && rtcApIndex == 0) ? dynSsid : (rtcApIndex < WIFI_AP_COUNT ? WIFI_APS[rtcApIndex].ssid : "");
    String fastPass = (dynSsid.length() > 0 && rtcApIndex == 0) ? dynPass : (rtcApIndex < WIFI_AP_COUNT ? WIFI_APS[rtcApIndex].pass : "");
    if (fastSsid.length() > 0) {
      LOG("[WIFI] Jalur cepat ke %s (ch %u)\n", fastSsid.c_str(), rtcChannel);
      WiFi.begin(fastSsid.c_str(), fastPass.c_str(), rtcChannel, rtcBssid);
      if (waitConnected(FAST_CONNECT_MS)) return true;
      WiFi.disconnect(false, false);
    }
  }

  // 2) Jalur biasa: Coba Wi-Fi dinamis dari Web App terlebih dahulu
  if (dynSsid.length() > 0) {
    LOG("[WIFI] Mencoba AP utama (NVS): %s\n", dynSsid.c_str());
    WiFi.begin(dynSsid.c_str(), dynPass.c_str());
    if (waitConnected(SLOW_CONNECT_MS)) {
      saveApInfo(0);
      return true;
    }
    WiFi.disconnect(false, false);
    LOG("[WIFI] AP utama gagal, beralih ke AP cadangan...\n");
  }

  // 3) Fallback: Coba tiap AP cadangan di secrets.h
  for (uint8_t i = 0; i < WIFI_AP_COUNT; i++) {
    LOG("[WIFI] Mencoba AP cadangan %s\n", WIFI_APS[i].ssid);
    WiFi.begin(WIFI_APS[i].ssid, WIFI_APS[i].pass);
    if (waitConnected(SLOW_CONNECT_MS)) {
      saveApInfo(i + (dynSsid.length() > 0 ? 1 : 0));
      return true;
    }
    WiFi.disconnect(false, false);
  }

  rtcHasBssid = false;  // Info lama tidak berlaku lagi
  return false;
}

// ------------------------------ HTTP -----------------------------------------

static Reply postIngest(const char* body) {
  Reply r;
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  WiFiClientSecure secure;
  WiFiClient plain;
  bool began = false;

  if (strncmp(API_URL, "https://", 8) == 0) {
#if USE_INSECURE_TLS
    secure.setInsecure();
#else
    secure.setCACert(ROOT_CA);
#endif
    began = http.begin(secure, API_URL);
  } else {
    began = http.begin(plain, API_URL);  // HTTP biasa: hanya untuk uji lokal
  }
  if (!began) return r;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);

  r.httpCode = http.POST((uint8_t*)body, strlen(body));
  if (r.httpCode == 200) {
    String p = http.getString();  // Balasan kecil, parsing sederhana tanpa library
    r.ok = p.indexOf("\"ok\":true") >= 0;
    if (p.indexOf("\"cmd\":\"restart\"") >= 0) {
      r.cmd = "restart";
    } else if (p.indexOf("\"cmd\":\"sleep\"") >= 0) {
      r.cmd = "sleep";
      int i = p.indexOf("\"sleep_s\":");
      if (i >= 0) r.sleepS = (uint32_t)p.substring(i + 10).toInt();
    } else if (p.indexOf("\"cmd\":\"update_wifi\"") >= 0) {
      r.cmd = "update_wifi";
      int sIdx = p.indexOf("\"ssid\":\"");
      if (sIdx >= 0) {
        int sEnd = p.indexOf("\"", sIdx + 8);
        if (sEnd > sIdx + 8) r.newSsid = p.substring(sIdx + 8, sEnd);
      }
      int pIdx = p.indexOf("\"pass\":\"");
      if (pIdx >= 0) {
        int pEnd = p.indexOf("\"", pIdx + 8);
        if (pEnd > pIdx + 8) r.newPass = p.substring(pIdx + 8, pEnd);
      }
    }
  }
  http.end();
  return r;
}

// ------------------------------ SETUP ----------------------------------------
void setup() {
#if DEBUG_SERIAL
  Serial.begin(115200);
#endif
#if LED_ENABLED
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);  // Mati
#endif

  armFailsafe();
  rtcBootCount++;
  LOG("\n[SISTEM] Bangun. boot=%lu reset=%s\n", (unsigned long)rtcBootCount, resetReasonStr());

  // 1. Tegangan supercap
  float vcap = readVcap();
  LOG("[SENSOR] Vcap: %.3f V\n", vcap);

  // 2. Terlalu rendah untuk WiFi: tidur tanpa kirim
  if (vcap < MIN_WIFI_VOLTAGE) {
    LOG("[WARNING] Tegangan kritis, batalkan WiFi\n");
    ledPulse(3, 80);
    goToSleep(SLEEP_CRITICAL_S);
  }

  // 3. Status penjadwalan
  float predicted = arimaPredict(vcap);
  const char* status;
  uint32_t sleepS;
  if (predicted > 4.10f) {
    status = "safe";
    sleepS = SLEEP_SAFE_S;
  } else if (predicted >= 4.00f) {
    status = "medium";
    sleepS = SLEEP_MEDIUM_S;
  } else {
    status = "critical";
    sleepS = SLEEP_CRITICAL_S;
  }
  g_plannedSleepS = sleepS;
  LOG("[ARIMA] pred=%.3f V status=%s tidur=%lu s\n", predicted, status, (unsigned long)sleepS);

  // 4. WiFi
  uint32_t wifiStart = millis();
  bool connected = connectWifi();
  uint32_t wifiMs = millis() - wifiStart;
  LOG("[WIFI] %s dalam %lu ms\n", connected ? "Tersambung" : "GAGAL", (unsigned long)wifiMs);

  bool restartRequested = false;

  if (connected) {
    // 5. Satu POST ke server (sertakan nama SSID aktif)
    char body[384];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"vcap\":%.3f,\"status\":\"%s\",\"predicted_v\":%.3f,"
             "\"sleep_s\":%lu,\"reset_reason\":\"%s\",\"boot_count\":%lu,"
             "\"wifi_ms\":%lu,\"active_ms\":%lu,\"ssid\":\"%s\"}",
             DEVICE_ID, vcap, status, predicted, (unsigned long)sleepS, resetReasonStr(),
             (unsigned long)rtcBootCount, (unsigned long)wifiMs,
             (unsigned long)millis(), WiFi.SSID().c_str());

    Reply reply = postIngest(body);
    LOG("[HTTP] kode=%d ok=%d cmd=%s\n", reply.httpCode, reply.ok, reply.cmd.c_str());

    if (reply.ok) {
      ledPulse(1, 120);
      // 6. Perintah dari server
      if (reply.cmd == "restart") restartRequested = true;
      if (reply.cmd == "sleep" && reply.sleepS >= 60) sleepS = reply.sleepS;
      if (reply.cmd == "update_wifi" && reply.newSsid.length() > 0) {
        LOG("[WIFI] Konfigurasi Wi-Fi baru diterima: %s\n", reply.newSsid.c_str());
        Preferences prefs;
        prefs.begin("iot_wifi", false); // read-write
        prefs.putString("ssid", reply.newSsid);
        prefs.putString("pass", reply.newPass);
        prefs.end();
        rtcHasBssid = false; // reset cache jalur cepat
        restartRequested = true; // restart agar langsung konek ke Wi-Fi baru
      }
    } else {
      if (reply.httpCode == 401) LOG("[HTTP] 401: DEVICE_ID atau DEVICE_KEY salah\n");
      ledPulse(3, 80);
    }
  } else {
    ledPulse(3, 80);
  }

  if (restartRequested) {
    LOG("[SISTEM] Perintah restart dari server\n");
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    esp_restart();
  }

  goToSleep(sleepS);
}

void loop() {
  // Tidak dipakai: semua pekerjaan selesai di setup(), lalu deep sleep.
}
