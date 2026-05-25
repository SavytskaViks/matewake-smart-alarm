#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>       
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_CCS811.h>
#include <BH1750.h>
#include <ClosedCube_HDC1080.h>
#include <driver/i2s.h>
#include <TFT_eSPI.h>
#include <time.h>

#define RESET_WIFI_PIN 0

const char* mqtt_server = "broker.hivemq.com";
const int   mqtt_port   = 1883;

const char* mqtt_topic_settings    = "diploma/smart_alarm/001/settings";
const char* mqtt_topic_optimized   = "diploma/smart_alarm/001/optimized";
const char* mqtt_topic_environment = "diploma/smart_alarm/001/environment";

WiFiClient espClient;
PubSubClient client(espClient);

const long GMT_OFFSET_SEC = 2 * 3600;
const int  DST_OFFSET_SEC = 3600;

String nowHHMM() {
  struct tm ti;
  if (!getLocalTime(&ti)) return "";
  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", &ti);
  return String(buf);
}

struct SleepSegment {
  int cycle;
  String phase;
  String start;
  String end;
  int durationMin;
};

struct ActiveCycle {
  int id;
  String n1;
  String n2;
  String n3;
  String rem;
  bool valid;
};

ActiveCycle activeCycle = {0, "", "", "", "", false};
int activeCycleIndex = -1;

SleepSegment dailySegments[20];
int segmentCount = 0;

String bedtime = "";
String wakeStart = "";
String wakeEnd = "";

String recommendedWakeTime = "";
String optimizedWakeTime   = "";
bool wakeTimeLocked = false;

bool hasWakeWindow = false;
bool hasSegments   = false;

const int WINDOW_SIZE = 600;

uint8_t noiseScoreBuf[WINDOW_SIZE]  = {0};
uint8_t lightScoreBuf[WINDOW_SIZE]  = {0};
uint8_t motionScoreBuf[WINDOW_SIZE] = {0};

int bufPos    = 0;
bool bufFilled = false;

unsigned long lastWindowProcess = 0;

