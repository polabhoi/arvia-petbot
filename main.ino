// ==================== VOICE ASSISTANT WITH GEMINI AI - OLED FACES OUTPUT + WIFI MANAGER
// Integrated single-file sketch: faces, AI, STT (Deepgram), Gemini, I2S recording
// + WiFi Manager (AP "arvia", password "adminpola1") to configure WiFi via phone
// ===================================================================================

#include <WiFi.h>
#include <SPIFFS.h>
#include "driver/i2s_std.h"
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>                 // for NTP sync and local time

#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <WebServer.h> // WiFi manager web server

// Include your faces header (must provide epd_bitmap_allArray and frames 0..129)
#include "faces.h"

// === WiFi Credentials (defaults, NOT used when WiFi Manager runs) ===
const char* ssid = "alpha";
const char* password = "polastee";

// === API Keys ===
// === Deepgram API Key ===
const char* deepgramApiKey = "ed4cfc0929d0662f5ab9fc0427886742e85a1b97";

// Gemini (kept from original; replace if needed)
const char* gemini_KEY = "AIzaSyCDdiCu7BDxP1RQeYYtytTDwq4fcjoj8Go";

// === Pin Definitions ===
#define TTP223_PIN 13            // touch to record (AI mode only)
#define MODE_TTP223_PIN 12       // touch to toggle NORMAL <-> AI (short) OR reset WiFi (hold)
#define EXPRESSION_TTP223_PIN 14 // TTP223 for expressions

// === I2S pins ===
#define I2S_WS      27
#define I2S_SD      35
#define I2S_SCK     26

