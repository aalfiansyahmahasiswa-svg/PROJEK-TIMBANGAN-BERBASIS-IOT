#include <HX711.h>
#include <EEPROM.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ---------------- WIFI + FIREBASE ----------------
const char* WIFI_SSID = "Wifi Surga";
const char* WIFI_PASS = "87654321";

const String FIREBASE_HOST = "timbang-6ce0e-default-rtdb.firebaseio.com"; // tanpa https://
const String SECRET_KEY = "BbCKlu3Hb96UrOYGEWCLKv3tJKIi2Q7qxUVKW075";

const String DEVICE_ID = "scale01"; // id perangkat (digunakan di path)
WiFiClientSecure client;

// ---------------- LCD & SENSORS ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// PIN
const int HX711_DOUT = D6;
const int HX711_SCK  = D7;

const int TRIG_PIN = D5;
const int ECHO_PIN = D8;

const int BUZZER_PIN = D3;

HX711 scale;

// EEPROM
float scale_factor = 213.15;
float tare_offset  = 0.0;

const int EEPROM_SIZE = 32;
const int ADDR_SCALE = 0;
const int ADDR_TARE  = 4;

float MOUNT_HEIGHT_CM = 196.0;

// smoothing
const int WEIGHT_SAMPLES = 5;
const int HEIGHT_SAMPLES = 5;

float weightBuffer[WEIGHT_SAMPLES];
int weightIndex = 0;

long heightBuffer[HEIGHT_SAMPLES];
int heightIndex = 0;

// Stabilitas Objek
int stableCount = 0;
const int stableNeeded = 10;
float weightThreshold = 1.0;   // minimal berat untuk dianggap ada objek (kg)
float heightThreshold = 5.0;   // minimal tinggi untuk dianggap objek (cm)
bool objectDetected = false;

// measurement state
unsigned long measureStart = 0;
bool measuring = false;
bool resultShown = false;

float totalWeight = 0;
int countWeight = 0;

float totalHeight = 0;
int countHeight = 0;

// ---------------- helper functions ----------------
void lcdShow(String a, String b = "") {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(a);
  lcd.setCursor(0,1); lcd.print(b);
}

void loadCalibration() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(ADDR_SCALE, scale_factor);
  EEPROM.get(ADDR_TARE,  tare_offset);

  if (!isfinite(scale_factor) || scale_factor == 0) scale_factor = 213.15;
  if (!isfinite(tare_offset)) tare_offset = 0.0;
}

void saveCalibration() {
  EEPROM.put(ADDR_SCALE, scale_factor);
  EEPROM.put(ADDR_TARE,  tare_offset);
  EEPROM.commit();
}

long readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long d = pulseIn(ECHO_PIN, HIGH, 30000);
  if (d == 0) return -1;
  return d / 58;
}

float readWeight() {
  if (!scale.is_ready()) return 0;
  float w = scale.get_units() - tare_offset;
  if (w < 0) w = 0;
  return w;
}

float smoothWeight(float newVal) {
  weightBuffer[weightIndex] = newVal;
  weightIndex = (weightIndex + 1) % WEIGHT_SAMPLES;
  float sum = 0;
  for (int i=0;i<WEIGHT_SAMPLES;i++) sum += weightBuffer[i];
  return sum / WEIGHT_SAMPLES;
}

float smoothHeight(long newVal) {
  heightBuffer[heightIndex] = newVal;
  heightIndex = (heightIndex + 1) % HEIGHT_SAMPLES;
  long sum = 0;
  for (int i=0;i<HEIGHT_SAMPLES;i++) sum += heightBuffer[i];
  return (float)sum / HEIGHT_SAMPLES;
}

// ---------------- Firebase helpers ----------------
// Performs HTTPS PUT to pathWithQuery (e.g. "/scales/scale01/latest.json?auth=KEY")
bool firebasePut(const String &pathWithQuery, const String &json) {
  client.setInsecure();
  if (!client.connect(FIREBASE_HOST.c_str(), 443)) {
    Serial.println("[FB] connect failed (put)");
    return false;
  }

  client.print(String("PUT ") + pathWithQuery + " HTTP/1.1\r\n");
  client.print(String("Host: ") + FIREBASE_HOST + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: " + String(json.length()) + "\r\n\r\n");
  client.print(json);

  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 4000) {
      Serial.println("[FB] put timeout");
      client.stop();
      return false;
    }
  }
  // read and discard response
  while (client.available()) client.readStringUntil('\n');
  client.stop();
  return true;
}

// Get epoch time via NTP (returns 0 on failure). We already call configTime in setup.
long getEpoch() {
  time_t now = time(nullptr);
  if (now > 100000) return (long)now; // valid epoch
  // fallback to millis-based timestamp (not real epoch)
  return (long)(millis() / 1000);
}
bool firebasePatch(const String &pathWithQuery, const String &json) {
  client.setInsecure();
  if (!client.connect(FIREBASE_HOST.c_str(), 443)) {
    Serial.println("[FB] connect failed (patch)");
    return false;
  }

  client.print(String("PATCH ") + pathWithQuery + " HTTP/1.1\r\n");
  client.print("Host: " + FIREBASE_HOST + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: " + String(json.length()) + "\r\n\r\n");
  client.print(json);

  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 4000) {
      Serial.println("[FB] patch timeout");
      client.stop();
      return false;
    }
  }

  while (client.available()) client.readStringUntil('\n');
  client.stop();
  return true;
}

