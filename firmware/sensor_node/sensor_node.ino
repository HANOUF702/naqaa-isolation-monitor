#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <MFRC522.h>

// المنافذ الثابتة للوحة البريدبورد
const int NTC_PIN    = 32;
const int MQ_PIN     = 34;
const int LED_SAFE   = 4;
const int LED_WATCH  = 16;
const int LED_ALERT  = 17;
const int BUZZER_PIN = 22;

// منافذ RFID-RC522
#define RFID_SS_PIN  5
#define RFID_RST_PIN 27
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

// عتبات NTC معايرة أعلى من قراءة الغرفة الحالية (~3370-3605) لتكون الغرفة SAFE
const int NTC_LOW_ALERT   = 500;
const int NTC_ENTER_WATCH = 3620;
const int NTC_EXIT_WATCH  = 3605;
const int NTC_ENTER_ALERT = 3680;
const int NTC_EXIT_ALERT  = 3665;

// عتبات MQ معايرة (MQ_LOW_WATCH = -1 لتفادي التحذيرات الخاطئة من هبوط لحظي)
const int MQ_LOW_WATCH    = -1;
const int MQ_ENTER_WATCH  = 260;
const int MQ_EXIT_WATCH   = 230;
const int MQ_ENTER_ALERT  = 420;
const int MQ_EXIT_ALERT   = 380;

// MAC Address الخاص بشاشة ESP32-2432S028R
uint8_t displayMacAddress[] = {0x8C, 0x94, 0xDF, 0x4F, 0xD2, 0x38};
bool enableEspNowSend = true;

typedef struct __attribute__((packed)) {
  int rawAdc;
  int mqAdc;
  uint8_t stateCode;     // 0 = SAFE, 1 = WATCH, 2 = ALERT
  char stateLabel[8];
  uint16_t rfidScanCount;
  char lastCardUid[18];
  char staffRole[14];
  char causeLabel[16];
} NaqaaPacket;

NaqaaPacket packetToSend;
int currentState = 0;
int ntcState = 0;
int mqState = 0;
int lastPrintedState = -1;

uint16_t rfidScanCount = 0;
char currentUidStr[18] = "NONE";
char currentRoleStr[14] = "NONE";
unsigned long lastCardTime = 0;
char lastProcessedUid[18] = "";

// حالة قفل إنذار البطاقة غير المسموحة (يبقى حتى يُلغى عبر بطاقة مصرحة أو أمر إيقاف من الموقع)
bool unauthAlarmLatched = false;

// تحكم غير معطّل بالجرس
unsigned long lastBuzzerPulseMs = 0;
unsigned long buzzerOffAtMs = 0;
bool buzzerActive = false;

int remainingSoftBeeps = 0;
unsigned long nextSoftBeepAtMs = 0;

void triggerBuzzerMs(unsigned long durationMs) {
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerActive = true;
  buzzerOffAtMs = millis() + durationMs;
}

// نغمة تأكيد خفيفة وسريعة ("طن طن") للبطاقات المسموحة
void startAuthorizedSoftDoubleBeep() {
  remainingSoftBeeps = 2;
  nextSoftBeepAtMs = millis();
}

// دالة استدعاء جاهزة لربط زر/أمر "إيقاف الإنذار" من موقع الأدمن لاحقاً
void clearUnauthAlarmFromAdmin() {
  unauthAlarmLatched = false;
  if (strcmp(currentRoleStr, "UNAUTH") == 0) {
    strncpy(currentRoleStr, "NONE", sizeof(currentRoleStr) - 1);
    currentRoleStr[sizeof(currentRoleStr) - 1] = '\0';
  }
  digitalWrite(BUZZER_PIN, LOW);
  buzzerActive = false;
}