int toMin(const String& hhmm) {
  if (hhmm.length() < 5) return -1;
  int h = hhmm.substring(0, 2).toInt();
  int m = hhmm.substring(3, 5).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

String minToHHMM(int totalMin) {
  totalMin %= 1440;
  if (totalMin < 0) totalMin += 1440;
  int h = totalMin / 60;
  int m = totalMin % 60;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
  return String(buf);
}

bool inRange(int a, int b, int x) {
  if (a < 0 || b < 0 || x < 0) return false;
  if (a <= b) return (x >= a && x <= b);
  return (x >= a) || (x <= b);
}

#define CCS_WAK 15
#define PIR_PIN 27
#define I2S_SCK 14
#define I2S_WS  12
#define I2S_SD  32

Adafruit_BME280 bme;
Adafruit_CCS811 ccs;
BH1750 lightMeter;
ClosedCube_HDC1080 hdc1080;
TFT_eSPI tft = TFT_eSPI();

bool HAS_BME = false;
bool HAS_BMP = false;
bool HAS_CCS = false;
bool HAS_BH  = false;
bool HAS_HDC = false;

int lastPirState = LOW;
int stuckCounter = 0;

uint8_t getBME_ID(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(0xD0);
  Wire.endTransmission();
  Wire.requestFrom(addr, (uint8_t)1);
  if (Wire.available()) return Wire.read();
  return 0;
}

int readStablePIR() {
  int currentState = digitalRead(PIR_PIN);
  if (currentState == HIGH) stuckCounter++;
  else stuckCounter = 0;

  if (stuckCounter > 15) {
    pinMode(PIR_PIN, OUTPUT);
    digitalWrite(PIR_PIN, LOW);
    delay(50);
    pinMode(PIR_PIN, INPUT);
    stuckCounter = 0;
    lastPirState = LOW;
    return 0;
  }
  int result = (currentState == HIGH && lastPirState == LOW) ? 1 : 0;
  lastPirState = currentState;
  return result;
}

void initI2S() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = -1,
    .data_in_num  = I2S_SD
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

int readMIC() {
  const int samples = 256;
  int16_t buf[samples];
  size_t br = 0;
  i2s_read(I2S_NUM_0, buf, samples * 2, &br, portMAX_DELAY);
  int count = br / 2;
  long sum = 0;
  for (int i = 0; i < count; i++) sum += abs(buf[i]);
  return (count > 0) ? (sum / count) : 0;
}

enum Phase { PH_UNKNOWN, PH_N1, PH_N2, PH_N3, PH_REM };

Phase currentPhase = PH_UNKNOWN;
int currentCycle = -1;
unsigned long currentPhaseEndMillis = 0;
bool phaseMachineStarted = false;
bool restartedFromN1 = false;

const char* phaseName(Phase p) {
  switch (p) {
    case PH_N1:  return "N1";
    case PH_N2:  return "N2";
    case PH_N3:  return "N3";
    case PH_REM: return "REM";
    default:     return "UNKNOWN";
  }
}

String phaseToString(Phase p) { return String(phaseName(p)); }
Phase stringToPhase(const String& s) {
  if (s == "N1")  return PH_N1;
  if (s == "N2")  return PH_N2;
  if (s == "N3")  return PH_N3;
  if (s == "REM") return PH_REM;
  return PH_UNKNOWN;
}

int findSegmentIndexByCycleAndPhase(int cycle, Phase phase) {
  String phaseStr = phaseToString(phase);
  for (int i = 0; i < segmentCount; i++) {
    if (dailySegments[i].cycle == cycle && dailySegments[i].phase == phaseStr)
      return i;
  }
  return -1;
}

int getSegmentDurationByCycleAndPhase(int cycle, Phase phase) {
  int idx = findSegmentIndexByCycleAndPhase(cycle, phase);
  if (idx >= 0) return dailySegments[idx].durationMin;
  switch (phase) {
    case PH_N1:  return 10;
    case PH_N2:  return 30;
    case PH_N3:  return 40;
    case PH_REM: return 20;
    default:     return 0;
  }
}

Phase adaptPhaseByDisturbance(Phase basePhase, int score) {
  if (score >= 300) return PH_N1;
  if (score >= 200) {
    if (basePhase == PH_REM || basePhase == PH_N3) return PH_N2;
    return basePhase;
  }
  if (score >= 100) {
    if (basePhase == PH_REM) return PH_N3;
    return basePhase;
  }
  return basePhase;
}

enum Mode { MODE_NIGHT, MODE_PREWAKE, MODE_WAKE, MODE_AFTER };

const char* modeName(Mode m) {
  switch (m) {
    case MODE_PREWAKE: return "PRE_WAKE";
    case MODE_WAKE:    return "WAKE_WINDOW";
    case MODE_AFTER:   return "AFTER";
    default:           return "NIGHT";
  }
}

Phase getScheduledPhase(const String& now) {
  int nowM = toMin(now);
  int bedM = toMin(bedtime);
  if (nowM < 0 || bedM < 0) return PH_UNKNOWN;
  if (nowM < bedM) nowM += 1440;

  for (int i = 0; i < segmentCount; i++) {
    int startM = toMin(dailySegments[i].start);
    int endM   = toMin(dailySegments[i].end);
    if (startM < 0 || endM < 0) continue;
    if (startM < bedM) startM += 1440;
    if (endM   < bedM) endM   += 1440;
    if (nowM >= startM && nowM < endM)
      return stringToPhase(dailySegments[i].phase);
  }
  return PH_UNKNOWN;
}

int getCurrentSegmentIndex(const String& now) {
  int nowM = toMin(now);
  int bedM = toMin(bedtime);
  if (nowM < 0 || bedM < 0) return -1;
  if (nowM < bedM) nowM += 1440;

  for (int i = 0; i < segmentCount; i++) {
    int startM = toMin(dailySegments[i].start);
    int endM   = toMin(dailySegments[i].end);
    if (startM < 0 || endM < 0) continue;
    if (startM < bedM) startM += 1440;
    if (endM   < bedM) endM   += 1440;
    if (nowM >= startM && nowM < endM) return i;
  }
  return -1;
}

bool isGoodToWake(Phase p) { return p == PH_N1 || p == PH_N2; }

uint8_t getNoiseScore(int mic)    { return mic > 600 ? 2 : mic > 300 ? 1 : 0; }
uint8_t getLightScore(float lux)  { return isnan(lux) ? 0 : lux > 30 ? 2 : lux > 10 ? 1 : 0; }
uint8_t getMotionScore(int pir)   { return pir == 1 ? 2 : 0; }

void pushDisturbanceSample(int mic, float lux, int pir) {
  noiseScoreBuf[bufPos]  = getNoiseScore(mic);
  lightScoreBuf[bufPos]  = getLightScore(lux);
  motionScoreBuf[bufPos] = getMotionScore(pir);
  if (++bufPos >= WINDOW_SIZE) { bufPos = 0; bufFilled = true; }
}

int getDisturbanceScore() {
  int count = bufFilled ? WINDOW_SIZE : bufPos;
  int sum = 0;
  for (int i = 0; i < count; i++) {
    sum += noiseScoreBuf[i] + lightScoreBuf[i] + motionScoreBuf[i];
  }
  return sum;
}

void initPhaseMachineFromSchedule(const String& now) {
  int idx = getCurrentSegmentIndex(now);
  if (idx < 0) return;

  currentCycle = dailySegments[idx].cycle;
  currentPhase = stringToPhase(dailySegments[idx].phase);

  int nowM = toMin(now);
  int endM = toMin(dailySegments[idx].end);
  int bedM = toMin(bedtime);
  if (nowM < bedM) nowM += 1440;
  if (endM < bedM) endM += 1440;

  int remaining = endM - nowM;
  if (remaining <= 0) remaining = dailySegments[idx].durationMin;

  currentPhaseEndMillis = millis() + (unsigned long)remaining * 60000UL;
  phaseMachineStarted = true;

  Serial.printf("[INIT] cycle=%d phase=%s remaining=%d min\n",
                currentCycle, phaseName(currentPhase), remaining);
}

void processDisturbanceWindow(const String& now) {
  if (!hasSegments) return;
  if (!phaseMachineStarted) initPhaseMachineFromSchedule(now);
  if (!phaseMachineStarted || currentPhase == PH_UNKNOWN || currentCycle < 0) return;

  int score    = getDisturbanceScore();
  Phase oldPhase = currentPhase;
  Phase newPhase = adaptPhaseByDisturbance(currentPhase, score);

  Serial.printf("[WINDOW] score=%d cycle=%d current=%s adapted=%s\n",
                score, currentCycle, phaseName(oldPhase), phaseName(newPhase));

  if (newPhase != oldPhase) {
    if (score >= 300) {
      currentPhase = PH_N1;
      currentCycle = 1;
      restartedFromN1 = true;
      int dur = getSegmentDurationByCycleAndPhase(1, PH_N1);
      currentPhaseEndMillis = millis() + (unsigned long)dur * 60000UL;
      Serial.println("[RESTART SLEEP] Strong disturbance -> restart from N1 cycle 1");
    } else {
      currentPhase = newPhase;
      int dur = getSegmentDurationByCycleAndPhase(currentCycle, currentPhase);
      currentPhaseEndMillis = millis() + (unsigned long)dur * 60000UL;
      Serial.printf("[SHIFT] cycle=%d phase=%s dur=%d min\n",
                    currentCycle, phaseName(currentPhase), dur);
    }
  }
  memset(noiseScoreBuf,  0, sizeof(noiseScoreBuf));
  memset(lightScoreBuf,  0, sizeof(lightScoreBuf));
  memset(motionScoreBuf, 0, sizeof(motionScoreBuf));
  bufPos = 0;
  bufFilled = false;
}

void updatePhaseMachine() {
  if (!phaseMachineStarted || currentPhase == PH_UNKNOWN) return;
  if ((long)(millis() - currentPhaseEndMillis) < 0) return;
  if (restartedFromN1) {
    Phase nextPhase;
    if (currentPhase == PH_N1)      nextPhase = PH_N2;
    else if (currentPhase == PH_N2) nextPhase = PH_N3;
    else if (currentPhase == PH_N3) nextPhase = PH_REM;
    else { restartedFromN1 = false; return; }

    int dur = getSegmentDurationByCycleAndPhase(1, nextPhase);
    currentPhase = nextPhase;
    currentPhaseEndMillis = millis() + (unsigned long)dur * 60000UL;
    Serial.printf("[RESTART FLOW] phase=%s\n", phaseName(currentPhase));
    return;
  }

  int idx = findSegmentIndexByCycleAndPhase(currentCycle, currentPhase);

  if (idx < 0) {
    for (int i = 0; i < segmentCount; i++) {
      if (dailySegments[i].cycle == currentCycle + 1) {
        currentCycle = dailySegments[i].cycle;
        currentPhase = stringToPhase(dailySegments[i].phase);
        currentPhaseEndMillis = millis() + (unsigned long)dailySegments[i].durationMin * 60000UL;
        Serial.printf("[RECOVER NEXT CYCLE] cycle=%d phase=%s\n",
                      currentCycle, phaseName(currentPhase));
        return;
      }
    }
    return;
  }

  if (idx + 1 < segmentCount && dailySegments[idx + 1].cycle == currentCycle) {
    currentPhase = stringToPhase(dailySegments[idx + 1].phase);
    currentPhaseEndMillis = millis() + (unsigned long)dailySegments[idx + 1].durationMin * 60000UL;
    Serial.printf("[NEXT] cycle=%d phase=%s\n", currentCycle, phaseName(currentPhase));
    return;
  }

  if (idx + 1 < segmentCount) {
    currentCycle = dailySegments[idx + 1].cycle;
    currentPhase = stringToPhase(dailySegments[idx + 1].phase);
    currentPhaseEndMillis = millis() + (unsigned long)dailySegments[idx + 1].durationMin * 60000UL;
    Serial.printf("[NEXT CYCLE] cycle=%d phase=%s\n", currentCycle, phaseName(currentPhase));
  }
}

Mode getMode(const String& now) {
  if (!hasWakeWindow) return MODE_NIGHT;
  int nowM = toMin(now);
  int ws   = toMin(wakeStart);
  int we   = toMin(wakeEnd);
  if (nowM < 0 || ws < 0 || we < 0) return MODE_NIGHT;
  int pre = ws - 30;
  if (pre < 0) pre += 1440;
  if (inRange(ws, we, nowM))  return MODE_WAKE;
  if (inRange(pre, ws, nowM)) return MODE_PREWAKE;
  return MODE_NIGHT;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.println(">>> CALLBACK FIRED <<<");

  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.print("JSON Error: ");
    Serial.println(error.c_str());
    return;
  }

  bedtime   = doc["bedtime"].as<String>();
  wakeStart = doc["wake_start"].as<String>();
  wakeEnd   = doc["wake_end"].as<String>();
  recommendedWakeTime = doc["recommended_wake"].as<String>();
  optimizedWakeTime   = recommendedWakeTime;
  wakeTimeLocked = false;

  JsonArray segments = doc["segments"];
  segmentCount = 0;
  for (JsonObject s : segments) {
    if (segmentCount < 20) {
      dailySegments[segmentCount].cycle       = s["cycle"].as<int>();
      dailySegments[segmentCount].phase       = s["phase"].as<String>();
      dailySegments[segmentCount].start       = s["start"].as<String>();
      dailySegments[segmentCount].end         = s["end"].as<String>();
      dailySegments[segmentCount].durationMin = s["duration_min"].as<int>();
      segmentCount++;
    }
  }

  hasWakeWindow = (wakeStart.length() == 5 && wakeEnd.length() == 5);
  hasSegments   = (segmentCount > 0);

  for (int j = 0; j < segmentCount; j++) {
    Serial.printf("Segment %d | cycle=%d | phase=%s | %s -> %s | dur=%d\n",
                  j,
                  dailySegments[j].cycle,
                  dailySegments[j].phase.c_str(),
                  dailySegments[j].start.c_str(),
                  dailySegments[j].end.c_str(),
                  dailySegments[j].durationMin);
  }

  activeCycle = {0, "", "", "", "", false};
  activeCycleIndex = -1;
  currentPhase = PH_UNKNOWN;
  currentCycle = -1;
  currentPhaseEndMillis = 0;
  phaseMachineStarted = false;
  restartedFromN1 = false;

  memset(noiseScoreBuf,  0, sizeof(noiseScoreBuf));
  memset(lightScoreBuf,  0, sizeof(lightScoreBuf));
  memset(motionScoreBuf, 0, sizeof(motionScoreBuf));
  bufPos = 0;
  bufFilled = false;
  lastWindowProcess = millis();

  Serial.println("\n--- НОВІ НАЛАШТУВАННЯ ОТРИМАНО ---");
  Serial.printf("Bedtime: %s\n", bedtime.c_str());
  Serial.printf("Wake window: %s - %s\n", wakeStart.c_str(), wakeEnd.c_str());
  Serial.printf("Planned wake: %s\n", recommendedWakeTime.c_str());
  Serial.printf("hasWakeWindow: %s | hasSegments: %s\n",
                hasWakeWindow ? "true" : "false",
                hasSegments   ? "true" : "false");
}