// Send result -> latest + history
void sendResultToFirebase(int height_cm, int weight_kg) {
  long ts = getEpoch();

  String json = "{";
  json += "\"height_cm\":" + String(height_cm) + ",";
  json += "\"weight_kg\":" + String(weight_kg) + ",";
  json += "\"ts\":" + String(ts);
  json += "}";

  String path =
    "/scale01/iot_temp.json?auth=" + SECRET_KEY;

  firebasePut(path, json);

  Serial.println("[FB] iot_temp updated");
}


// ---------------- setup ----------------
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  lcdShow("TT: 0 cm", "BB: 0 kg");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  loadCalibration();

  scale.begin(HX711_DOUT, HX711_SCK);
  scale.set_scale(scale_factor);

  lcdShow("Auto Tare...", "");
  scale.tare();
  tare_offset = 0;
  saveCalibration();
  delay(600);

  for(int i=0;i<WEIGHT_SAMPLES;i++) weightBuffer[i] = 0;
  for(int i=0;i<HEIGHT_SAMPLES;i++) heightBuffer[i] = 0;

  // connect WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("[WIFI] connecting...");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WIFI] connected: " + WiFi.localIP().toString());
    lcdShow("WiFi OK", WiFi.localIP().toString());
    delay(800);

    // try to sync time for epoch timestamps
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    Serial.println("[TIME] waiting for NTP...");
    unsigned long tstart = millis();
    while (time(nullptr) < 1600000000 && millis() - tstart < 8000) {
      delay(200);
    }
    if (time(nullptr) > 1600000000) {
      Serial.println("[TIME] NTP OK: " + String((long)time(nullptr)));
    } else {
      Serial.println("[TIME] NTP failed, will use millis()");
    }
  } else {
    Serial.println("[WIFI] failed");
    lcdShow("WiFi Failed", "");
    delay(1200);
  }

  lcdShow("TT: 0 cm", "BB: 0 kg");
  delay(800);
}

// ---------------- main loop ----------------
void loop() {
  // read sensors (smoothed)
  float w = smoothWeight(readWeight());
  long d = smoothHeight(readDistance());

  // compute height from mount height minus measured distance
  float height_cm = MOUNT_HEIGHT_CM - d;
  if (height_cm < 0) height_cm = 0;
  if (height_cm > 196) height_cm = 196;

  // detect object presence (thresholds)
  if (w > weightThreshold && height_cm > heightThreshold) {
    stableCount++;
    if (stableCount >= stableNeeded) objectDetected = true;
  } else {
    // reset if object not present
    stableCount = 0;
    objectDetected = false;
    measuring = false;
    resultShown = false;
    // display idle
    lcdShow("TT: 0 cm", "BB: 0 kg");
    delay(80);
    return;
  }

  if (!objectDetected) return;

  // start measuring window if not already
  if (!measuring) {
    measuring = true;
    measureStart = millis();
    // reset accumulators to avoid old noise
    totalWeight = 0;
    countWeight = 0;
    totalHeight = 0;
    countHeight = 0;
    // also reset smoothing buffers to make reading fresh
    for (int i = 0; i < WEIGHT_SAMPLES; i++) weightBuffer[i] = 0;
    for (int i = 0; i < HEIGHT_SAMPLES; i++) heightBuffer[i] = 0;
    weightIndex = 0;
    heightIndex = 0;

    lcdShow("Mengukur...", "Tahan posisi");
  }

  // accumulate
  totalWeight += w;
  countWeight++;
  totalHeight += height_cm;
  countHeight++;

  // show current read
  lcdShow(
    "TT: " + String((int)(height_cm + 0.5)) + " cm",
    "BB: " + String((int)(w + 0.5)) + " kg"
  );

  // if measuring window finished (8s)
  if (millis() - measureStart >= 8000 && !resultShown) {
    int heightDisplay = (int)(height_cm + 0.5);
    int weightDisplay = (int)(w + 0.5);
    tone(BUZZER_PIN, 1500);
    delay(800);
    noTone(BUZZER_PIN);
    for (int i = 0; i < 3; i++) {
      lcd.noBacklight(); delay(150);
      lcd.backlight(); delay(150);
    }
    lcdShow("HASIL:", String(heightDisplay) + "cm  " + String(weightDisplay) + "kg");
    resultShown = true;
    Serial.println("[MEAS] final: h=" + String(heightDisplay) + " w=" + String(weightDisplay));
    sendResultToFirebase(heightDisplay, weightDisplay);

    delay(6000);
    // reset states for next measurement
    measuring = false;
    resultShown = false;
    stableCount = 0;
    objectDetected = false;
    lcdShow("TT: 0 cm", "BB: 0 kg");
    delay(500);
    return;
  }

  delay(60);
}