// === OLED with U8g2 for faces ===
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// === OLED with Adafruit for text ===
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 textDisplay(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// === Audio Settings ===
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define GAIN_BOOSTER_I2S 10
#define AUDIO_FILE "/test_audio.wav"

// === Gemini Settings ===
#define MAX_TOKENS 100
#define TIMEOUT_GEMINI 10000
#define TIMEOUT_DEEPGRAM 20000

// === Time/clock settings ===
#define IST_OFFSET_SECONDS 19800   // +5:30 hours
#define SAD_DURATION_MS 10000UL    // SAD plays for 10 seconds (unchanged)
#define IDLE_TIMEOUT_MS 25000UL    // changed: 25s idle -> SAD
#define CLOCK_REFRESH_MS 1000UL    // update clock display every 1s

// === MODE long-press / hint timing (updated per your request) ===
const unsigned long MODE_SHOW_HOLD_AFTER_MS = 2000UL; // show hold hint after 2s of continuous press
const unsigned long MODE_LONGPRESS_MS = 5000UL; // 5 seconds hold to reset WiFi / enable hotspot

// === Expression States ===
enum ExpressionState {
  MODE_CHANGE,    // 00-15 (Normal to AI) and 114-129 (AI to Normal)
  AI_ACTIVE,      // 16-35  
  ANGRY,          // 36-51
  SAD,            // 52-82
  NORMAL,         // 83-99
  HAPPY,          // 100-113
  ANSWER_DISPLAY, // Special state for showing answers
  CLOCK           // Show internet clock
};

// === Global Variables ===
i2s_chan_handle_t rx_handle;
bool flg_is_recording = false;
bool flg_I2S_initialized = false;

// touch states for record pin
bool lastTouchState = false;
unsigned long lastTouchTime = 0;

// mode toggle pin states (we'll use these to handle short vs long press)
bool lastModeTouchState = false;
unsigned long modeTouchStart = 0;
bool modeLongPressActive = false;
bool modeHoldHintShown = false;

// track whether mode button is currently being held (used to suspend animations)
bool modeCurrentlyTouched = false;

// expression touch states
bool lastExpressionTouchState = false;
bool currentExpressionTouchState = false;
unsigned long expressionTouchStartTime = 0;
unsigned long lastExpressionTime = 0;
unsigned long lastActivityTime = 0;
unsigned long lastPettingTime = 0;
bool isPetting = false;
bool angryTriggered = false;

// mode: false = NORMAL mode (default), true = AI mode
bool aiMode = false; // Start in NORMAL mode
ExpressionState currentExpression = NORMAL;
ExpressionState previousExpression = NORMAL;

// Animation control
unsigned long lastFaceUpdate = 0;
int currentFaceIndex = 83; // Start with normal faces
bool showingModeChange = false;
unsigned long modeChangeStart = 0;
unsigned long angryStartTime = 0;
bool angryShown = false;
unsigned long happyStartTime = 0;
bool normalToAI = false; // Direction of mode change

// SAD tracking
unsigned long sadStartTime = 0;

// CLOCK tracking
unsigned long lastClockRefresh = 0;

// Answer display
String currentAnswer = "";
unsigned long answerDisplayStart = 0;
bool showingAnswer = false;

// === TRANSCRIPTION STORAGE ===
String currentTranscription = "";
String previousTranscription = "";
String geminiResponse = "";
unsigned long transcriptionTime = 0;

// === User activity tracking (only true user interactions) ===
unsigned long lastUserActivityTime = 0; // <-- track only actual user input/activity

// === WAV Header Structure ===
struct WAV_HEADER {
  char  riff[4] = {'R','I','F','F'};
  long  flength = 0;
  char  wave[4] = {'W','A','V','E'};
  char  fmt[4]  = {'f','m','t',' '};
  long  chunk_size = 16;
  short format_tag = 1;
  short num_chans = 1;
  long  srate = SAMPLE_RATE;
  long  bytes_per_sec = SAMPLE_RATE * (BITS_PER_SAMPLE/8);
  short bytes_per_samp = (BITS_PER_SAMPLE/8);
  short bits_per_samp = BITS_PER_SAMPLE;
  char  dat[4] = {'d','a','t','a'};
  long  dlength = 0;
} myWAV_Header;

// === I2S Configuration ===
i2s_std_config_t std_cfg = {
  .clk_cfg = {
    .sample_rate_hz = SAMPLE_RATE,
    .clk_src = I2S_CLK_SRC_DEFAULT,
    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  },
  .slot_cfg = {
    .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
    .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
    .slot_mode = I2S_SLOT_MODE_MONO,
    .slot_mask = I2S_STD_SLOT_LEFT,
    .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
    .ws_pol = false,
    .bit_shift = true,
    .msb_right = false,
  },
  .gpio_cfg = {
    .mclk = I2S_GPIO_UNUSED,
    .bclk = (gpio_num_t) I2S_SCK,
    .ws   = (gpio_num_t) I2S_WS,
    .dout = I2S_GPIO_UNUSED,
    .din  = (gpio_num_t) I2S_SD,
    .invert_flags = {
      .mclk_inv = false,
      .bclk_inv = false,
      .ws_inv = false,
    },
  },
};

// Forward declarations of key functions used before their definitions
void displayFace(int faceIndex);
void displayAnswerBox(String answer);
void displayClock();
void updateExpression();
void setExpression(ExpressionState newExpression);
void showAnswer(String answer);
void exitClockIfActive();
void checkExpressionTouch();
bool I2S_Record_Init();
bool Record_Start(String audio_filename);
bool Record_Available(String audio_filename, float* audiolength_sec);
void startRecording();
void stopRecording();
String SpeechToText_Deepgram(String audio_filename);
String parseDeepgramResponse(String response);
String extractDeepgramTextManually(String response);
String sendToGemini(String query);
String parseGeminiResponse(String response);
String extractTextManually(String response);
void displayWifiStatus(const String &line1, const String &line2 = "");
void startConfigPortal(); // forward (already defined later)
void resetWifiNow();      // helper to delete wifiFile and startConfigPortal

// ---------------- WiFi Manager (place AFTER includes & enums, BEFORE setup()) ----------------
WebServer server(80);

String wifi_ssid = "";
String wifi_pass = "";
const char* wifiFile = "/wifi.txt";

// Save wifi creds to SPIFFS (each on its own line)
void saveWiFi(const String& ssid_in, const String& pass_in) {
  File f = SPIFFS.open(wifiFile, FILE_WRITE);
  if (!f) {
    Serial.println("Failed to open wifi file for write");
    return;
  }
  f.println(ssid_in);
  f.println(pass_in);
  f.close();
  Serial.println("WiFi saved to SPIFFS.");
}

// Load wifi creds from SPIFFS; return true if found
bool loadWiFi() {
  if (!SPIFFS.exists(wifiFile)) {
    Serial.println("WiFi file not found");
    return false;
  }

  File f = SPIFFS.open(wifiFile, FILE_READ);
  if (!f) {
    Serial.println("Failed to open wifi file for read");
    return false;
  }

  wifi_ssid = f.readStringUntil('\n');
  wifi_ssid.trim();
  wifi_pass = f.readStringUntil('\n');
  wifi_pass.trim();
  f.close();

  Serial.print("Loaded WiFi SSID: ");
  Serial.println(wifi_ssid);
  return (wifi_ssid.length() > 0);
}

void handleRoot() {
  String page =
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>PetRobot WiFi Setup</title>"
    "<style>"
      "html,body{height:100%;margin:0;font-family:Arial,Helvetica,sans-serif;background:#f4f6f8;color:#1f2937}"
      ".wrap{max-width:480px;margin:28px auto;padding:20px;background:#ffffff;border-radius:10px;box-shadow:0 6px 18px rgba(31,41,55,0.08)}"
      "h3{margin:0 0 12px 0;font-size:20px;color:#111827}"
      "p.note{margin:10px 0 0 0;color:#6b7280;font-size:13px}"
      "form{display:flex;flex-direction:column;gap:12px}"
      "label{font-size:13px;color:#374151}"
      "input[type=text],input[type=password]{width:100%;padding:10px 12px;border:1px solid #e5e7eb;border-radius:8px;box-sizing:border-box;font-size:14px;background:#fbfdff}"
      "input[type=text]:focus,input[type=password]:focus{outline:none;border-color:#60a5fa;box-shadow:0 0 0 3px rgba(96,165,250,0.08)}"
      "input[type=submit]{background:#0ea5a4;color:white;border:0;padding:10px 14px;border-radius:8px;font-size:15px;cursor:pointer;transition:transform .08s ease,box-shadow .08s ease}"
      "input[type=submit]:hover{transform:translateY(-1px);box-shadow:0 6px 18px rgba(14,165,164,0.12)}"
      ".foot{display:flex;justify-content:space-between;align-items:center;margin-top:6px;font-size:12px;color:#6b7280}"
      "@media (max-width:420px){.wrap{margin:12px;padding:14px}h3{font-size:18px}}"
    "</style>"
    "</head><body>"
    "<div class='wrap'>"
      "<h3>Arvia: WiFi Setup</h3>"
      "<form method='POST' action='/save'>"
        "<label for='ssid'>SSID</label>"
        "<input id='ssid' name='ssid' maxlength='32' type='text' placeholder='Your WiFi name (SSID)'>"
        "<label for='pass'>Password</label>"
        "<input id='pass' name='pass' type='password' maxlength='64' placeholder='WiFi password (leave empty for open networks)'>"
        "<input type='submit' value='Save & Reboot'>"
      "</form>"
      "<div class='foot'>"
        "<p class='note'>Connect to hotspot <code>arvia</code> then open this page.</p>"
        "<p style='color:#9ca3af;margin:0;font-size:12px'>v1</p>"
      "</div>"
    "</div>"
    "</body></html>";
  server.send(200, "text/html", page);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID empty");
    return;
  }

  saveWiFi(ssid, pass);

  server.send(200, "text/html",
    "<html><body><h3>Saved! Rebooting...</h3></body></html>");

  delay(800);
  ESP.restart();
}

