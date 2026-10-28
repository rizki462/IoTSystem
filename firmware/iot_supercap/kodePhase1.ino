#define BLYNK_TEMPLATE_ID "TMPL6_AuoG8Iz"
#define BLYNK_TEMPLATE_NAME "testing solar panel dan supercapasitor"
#define BLYNK_AUTH_TOKEN "7sHYb4UEmETC-t0nXGmB3LayDbc7ONKO"
#define BLYNK_PRINT Serial

// --- LIBRARY KHUSUS ESP32 ---
#include <WiFi.h>
#include <WiFiMulti.h>
#include <BlynkSimpleEsp32.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

WiFiMulti wifiMulti;

// --- KONFIGURASI CLOUD & API ---
const String GAS_URL = "https://script.google.com/macros/s/AKfycbwrWPH7UjAHjCwxnJb2kt2V68bjCvW9to0z9QsTrjRlxvbtH8YDJxFCTlRIyCBwTO4o/exec";
const String city = "Sukabumi";
const String apiKey = "1f9127ad2063e43377e6ec85b58a4e7a";

// --- KONFIGURASI PIN HARDWARE ESP32 (Murni Direct tanpa LDO/MOSFET) ---
const int pinLED = 2;           // GPIO 2 (Built-in LED)
const int pinVoltase = 32;      // GPIO 32 (Membaca V-Cap via Divider)
const int pinSoil = 34;         // GPIO 34 (Membaca Sensor Tanah)
const int pinPowerSensor = 26;  // GPIO 26 (Saklar daya sensor tanah)
const int pinKontrolADC = 25;   // GPIO 25 (BC547 Kedua untuk pemutus arus ADC)
const int pinRelay = 27;        // GPIO 27 (Relay Booster MT3608 - Active LOW)

// --- VARIABEL DATA GLOBAL ---
float voltage = 0.0;
String teksStatusARIMA = "";
int durasiTidurGlobal = 900;
int soilPercent = 0;
int lightIntensityAPI = 0;

// --- DEKLARASI FUNGSI ---
float prediksiTeganganARIMA(float teganganSekarang);
void fetchWeatherAPI();
void sendToGoogleSheets();
void indikatorLED(int mode);