void updateBuzzer() {
  unsigned long now = millis();

  if (buzzerActive && (long)(now - buzzerOffAtMs) >= 0) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
  }

  // أولوية لنبضات "طن طن" الخفيفة السريعة عند تمرير بطاقة مسموحة
  if (remainingSoftBeeps > 0) {
    if (!buzzerActive && (long)(now - nextSoftBeepAtMs) >= 0) {
      triggerBuzzerMs(45);   // نبضة خفيفة وسريعة
      remainingSoftBeeps--;
      nextSoftBeepAtMs = now + 120; // فاصل سريع بين النبضتين
    }
    return;
  }

  // رنين مستمر للبازر عند بطاقة غير مسموحة (UNAUTH) حتى يتم إيقافه من الأدمن
  if (unauthAlarmLatched) {
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerActive = true;
    buzzerOffAtMs = now + 200;
    return;
  }

  if (currentState == 0) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
    return;
  }

  // تنبيه متقطع للحساسات (WATCH / ALERT ناتج عن NTC أو MQ)
  unsigned long intervalMs = (currentState == 2) ? 2200 : 5000;
  unsigned long pulseMs    = (currentState == 2) ? 140  : 60;

  if (!buzzerActive && (now - lastBuzzerPulseMs >= intervalMs)) {
    lastBuzzerPulseMs = now;
    triggerBuzzerMs(pulseMs);
  }
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
  Serial.print("ESP-NOW Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

const char* getStateName(int state) {
  if (state == 1) return "WATCH";
  if (state == 2) return "ALERT";
  return "SAFE";
}

// تصنيف البطاقات: DOCTOR / NURSE / GUEST مسموحة، و UNAUTH غير مسموحة
void classifyCard(const char* uidStr, const char* &roleOut, bool &isAuthorizedOut) {
  if (strcmp(uidStr, "E9 AE 14 05") == 0) {
    roleOut = "DOCTOR";
    isAuthorizedOut = true;
  } else if (strcmp(uidStr, "1A 52 67 32") == 0) {
    roleOut = "NURSE";
    isAuthorizedOut = true;
  } else if (strcmp(uidStr, "45 49 E5 00") == 0) {
    roleOut = "GUEST";
    isAuthorizedOut = true;
  } else if (strcmp(uidStr, "0B FE D1 3E") == 0) {
    roleOut = "UNAUTH";
    isAuthorizedOut = false;
  } else {
    roleOut = "UNAUTH";
    isAuthorizedOut = false;
  }
}

void pollRfid() {
  if (!rfid.PICC_IsNewCardPresent()) {
    return;
  }
  if (!rfid.PICC_ReadCardSerial()) {
    return;
  }

  char uidBuf[18] = {0};
  int pos = 0;
  for (byte i = 0; i < rfid.uid.size && pos < (int)sizeof(uidBuf) - 4; i++) {
    pos += snprintf(uidBuf + pos, sizeof(uidBuf) - pos, "%s%02X", (i == 0 ? "" : " "), rfid.uid.uidByte[i]);
  }

  unsigned long now = millis();
  if (strcmp(uidBuf, lastProcessedUid) != 0 || (now - lastCardTime > 1500)) {
    strncpy(currentUidStr, uidBuf, sizeof(currentUidStr) - 1);
    currentUidStr[sizeof(currentUidStr) - 1] = '\0';

    strncpy(lastProcessedUid, uidBuf, sizeof(lastProcessedUid) - 1);
    lastProcessedUid[sizeof(lastProcessedUid) - 1] = '\0';

    lastCardTime = now;
    rfidScanCount++;

    const char* roleName = "NONE";
    bool isAuthorized = true;
    classifyCard(currentUidStr, roleName, isAuthorized);

    strncpy(currentRoleStr, roleName, sizeof(currentRoleStr) - 1);
    currentRoleStr[sizeof(currentRoleStr) - 1] = '\0';

    if (isAuthorized) {
      // بطاقة مسموحة: لا ترفع إنذار، وتطلق نبضتين خفيفتين وسريعتين ("طن طن")
      unauthAlarmLatched = false;
      startAuthorizedSoftDoubleBeep();
    } else {
      // بطاقة غير مسموحة: تفعّل إنذار ALERT وجرس مستمر حتى إيقافه
      unauthAlarmLatched = true;
      digitalWrite(BUZZER_PIN, HIGH);
      buzzerActive = true;
    }

    Serial.print("RFID Scanned -> UID: ");
    Serial.print(currentUidStr);
    Serial.print(" | Role: ");
    Serial.print(currentRoleStr);
    Serial.print(" | Auth: ");
    Serial.print(isAuthorized ? "YES" : "NO");
    Serial.print(" | Count: ");
    Serial.println(rfidScanCount);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void applyLedOutputs(int state) {
  digitalWrite(LED_SAFE,  state == 0 ? HIGH : LOW);
  digitalWrite(LED_WATCH, state == 1 ? HIGH : LOW);
  digitalWrite(LED_ALERT, state == 2 ? HIGH : LOW);
}

int stepHighState(int val, int prev, int eWatch, int xWatch, int eAlert, int xAlert) {
  if (prev == 0) {
    if (val >= eAlert) return 2;
    if (val >= eWatch) return 1;
    return 0;
  } else if (prev == 1) {
    if (val >= eAlert) return 2;
    if (val <= xWatch) return 0;
    return 1;
  } else {
    if (val <= xAlert) {
      return (val <= xWatch) ? 0 : 1;
    }
    return 2;
  }
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  delay(300);

  pinMode(LED_SAFE, OUTPUT);
  pinMode(LED_WATCH, OUTPUT);
  pinMode(LED_ALERT, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_SAFE, LOW);
  digitalWrite(LED_WATCH, LOW);
  digitalWrite(LED_ALERT, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  SPI.begin(18, 19, 23, RFID_SS_PIN);
  rfid.PCD_Init();

  WiFi.mode(WIFI_STA);
  delay(200);

  Serial.println("=== NAQAA SENSOR NODE (AUTHORIZED SOFT BEEP + UNAUTH LATCH) ===");
  Serial.print("Board MAC Address: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_send_cb(onDataSent);

  if (enableEspNowSend) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, displayMacAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      Serial.println("Display peer added for ESP-NOW");
    } else {
      Serial.println("Failed to add display peer");
    }
  }
}

void loop() {
  pollRfid();

  int ntcRaw = analogRead(NTC_PIN);
  int mqRaw  = analogRead(MQ_PIN);

  int ntcHighState = stepHighState(ntcRaw, ntcState, NTC_ENTER_WATCH, NTC_EXIT_WATCH, NTC_ENTER_ALERT, NTC_EXIT_ALERT);
  bool ntcLowAlert = (ntcRaw <= NTC_LOW_ALERT);
  ntcState = ntcLowAlert ? 2 : ntcHighState;

  int mqHighState = stepHighState(mqRaw, mqState, MQ_ENTER_WATCH, MQ_EXIT_WATCH, MQ_ENTER_ALERT, MQ_EXIT_ALERT);
  bool mqLowWatch = (MQ_LOW_WATCH >= 0) && (mqRaw <= MQ_LOW_WATCH);
  mqState = max(mqHighState, mqLowWatch ? 1 : 0);

  int rfidEffectState = unauthAlarmLatched ? 2 : 0;

  currentState = max(ntcState, max(mqState, rfidEffectState));
  const char* label = getStateName(currentState);

  const char* cause = "NONE";
  if (currentState > 0) {
    if (unauthAlarmLatched && currentState == 2) {
      cause = "RFID UNAUTH";
    } else if (ntcLowAlert && currentState == 2) {
      cause = "NTC LOW";
    } else if (ntcHighState == currentState && ntcHighState > 0) {
      cause = "NTC HIGH";
    } else if (mqHighState == currentState && mqHighState > 0) {
      cause = "MQ HIGH";
    } else if (mqLowWatch && currentState == 1) {
      cause = "MQ LOW";
    }
  }

  Serial.print("NTC (32): ");
  Serial.print(ntcRaw);
  Serial.print(" | MQ (34): ");
  Serial.print(mqRaw);
  Serial.print(" | RFID: ");
  Serial.print(currentUidStr);
  Serial.print(" (");
  Serial.print(currentRoleStr);
  Serial.print(", #");
  Serial.print(rfidScanCount);
  Serial.print(") | State: ");
  Serial.print(label);
  Serial.print(" | Cause: ");
  Serial.println(cause);

  bool stateChanged = (currentState != lastPrintedState);
  if (stateChanged) {
    lastPrintedState = currentState;
    lastBuzzerPulseMs = millis();
    if (currentState == 0 && !unauthAlarmLatched && remainingSoftBeeps == 0) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerActive = false;
    }
    Serial.print(">>> State changed to: ");
    Serial.print(label);
    Serial.print(" (Cause: ");
    Serial.print(cause);
    Serial.println(")");
  }

  applyLedOutputs(currentState);
  updateBuzzer();

  if (enableEspNowSend) {
    packetToSend.rawAdc = ntcRaw;
    packetToSend.mqAdc  = mqRaw;
    packetToSend.stateCode = (uint8_t)currentState;
    strncpy(packetToSend.stateLabel, label, sizeof(packetToSend.stateLabel) - 1);
    packetToSend.stateLabel[sizeof(packetToSend.stateLabel) - 1] = '\0';
    packetToSend.rfidScanCount = rfidScanCount;
    strncpy(packetToSend.lastCardUid, currentUidStr, sizeof(packetToSend.lastCardUid) - 1);
    packetToSend.lastCardUid[sizeof(packetToSend.lastCardUid) - 1] = '\0';
    strncpy(packetToSend.staffRole, currentRoleStr, sizeof(packetToSend.staffRole) - 1);
    packetToSend.staffRole[sizeof(packetToSend.staffRole) - 1] = '\0';
    strncpy(packetToSend.causeLabel, cause, sizeof(packetToSend.causeLabel) - 1);
    packetToSend.causeLabel[sizeof(packetToSend.causeLabel) - 1] = '\0';

    esp_now_send(displayMacAddress, (uint8_t *)&packetToSend, sizeof(packetToSend));
  }

  delay(350);
}