void startConfigPortal() {
  Serial.println("\n=== STARTING WiFi CONFIG PORTAL ===");

  WiFi.mode(WIFI_AP);
  WiFi.softAP("arvia", "adminpola1"); // changed hotspot name/password

  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP: ");
  Serial.println(apIP);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);

  server.begin();
  Serial.println("Config portal running. Connect with phone to configure WiFi.");

  // Blocking loop — stay here until user submits credentials (this mirrors previous behavior)
  while (true) {
    server.handleClient();
    delay(10);
  }
}
// ---------------- end WiFi Manager ----------------

// Helper to remove wifi file and start config portal (called on long-press)
void resetWifiNow() {
  Serial.println("Resetting WiFi now (long-press)...");
  displayWifiStatus("Entering Hotspot", "Open arvia");
  delay(400);

  if (SPIFFS.exists(wifiFile)) {
    SPIFFS.remove(wifiFile);
    Serial.println("wifi file removed from SPIFFS");
  } else {
    Serial.println("wifi file not present");
  }

  delay(300);
  // Start the config portal (blocking until user saves). This avoids a reboot.
  startConfigPortal();
}

// ==================== FACE DISPLAY FUNCTIONS ====================
void displayFace(int faceIndex) {
  u8g2.clearBuffer();
  u8g2.drawXBMP(0, 0, 128, 64, epd_bitmap_allArray[faceIndex]);
  u8g2.sendBuffer();
}

void displayAnswerBox(String answer) {
  textDisplay.clearDisplay();
  
  // Draw box border
  textDisplay.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  
  // Draw title
  textDisplay.setTextSize(1);
  textDisplay.setTextColor(SSD1306_WHITE);
  textDisplay.setCursor(5, 8);
  textDisplay.println("ANSWER:");
  
  // Draw divider line
  textDisplay.drawFastHLine(0, 15, SCREEN_WIDTH, SSD1306_WHITE);
  
  // Display answer text with word wrapping
  textDisplay.setTextSize(1);
  int yPos = 20;
  int maxCharsPerLine = 25;
  
  String remainingText = answer;
  while (remainingText.length() > 0 && yPos < SCREEN_HEIGHT - 8) {
    int charsToTake = (remainingText.length() < maxCharsPerLine) ? remainingText.length() : maxCharsPerLine;
    String line = remainingText.substring(0, charsToTake);
    
    // Check if we need to break at a space
    if (charsToTake == maxCharsPerLine && remainingText.length() > maxCharsPerLine) {
      int lastSpace = line.lastIndexOf(' ');
      if (lastSpace > 0) {
        line = remainingText.substring(0, lastSpace);
        charsToTake = lastSpace;
      }
    }
    
    textDisplay.setCursor(5, yPos);
    textDisplay.println(line);
    yPos += 8;
    remainingText = remainingText.substring(charsToTake);
    remainingText.trim();
  }
  
  textDisplay.display();
}

// Helper to show short wifi/status messages
void displayWifiStatus(const String &line1, const String &line2) {
  textDisplay.clearDisplay();
  textDisplay.setTextSize(1);
  textDisplay.setTextColor(SSD1306_WHITE);

  // draw border
  textDisplay.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

  // Line 1
  int16_t bx, by;
  uint16_t bw, bh;
  textDisplay.getTextBounds(line1, 0, 0, &bx, &by, &bw, &bh);
  int x = (SCREEN_WIDTH - bw) / 2;
  int y = 20;
  textDisplay.setCursor(max(0, x), y);
  textDisplay.println(line1);

  // Line 2 (optional)
  if (line2.length() > 0) {
    textDisplay.getTextBounds(line2, 0, 0, &bx, &by, &bw, &bh);
    int x2 = (SCREEN_WIDTH - bw) / 2;
    int y2 = y + 14;
    textDisplay.setCursor(max(0, x2), y2);
    textDisplay.println(line2);
  }

  textDisplay.display();
}

// ----------------- TWEAKED displayClock() -----------------
// Time slightly smaller (textSize 2), centered vertically on screen.
// Date kept at usual position near bottom.
void displayClock() {
  unsigned long now = millis();
  if (now - lastClockRefresh < CLOCK_REFRESH_MS) return;
  lastClockRefresh = now;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    textDisplay.clearDisplay();
    textDisplay.setTextSize(1);
    textDisplay.setTextColor(SSD1306_WHITE);
    textDisplay.setCursor(10, 25);
    textDisplay.println("Time not set");
    textDisplay.display();
    return;
  }

  // Format time and date strings
  char timeStr[16];
  char dateStr[20];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  strftime(dateStr, sizeof(dateStr), "%d-%m-%Y", &timeinfo);

  textDisplay.clearDisplay();

  // ---------- TIME (textSize 2) centered vertically and horizontally ----------
  textDisplay.setTextSize(2);  // big but not too big
  textDisplay.setTextColor(SSD1306_WHITE);

  int16_t bx, by;
  uint16_t bw, bh;

  // measure time bounds
  textDisplay.getTextBounds(timeStr, 0, 0, &bx, &by, &bw, &bh);

  // desired vertical center for the time block:
  // keep some space at bottom for date (approx 12 px), so shift up slightly
  int availableHeight = SCREEN_HEIGHT - 12; // reserve 12 px for date area
  int timeY_top = (availableHeight - bh) / 2; // top y of time block
  if (timeY_top < 0) timeY_top = 0;

  int timeX = (SCREEN_WIDTH - bw) / 2;
  // setCursor expects baseline y, so add bh to top y
  textDisplay.setCursor(timeX, timeY_top + bh);
  textDisplay.println(timeStr);

  // ---------- DATE (small center aligned) ----------
  textDisplay.setTextSize(1);
  textDisplay.getTextBounds(dateStr, 0, 0, &bx, &by, &bw, &bh);
  int dateX = (SCREEN_WIDTH - bw) / 2;
  int dateY = SCREEN_HEIGHT - 10; // usual position near bottom
  textDisplay.setCursor(dateX, dateY);
  textDisplay.println(dateStr);

  textDisplay.display();
}
// ---------------------------------------------------------