void setup() {
  // Inisialisasi Pin Pendukung
  pinMode(pinRelay, OUTPUT);
  digitalWrite(pinRelay, HIGH);  // HIGH = MATI pada Relay Active Low

  pinMode(pinPowerSensor, OUTPUT);
  digitalWrite(pinPowerSensor, LOW);

  pinMode(pinKontrolADC, OUTPUT);
  digitalWrite(pinKontrolADC, LOW);

  pinMode(pinLED, OUTPUT);
  digitalWrite(pinLED, LOW);

  WiFi.mode(WIFI_OFF);  // Matikan Wi-Fi di awal

  Serial.begin(115200);
  Serial.println("\n\n[SISTEM] ESP32 Bangun dengan Jalur PURE DIRECT!");

  // =========================================================
  // 1. CEK TEGANGAN SUPERKAPASITOR (GPIO 32 via BC547 Kedua)
  // =========================================================
  digitalWrite(pinKontrolADC, HIGH);      // Nyalakan BC547 pembagi ADC
  delay(50);                              // Setting time ADC
  int rawValue = analogRead(pinVoltase);  
  digitalWrite(pinKontrolADC, LOW);       // Matikan kembali agar tidak ada leakage current

  voltage = (rawValue / 4095.0) * 3.3 * 2.15;
  Serial.print("[SENSOR] Tegangan VCap: ");
  Serial.print(voltage);
  Serial.println("V");

  // =========================================================
  // 1.5. PENGAMANAN TEGANGAN KRITIS
  // =========================================================
  if (voltage < 3.80) {
    Serial.println("[WARNING] Tegangan kritis! Batalkan Wi-Fi.");
    indikatorLED(3); // LED Berkedip cepat 5x
    
    esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL); // Tidur 1 jam
    esp_deep_sleep_start();
  }

  // Jika aman, berikan indikator LED aman (Kedip 3x yang sudah dipercepat)
  indikatorLED(2);

  // =========================================================
  // 2. BACA SENSOR KELEMBABAN TANAH (GPIO 34)
  // =========================================================
  Serial.println("[PROSES] Menyalakan Booster & Sensor Tanah...");

  digitalWrite(pinRelay, LOW);    // Nyalakan Booster MT3608
  digitalWrite(pinPowerSensor, HIGH);
  delay(200);                     // [REVISI] Dipercepat jadi 200ms agar menghemat daya sebelum Wi-Fi nyala

  int rawSoil = analogRead(pinSoil);  

  digitalWrite(pinPowerSensor, LOW);
  digitalWrite(pinRelay, HIGH);   // Matikan Booster SECEPAT MUNGKIN

  soilPercent = map(rawSoil, 3200, 1600, 0, 100);
  soilPercent = constrain(soilPercent, 0, 100);

  Serial.print("[SENSOR] Kelembaban Tanah: ");
  Serial.print(soilPercent);
  Serial.println("%");

  // =========================================================
  // 3. HITUNG ARIMA TERLEBIH DAHULU (Pindah sebelum Wi-Fi)
  // =========================================================
  // [REVISI] Proses kalkulasi dilakukan saat Wi-Fi masih mati agar beban CPU aman
  float prediksi = prediksiTeganganARIMA(voltage);

  // [REVISI THRESHOLD] Menggeser batas agar Mode Medium & Critical bisa tereksekusi
  if (prediksi > 4.10) {
    durasiTidurGlobal = 900;
    teksStatusARIMA = "is Safe (15 minutes) | Prediction: " + String(prediksi, 2) + "V";
  } else if (prediksi >= 4.00 && prediksi <= 4.10) {
    durasiTidurGlobal = 1800;
    teksStatusARIMA = "is Medium (30 minutes) | Prediction: " + String(prediksi, 2) + "V";
  } else {
    durasiTidurGlobal = 3600;  
    teksStatusARIMA = "is Critical (60 minutes) | Prediction: " + String(prediksi, 2) + "V";
  }
  Serial.println("\n[ARIMA STATUS] " + teksStatusARIMA);

  // =========================================================
  // 4. BANGUNKAN RADIO WI-FI & KONEKSI
  // =========================================================
  Serial.println("[WIFI] Membangunkan Radio Wi-Fi...");
  
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP("ruang sempit", "ruangsempit135790");
  wifiMulti.addAP("Bestservice.id", "bestservice");

  // [REVISI] Sedikit dinaikkan jadi 2dBm (tetap hemat, tapi menghindari timeout karena sinyal lemah)
  WiFi.setTxPower(WIFI_POWER_2dBm);

  indikatorLED(4); // [REVISI] Hanya menyalakan LED TANPA DELAY!

  int timeout = 0;
  // [REVISI] Batas maksimal koneksi dipersingkat menjadi 8 detik (16 * 500ms)
  while (wifiMulti.run() != WL_CONNECTED && timeout < 16) {
    delay(500);
    Serial.print(".");
    yield();
    timeout++;
  }
  
  digitalWrite(pinLED, LOW); // Matikan LED segera setelah status Wi-Fi jelas

  // =========================================================
  // 5. PROSES PENGAMBILAN & TRANSMISI DATA
  // =========================================================
  if (WiFi.status() == WL_CONNECTED) {
    fetchWeatherAPI();
    Blynk.config(BLYNK_AUTH_TOKEN);
    Blynk.connect(3000);
    if (Blynk.connected()) {
      Blynk.virtualWrite(V1, voltage);
      Blynk.virtualWrite(V2, soilPercent);
      Blynk.virtualWrite(V5, lightIntensityAPI);
      Blynk.virtualWrite(V6, teksStatusARIMA);
      Blynk.run();
    }
    sendToGoogleSheets();
    
    // Berhasil & Data Terkirim
    indikatorLED(1);
  } else {
    Serial.println("\n[WIFI] Timeout! Transmisi batal.");
    indikatorLED(3);
  }

  // =========================================================
  // 6. DEEP SLEEP MURNI DIRECT
  // =========================================================
  digitalWrite(pinLED, LOW);
  
  Serial.printf("[SISTEM] Transmisi selesai. Deep Sleep %d Detik...\n", durasiTidurGlobal);
  
  WiFi.disconnect(true);
  delay(200); // [REVISI] Persingkat jeda matikan Wi-Fi

  esp_sleep_enable_timer_wakeup((uint64_t)durasiTidurGlobal * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {
  // Kosong
}

// =========================================================
// FUNGSI PENDUKUNG & STANDARISASI LED
// =========================================================

void indikatorLED(int mode) {
  switch (mode) {
    case 1: // Berhasil & Data Terkirim: LED hidup 0.5 detik (Diet Daya)
      digitalWrite(pinLED, HIGH);
      delay(500);
      digitalWrite(pinLED, LOW);
      break;

    case 2: // Tegangan Aman: LED Berkedip 3x (Dipercepat sedikit)
      for (int i = 0; i < 3; i++) {
        digitalWrite(pinLED, HIGH);
        delay(100);
        digitalWrite(pinLED, LOW);
        delay(100);
      }
      break;

    case 3: // Tegangan Kritis / Gagal: LED Berkedip cepat 5x
      for (int i = 0; i < 5; i++) {
        digitalWrite(pinLED, HIGH);
        delay(80);
        digitalWrite(pinLED, LOW);
        delay(80);
      }
      break;

    case 4: // Proses Mencari Wi-Fi: [REVISI] HANYA menyalakan pin tanpa delay!
      digitalWrite(pinLED, HIGH);
      // Dihapus: delay(3000) yang menyebabkan ESP mati.
      break;
  }
}

void fetchWeatherAPI() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = "https://api.openweathermap.org/data/2.5/weather?q=" + city + "&appid=" + apiKey;

  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode > 0) {
      String payload = http.getString();
      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        int cloudCover = doc["clouds"]["all"];
        lightIntensityAPI = 100 - cloudCover;
      }
    }
    http.end();
  }
}

void sendToGoogleSheets() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String teksUrl = teksStatusARIMA;
  teksUrl.replace(" ", "%20");

  String url = GAS_URL + "?v=" + String(voltage, 2) + "&l=" + String(lightIntensityAPI) + "&s=" + String(soilPercent) + "&info=" + teksUrl;

  if (http.begin(client, url)) {
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int httpCode = http.GET();
    if (httpCode > 0) {
      String payload = http.getString();
      if (payload == "SUCCESS_SAVED") {
        for (int i = 0; i < 3; i++) {
          digitalWrite(pinLED, HIGH);
          delay(100);
          digitalWrite(pinLED, LOW);
          delay(100);
        }
      }
    }
    http.end();
  }
}

float prediksiTeganganARIMA(float teganganSekarang) {
  float const_val = 4.3387;
  float ar_L1_val = 0.8297;
  return const_val + (ar_L1_val * (teganganSekarang - const_val));
}