# Naqaa Isolation Monitor

An ESP32-based isolation room monitoring system using ESP-NOW wireless communication between a sensor node (NTC, MQ, RFID-RC522, LEDs, Buzzer) and an ESP32-2432S028 (CYD) display node.

## Structure
- `firmware/sensor_node/sensor_node.ino`: Sensor readings, hysteresis state machine (`SAFE`, `WATCH`, `ALERT`), RFID role classification (`DOCTOR`, `NURSE`, `GUEST`, `UNAUTH`), buzzer alerts, and ESP-NOW transmission.
- `firmware/display_cyd/display_cyd.ino`: Splash screen and real-time CYD monitor UI over ESP-NOW.
- `firmware/display_cyd/Naqaa.h`: Logo bitmap header.
- `docs/wiring-and-pinout.md`: Hardware pinout, thresholds, and MAC addresses.