// ==================== MAIN EXPRESSION/ANIMATION LOGIC ====================
void updateExpression() {
  unsigned long currentTime = millis();
  
  // If mode button hold-hint flag is set and the MODE button is currently being held,
  // suppress face animations but do not display any hold message.
  if (modeHoldHintShown && modeCurrentlyTouched) {
    // Clear face display and text display so nothing changes visually while holding.
    u8g2.clearBuffer();
    u8g2.sendBuffer();
    textDisplay.clearDisplay();
    textDisplay.display();
    return; // skip animations while holding
  }
  
  // Handle answer display (no animations)
  if (showingAnswer) {
    if (currentTime - answerDisplayStart < 5000) { // Show answer for 5 seconds
      return; // Don't update animations while showing answer
    } else {
      showingAnswer = false;
      // Return to previous expression
      currentExpression = previousExpression;
      lastActivityTime = currentTime;
    }
  }
  
  // Handle mode change animation (shows for 2 seconds)
  if (showingModeChange) {
    if (currentTime - modeChangeStart < 2000) {
      // Animate through mode change faces
      if (currentTime - lastFaceUpdate > 100) {
        if (normalToAI) {
          // Normal to AI: show faces 00-15
          displayFace(currentFaceIndex);
          currentFaceIndex++;
          if (currentFaceIndex > 15) currentFaceIndex = 0;
        } else {
          // AI to Normal: show faces 114-129
          displayFace(currentFaceIndex);
          currentFaceIndex++;
          if (currentFaceIndex > 129) currentFaceIndex = 114;
        }
        lastFaceUpdate = currentTime;
      }
      return;
    } else {
      showingModeChange = false;
      // Set appropriate expression after mode change
      currentExpression = aiMode ? AI_ACTIVE : NORMAL;
      currentFaceIndex = aiMode ? 16 : 83; // Start at 16 for AI mode, 83 for Normal
      lastActivityTime = currentTime;
    }
  }
  
  // Auto-transition from angry to normal/AI after 3 seconds
  if (currentExpression == ANGRY && !isPetting) {
    if (currentTime - angryStartTime > 3000) { // 3 seconds of anger
      if (aiMode) {
        setExpression(AI_ACTIVE);
      } else {
        setExpression(NORMAL);
      }
      angryTriggered = false;
    }
  }
  
  // Auto-transition from happy to normal/AI after 5 seconds
  if (currentExpression == HAPPY && !isPetting) {
    if (currentTime - happyStartTime > 5000) { // 5 seconds of happiness
      if (aiMode) {
        setExpression(AI_ACTIVE);
      } else {
        setExpression(NORMAL);
      }
    }
  }
  
  // If we're in SAD state, let it run for SAD_DURATION_MS, then switch to CLOCK
  if (currentExpression == SAD) {
    if (sadStartTime == 0) sadStartTime = currentTime; // ensure it's initialized
    if (currentTime - sadStartTime > SAD_DURATION_MS) {
      // Transition to internet clock
      setExpression(CLOCK);
      // Immediately display clock once
      displayClock();
      return;
    }
  }

  // If we're in CLOCK state, keep showing clock (no face animations) until user interacts
  if (currentExpression == CLOCK) {
    displayClock();
    return; // don't run other animations while clock is displayed
  }
  
  // Check for inactivity (IDLE_TIMEOUT_MS) using user activity timestamp
  if ((currentTime - lastUserActivityTime > IDLE_TIMEOUT_MS) && 
      currentExpression != SAD && 
      currentExpression != ANGRY && 
      currentExpression != HAPPY &&
      !showingModeChange &&
      !showingAnswer) {
    setExpression(SAD);
    sadStartTime = millis();
  }
  
  // Update face animation based on current expression
  if (currentTime - lastFaceUpdate > 150 && !showingAnswer) { // Adjust speed as needed
    switch(currentExpression) {
      case MODE_CHANGE:
        // Handled above
        break;
        
      case AI_ACTIVE:
        // Show AI active faces (16-35) continuously
        displayFace(currentFaceIndex);
        currentFaceIndex++;
        if (currentFaceIndex > 35) currentFaceIndex = 16;
        break;
        
      case ANGRY:
        // Show angry faces (36-51) continuously
        displayFace(currentFaceIndex);
        currentFaceIndex++;
        if (currentFaceIndex > 51) currentFaceIndex = 36;
        break;
        
      case SAD:
        // Show sad faces (52-82) continuously
        displayFace(currentFaceIndex);
        currentFaceIndex++;
        if (currentFaceIndex > 82) currentFaceIndex = 52;
        break;
        
      case NORMAL:
        // Show normal faces (83-99) continuously
        displayFace(currentFaceIndex);
        currentFaceIndex++;
        if (currentFaceIndex > 99) currentFaceIndex = 83;
        break;
        
      case HAPPY:
        // Show happy faces (100-113) continuously
        displayFace(currentFaceIndex);
        currentFaceIndex++;
        if (currentFaceIndex > 113) currentFaceIndex = 100;
        break;
        
      case ANSWER_DISPLAY:
        // No animations during answer display
        break;
        
      case CLOCK:
        // handled earlier
        break;
    }
    lastFaceUpdate = currentTime;
  }
}