void reconnectMQTT() {
  if (client.connected()) return;
  Serial.print("Attempting MQTT connection...");
  String clientId = "ESP32Alarm-" + String(random(0xffff), HEX);
  if (client.connect(clientId.c_str())) {
    Serial.println("connected");
    bool ok = client.subscribe(mqtt_topic_settings);
    Serial.printf("Subscribed to settings: %s\n", ok ? "YES" : "NO");
  } else {
    Serial.printf("failed, rc=%d try again later\n", client.state());
  }
}

unsigned long lastSend = 0;
Phase lastRealPhase = PH_UNKNOWN;
String lastSentWake = "";
unsigned long lastEnvironmentSend = 0;

void publishOptimized(const String& now, Mode mode, Phase baseSch, Phase activeSch, Phase real,
                      int mic, float lux, int pir, int eco2, int tvoc, float t, float h) {
  StaticJsonDocument<1024> doc;
  doc["ts"]              = now;
  doc["mode"]            = modeName(mode);
  doc["base_phase"]      = phaseName(baseSch);
  doc["active_phase"]    = phaseName(activeSch);
  doc["real_phase"]      = phaseName(real);
  doc["recommended_wake"]= optimizedWakeTime;
  doc["recommended_now"] = (mode == MODE_WAKE && isGoodToWake(real));

  JsonObject s = doc.createNestedObject("signals");
  s["mic"]  = mic;
  s["pir"]  = pir;
  s["eco2"] = eco2;
  s["tvoc"] = tvoc;
  if (!isnan(lux)) s["lux"]  = lux;
  if (!isnan(t))   s["temp"] = t;
  if (!isnan(h))   s["hum"]  = h;

  char out[1024];
  size_t n = serializeJson(doc, out);
  client.publish(mqtt_topic_optimized, out, n);
}

