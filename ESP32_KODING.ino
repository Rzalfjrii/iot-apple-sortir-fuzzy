#include <Wire.h>
#include "Adafruit_SGP30.h"
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

// ——— Wi-Fi Credentials —————————————————
const char* ssid     = "UIN-WIFI";
const char* password = "";

// ——— Firebase URLs —————————————————
const char* buahUrl   = "https://sistemsortir-34769-default-rtdb.asia-southeast1.firebasedatabase.app/buahApel.json";
const char* jumlahUrl = "https://sistemsortir-34769-default-rtdb.asia-southeast1.firebasedatabase.app/jumlah.json";

// ——— Pin Definitions —————————————————
const int COLOR_S0   = 25;
const int COLOR_S1   = 33;
const int COLOR_S2   = 27;
const int COLOR_S3   = 26;
const int COLOR_OUT  = 14;
const int SDA_PIN    = 19;
const int SCL_PIN    = 18;
const int SERVO_PIN  = 12;

Adafruit_SGP30 sgp;
Servo servo;

int lastStatus = -1;      // -1 = belum pernah, 0=busuk, 1=matang
int countMatang = 0;
int countBusuk = 0;

// --- Prototypes ---
float readGreen();
uint16_t readCO2();
float muGreenLow(float x);
float muGreenHigh(float x);
float muCO2Low(float x);
float muCO2High(float x);
void sendToFirebase(const char* url, const String& json);
void initTime();
String getTimestamp();

void setup() {
  Serial.begin(115200);

  // Connect Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Wi-Fi connected");

  // Sync time via NTP
  initTime();

  // Initialize I2C and SGP30 sensor
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!sgp.begin()) {
    Serial.println("SGP30 sensor not found!");
    while (1) delay(1000);
  }
  sgp.IAQinit();
  delay(1000);

  // Initialize TCS3200 color sensor pins
  pinMode(COLOR_S0, OUTPUT);
  pinMode(COLOR_S1, OUTPUT);
  pinMode(COLOR_S2, OUTPUT);
  pinMode(COLOR_S3, OUTPUT);
  pinMode(COLOR_OUT, INPUT);
  digitalWrite(COLOR_S0, HIGH); // Set frequency scaling to 20%
  digitalWrite(COLOR_S1, LOW);

  // Initialize Servo
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN);
  servo.write(90);  // Neutral position
}

void loop() {
  delay(2000);  // Delay 2 seconds between readings

  // Read sensors
  float green = readGreen();
  uint16_t co2 = readCO2();

  // Display raw sensor data
  Serial.printf("Raw -> Green: %.2f, CO2: %u ppm\n", green, co2);

  // Check for no fruit condition (out of range)
  if (green > 100.0f || co2 > 600) {
    Serial.println("No fruit detected (out of range)");
    servo.write(90);
    lastStatus = -1;
    return;
  }

 // Hitung derajat keanggotaan untuk masing-masing aturan
float a1 = muGreenLow(green) * muCO2Low(co2);     // Aturan 1: Matang (z=1)
float a2 = muGreenHigh(green) * muCO2High(co2);   // Aturan 2: Busuk (z=0)
float a3 = muGreenLow(green) * muCO2High(co2);    // Aturan 3: Matang (z=1)
float a4 = muGreenHigh(green) * muCO2Low(co2);    // Aturan 4: Busuk (z=0)

// Hitung jumlah total derajat keanggotaan
float sumA = a1 + a2 + a3 + a4;

// Defuzzifikasi menggunakan metode Sugeno
float z = (sumA > 0) ? ((a1*1.0f + a2*0.0f + a3*1.0f + a4*0.0f) / sumA) : 0.0f;

// Tentukan apakah buah matang berdasarkan nilai z
bool isMatang = (z > 0.5f);


  Serial.printf("Fuzzy z=%.2f -> %s\n", z, isMatang ? "Matang" : "Busuk");

  // Control servo based on classification
  servo.write(isMatang ? 135 : 90);
  if (isMatang) {
    delay(7000);  // Hold servo position for 7 seconds if matang
    servo.write(90);  // Return servo to neutral
  }

  // Send data to Firebase only on status change
  int statusCode = isMatang ? 1 : 0;
  if (statusCode != lastStatus) {
    lastStatus = statusCode;

    if (isMatang) countMatang++;
    else countBusuk++;

    // Prepare JSON data for fruit status
    String jBuah = "{";
    jBuah += String("\"status\":\"") + (isMatang ? "matang" : "busuk") + "\",";
    jBuah += String("\"warna\":") + String(green, 1) + ",";
    jBuah += String("\"co2\":") + String(co2) + ",";
    jBuah += String("\"terakhir\":\"") + getTimestamp() + "\"";
    jBuah += "}";
    sendToFirebase(buahUrl, jBuah);

    // Prepare JSON data for counts
    String jJumlah = "{";
    jJumlah += "\"matang\":" + String(countMatang) + ",";
    jJumlah += "\"busuk\":" + String(countBusuk) + "}";
    sendToFirebase(jumlahUrl, jJumlah);
  }
}

// --- Read green frequency from TCS3200 ---
float readGreen() {
  digitalWrite(COLOR_S2, HIGH);
  digitalWrite(COLOR_S3, HIGH);
  unsigned long pulse = pulseIn(COLOR_OUT, LOW, 100000UL);
  return pulse / 10.0f;
}

// --- Read CO2 value from SGP30 ---
uint16_t readCO2() {
  sgp.IAQmeasure();
  return sgp.eCO2;
}

// --- Fuzzy membership functions ---

float muGreenLow(float x) {
  if (x <= 0) return 1.0f;
  if (x >= 20) return 0.0f;
  return (20 - x) / 20;
}

float muGreenHigh(float x) {
  if (x <= 18) return 0.0f;
  if (x >= 100) return 1.0f;
  return (x - 18) / (100 - 18);
}

float muCO2Low(float x) {
  if (x <= 400) return 1.0f;
  if (x >= 460) return 0.0f;
  return (460 - x) / (460 - 400);
}

float muCO2High(float x) {
  if (x <= 420) return 0.0f;
  if (x >= 600) return 1.0f;
  return (x - 420) / (600 - 420);
}

// --- Send JSON data to Firebase via HTTP PUT ---
void sendToFirebase(const char* url, const String& json) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(json);
  Serial.printf("Firebase HTTP %d -> %s\n", code, url);
  http.end();
}

// --- Initialize NTP time ---
void initTime() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time synchronization");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {  // wait until time is set ( > 2 hours since epoch)
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println(" synchronized!");
}

// --- Get current timestamp as string (local time WIB/GMT+7) ---
String getTimestamp() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);  // use localtime (WIB)
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}