void setExpression(ExpressionState newExpression) {
  if (currentExpression != newExpression) {
    previousExpression = currentExpression;
    currentExpression = newExpression;
    lastActivityTime = millis();
    
    // Reset special timers as needed
    switch(newExpression) {
      case MODE_CHANGE: 
        currentFaceIndex = normalToAI ? 0 : 114; // 00-15 for Normal→AI, 114-129 for AI→Normal
        break;
      case AI_ACTIVE: 
        currentFaceIndex = 16; // 16-35 range
        break;
      case ANGRY: 
        currentFaceIndex = 36; 
        angryStartTime = millis();
        angryShown = true;
        Serial.println("ANGRY expression triggered!");
        break;
      case SAD: 
        currentFaceIndex = 52; 
        sadStartTime = millis();
        Serial.println("SAD expression triggered!");
        break;
      case NORMAL: 
        currentFaceIndex = 83; 
        break;
      case HAPPY: 
        currentFaceIndex = 100; 
        happyStartTime = millis();
        Serial.println("HAPPY expression triggered!");
        break;
      case ANSWER_DISPLAY:
        // No face index change for answer display
        break;
      case CLOCK:
        // Reset clock refresh timer
        lastClockRefresh = 0;
        // Clear any sad start time
        sadStartTime = 0;
        Serial.println("CLOCK display started");
        break;
    }
    
    Serial.print("Expression changed to: ");
    switch(newExpression) {
      case MODE_CHANGE: Serial.println("MODE_CHANGE"); break;
      case AI_ACTIVE: Serial.println("AI_ACTIVE"); break;
      case ANGRY: Serial.println("ANGRY"); break;
      case SAD: Serial.println("SAD"); break;
      case NORMAL: Serial.println("NORMAL"); break;
      case HAPPY: Serial.println("HAPPY"); break;
      case ANSWER_DISPLAY: Serial.println("ANSWER_DISPLAY"); break;
      case CLOCK: Serial.println("CLOCK"); break;
    }
  }
}

void showAnswer(String answer) {
  showingAnswer = true;
  answerDisplayStart = millis();
  currentAnswer = answer;
  displayAnswerBox(answer);
  Serial.println("Displaying answer on OLED");
}

// If user interacts while clock is active, exit clock immediately to AI or NORMAL
void exitClockIfActive() {
  if (currentExpression == CLOCK) {
    setExpression(aiMode ? AI_ACTIVE : NORMAL);
  }
}

void checkExpressionTouch() {
  bool currentTouch = (digitalRead(EXPRESSION_TTP223_PIN) == HIGH);
  unsigned long currentTime = millis();
  
  // Detect touch press (rising edge)
  if (currentTouch && !lastExpressionTouchState) {
    // Touch started
    expressionTouchStartTime = currentTime;
    lastExpressionTouchState = true;
    lastActivityTime = currentTime;
    lastUserActivityTime = currentTime;   // user interacted
    Serial.println("Expression touch started");
    exitClockIfActive(); // leave clock if it's on
  }
  
  // Detect touch release (falling edge)
  if (!currentTouch && lastExpressionTouchState) {
    // Touch ended - check duration
    unsigned long touchDuration = currentTime - expressionTouchStartTime;
    
    if (touchDuration < 1000) { // Short tap (less than 1 second)
      // Instant tap - get angry
      setExpression(ANGRY);
      angryTriggered = true;
      isPetting = false;
      lastUserActivityTime = currentTime;   // user interacted
      Serial.println("Short tap - ANGRY!");
    } 
    else if (touchDuration >= 2000) { // Long press (2 seconds or more)
      // Long press - start petting (happy)
      setExpression(HAPPY);
      isPetting = true;
      lastPettingTime = currentTime;
      lastUserActivityTime = currentTime;   // user interacted
      Serial.println("Long press - HAPPY petting started!");
    }
    
    lastExpressionTouchState = false;
    Serial.print("Touch duration: ");
    Serial.print(touchDuration);
    Serial.println(" ms");
  }
  
  // If currently touching and it's been more than 2 seconds, ensure we're in happy state
  if (currentTouch && lastExpressionTouchState) {
    unsigned touchDuration = currentTime - expressionTouchStartTime;
    if (touchDuration >= 2000 && currentExpression != HAPPY) {
      setExpression(HAPPY);
      isPetting = true;
      lastPettingTime = currentTime;
      lastUserActivityTime = currentTime; // keep activity updated while petting
      exitClockIfActive(); // leave clock if it's on
    }
  }
  
  // If petting stopped (no touch for 1 second), end petting state
  if (isPetting && !currentTouch && (currentTime - lastPettingTime > 1000)) {
    isPetting = false;
    Serial.println("Petting stopped");
    lastUserActivityTime = currentTime; // mark transition as user-related
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== VOICE ASSISTANT WITH GEMINI AI ===");

  pinMode(TTP223_PIN, INPUT);
  pinMode(MODE_TTP223_PIN, INPUT);
  pinMode(EXPRESSION_TTP223_PIN, INPUT);

  // Initialize U8g2 for faces
  u8g2.begin();
  u8g2.setFlipMode(0);
  
  // Initialize Adafruit for text
  if(!textDisplay.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed for text display"));
    for(;;); // Don't proceed, loop forever
  }
  
  textDisplay.clearDisplay();
  textDisplay.display();

  displayFace(83); // Start with normal face

  // Initialize SPIFFS early so WiFi manager can read/write credentials
  if (!SPIFFS.begin(true)) {
    Serial.println("ERROR: SPIFFS initialization failed!");
  } else {
    Serial.println("SPIFFS initialized.");
  }

  // --- WiFi Manager flow: load saved credentials or start config portal ---
  Serial.println("Loading saved WiFi...");
  if (loadWiFi()) {
    Serial.print("Connecting to saved WiFi: ");
    Serial.println(wifi_ssid);

    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 12000) {
      delay(200);
      Serial.print(".");
      updateExpression(); // keep animations alive while trying to connect
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected!");
      // configure NTP for IST
      configTime(IST_OFFSET_SECONDS, 0, "pool.ntp.org", "time.google.com");
    } else {
      Serial.println("\nFailed to connect to saved WiFi. Starting config portal...");
      startConfigPortal(); // blocking; will restart after saving creds
    }
  } else {
    Serial.println("No saved WiFi credentials. Starting config portal...");
    startConfigPortal(); // blocking; will restart after saving creds
  }

  if (I2S_Record_Init()) {
    Serial.println("I2S Microphone initialized");
  } else {
    Serial.println("ERROR: I2S initialization failed");
  }

  Serial.println("\n=== READY ===");
  Serial.println("Touch TTP223 sensor to speak or MODE sensor to toggle mode");
  Serial.println("Short tap expression sensor for ANGRY, Long press for HAPPY");
  Serial.println("Hold MODE sensor >2s to show hold hint; hold 5s to open hotspot (arvia)");

  // Set initial expression - START IN NORMAL MODE
  setExpression(NORMAL);
  lastActivityTime = millis();
  lastUserActivityTime = millis(); // initialize user activity timer
}