void publishEnvironment(const String& now, float t, float h, float lux, int eco2, int tvoc, float p) {
  StaticJsonDocument<512> doc;
  doc["ts"] = now;
  if (!isnan(t))   doc["temp"]     = t;
  if (!isnan(h))   doc["hum"]      = h;
  if (!isnan(lux)) doc["lux"]      = lux;
  if (!isnan(p))   doc["pressure"] = p;
  doc["eco2"] = eco2;
  doc["tvoc"] = tvoc;

  char out[512];
  size_t n = serializeJson(doc, out);
  client.publish(mqtt_topic_environment, out, n);
}

void showTftMessage(const String& line1, const String& line2 = "", uint16_t color = TFT_YELLOW) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(color);
  tft.setCursor(10, 80);
  tft.print(line1);
  if (line2.length()) {
    tft.setCursor(10, 110);
    tft.print(line2);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("=== SMART ALARM SYSTEM START ===");
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW);
  tft.println("Initializing...");

  Wire.begin(21, 22);
  pinMode(PIR_PIN, INPUT);
  pinMode(RESET_WIFI_PIN, INPUT_PULLUP);

  uint8_t id76 = getBME_ID(0x76);
  uint8_t id77 = getBME_ID(0x77);
  if      (id76 == 0x60 && bme.begin(0x76)) HAS_BME = true;
  else if (id76 == 0x58 && bme.begin(0x76)) HAS_BMP = true;
  else if (id77 == 0x60 && bme.begin(0x77)) HAS_BME = true;
  else if (id77 == 0x58 && bme.begin(0x77)) HAS_BMP = true;

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) HAS_BH = true;

  hdc1080.begin(0x40);
  float test = hdc1080.readTemperature();
  if (test > -40 && test < 125) HAS_HDC = true;

  pinMode(CCS_WAK, OUTPUT);
  digitalWrite(CCS_WAK, LOW);
  HAS_CCS = ccs.begin();

  initI2S();

  if (digitalRead(RESET_WIFI_PIN) == LOW) {
    Serial.println("RESET button held — clearing saved Wi-Fi credentials...");
    showTftMessage("Скидання Wi-Fi...", "Відпустіть кнопку", TFT_RED);
    delay(3000);
    WiFiManager wm;
    wm.resetSettings();
    Serial.println("Wi-Fi credentials cleared. Restarting...");
    ESP.restart();
  }
  showTftMessage("Підключення", "до Wi-Fi...");

  WiFiManager wm;

  wm.setTitle("MateWake Smart Alarm");
  wm.setConnectTimeout(30);     
  wm.setConfigPortalTimeout(180); 

  bool connected = wm.autoConnect("MateWake-Setup");

  if (!connected) {
    Serial.println("Wi-Fi connection failed — restarting");
    showTftMessage("Wi-Fi помилка", "Перезавантаження...", TFT_RED);
    delay(3000);
    ESP.restart();
  }

  Serial.print("Wi-Fi connected! IP: ");
  Serial.println(WiFi.localIP());
  showTftMessage("Wi-Fi OK", WiFi.localIP().toString(), TFT_GREEN);
  delay(1500);

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(4096);

  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");

  tft.fillScreen(TFT_BLACK);
}

