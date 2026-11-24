#include <WiFi.h>
#include <esp_now.h>
#include <TFT_eSPI.h>
#include "Naqaa.h"

#define TFT_BL_PIN 21
TFT_eSPI tft = TFT_eSPI();

const unsigned long SPLASH_DURATION_MS = 10000;
unsigned long splashStartTime = 0;
bool splashFinished = false;
bool monitorInitialized = false;

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

volatile bool newDataAvailable = false;
NaqaaPacket latestPacket = {0, 0, 0, "SAFE", 0, "NONE", "NONE", "NONE"};

int lastDrawnStateCode = -1;
bool staticLayoutDrawn = false;

uint16_t getStateColor(uint8_t code) {
  if (code == 1) return TFT_CYAN;
  if (code == 2) return TFT_RED;
  return TFT_GREEN;
}

void drawStaticLayout(uint8_t stateCode) {
  uint16_t stateColor = getStateColor(stateCode);

  tft.fillScreen(TFT_BLACK);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("NAQAA ISOLATION MONITOR", 10, 6);

  tft.fillCircle(304, 14, 6, stateColor);
  tft.drawFastHLine(10, 28, 302, stateColor);

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("ESP-NOW Active | Sensor: 30:76:F5:F0:0D:A8", 10, 218);

  staticLayoutDrawn = true;
  lastDrawnStateCode = (int)stateCode;
}

void updateDynamicFields(const NaqaaPacket &pkt) {
  uint16_t stateColor = getStateColor(pkt.stateCode);

  if (!staticLayoutDrawn || lastDrawnStateCode != (int)pkt.stateCode) {
    drawStaticLayout(pkt.stateCode);
  }

  tft.setTextSize(3);
  tft.setTextColor(stateColor, TFT_BLACK);
  String stateLine = "STATE: " + String(pkt.stateLabel) + "    ";
  tft.drawString(stateLine, 10, 38);

  tft.setTextSize(2);
  tft.setTextColor(stateColor, TFT_BLACK);
  String causeLine = "CAUSE: " + String(pkt.causeLabel) + "      ";
  tft.drawString(causeLine, 10, 74);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String ntcLine = "NTC ADC: " + String(pkt.rawAdc) + "    ";
  String mqLine  = "MQ  ADC: " + String(pkt.mqAdc) + "    ";
  tft.drawString(ntcLine, 10, 106);
  tft.drawString(mqLine,  10, 130);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  String rfidLine = "RFID: " + String(pkt.lastCardUid) + "      ";
  tft.drawString(rfidLine, 10, 160);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String roleLine = "Role: " + String(pkt.staffRole) + " (#" + String(pkt.rfidScanCount) + ")      ";
  tft.drawString(roleLine, 10, 186);
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
#else
void onDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
#endif
  if (len == sizeof(NaqaaPacket)) {
    memcpy((void *)&latestPacket, incomingData, sizeof(NaqaaPacket));
    newDataAvailable = true;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);
  tft.pushImage(0, 0, Naqaa_width, Naqaa_height, Naqaa);
  splashStartTime = millis();

  WiFi.mode(WIFI_STA);
  delay(200);

  Serial.print("Display MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW on Display");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);
  Serial.println("Listening for NAQAA packets via ESP-NOW...");
}

void loop() {
  unsigned long now = millis();

  if (!splashFinished) {
    if (now - splashStartTime >= SPLASH_DURATION_MS) {
      splashFinished = true;
    } else {
      delay(20);
      return;
    }
  }

  if (!monitorInitialized) {
    drawStaticLayout(latestPacket.stateCode);
    updateDynamicFields(latestPacket);
    monitorInitialized = true;
  }

  if (newDataAvailable) {
    noInterrupts();
    NaqaaPacket copyPkt;
    memcpy(&copyPkt, (const void *)&latestPacket, sizeof(NaqaaPacket));
    newDataAvailable = false;
    interrupts();

    Serial.print("Received -> NTC: ");
    Serial.print(copyPkt.rawAdc);
    Serial.print(" | MQ: ");
    Serial.print(copyPkt.mqAdc);
    Serial.print(" | Cause: ");
    Serial.print(copyPkt.causeLabel);
    Serial.print(" | State: ");
    Serial.print(copyPkt.stateLabel);
    Serial.print(" | RFID: ");
    Serial.print(copyPkt.lastCardUid);
    Serial.print(" | Role: ");
    Serial.print(copyPkt.staffRole);
    Serial.print(" (#");
    Serial.print(copyPkt.rfidScanCount);
    Serial.println(")");

    updateDynamicFields(copyPkt);
  }

  delay(50);
}