// ==================== MAIN LOOP ====================
void loop() {
  // Update facial expressions (handles SAD->CLOCK transition)
  updateExpression();
  
  // Check expression touch input
  checkExpressionTouch();

  // --- MODE pin handling: short tap = toggle, long-press (5s) = start config portal/reset WiFi ---
  bool modeTouched = (digitalRead(MODE_TTP223_PIN) == HIGH);
  unsigned long now = millis();

  // update global tracking for mode being currently touched
  modeCurrentlyTouched = modeTouched;

  if (modeTouched && !lastModeTouchState) {
    // Rising edge - touch started
    lastModeTouchState = true;
    modeTouchStart = now;
    modeLongPressActive = false;
    modeHoldHintShown = false;
    Serial.println("MODE touch started");
  }

  if (modeTouched && lastModeTouchState) {
    // still pressed - check for hold-hint after MODE_SHOW_HOLD_AFTER_MS
    if (!modeHoldHintShown && (now - modeTouchStart >= MODE_SHOW_HOLD_AFTER_MS)) {
      modeHoldHintShown = true;
      // NOTE: intentionally do not show any message on screen here.
      Serial.println("MODE hold hint flag set (2s elapsed) - display suppressed");
    }

    // check for long press (trigger after MODE_LONGPRESS_MS)
    if (!modeLongPressActive && (now - modeTouchStart >= MODE_LONGPRESS_MS)) {
      // Trigger long-press action
      modeLongPressActive = true;
      Serial.println("MODE long-press detected: entering hotspot/config portal");
      // Clear wifi file and start config portal (blocking)
      resetWifiNow();
      // Note: startConfigPortal() is blocking and will not return until new creds saved.
      // If it ever returns, reset flags:
      lastModeTouchState = false;
      modeLongPressActive = false;
      modeHoldHintShown = false;
      modeCurrentlyTouched = false;
    }
  }

  if (!modeTouched && lastModeTouchState) {
    // Released - it was a short press if long-press didn't activate
    unsigned long duration = now - modeTouchStart;
    if (!modeLongPressActive) {
      // treat as short tap - toggle AI mode
      Serial.print("MODE short tap (duration ms): ");
      Serial.println(duration);

      // Toggle mode (existing behavior)
      bool oldMode = aiMode;
      aiMode = !aiMode;
      
      // Determine direction for mode change animation
      normalToAI = !oldMode; // If old mode was normal, now going to AI
      
      Serial.print("Mode toggled. AI mode = ");
      Serial.println(aiMode ? "TRUE" : "FALSE");

      // Show mode change animation
      setExpression(MODE_CHANGE);
      showingModeChange = true;
      modeChangeStart = millis();

      // Mark user activity for mode toggle and exit clock if active
      lastUserActivityTime = millis();
      exitClockIfActive();

      if (!aiMode) {
        Serial.println("NORMAL mode ON. AI features disabled.");
      } else {
        Serial.println("AI mode ON. Touch to record.");
      }
    } else {
      // long-press already handled in the hold branch (resetWifiNow called)
      Serial.println("MODE released after long-press (config portal active)");
    }
    lastModeTouchState = false;
    modeLongPressActive = false;
    // clear persistent-hold flag on release so animations resume
    modeHoldHintShown = false;
    modeCurrentlyTouched = false;
  }

  // If we are in NORMAL mode: do not run AI touch-record flow
  if (!aiMode) {
    // Ensure we're showing normal faces in normal mode (unless in special state)
    // Don't override SAD or CLOCK so they can run their full animation / display.
    if (currentExpression != NORMAL && !showingModeChange && 
        currentExpression != ANGRY && currentExpression != HAPPY &&
        currentExpression != SAD && currentExpression != CLOCK && !showingAnswer) {
      setExpression(NORMAL);
    }
    delay(100);
    return;
  }

  // AI mode: ensure we're showing AI active faces (unless in special state)
  // Don't override SAD or CLOCK so they can run through their durations.
  if (currentExpression != AI_ACTIVE && !showingModeChange && 
      currentExpression != ANGRY && currentExpression != HAPPY &&
      currentExpression != SAD && currentExpression != CLOCK && !showingAnswer) {
    setExpression(AI_ACTIVE);
  }

  // AI mode: existing touch to record behavior (TTP223_PIN)
  bool isTouched = (digitalRead(TTP223_PIN) == HIGH);

  if (isTouched && !lastTouchState) {
    Serial.println("\n>>> Recording... Speak now!");
    startRecording();
    lastTouchState = true;
    lastTouchTime = millis();
    lastActivityTime = millis();
    lastUserActivityTime = millis(); // <-- user started recording
    exitClockIfActive(); // leave clock if on
  }

  if (!isTouched && lastTouchState) {
    Serial.println("\n>>> Processing...");
    stopRecording();
    lastTouchState = false;
    lastTouchTime = millis();
    lastActivityTime = millis();
    lastUserActivityTime = millis(); // <-- user finished recording
    exitClockIfActive(); // leave clock if on
  }

  if (flg_is_recording) {
    Record_Start(AUDIO_FILE);
    static unsigned long lastProgressTime = 0;
    if (millis() - lastProgressTime > 1000) {
      Serial.print(".");
      lastProgressTime = millis();
    }
  }

  delay(50);
}

