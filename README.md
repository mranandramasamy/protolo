# Protolo: ESP32 E-Paper Content Engine & Local Dashboard

Protolo is an offline-first, battery-optimized productivity companion built on the ESP32. It uses an E-Paper display to render notes, flashcards, stories, and memory pegs while consuming practically zero power. 

Instead of relying on external cloud APIs, Protolo acts as its own local web server. You connect to it via Wi-Fi, manage your content through a clean web dashboard, and sync it directly to the device's permanent flash memory.

## ✨ Features

* **🔋 Ultra-Low Power Architecture:** The ESP32 spends most of its life offline or in deep sleep. The Wi-Fi radio is strictly spun up on demand and aggressively torn down the moment you exit the sync menu to preserve battery.
* **🌐 Local-First Web Dashboard:** No cloud, no subscriptions. The device hosts its own HTML/JS interface on your local network. 
* **🧠 Chunked Transfer Encoding:** Engineered to handle massive amounts of text and JSON data without crashing. The web server streams data to the browser in memory-safe chunks, bypassing ESP32 RAM fragmentation limits.
* **💾 Persistent Storage (LittleFS):** All data is serialized via `ArduinoJson` and written directly to the ESP32's internal flash memory (`/config.json`). Your content survives reboots, deep sleep cycles, and battery swaps.
* **📚 Rich Content Modules:**
  * **Sleep Quotes:** Custom lockscreen text that remains on the e-paper display while the device is fully powered off.
  * **Notes Scratchpad:** Directory for standard text notes.
  * **Memorize Pegs:** Sequenced lists for active recall.
  * **Flashcard Decks:** Nested front-to-back study cards.
  * **Long Stories:** Scrollable long-form text reading.

## 🛠️ Hardware Requirements

* ESP32 Microcontroller (with at least 4MB Flash)
* SPI E-Paper Display (e-ink)
* Physical Push Buttons (Boot/Select, Power/Back)
* Battery & Power Management Circuit

## 📦 Software Dependencies

This project is built using the Arduino IDE framework for ESP32. Ensure you have the following libraries installed:
* `ArduinoJson` (v6 or v7)
* `lvgl` (Light and Versatile Graphics Library)
* `WebServer` & `WiFi` (Built into ESP32 core)
* `LittleFS` (Built into ESP32 core)

### The core component library was fetched from 
[ESP32-S3-ePaper-1.54](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54)
