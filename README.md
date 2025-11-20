# IoT Smart Thermometer

## 📋 Overview
This project implements a **Wi-Fi and Cloud-connected Smart Thermometer** designed for real-time temperature monitoring during cooking or grilling.  
Built on the **Arduino Uno WiFi Rev2**, the system combines accurate sensing (Type-K thermocouple + MAX31856 amplifier) with **MQTT over TLS** communication and a **local web server** for dual connectivity.

Users can:
- View live temperatures on a **TFT LCD display**  
- Receive secure **MQTT updates** via HiveMQ Cloud  
- Control target temperature remotely or through the local network  
- Get a **green LED alert** when the desired temperature is reached  

---

## Features
- **Secure Cloud Communication:** MQTT over TLS (port 8883)  
- **Local + Remote Access:** HTTP web server + HiveMQ Cloud dashboard  
- **Real-Time Updates:** Publishes data every 2 seconds with ~300 ms latency  
- **Visual Feedback:** Synchronized LED + LCD alerts on completion  
- **Offline Resilience:** Non-blocking Wi-Fi logic ensures continuous sensing  
- **Low Power:** ~230 mA @ 5 V average consumption  

---

## Hardware Components
| Component | Description |
|------------|-------------|
| Arduino Uno WiFi Rev2 | Main MCU with NINA-W102 Wi-Fi module |
| MAX31856 Thermocouple Amplifier | SPI-based precision temperature converter |
| Type-K Thermocouple Probe | Industrial-grade sensor (0–1000 °F) |
| Adafruit ST7789 TFT Display | 1.14" color LCD for live readings |
| Green LED + 220 Ω Resistor | Visual "done" indicator |
| Breadboard (400-tie) | Compact prototyping platform |
| 5 V USB Power Supply | Stable power input |

---

## Software & Libraries
| Library | Purpose |
|----------|----------|
| `WiFiNINA` | Handles Wi-Fi + TLS security |
| `ArduinoMqttClient` | MQTT publish/subscribe logic |
| `Adafruit_MAX31856` | Thermocouple interface |
| `Adafruit_GFX` & `Adafruit_ST7789` | Display control + graphics rendering |

## Results Summary
| Metric | Value | Notes |
|---------|-------|-------|
| Latency | ~300 ms | Avg. end-to-end MQTT delay |
| Accuracy | ±1 °F | Compared to calibrated probe |
| Uptime | 99.9 % | Verified over 8 h continuous test |
| Power | 230 mA @ 5 V | Efficient for IoT device |

---

## Security
- TLS 1.3 encryption via WiFiNINA  
- HiveMQ Cloud authentication (username/password)  
- Secure MQTT topic namespace per MAC address  

### 1. Clone Repository
```bash
git clone https://github.com/<your-username>/iot-smart-thermometer.git
cd iot-smart-thermometer
