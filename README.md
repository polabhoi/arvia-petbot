# 🤖 Arvia: Gemini AI Voice Assistant (ESP32)

Arvia is a personalized voice assistant built for the ESP32 platform, utilizing animated facial expressions displayed on dual OLED screens. It integrates deep learning models for Speech-to-Text (Deepgram) and Generative AI (Gemini), all powered by an integrated WiFi Manager for easy setup.

## ✨ Features

- **Dual OLED Display**: One OLED (U8g2) displays expressive, animated faces based on the robot's state, and a second OLED (Adafruit GFX) displays text responses or status messages.
- **Voice Interaction (AI Mode)**: Uses an I2S microphone to record user speech, sends the audio to the Deepgram API for transcription, and forwards the transcription to the Gemini API for a concise, single-sentence response.
- **Integrated WiFi Manager**: Automatically hosts an access point (`arvia` / `adminpola1`) if no saved credentials are found, or if the MODE button is held for 5 seconds.
- **Expressive Animations**: Features dedicated states for Normal, AI Active, Angry, Sad, Happy, and Mode transitions.
- **Power-Saving Clock Mode**: Automatically transitions to a digital clock display during extended inactivity.
- **User Feedback**: Touch inputs for recording, mode toggling, and instant emotional responses.

## 🛠️ Hardware Requirements & Pinout

This project is designed for an ESP32 Dev Board with two TTP223 capacitive touch sensors, two 128x64 OLED displays, and an I2S digital microphone (e.g., InMP441).

### ESP32 Pin Connections

| Function | Component | ESP32 GPIO Pin |
|----------|-----------|----------------|
| **I2S Microphone** | I2S_WS (Word Select) | 27 |
|  | I2S_SD (Data) | 35 |
|  | I2S_SCK (Bit Clock) | 26 |
| **I2C OLED Displays** | SDA (Data) | Standard I2C Pin (e.g., GPIO 21) |
|  | SCL (Clock) | Standard I2C Pin (e.g., GPIO 22) |
| **Touch Inputs (TTP223)** | Record Trigger (TTP223_PIN) | 13 |
|  | Mode Toggle/Reset (MODE_TTP223_PIN) | 12 |
|  | Expression Control (EXPRESSION_TTP223_PIN) | 14 |

> **Note**: The I2C pins (SDA/SCL) are determined by your specific ESP32 board. The code assumes standard I2C connection is established via the Wire.h library.

## ⚙️ Software Setup

### 1. Arduino IDE and Board Setup

- Install the ESP32 board support package in the Arduino IDE.
- Set the Partition Scheme to "No OTA (Large APP)" or another scheme that allows enough space for the large bitmap array and the SPIFFS filesystem.
- Format the SPIFFS file system before uploading for the first time (Tools -> "ESP32 Data Sketch Upload" is recommended after installing the necessary Python tool).

### 2. Libraries

Ensure you have the following libraries installed via the Arduino Library Manager:

- **U8g2** (Universal 8bit Graphics Library)
- **Adafruit GFX Library**
- **Adafruit SSD1306**
- **ArduinoJson**
- **WebServer** (The ESP32 built-in WebServer library)

The I2S driver (`driver/i2s_std.h`) is part of the ESP32 framework and should be available automatically.

### 3. API Keys

Before compiling, you MUST replace the placeholder API keys in `wifimanager.ino`:

```cpp
// === API Keys ===
// === Deepgram API Key ===
const char* deepgramApiKey = "YOUR_DEEPGRAM_API_KEY"; // REPLACE ME
// Gemini (kept from original; replace if needed)
const char* gemini_KEY = "YOUR_GEMINI_API_KEY";       // REPLACE ME
```

## 🕹️ Usage Instructions

### 1. WiFi Configuration (Initial Setup or Reset)

- The device will automatically enter the configuration portal if it can't connect to a saved WiFi network.
- To force the portal: Hold the Mode Toggle/Reset pin (GPIO 12) continuously for 5 seconds.
- Connect to the Hotspot: On your phone or PC, connect to the WiFi Access Point named `arvia` using the password `adminpola1`.
- Configure: Open a web browser to `192.168.4.1` to enter your home WiFi SSID and password. The device will save credentials to SPIFFS and reboot.

### 2. Modes and Interaction

The device operates in two primary modes, toggled by a short press of the Mode Toggle/Reset pin (GPIO 12).

| Mode | Input Action | Pin | Resulting Action/State |
|------|--------------|-----|------------------------|
| **NORMAL (Default)** | Short press | GPIO 12 (Mode) | Switches to AI Mode (with animation transition) |
| **AI ACTIVE** | Short press | GPIO 12 (Mode) | Switches to NORMAL Mode (with animation transition) |
| **AI ACTIVE** | Touch / Hold | GPIO 13 (Record) | Starts/Stops audio recording for AI processing. |
| **ANY MODE** | Short Tap | GPIO 14 (Expression) | Triggers the ANGRY expression (3 seconds). |
| **ANY MODE** | Long Press (>2s) | GPIO 14 (Expression) | Triggers the HAPPY expression (5 seconds). |
| **IDLE** | Inactivity (25s) | N/A | Switches to SAD expression (10s) -> then Clock Display. |
| **CLOCK** | Any Touch Input | All pins | Exits clock mode and returns to AI/NORMAL (based on last state). |

### 3. AI Interaction Details

- Ensure you are in **AI ACTIVE** mode (faces 16-35 playing).
- Touch the **Record Pin (GPIO 13)** and speak your query.
- Release the **Record Pin (GPIO 13)** to stop recording and initiate processing.
- The recorded WAV file is sent to Deepgram for STT.
- The transcription (max 15 words) is sent to Gemini for a concise text response.
- The response is displayed on the second OLED screen for 5 seconds (ANSWER_DISPLAY state).

## 🖼️ Face Animation Indexes

The sketch uses an array of 130 bitmap frames (`epd_bitmap_allArray`) to generate the expressive animations.

| State Name | Face Index Range | Description |
|------------|------------------|-------------|
| MODE_CHANGE (N → AI) | 00 - 15 | Transition animation from Normal to AI mode. |
| AI_ACTIVE | 16 - 35 | Active, listening, or thinking animation in AI mode. |
| ANGRY | 36 - 51 | Animated expression triggered by a short tap on the expression pin. |
| SAD | 52 - 82 | Long animation sequence for when the device is idle. |
| NORMAL (Default) | 83 - 99 | Default blinking and subtle movements. |
| HAPPY | 100 - 113 | Animated expression triggered by a long press on the expression pin (petting). |
| MODE_CHANGE (AI → N) | 114 - 129 | Transition animation from AI mode back to Normal. |

The raw bitmap data is contained in the accompanying `faces.h` header file.