unsigned long lastUpdate = 0;

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) reconnectMQTT();
    client.loop();
  }

  static unsigned long lastDbg = 0;
  if (millis() - lastDbg > 5000) {
    lastDbg = millis();
    Serial.printf("DBG WiFi=%s MQTT=%s IP=%s\n",
                  WiFi.status() == WL_CONNECTED ? "OK" : "NO",
                  client.connected() ? "OK" : "NO",
                  WiFi.localIP().toString().c_str());
  }

  if (millis() - lastUpdate > 1000) {
    lastUpdate = millis();

    int pir = readStablePIR();

    float t   = (HAS_BME || HAS_BMP) ? bme.readTemperature() : NAN;
    float p   = (HAS_BME || HAS_BMP) ? bme.readPressure() / 100.0 : NAN;
    float h   = HAS_BME ? bme.readHumidity() : NAN;
    float lux = HAS_BH  ? lightMeter.readLightLevel() : NAN;
    int mic   = readMIC();

    int eco2 = -1, tvoc = -1;
    if (HAS_CCS && ccs.available() && !ccs.readData()) {
      eco2 = ccs.geteCO2();
      tvoc = ccs.getTVOC();
    }

    String now = nowHHMM();

    pushDisturbanceSample(mic, lux, pir);

    if (millis() - lastWindowProcess >= 600000UL) {
      lastWindowProcess = millis();
      processDisturbanceWindow(now);
    }

    Mode mode = getMode(now);
    Phase baseSch = hasSegments ? getScheduledPhase(now) : PH_UNKNOWN;

    if (!phaseMachineStarted && hasSegments)
      initPhaseMachineFromSchedule(now);

    updatePhaseMachine();

    Phase activeSch = phaseMachineStarted ? currentPhase : baseSch;
    Phase real = activeSch;

    bool lightBySensors = (pir == 1) || (mic > 200) || (!isnan(lux) && lux > 20);

    if (mode == MODE_WAKE) {
      if ((hasSegments && isGoodToWake(real)) || (!hasSegments && lightBySensors)) {
        if (!wakeTimeLocked && now != "") {
          optimizedWakeTime = now;
          wakeTimeLocked = true;
        }
      } else {
        if (!wakeTimeLocked) optimizedWakeTime = wakeEnd;
      }
    } else {
      optimizedWakeTime = recommendedWakeTime;
      wakeTimeLocked = false;
    }

    unsigned long interval = (mode == MODE_WAKE) ? 10000UL : 60000UL;
    bool importantChange = (real != lastRealPhase) || (optimizedWakeTime != lastSentWake);

    if ((mode == MODE_PREWAKE || mode == MODE_WAKE) &&
        WiFi.status() == WL_CONNECTED && client.connected() &&
        (millis() - lastSend > interval || importantChange)) {
      lastSend = millis();
      lastRealPhase = real;
      lastSentWake = optimizedWakeTime;
      publishOptimized(now, mode, baseSch, activeSch, real, mic, lux, pir, eco2, tvoc, t, h);
      Serial.println("[MQTT] optimized sent");
    }

    if (WiFi.status() == WL_CONNECTED && client.connected() &&
        (millis() - lastEnvironmentSend >= 30000UL)) {
      lastEnvironmentSend = millis();
      publishEnvironment(now, t, h, lux, eco2, tvoc, p);
      Serial.println("[MQTT] environment sent");
    }

    Serial.println("------ DATA ------");
    Serial.printf("TIME: %s | MODE: %s\n", now.length() ? now.c_str() : "--:--", modeName(mode));
    Serial.printf("Temp: %.2f | Hum: %.2f | Lux: %.2f | MIC: %d | PIR: %d\n", t, h, lux, mic, pir);
    Serial.printf("eCO2: %d | TVOC: %d\n", eco2, tvoc);
    Serial.printf("Disturbance score: %d\n", getDisturbanceScore());
    Serial.printf("Phase (base->active->real): %s -> %s -> %s\n",
                  phaseName(baseSch), phaseName(activeSch), phaseName(real));
    Serial.printf("wakeStart=%s wakeEnd=%s\n", wakeStart.c_str(), wakeEnd.c_str());
    if (recommendedWakeTime != "")
      Serial.printf("Planned wake: %s | Optimized wake: %s\n",
                    recommendedWakeTime.c_str(), optimizedWakeTime.c_str());
    Serial.println("------------------");
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(5);
    tft.setTextColor(TFT_CYAN);
    String timeStr = now.length() ? now : "--:--";
    int timeX = (tft.width() - timeStr.length() * 30) / 2;
    tft.setCursor(timeX, 5);
    tft.print(timeStr);
    tft.drawFastHLine(5, 58, tft.width() - 10, 0x4208);
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW);
    tft.setCursor(10, 66);  tft.print("TEMP");
    tft.setCursor(tft.width() / 2 + 10, 66); tft.print("HUM");
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(10, 86);
    char tempBuf[10]; snprintf(tempBuf, sizeof(tempBuf), "%.1fC", t);
    tft.print(tempBuf);
    tft.setCursor(tft.width() / 2 + 10, 86);
    char humBuf[10]; snprintf(humBuf, sizeof(humBuf), "%.0f%%", h);
    tft.print(humBuf);
    tft.drawFastHLine(5, 108, tft.width() - 10, 0x4208);
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW);
    tft.setCursor(10, 116); tft.print("PRESS");
    tft.setCursor(tft.width() / 2 + 10, 116); tft.print("CO2");
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(10, 136);
    char presBuf[12]; snprintf(presBuf, sizeof(presBuf), "%.0fhPa", p);
    tft.print(presBuf);
    tft.setCursor(tft.width() / 2 + 10, 136);
    char eco2Buf[10]; snprintf(eco2Buf, sizeof(eco2Buf), "%dppm", eco2 > 0 ? eco2 : 0);
    tft.print(eco2Buf);
    tft.drawFastHLine(5, 158, tft.width() - 10, 0x4208);
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW);
    int tvocLabelX = (tft.width() - 4 * 12) / 2;
    tft.setCursor(tvocLabelX, 166); tft.print("TVOC");
    tft.setTextColor(TFT_WHITE);
    char tvocBuf[12]; snprintf(tvocBuf, sizeof(tvocBuf), "%dppb", tvoc > 0 ? tvoc : 0);
    int tvocX = (tft.width() - strlen(tvocBuf) * 12) / 2;
    tft.setCursor(tvocX, 186);
    tft.print(tvocBuf);

    if (WiFi.status() != WL_CONNECTED || !client.connected()) {
      tft.setTextSize(1);
      tft.setCursor(10, tft.height() - 10);
      tft.setTextColor(TFT_RED);
      tft.print(WiFi.status() != WL_CONNECTED ? "! WiFi OFF" : "! MQTT OFF");
    }
  }
}