// ==================== START RECORDING FUNCTION ====================
void startRecording() {
  if (flg_is_recording) {
    return;
  }
  flg_is_recording = true;
  lastUserActivityTime = millis(); // user tapped to start recording
  Record_Start(AUDIO_FILE);
}

// ==================== STOP RECORDING FUNCTION ====================
void stopRecording() {
  if (!flg_is_recording) {
    return;
  }

  float record_length;
  if (Record_Available(AUDIO_FILE, &record_length)) {
    Serial.print("Audio length: ");
    Serial.print(record_length);
    Serial.println("s");

    if (record_length > 0.5) {
      String newTranscription = SpeechToText_Deepgram(AUDIO_FILE);

      if (newTranscription.length() > 0 && newTranscription != "No transcription found" && newTranscription != "Transcription failed") {
        currentTranscription = newTranscription;
        lastUserActivityTime = millis(); // user activity (speech used)

        Serial.println("=================================");
        Serial.print("YOU: ");
        Serial.println(currentTranscription);
        Serial.println("=================================");

        // SEND TO GEMINI AI
        String response = sendToGemini(currentTranscription);

        if (response.length() > 0) {
          geminiResponse = response;
          lastUserActivityTime = millis(); // user activity (response displayed)

          Serial.println("=================================");
          Serial.print("ASSISTANT: ");
          Serial.println(geminiResponse);
          Serial.println("=================================");

          // Show answer on OLED instead of serial monitor
          showAnswer(geminiResponse);

          // Check for interrupt during display
          unsigned long start = millis();
          bool interrupted = false;
          while (millis() - start < 5000) {
            // No need to update expressions here - answer is being displayed
            checkExpressionTouch(); // Check for interrupts
            
            if (digitalRead(TTP223_PIN) == HIGH) {
              delay(30);
              if (digitalRead(TTP223_PIN) == HIGH) {
                while (digitalRead(TTP223_PIN) == HIGH) {
                  delay(10);
                }
                interrupted = true;
                break;
              }
            }
            delay(20);
          }

          if (interrupted) {
            Serial.println("User tapped during display — starting new recording.");

            // Remove old audio and start fresh recording
            if (SPIFFS.exists(AUDIO_FILE)) {
              SPIFFS.remove(AUDIO_FILE);
            }

            // Start recording immediately
            startRecording();
            lastTouchState = true;
            lastTouchTime = millis();
            lastUserActivityTime = millis();

            return;
          }
        }

      } else {
        Serial.println("No speech detected");
        lastUserActivityTime = millis(); // mark activity even if no speech
      }
    } else {
      Serial.println("Recording too short");
      lastUserActivityTime = millis(); // mark activity (user attempted a recording)
    }

    SPIFFS.remove(AUDIO_FILE);
  }

  Serial.println("\n=== READY ===");
}

// ==================== DEEPGRAM STT FUNCTION - ENGLISH ONLY ====================
String SpeechToText_Deepgram(String audio_filename) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(TIMEOUT_DEEPGRAM);

  if (!client.connect("api.deepgram.com", 443)) {
    return "Connection failed";
  }

  // Request path: add model/punctuate etc as desired
  String url = "/v1/listen?model=general&language=en&punctuate=true";

  File audioFile = SPIFFS.open(audio_filename, FILE_READ);
  if (!audioFile) {
    return "File error";
  }

  size_t audio_size = audioFile.size();

  client.print(String("POST ") + url + " HTTP/1.1\r\n");
  client.print("Host: api.deepgram.com\r\n");
  client.print("Authorization: Token ");
  client.print(deepgramApiKey);
  client.print("\r\n");
  client.print("Content-Type: audio/wav\r\n");
  client.print("Content-Length: ");
  client.print(audio_size);
  client.print("\r\n");
  client.print("Connection: close\r\n");
  client.print("\r\n");

  // stream the audio bytes
  const size_t bufferSize = 1024;
  uint8_t buffer[bufferSize];
  while (audioFile.available()) {
    size_t bytesRead = audioFile.read(buffer, bufferSize);
    if (bytesRead > 0) {
      client.write(buffer, bytesRead);
    }
  }
  audioFile.close();

  // read response (headers + JSON body). Collect whole body (strip headers)
  String fullResponse = "";
  unsigned long startTime = millis();
  bool headersComplete = false;
  String body = "";

  while (client.connected() && (millis() - startTime < TIMEOUT_DEEPGRAM)) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      if (!headersComplete) {
        if (line == "\r") {
          headersComplete = true; // next bytes are body
        }
        continue;
      } else {
        body += line + "\n";
      }
    }
    delay(5);
  }

  client.stop();
  body.trim();
  if (body.length() == 0) return "No response";

  return parseDeepgramResponse(body);
}

// ==================== PARSE DEEPGRAM RESPONSE ====================
String parseDeepgramResponse(String responseBody) {
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, responseBody);
  if (err) {
    return extractDeepgramTextManually(responseBody);
  }

  if (doc.containsKey("results")) {
    JsonObject results = doc["results"];
    if (results.containsKey("channels")) {
      JsonArray channels = results["channels"];
      if (channels.size() > 0) {
        JsonObject ch = channels[0];
        if (ch.containsKey("alternatives")) {
          JsonArray alts = ch["alternatives"];
          if (alts.size() > 0) {
            JsonObject firstAlt = alts[0];
            if (firstAlt.containsKey("transcript")) {
              String transcript = firstAlt["transcript"].as<String>();
              transcript.trim();
              if (transcript.length() > 0) {
                return transcript;
              }
            }
          }
        }
      }
    }
  }

  return extractDeepgramTextManually(responseBody);
}

