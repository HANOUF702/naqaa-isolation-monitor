# Wiring & Pinout — Naqaa

## Sensor Node (ESP32)
- **NTC:** GPIO 32
- **MQ:** GPIO 34
- **LED_SAFE:** GPIO 4
- **LED_WATCH:** GPIO 16
- **LED_ALERT:** GPIO 17
- **BUZZER_PIN:** GPIO 22
- **RFID-RC522:** SS = GPIO 5, RST = GPIO 27 (SPI: SCK = 18, MISO = 19, MOSI = 23)
- **Display MAC Target:** `8C:94:DF:4F:D2:38`

## Display Node (ESP32-2432S028 / CYD)
- **Backlight (`TFT_BL_PIN`):** GPIO 21
- **Display MAC:** `8C:94:DF:4F:D2:38`