// ==================== SIMPLE MANUAL EXTRACTION FOR DEEPGRAM TEXT ====================
String extractDeepgramTextManually(String response) {
  int idx = response.indexOf("\"transcript\"");
  if (idx != -1) {
    int colon = response.indexOf(':', idx);
    if (colon != -1) {
      int quoteStart = response.indexOf('"', colon+1);
      if (quoteStart != -1) {
        int quoteEnd = response.indexOf('"', quoteStart+1);
        while (quoteEnd != -1 && response.charAt(quoteEnd-1) == '\\') {
          quoteEnd = response.indexOf('"', quoteEnd+1);
        }
        if (quoteEnd != -1) {
          String text = response.substring(quoteStart+1, quoteEnd);
          text.trim();
          text.replace("\\n", " ");
          text.replace("\\\"", "\"");
          return text;
        }
      }
    }
  }
  return "No transcription found";
}

// ==================== GEMINI AI FUNCTION ====================
String sendToGemini(String query) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(TIMEOUT_GEMINI);

  if (!client.connect("generativelanguage.googleapis.com", 443)) {
    return "Connection failed";
  }

  String url = "/v1beta/models/gemini-2.0-flash:generateContent?key=" + String(gemini_KEY);

  String payload = "{";
  payload += "\"contents\": [{\"parts\":[{\"text\": \"";
  payload += "Answer this in one short sentence (max 15 words): ";
  payload += query;
  payload += "\"}]}],";
  payload += "\"generationConfig\": {";
  payload += "\"maxOutputTokens\": " + String(MAX_TOKENS) + ",";
  payload += "\"temperature\": 0.7";
  payload += "}";
  payload += "}";

  client.println("POST " + url + " HTTP/1.1");
  client.println("Host: generativelanguage.googleapis.com");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.print("Content-Length: ");
  client.println(payload.length());
  client.println();
  client.println(payload);

  String fullResponse = "";
  unsigned long startTime = millis();

  while (client.connected() && (millis() - startTime < TIMEOUT_GEMINI)) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      fullResponse += line + "\n";
    }
  }

  client.stop();
  return parseGeminiResponse(fullResponse);
}

// ==================== CLEAN GEMINI RESPONSE PARSING ====================
String parseGeminiResponse(String response) {
  response.trim();

  if (response.length() == 0) {
    return "No response";
  }

  int jsonStart = response.indexOf('{');
  int jsonEnd = response.lastIndexOf('}');

  if (jsonStart == -1 || jsonEnd == -1) {
    return "Invalid response";
  }

  String jsonString = response.substring(jsonStart, jsonEnd + 1);

  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, jsonString);

  if (error) {
    return extractTextManually(response);
  }

  if (doc.containsKey("candidates")) {
    JsonArray candidates = doc["candidates"];
    if (candidates.size() > 0) {
      JsonObject candidate = candidates[0];
      if (candidate.containsKey("content")) {
        JsonObject content = candidate["content"];
        if (content.containsKey("parts")) {
          JsonArray parts = content["parts"];
          if (parts.size() > 0) {
            JsonObject part = parts[0];
            if (part.containsKey("text")) {
              String answer = part["text"].as<String>();
              answer.trim();
              answer.replace("\n", " ");
              return answer;
            }
          }
        }
      }
    }
  }

  return extractTextManually(response);
}

// ==================== SIMPLE TEXT EXTRACTION ====================
String extractTextManually(String response) {
  int textIndex = response.indexOf("\"text\":\"");
  if (textIndex != -1) {
    textIndex += 8;
    int endIndex = response.indexOf("\"", textIndex);
    if (endIndex != -1) {
      String text = response.substring(textIndex, endIndex);
      text.trim();
      text.replace("\\n", " ");
      return text;
    }
  }
  return "No response text";
}

// ==================== I2S INITIALIZATION ====================
bool I2S_Record_Init() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  i2s_channel_init_std_mode(rx_handle, &std_cfg);
  i2s_channel_enable(rx_handle);
  flg_I2S_initialized = true;
  return true;
}

// ==================== RECORDING FUNCTIONS ====================
bool Record_Start(String audio_filename) {
  if (!flg_I2S_initialized) return false;

  if (!flg_is_recording) {
    flg_is_recording = true;
    if (SPIFFS.exists(audio_filename)) {
      SPIFFS.remove(audio_filename);
    }
    File audio_file = SPIFFS.open(audio_filename, FILE_WRITE);
    audio_file.write((uint8_t*)&myWAV_Header, 44);
    audio_file.close();
    return true;
  }

  if (flg_is_recording) {
    int16_t audio_buffer[1024];
    size_t bytes_read = 0;
    i2s_channel_read(rx_handle, audio_buffer, sizeof(audio_buffer), &bytes_read, portMAX_DELAY);

    for (int16_t i = 0; i < (bytes_read / 2); ++i) {
      audio_buffer[i] = audio_buffer[i] * GAIN_BOOSTER_I2S;
    }

    File audio_file = SPIFFS.open(audio_filename, FILE_APPEND);
    if (audio_file) {
      audio_file.write((uint8_t*)audio_buffer, bytes_read);
      audio_file.close();
      return true;
    }
  }
  return false;
}

bool Record_Available(String audio_filename, float* audiolength_sec) {
  if (!flg_is_recording) return false;

  File audio_file = SPIFFS.open(audio_filename, "r+");
  long filesize = audio_file.size();
  audio_file.seek(0);
  myWAV_Header.flength = filesize;
  myWAV_Header.dlength = (filesize - 8);
  audio_file.write((uint8_t*)&myWAV_Header, 44);
  audio_file.close();

  flg_is_recording = false;
  *audiolength_sec = (float)(filesize - 44) / (SAMPLE_RATE * (BITS_PER_SAMPLE / 8));
  return true;
}
