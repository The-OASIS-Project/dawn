/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * DAP2 Tier 2 satellite for ESP32-S3 TFT Feather.
 * Push-to-talk voice client using WebSocket protocol.
 *
 * Hardware: Adafruit ESP32-S3 TFT Feather with NeoPixels,
 *           I2S speaker (MAX98357), analog microphone, push button.
 *
 * Libraries required:
 *   - arduinoWebSockets (Links2004)
 *   - ArduinoJson (v7+)
 *   - Adafruit GFX, Adafruit ST7789, Adafruit NeoPixel
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s_std.h>
#include <driver/i2s_types.h>
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_NeoPixel.h>
#include <SPI.h>
#include <Preferences.h>
#include <esp_random.h>

#include "arduino_secrets.h"

/* ── Configuration (from arduino_secrets.h — gitignored) ─────────────────── */
const char *ssid = SECRET_SSID;
const char *password = SECRET_PASSWORD;

const char *SERVER_IP = SECRET_SERVER_IP;
const uint16_t SERVER_PORT = SECRET_SERVER_PORT;

const char *SATELLITE_NAME = SECRET_SATELLITE_NAME;
const char *SATELLITE_LOCATION = SECRET_SATELLITE_LOCATION;

/* ── Pin definitions (ESP32-S3 TFT Feather) ────────────────────────────────── */
#define BUTTON_PIN 18
#define I2S_BCLK 5
#define I2S_LRCLK 6
#define I2S_DOUT 9
#define MIC_ADC_PIN 1

/* ── NeoPixel configuration ────────────────────────────────────────────────── */
#define NEOPIXEL_PIN 17
#define NEOPIXEL_COUNT 3
#define NEOPIXEL_TYPE NEO_GRB + NEO_KHZ800

/* ── TFT Display ───────────────────────────────────────────────────────────── */
#define TFT_BACKLIGHT 45

/* ── Audio configuration ───────────────────────────────────────────────────── */
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define RECORD_TIME 15
#define BUFFER_SIZE (SAMPLE_RATE * RECORD_TIME)

/* ── DAP2 binary message types ─────────────────────────────────────────────── */
#define WS_BIN_AUDIO_IN 0x01
#define WS_BIN_AUDIO_IN_END 0x02
#define WS_BIN_AUDIO_OUT 0x11
#define WS_BIN_AUDIO_SEGMENT_END 0x12

/* ── Audio streaming ───────────────────────────────────────────────────────── */
#define AUDIO_SEND_CHUNK_SAMPLES 1600 /* 100ms of 16kHz audio */
#define AUDIO_SEND_CHUNK_BYTES (AUDIO_SEND_CHUNK_SAMPLES * 2 + 1)

/* ── TTS ring buffer ──────────────────────────────────────────────────────── */
#define TTS_SAMPLE_RATE 22050                /* Daemon sends TTS at native Piper rate */
#define TTS_BUFFER_SAMPLES (1 << 19)         /* 524,288 samples = ~23.8s at 22050Hz, ~1.0MB PSRAM */
#define TTS_BUFFER_MASK (TTS_BUFFER_SAMPLES - 1)

/* ── TFT Display settings ─────────────────────────────────────────────────── */
#define TEXT_SIZE 2           /* Small text for setup log lines */
#define TEXT_SIZE_STATUS 3    /* Large text for centered status display */
#define STATUS_LINE_HEIGHT 20
#define MAX_STATUS_LINES 6
#define PROGRESS_BAR_Y 120
#define PROGRESS_BAR_HEIGHT 10

/* Status area for partial redraws (avoids full fillScreen flash).
 * Covers the top portion of the 240x135 TFT where status/detail text lives. */
#define STATUS_AREA_Y 0
#define STATUS_AREA_H 110 /* Leaves room for progress bar at bottom */

/* ── Debug output control ─────────────────────────────────────────────────── */
#define DEBUG_VERBOSE 0 /* Set to 1 to print UUIDs, session IDs, secrets to Serial */

/* ── Debounce ──────────────────────────────────────────────────────────────── */
#define DEBOUNCE_TIME 50
unsigned long lastDebounceTime = 0;
int buttonState = HIGH;
int lastButtonState = HIGH;

/* ── Recording buffer ──────────────────────────────────────────────────────── */
int16_t *audioBuffer = NULL;
size_t sampleCount = 0;
bool isRecording = false;
size_t lastSentSample = 0;

uint32_t recStartUs = 0, recEndUs = 0, nextSampleTime = 0;
float recFs = SAMPLE_RATE;
const uint32_t sampleInterval_us = (uint32_t)((1000000ULL + SAMPLE_RATE / 2) / SAMPLE_RATE);

/* ── TTS ring buffer ──────────────────────────────────────────────────────── */
int16_t *ttsBuffer = NULL;
volatile size_t ttsWritePos = 0;
volatile size_t ttsReadPos = 0;
volatile size_t ttsAvailable = 0;
volatile bool ttsPlaying = false;
volatile bool ttsComplete = false;

/* ── WebSocket client ──────────────────────────────────────────────────────── */
WebSocketsClient webSocket;
bool wsConnected = false;
bool registered = false;
char reconnectSecret[128] = {0};
uint32_t sessionId = 0;

/* ── NVS persistence ─────────────────────────────────────────────────────── */
Preferences nvsPrefs;
static char persistentUUID[37] = {0};

/* ── Audio send buffer (allocated in PSRAM) ────────────────────────────────── */
uint8_t *wsSendBuf = NULL;

/* ── I2S playback buffers (static to avoid stack overflow on 8KB Arduino stack) */
static int16_t i2s_mono_buf[256];
static int16_t i2s_stereo_buf[640]; /* 320 stereo frames — fits 128 input at 2.177x ratio */
static int16_t tts_rx_buf[256];     /* Aligned temp buffer for TTS chunk reception */

/* ── TTS ring buffer spinlock (protects producer/consumer during webSocket.loop) */
static portMUX_TYPE tts_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* ── Persistent I2S channel handle (created once in setup, enable/disable per playback) */
static i2s_chan_handle_t g_i2s_tx = NULL;

/* ── Timing ────────────────────────────────────────────────────────────────── */
unsigned long lastPingTime = 0;

/* ── Hardware objects ──────────────────────────────────────────────────────── */
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEOPIXEL_TYPE);

/* ── NeoPixel state ────────────────────────────────────────────────────────── */
enum NeoPixelMode {
   NEO_IDLE_CYCLING,
   NEO_RECORDING,
   NEO_PLAYING,
   NEO_WAITING,
   NEO_ERROR,
   NEO_OFF
};

struct ColorState {
   uint8_t current_hue;
   uint8_t target_hue;
   uint8_t current_sat;
   uint8_t target_sat;
   uint8_t current_val;
   uint8_t target_val;
   float transition_progress;
   bool transitioning;
};

ColorState globalColorState;
volatile NeoPixelMode currentNeoMode = NEO_IDLE_CYCLING;
volatile uint32_t lastIdleActivity = 0;
volatile uint32_t errorStartTime = 0;
const uint32_t IDLE_TIMEOUT_MS = 60000;
const uint32_t ERROR_DISPLAY_MS = 3000;

TaskHandle_t neoPixelTaskHandle = NULL;

/* ═══════════════════════════════════════════════════════════════════════════
 * NeoPixel Functions
 * ═══════════════════════════════════════════════════════════════════════════ */

void setNeoPixelMode(NeoPixelMode mode) {
   currentNeoMode = mode;
   if (mode == NEO_ERROR) {
      errorStartTime = millis();
   } else if (mode == NEO_IDLE_CYCLING) {
      lastIdleActivity = millis();
   }
}

void setNeoPixelColor(uint8_t r, uint8_t g, uint8_t b) {
   uint32_t color = strip.Color(r, g, b);
   for (int i = 0; i < NEOPIXEL_COUNT; i++) {
      strip.setPixelColor(i, color);
   }
   strip.show();
}

void turnOffNeoPixels() {
   for (int i = 0; i < NEOPIXEL_COUNT; i++) {
      strip.setPixelColor(i, 0);
   }
   strip.show();
}

void neoPixelTask(void *pvParameters) {
   const TickType_t xDelay = pdMS_TO_TICKS(50);
   const float TRANSITION_SPEED = 0.015f;
   const uint32_t NEW_COLOR_INTERVAL = 3000;

   globalColorState.current_hue = random(0, 256);
   globalColorState.target_hue = random(0, 256);
   globalColorState.current_sat = 255;
   globalColorState.target_sat = 255;
   globalColorState.current_val = 200;
   globalColorState.target_val = 200;
   globalColorState.transition_progress = 0.0f;
   globalColorState.transitioning = true;

   uint32_t lastColorChange = millis();
   lastIdleActivity = millis();

   /* Track previous mode to detect transitions — static modes (RECORDING, PLAYING,
    * WAITING) only need strip.show() once on entry, not every 50ms tick. */
   NeoPixelMode prevMode = NEO_OFF;

   while (1) {
      uint32_t currentTime = millis();
      NeoPixelMode mode = currentNeoMode; /* snapshot volatile once per tick */
      bool modeChanged = (mode != prevMode);

      switch (mode) {
         case NEO_IDLE_CYCLING: {
            if (currentTime - lastIdleActivity >= IDLE_TIMEOUT_MS) {
               currentNeoMode = NEO_OFF;
               turnOffNeoPixels();
               break;
            }

            if (modeChanged) {
               /* Re-entering idle — reset transition so colors start moving immediately */
               globalColorState.transitioning = true;
               globalColorState.transition_progress = 0.0f;
               lastColorChange = currentTime;
            }

            if (currentTime - lastColorChange >= NEW_COLOR_INTERVAL) {
               if (!globalColorState.transitioning || globalColorState.transition_progress > 0.9f) {
                  globalColorState.target_hue = random(0, 256);
                  globalColorState.target_sat = random(200, 256);
                  globalColorState.target_val = random(150, 255);
                  globalColorState.transition_progress = 0.0f;
                  globalColorState.transitioning = true;
                  lastColorChange = currentTime;
               }
            }

            if (globalColorState.transitioning) {
               int16_t hue_diff = globalColorState.target_hue - globalColorState.current_hue;
               if (hue_diff > 128) hue_diff -= 256;
               if (hue_diff < -128) hue_diff += 256;

               globalColorState.current_hue =
                  (uint8_t)(globalColorState.current_hue + hue_diff * TRANSITION_SPEED);
               globalColorState.current_sat = (uint8_t)(globalColorState.current_sat +
                  (globalColorState.target_sat - globalColorState.current_sat) * TRANSITION_SPEED);
               globalColorState.current_val = (uint8_t)(globalColorState.current_val +
                  (globalColorState.target_val - globalColorState.current_val) * TRANSITION_SPEED);

               globalColorState.transition_progress += TRANSITION_SPEED;

               if (globalColorState.transition_progress >= 1.0f) {
                  globalColorState.current_hue = globalColorState.target_hue;
                  globalColorState.current_sat = globalColorState.target_sat;
                  globalColorState.current_val = globalColorState.target_val;
                  globalColorState.transitioning = false;
                  globalColorState.transition_progress = 0.0f;
               }
            }

            uint32_t color = strip.ColorHSV(globalColorState.current_hue * 256,
                                            globalColorState.current_sat,
                                            globalColorState.current_val);
            for (int i = 0; i < NEOPIXEL_COUNT; i++) {
               strip.setPixelColor(i, color);
            }
            strip.show();
            break;
         }

         case NEO_RECORDING:
            if (modeChanged) setNeoPixelColor(255, 0, 0); /* red — set once */
            break;

         case NEO_PLAYING:
            if (modeChanged) setNeoPixelColor(0, 255, 0);
            break;

         case NEO_WAITING:
            if (modeChanged) setNeoPixelColor(255, 255, 0);
            break;

         case NEO_ERROR:
            if (modeChanged) setNeoPixelColor(255, 0, 0);
            if (currentTime - errorStartTime >= ERROR_DISPLAY_MS) {
               currentNeoMode = NEO_IDLE_CYCLING;
               lastIdleActivity = millis();
            }
            break;

         case NEO_OFF:
            if (modeChanged) turnOffNeoPixels();
            break;
      }

      prevMode = mode;
      vTaskDelay(xDelay);
   }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TFT Display Functions
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Update the TFT status display.
 *
 * Two modes:
 * - clearScreen=true: Large centered status text (TEXT_SIZE_STATUS=3). Uses partial
 *   fillRect on the status area to avoid full-screen flash. Good for "Recording...",
 *   "Thinking...", "Ready" etc.
 * - clearScreen=false: Scrolling log lines (TEXT_SIZE=2) for setup sequence. When the
 *   screen fills up, clears with fillRect and resets to the top.
 *
 * @param message  Status text (const char* to avoid Arduino String heap churn)
 * @param color    Text color (ST77XX_* constant)
 * @param clearScreen  true = large centered status, false = append log line
 */
void updateTFTStatus(const char *message, uint16_t color, bool clearScreen = false) {
   static int statusLine = 0;

   if (clearScreen) {
      /* Large centered status — partial redraw of status area only */
      tft.fillRect(0, STATUS_AREA_Y, tft.width(), STATUS_AREA_H, ST77XX_BLACK);
      tft.setTextSize(TEXT_SIZE_STATUS);
      tft.setTextColor(color);

      /* Measure text bounds for centering.
       * Each char at size 3 = 18px wide, 24px tall (6x8 base * 3). */
      int16_t x1, y1;
      uint16_t tw, th;
      tft.getTextBounds(message, 0, 0, &x1, &y1, &tw, &th);
      int16_t cx = (tft.width() - (int16_t)tw) / 2;
      int16_t cy = (STATUS_AREA_H - (int16_t)th) / 2;
      if (cx < 0) cx = 0;
      if (cy < 0) cy = 0;
      tft.setCursor(cx, cy);
      tft.print(message);

      /* Restore small text size for any subsequent log lines */
      tft.setTextSize(TEXT_SIZE);
      statusLine = 0;
      return;
   }

   /* Scrolling log line mode (setup sequence) */
   if (statusLine >= MAX_STATUS_LINES) {
      tft.fillRect(0, 0, tft.width(), MAX_STATUS_LINES * STATUS_LINE_HEIGHT, ST77XX_BLACK);
      statusLine = 0;
   }

   tft.setTextSize(TEXT_SIZE);
   tft.setCursor(0, statusLine * STATUS_LINE_HEIGHT);
   tft.setTextColor(color);
   tft.println(message);
   statusLine++;
}

void updateProgressBar(int percentage, uint16_t color) {
   tft.fillRect(0, PROGRESS_BAR_Y, tft.width(), PROGRESS_BAR_HEIGHT, ST77XX_BLACK);
   int barWidth = (percentage * tft.width()) / 100;
   tft.fillRect(0, PROGRESS_BAR_Y, barWidth, PROGRESS_BAR_HEIGHT, color);
}

void drawRecordingIndicator(bool isActive) {
   static bool lastState = false;
   if (isActive != lastState) {
      uint16_t color = isActive ? ST77XX_RED : ST77XX_BLACK;
      tft.fillCircle(tft.width() - 15, 15, 10, color);
      lastState = isActive;
   }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * NVS Persistence Functions
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Generate a random UUID v4 on first boot, persist in NVS.
 *
 * Avoids the MAC-derived UUID (which is predictable from WiFi sniffing).
 */
void loadOrCreateUUID() {
   nvsPrefs.begin("dawn", false);
   String stored = nvsPrefs.getString("uuid", "");
   if (stored.length() == 36) {
      stored.toCharArray(persistentUUID, sizeof(persistentUUID));
   } else {
      uint8_t bytes[16];
      esp_fill_random(bytes, sizeof(bytes));
      bytes[6] = (bytes[6] & 0x0F) | 0x40; /* version 4 */
      bytes[8] = (bytes[8] & 0x3F) | 0x80; /* variant 1 */
      snprintf(persistentUUID, sizeof(persistentUUID),
               "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
               bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
               bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
               bytes[15]);
      nvsPrefs.putString("uuid", persistentUUID);
#if DEBUG_VERBOSE
      Serial.printf("NVS: Generated new UUID: %s\n", persistentUUID);
#endif
   }
   nvsPrefs.end();
#if DEBUG_VERBOSE
   Serial.printf("NVS: UUID = %s\n", persistentUUID);
#endif
}

void saveReconnectSecret() {
   nvsPrefs.begin("dawn", false);
   nvsPrefs.putString("recon_sec", reconnectSecret);
   nvsPrefs.end();
}

void loadReconnectSecret() {
   nvsPrefs.begin("dawn", true);
   String stored = nvsPrefs.getString("recon_sec", "");
   stored.toCharArray(reconnectSecret, sizeof(reconnectSecret));
   nvsPrefs.end();
   if (strlen(reconnectSecret) > 0) {
#if DEBUG_VERBOSE
      Serial.println("NVS: Loaded reconnect secret from flash");
#else
      Serial.println("NVS: Reconnect secret loaded");
#endif
   }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DAP2 WebSocket Functions
 * ═══════════════════════════════════════════════════════════════════════════ */

void sendRegistration() {
   JsonDocument doc;
   doc["type"] = "satellite_register";
   JsonObject payload = doc["payload"].to<JsonObject>();
   payload["uuid"] = persistentUUID;
   payload["name"] = SATELLITE_NAME;
   payload["location"] = SATELLITE_LOCATION;
   payload["tier"] = 2;
   payload["protocol_version"] = "2.0";

   JsonObject caps = payload["capabilities"].to<JsonObject>();
   caps["local_asr"] = false;
   caps["local_tts"] = false;
   caps["wake_word"] = false;
   caps["push_to_talk"] = true;

   JsonObject hw = payload["hardware"].to<JsonObject>();
   hw["platform"] = "esp32s3";
   hw["memory_mb"] = ESP.getPsramSize() / (1024 * 1024);

   /* Include reconnect_secret if we have one from a previous session */
   if (strlen(reconnectSecret) > 0) {
      payload["reconnect_secret"] = reconnectSecret;
   }

   char msg[512];
   size_t len = serializeJson(doc, msg, sizeof(msg));
   webSocket.sendTXT((uint8_t *)msg, len);
#if DEBUG_VERBOSE
   Serial.printf("DAP2: Sent registration (uuid=%s)\n", persistentUUID);
#else
   Serial.println("DAP2: Sent registration");
#endif
}

void handleTextMessage(uint8_t *payload, size_t length) {
   const size_t MAX_JSON_SIZE = 4096;
   if (length > MAX_JSON_SIZE) {
      Serial.printf("DAP2: JSON message too large (%zu bytes), ignoring\n", length);
      return;
   }

   /* Static buffer avoids malloc/free heap fragmentation on every message.
    * Safe: handleTextMessage is only called from the WebSocket callback on Core 1. */
   static char jsonBuf[MAX_JSON_SIZE + 1];
   memcpy(jsonBuf, payload, length);
   jsonBuf[length] = '\0';

   JsonDocument doc;
   DeserializationError err = deserializeJson(doc, jsonBuf, length + 1);
   if (err) {
      Serial.printf("DAP2: JSON parse error: %s\n", err.c_str());
      return;
   }

   const char *type = doc["type"];
   if (!type) return;

   if (strcmp(type, "satellite_register_ack") == 0) {
      JsonObject p = doc["payload"];
      bool success = p["success"] | false;
      if (success) {
         registered = true;
         sessionId = p["session_id"] | 0;
         if (p["reconnect_secret"]) {
            const char *secret = p["reconnect_secret"];
            if (secret) {
               strncpy(reconnectSecret, secret, sizeof(reconnectSecret) - 1);
               reconnectSecret[sizeof(reconnectSecret) - 1] = '\0';
               saveReconnectSecret();
            }
         }
         setNeoPixelMode(NEO_IDLE_CYCLING);
         updateTFTStatus("Registered!", ST77XX_GREEN);
#if DEBUG_VERBOSE
         Serial.printf("DAP2: Registered (session=%u)\n", sessionId);
#else
         Serial.println("DAP2: Registered OK");
#endif
      } else {
         const char *msg = p["message"] | "Registration failed";
         Serial.printf("DAP2: Registration failed: %s\n", msg);
         updateTFTStatus("Reg failed!", ST77XX_RED);
         setNeoPixelMode(NEO_ERROR);
      }
   } else if (strcmp(type, "satellite_pong") == 0) {
      /* Keep-alive acknowledged */
   } else if (strcmp(type, "state") == 0) {
      const char *state = doc["payload"]["state"];
      if (!state) return;

      /* Extract detail string if present (tool names, status info) */
      const char *detail = doc["payload"]["detail"] | (const char *)NULL;

      if (strcmp(state, "listening") == 0) {
         setNeoPixelMode(NEO_WAITING);
         updateTFTStatus("Listening...", ST77XX_CYAN, true);
      } else if (strcmp(state, "thinking") == 0) {
         setNeoPixelMode(NEO_WAITING);
         if (detail) {
            updateTFTStatus(detail, ST77XX_YELLOW, true);
         } else {
            updateTFTStatus("Thinking...", ST77XX_YELLOW, true);
         }
      } else if (strcmp(state, "tool_call") == 0) {
         setNeoPixelMode(NEO_WAITING);
         if (detail) {
            updateTFTStatus(detail, ST77XX_MAGENTA, true);
         } else {
            updateTFTStatus("Calling tool...", ST77XX_MAGENTA, true);
         }
      } else if (strcmp(state, "processing") == 0) {
         setNeoPixelMode(NEO_WAITING);
         if (detail) {
            updateTFTStatus(detail, ST77XX_YELLOW, true);
         } else {
            updateTFTStatus("Processing...", ST77XX_YELLOW, true);
         }
      } else if (strcmp(state, "speaking") == 0) {
         setNeoPixelMode(NEO_PLAYING);
      } else if (strcmp(state, "idle") == 0) {
         ttsComplete = true;
         if (!ttsPlaying) {
            setNeoPixelMode(NEO_IDLE_CYCLING);
            updateTFTStatus("Ready", ST77XX_WHITE, true);
         }
      }
   } else if (strcmp(type, "error") == 0) {
      const char *code = doc["payload"]["code"] | "UNKNOWN";
      const char *msg = doc["payload"]["message"] | "";
      Serial.printf("DAP2: Error %s: %s\n", code, msg);
      char errBuf[64];
      snprintf(errBuf, sizeof(errBuf), "Err: %s", code);
      updateTFTStatus(errBuf, ST77XX_RED);
      setNeoPixelMode(NEO_ERROR);
   }
}

void handleBinaryMessage(uint8_t *payload, size_t length) {
   if (length < 1) return;
   uint8_t msgType = payload[0];

   if (msgType == WS_BIN_AUDIO_OUT && length > 1) {
      /* TTS audio chunk: 22050Hz 16-bit mono PCM from daemon (native Piper rate).
       * Copy through aligned temp buffer (payload+1 is odd-aligned — direct
       * int16_t* cast risks Xtensa LoadStoreAlignment exceptions). Bulk memcpy
       * into ring buffer keeps spinlock hold time to a minimum. */
      size_t total_samples = (length - 1) / 2;
      size_t offset = 0;
      size_t total_dropped = 0;

      while (offset < total_samples) {
         size_t chunk = total_samples - offset;
         if (chunk > 256) chunk = 256;

         /* Copy from potentially-unaligned WS buffer into aligned SRAM temp */
         memcpy(tts_rx_buf, payload + 1 + offset * 2, chunk * sizeof(int16_t));

         portENTER_CRITICAL(&tts_spinlock);
         size_t space = TTS_BUFFER_SAMPLES - ttsAvailable;
         size_t to_write = (chunk < space) ? chunk : space;
         /* Ring buffer write in up to two segments (handles wrap) */
         size_t first = TTS_BUFFER_SAMPLES - ttsWritePos;
         if (first >= to_write) {
            memcpy(&ttsBuffer[ttsWritePos], tts_rx_buf, to_write * sizeof(int16_t));
         } else {
            memcpy(&ttsBuffer[ttsWritePos], tts_rx_buf, first * sizeof(int16_t));
            memcpy(&ttsBuffer[0], tts_rx_buf + first, (to_write - first) * sizeof(int16_t));
         }
         ttsWritePos = (ttsWritePos + to_write) & TTS_BUFFER_MASK;
         ttsAvailable += to_write;
         portEXIT_CRITICAL(&tts_spinlock);

         if (to_write < chunk) total_dropped += (chunk - to_write);
         offset += chunk;
      }

      if (total_dropped > 0) {
         Serial.printf("TTS: Dropped %zu/%zu samples (buffer full)\n", total_dropped, total_samples);
      }

      /* Start playback once we have 500ms buffered */
      if (!ttsPlaying && ttsAvailable >= (TTS_SAMPLE_RATE / 2)) {
         ttsPlaying = true;
      }
   } else if (msgType == WS_BIN_AUDIO_SEGMENT_END) {
      /* Segment complete — keep playing, more may follow.
       * ttsComplete is set by "idle" state message when ALL TTS is done. */
   }
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
   switch (type) {
      case WStype_CONNECTED:
         wsConnected = true;
         registered = false;
         Serial.println("DAP2: WebSocket connected");
         updateTFTStatus("WS connected", ST77XX_GREEN, true);
         sendRegistration();
         break;

      case WStype_DISCONNECTED:
         wsConnected = false;
         registered = false;
         Serial.println("DAP2: WebSocket disconnected");
         updateTFTStatus("WS disconnected", ST77XX_RED, true);
         setNeoPixelMode(NEO_ERROR);
         break;

      case WStype_TEXT:
         handleTextMessage(payload, length);
         break;

      case WStype_BIN:
         handleBinaryMessage(payload, length);
         break;

      default:
         break;
   }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Recording Functions
 * ═══════════════════════════════════════════════════════════════════════════ */

void startRecording() {
   Serial.println("Starting recording...");
   updateTFTStatus("Recording...", ST77XX_RED, true);
   setNeoPixelMode(NEO_RECORDING);

   /* Reset TTS state for new interaction (under spinlock for atomicity) */
   portENTER_CRITICAL(&tts_spinlock);
   ttsPlaying = false;
   ttsComplete = false;
   ttsAvailable = 0;
   ttsWritePos = 0;
   ttsReadPos = 0;
   portEXIT_CRITICAL(&tts_spinlock);

   sampleCount = 0;
   lastSentSample = 0;
   isRecording = true;
   recStartUs = micros();
   nextSampleTime = recStartUs;
   drawRecordingIndicator(true);
}

void stopRecording() {
   Serial.println("Stopping recording...");
   isRecording = false;
   recEndUs = micros();
   uint32_t elapsed_us = recEndUs - recStartUs;
   if (elapsed_us == 0) elapsed_us = 1;
   recFs = (float)sampleCount * 1e6f / (float)elapsed_us;
   Serial.printf("Effective sample rate: %.2f Hz\n", recFs);
   Serial.printf("Recorded %d samples (%.1f seconds)\n", sampleCount,
                 (float)sampleCount / SAMPLE_RATE);

   drawRecordingIndicator(false);

   if (!wsConnected || !registered) {
      updateTFTStatus("Not connected!", ST77XX_RED);
      setNeoPixelMode(NEO_ERROR);
      return;
   }

   /* Send any remaining unsent audio in chunks (wsSendBuf fits one chunk) */
   while (lastSentSample < sampleCount) {
      size_t remaining = sampleCount - lastSentSample;
      size_t chunk = (remaining > AUDIO_SEND_CHUNK_SAMPLES) ? AUDIO_SEND_CHUNK_SAMPLES : remaining;
      wsSendBuf[0] = WS_BIN_AUDIO_IN;
      memcpy(wsSendBuf + 1, &audioBuffer[lastSentSample], chunk * 2);
      webSocket.sendBIN(wsSendBuf, chunk * 2 + 1);
      lastSentSample += chunk;
   }

   /* Send end-of-utterance marker */
   uint8_t endMarker[1] = {WS_BIN_AUDIO_IN_END};
   webSocket.sendBIN(endMarker, 1);

   updateTFTStatus("Processing...", ST77XX_YELLOW, true);
   setNeoPixelMode(NEO_WAITING);
   Serial.println("Audio sent, waiting for response...");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * I2S Playback Functions
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialize the persistent I2S channel (called once from setup).
 *
 * Creates and configures the channel but does NOT enable it.
 * playTTSStream/playAudioSamples call enable/disable per playback,
 * avoiding DMA descriptor alloc/free on every TTS response.
 */
void initI2S() {
   i2s_chan_config_t chan_cfg = {.id = I2S_NUM_0,
                                .role = I2S_ROLE_MASTER,
                                .dma_desc_num = 16,
                                .dma_frame_num = 512,
                                .auto_clear = true,
                                .auto_clear_before_cb = false,
                                .allow_pd = false,
                                .intr_priority = 0};

   ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &g_i2s_tx, NULL));

   i2s_std_config_t std_cfg = {};
   std_cfg.clk_cfg.sample_rate_hz = 48000;
   std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
   std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
   std_cfg.clk_cfg.ext_clk_freq_hz = 0;
   std_cfg.slot_cfg =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
   std_cfg.gpio_cfg.mclk = GPIO_NUM_NC;
   std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
   std_cfg.gpio_cfg.ws = (gpio_num_t)I2S_LRCLK;
   std_cfg.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
   std_cfg.gpio_cfg.din = GPIO_NUM_NC;
   std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
   std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
   std_cfg.gpio_cfg.invert_flags.ws_inv = false;

   ESP_ERROR_CHECK(i2s_channel_init_std_mode(g_i2s_tx, &std_cfg));
   Serial.println("I2S: Channel initialized (48kHz stereo)");
}

/**
 * @brief Play audio samples through I2S with resampling.
 *
 * Resamples from sourceSampleRate to 48kHz stereo for I2S output.
 * Used for local testing / debug playback.
 */
void playAudioSamples(int16_t *samples, size_t numSamples, float sourceSampleRate) {
   ESP_ERROR_CHECK(i2s_channel_enable(g_i2s_tx));
   delay(3);

   const float outFs = 48000.0f;
   const size_t CHUNK = 512;
   int16_t out[CHUNK * 2];

   size_t outFrames = (size_t)((float)numSamples * (outFs / sourceSampleRate) + 0.5f);
   float step = sourceSampleRate / outFs;
   float pos = 0.0f;
   size_t bytesWritten = 0;

   for (size_t produced = 0; produced < outFrames;) {
      size_t n = min(CHUNK, outFrames - produced);
      for (size_t j = 0; j < n; ++j) {
         size_t i0 = (size_t)pos;
         float frac = pos - (float)i0;
         int16_t a = samples[(i0 < numSamples) ? i0 : (numSamples - 1)];
         int16_t b = samples[(i0 + 1 < numSamples) ? (i0 + 1) : (numSamples - 1)];
         int16_t s = (int16_t)((1.0f - frac) * a + frac * b);
         out[2 * j + 0] = s;
         out[2 * j + 1] = s;
         pos += step;
      }
      size_t bytes = n * 2 * sizeof(int16_t);
      i2s_channel_write(g_i2s_tx, out, bytes, &bytesWritten, pdMS_TO_TICKS(200));
      produced += n;

      int percentage = (produced * 100) / outFrames;
      updateProgressBar(percentage, ST77XX_CYAN);
   }

   ESP_ERROR_CHECK(i2s_channel_disable(g_i2s_tx));
   updateProgressBar(0, ST77XX_BLACK);
}

/**
 * @brief Play streaming TTS audio from the ring buffer through I2S.
 *
 * Daemon sends 22050Hz 16-bit mono PCM via WS binary frames. This function
 * drains the ring buffer, resamples to 48kHz stereo, and writes to I2S.
 * Calls webSocket.loop() between writes to keep receiving data.
 */
void playTTSStream() {
   Serial.println("TTS: Starting streaming playback...");
   updateTFTStatus("Playing...", ST77XX_GREEN, true);

   ESP_ERROR_CHECK(i2s_channel_enable(g_i2s_tx));
   delay(3);

   /* Read from ring buffer, resample 22050→48000Hz, convert mono→stereo, write to I2S.
    * Buffers are static globals to avoid stack overflow on ESP32's 8KB default stack.
    * All float (not double) — ESP32-S3 has hardware single-precision FPU only.
    * Linear interpolation with cross-boundary carry for seamless chunk transitions. */
   const size_t IN_CHUNK = 128; /* Input samples per iteration (at 22050Hz) */
   int16_t *inBuf = i2s_mono_buf;
   int16_t *outBuf = i2s_stereo_buf; /* 640 entries = 320 stereo frames, fits ~278 output */
   size_t bytesWritten = 0;
   unsigned long lastDataTime = millis();

   const float step = (float)TTS_SAMPLE_RATE / 48000.0f; /* ~0.459 */
   float pos = 0.0f;        /* Fractional position in input buffer (can be negative for carry) */
   int16_t prevSample = 0;  /* Last sample of previous chunk for cross-boundary interpolation */

   while (true) {
      /* Flow control: only receive more data if ring buffer has room.
       * When buffer is nearly full, skip webSocket.loop() — this backs up the TCP
       * receive window, creating natural backpressure to the server via
       * lws_send_pipe_choked(). Prevents ring buffer overflow on long responses. */
      portENTER_CRITICAL(&tts_spinlock);
      size_t avail = ttsAvailable;
      portEXIT_CRITICAL(&tts_spinlock);

      if (avail < TTS_BUFFER_SAMPLES / 2) {
         webSocket.loop();
         /* Re-read after potential receive */
         portENTER_CRITICAL(&tts_spinlock);
         avail = ttsAvailable;
         portEXIT_CRITICAL(&tts_spinlock);
      }

      if (avail == 0) {
         if (ttsComplete) break;
         /* Timeout: if no data for 10 seconds and not explicitly complete, stop.
          * Needs to be generous — LLM thinking + TTS synthesis can take seconds. */
         if (millis() - lastDataTime > 10000) {
            Serial.println("TTS: Playback timeout (no data)");
            break;
         }
         delay(1);
         continue;
      }

      lastDataTime = millis();

      /* Bulk-read up to IN_CHUNK samples from ring buffer (under spinlock).
       * Two-segment memcpy handles ring wrap; keeps lock time to O(1). */
      size_t toRead = (avail < IN_CHUNK) ? avail : IN_CHUNK;
      portENTER_CRITICAL(&tts_spinlock);
      size_t first = TTS_BUFFER_SAMPLES - ttsReadPos;
      if (first >= toRead) {
         memcpy(inBuf, &ttsBuffer[ttsReadPos], toRead * sizeof(int16_t));
      } else {
         memcpy(inBuf, &ttsBuffer[ttsReadPos], first * sizeof(int16_t));
         memcpy(inBuf + first, &ttsBuffer[0], (toRead - first) * sizeof(int16_t));
      }
      ttsReadPos = (ttsReadPos + toRead) & TTS_BUFFER_MASK;
      ttsAvailable -= toRead;
      portEXIT_CRITICAL(&tts_spinlock);

      /* Resample 22050→48000Hz with linear interpolation, output as stereo.
       * pos can be negative at chunk start — that means the next output sample
       * needs interpolation between prevSample and inBuf[0] (cross-boundary). */
      size_t outFrames = 0;
      while (outFrames < 320) {
         int16_t s;
         if (pos < 0.0f) {
            /* Cross-boundary: interpolate between previous chunk's last sample
             * and current chunk's first sample. pos is in [-1, 0). */
            float frac = pos + 1.0f;
            s = (int16_t)((1.0f - frac) * prevSample + frac * inBuf[0]);
         } else {
            size_t i0 = (size_t)pos;
            if (i0 + 1 >= toRead) break; /* Need next chunk for interpolation pair */
            float frac = pos - (float)i0;
            s = (int16_t)((1.0f - frac) * inBuf[i0] + frac * inBuf[i0 + 1]);
         }
         outBuf[2 * outFrames + 0] = s;
         outBuf[2 * outFrames + 1] = s;
         outFrames++;
         pos += step;
      }
      /* Save last sample for cross-boundary interpolation, carry fractional pos */
      prevSample = inBuf[toRead - 1];
      pos -= (float)toRead;

      /* Write to I2S (200ms timeout — avoid infinite hang if DMA stalls) */
      if (outFrames > 0) {
         size_t bytes = outFrames * 2 * sizeof(int16_t);
         i2s_channel_write(g_i2s_tx, outBuf, bytes, &bytesWritten, pdMS_TO_TICKS(200));
      }
   }

   Serial.println("TTS: Playback finished");
   ESP_ERROR_CHECK(i2s_channel_disable(g_i2s_tx));

   /* Reset TTS state (under spinlock for atomicity with producer) */
   portENTER_CRITICAL(&tts_spinlock);
   ttsPlaying = false;
   ttsComplete = false;
   ttsAvailable = 0;
   ttsWritePos = 0;
   ttsReadPos = 0;
   portEXIT_CRITICAL(&tts_spinlock);

   setNeoPixelMode(NEO_IDLE_CYCLING);
   updateTFTStatus("Ready", ST77XX_WHITE, true);
   Serial.println("Ready for next recording.");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Utility
 * ═══════════════════════════════════════════════════════════════════════════ */

void reportMemoryUsage() {
   Serial.println("=== Memory Usage Report ===");
   Serial.printf("Total PSRAM: %d bytes\n", ESP.getPsramSize());
   Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
   Serial.printf("Used PSRAM: %d bytes\n", ESP.getPsramSize() - ESP.getFreePsram());

   size_t audioSize = BUFFER_SIZE * sizeof(int16_t);
   size_t ttsSize = TTS_BUFFER_SAMPLES * sizeof(int16_t);
   size_t sendBufSize = AUDIO_SEND_CHUNK_BYTES;
   size_t totalAllocation = audioSize + ttsSize + sendBufSize;

   Serial.printf("Audio buffer: %d bytes\n", audioSize);
   Serial.printf("TTS buffer: %d bytes\n", ttsSize);
   Serial.printf("Send buffer: %d bytes\n", sendBufSize);
   Serial.printf("Total static allocation: %d bytes\n", totalAllocation);

   Serial.printf("Heap free: %d bytes\n", ESP.getFreeHeap());
   Serial.printf("Heap total: %d bytes\n", ESP.getHeapSize());
   Serial.println("=============================");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Setup
 * ═══════════════════════════════════════════════════════════════════════════ */

void setup() {
   Serial.begin(115200);
   delay(1000);

   Serial.println("ESP32-S3 Dawn Satellite (DAP2 Tier 2)");

   /* Initialize TFT display */
   pinMode(TFT_BACKLIGHT, OUTPUT);
   digitalWrite(TFT_BACKLIGHT, HIGH);
   tft.init(135, 240);
   tft.setRotation(3);
   tft.fillScreen(ST77XX_BLACK);
   tft.setTextSize(TEXT_SIZE);
   tft.setTextWrap(true);
   updateTFTStatus("Dawn Satellite", ST77XX_BLUE, true);

   /* Initialize NeoPixel strip */
   strip.begin();
   strip.clear();
   strip.show();
   updateTFTStatus("NeoPixels: OK", ST77XX_GREEN);

   /* Initialize PSRAM */
   if (psramFound()) {
      Serial.println("PSRAM found and initialized");
      updateTFTStatus("PSRAM: OK", ST77XX_GREEN);
   } else {
      Serial.println("PSRAM not found! Enable PSRAM in board config.");
      updateTFTStatus("PSRAM: ERROR", ST77XX_RED);
      while (1)
         ;
   }

   /* Allocate buffers in PSRAM */
   audioBuffer = (int16_t *)ps_malloc(BUFFER_SIZE * sizeof(int16_t));
   if (!audioBuffer) {
      Serial.println("Failed to allocate audio buffer!");
      updateTFTStatus("Audio buf failed!", ST77XX_RED);
      while (1)
         ;
   }
   Serial.printf("Audio buffer: %d bytes in PSRAM\n", BUFFER_SIZE * sizeof(int16_t));

   ttsBuffer = (int16_t *)ps_malloc(TTS_BUFFER_SAMPLES * sizeof(int16_t));
   if (!ttsBuffer) {
      Serial.println("Failed to allocate TTS buffer!");
      updateTFTStatus("TTS buf failed!", ST77XX_RED);
      while (1)
         ;
   }
   Serial.printf("TTS buffer: %d bytes in PSRAM\n", TTS_BUFFER_SAMPLES * sizeof(int16_t));

   wsSendBuf = (uint8_t *)ps_malloc(AUDIO_SEND_CHUNK_BYTES);
   if (!wsSendBuf) {
      Serial.println("Failed to allocate send buffer!");
      updateTFTStatus("Send buf failed!", ST77XX_RED);
      while (1)
         ;
   }

   memset(audioBuffer, 0, BUFFER_SIZE * sizeof(int16_t));
   memset(ttsBuffer, 0, TTS_BUFFER_SAMPLES * sizeof(int16_t));

   updateTFTStatus("Buffers allocated", ST77XX_GREEN);

   /* Load persistent UUID and reconnect secret from NVS flash */
   loadOrCreateUUID();
   loadReconnectSecret();

   /* Initialize button and ADC */
   pinMode(BUTTON_PIN, INPUT_PULLUP);
   analogSetPinAttenuation(MIC_ADC_PIN, ADC_11db);
   analogReadResolution(12);

   /* Connect to WiFi — pulse orange on NeoPixels during connection.
    * NeoPixel task hasn't started yet, so we drive the strip directly. */
   WiFi.begin(ssid, password);
   updateTFTStatus("WiFi connecting...", ST77XX_YELLOW);
   Serial.print("Connecting to WiFi");
   int attempts = 0;
   while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      /* Pulse orange on NeoPixels (alternating bright/dim) */
      uint8_t bright = (attempts % 2) ? 180 : 40;
      uint32_t orange = strip.Color(bright, bright / 3, 0);
      for (int i = 0; i < NEOPIXEL_COUNT; i++) strip.setPixelColor(i, orange);
      strip.show();

      delay(500);
      Serial.print(".");
      attempts++;
   }

   if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      /* Brief green flash on NeoPixels */
      setNeoPixelColor(0, 200, 0);
      char ipBuf[32];
      snprintf(ipBuf, sizeof(ipBuf), "WiFi: %s", WiFi.localIP().toString().c_str());
      updateTFTStatus(ipBuf, ST77XX_GREEN);
   } else {
      Serial.println("\nWiFi connection failed!");
      setNeoPixelColor(255, 0, 0);
      updateTFTStatus("WiFi: FAILED", ST77XX_RED);
      while (1)
         ;
   }

   /* Initialize persistent I2S channel (create once, enable/disable per playback) */
   initI2S();
   updateTFTStatus("I2S: OK", ST77XX_GREEN);

   /* Create NeoPixel task on Core 0 */
   xTaskCreatePinnedToCore(neoPixelTask, "NeoPixel Task", 4096, NULL, 1, &neoPixelTaskHandle, 0);
   if (neoPixelTaskHandle != NULL) {
      updateTFTStatus("NeoPixel task: OK", ST77XX_GREEN);
   } else {
      updateTFTStatus("NeoPixel task: FAIL", ST77XX_RED);
   }

   /* Initialize WebSocket connection (WSS — daemon uses self-signed cert) */
   webSocket.beginSSL(SERVER_IP, SERVER_PORT, "/", "", "dawn-1.0");
   webSocket.onEvent(webSocketEvent);
   webSocket.setReconnectInterval(3000);
   Serial.printf("WebSocket: connecting to wss://%s:%d\n", SERVER_IP, SERVER_PORT);

   reportMemoryUsage();

   updateTFTStatus("Press button to talk", ST77XX_WHITE);
   Serial.println("Setup complete. Press the button to start recording.");
   setNeoPixelMode(NEO_IDLE_CYCLING);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main Loop
 * ═══════════════════════════════════════════════════════════════════════════ */

void loop() {
   /* Service WebSocket — skip during recording to keep ADC timing tight.
    * SSL processing in webSocket.loop() can take 10-50ms, which causes the
    * timer-based ADC sampler to fall behind and produce distorted catch-up bursts.
    * DAP1 had no network I/O during recording and worked fine; match that here. */
   if (!isRecording) {
      webSocket.loop();
   }

   /* ── Button debounce ─────────────────────────────────────────────────── */
   int reading = digitalRead(BUTTON_PIN);

   if (reading != lastButtonState) {
      lastDebounceTime = millis();
   }

   if ((millis() - lastDebounceTime) > DEBOUNCE_TIME) {
      if (reading != buttonState) {
         buttonState = reading;

         if (buttonState == LOW && !isRecording) {
            startRecording();
         } else if (buttonState == HIGH && isRecording) {
            stopRecording();
         }
      }
   }

   /* ── ADC sampling (while recording) ──────────────────────────────────── */
   while (isRecording && sampleCount < BUFFER_SIZE &&
          (int32_t)(micros() - nextSampleTime) >= 0) {
      int adcVal = analogRead(MIC_ADC_PIN);
      audioBuffer[sampleCount++] = (int16_t)((adcVal - 2048) << 4);
      nextSampleTime += sampleInterval_us;
   }

   /* ── Recording progress display ──────────────────────────────────────── */
   static unsigned long lastProgressUpdate = 0;
   if (isRecording && millis() - lastProgressUpdate >= 500) {
      int percentage = (sampleCount * 100) / BUFFER_SIZE;
      updateProgressBar(percentage, ST77XX_RED);
      float seconds = (float)sampleCount / SAMPLE_RATE;
      Serial.printf("Recording: %.1f seconds, %d samples\n", seconds, sampleCount);
      lastProgressUpdate = millis();
      drawRecordingIndicator(true);
   }

   /* ── Recording indicator blink ───────────────────────────────────────── */
   static unsigned long lastBlinkTime = 0;
   if (isRecording && millis() - lastBlinkTime >= 250) {
      drawRecordingIndicator((millis() / 250) % 2 == 0);
      lastBlinkTime = millis();
   }

   /* ── Buffer full: auto-stop recording ────────────────────────────────── */
   if (isRecording && sampleCount >= BUFFER_SIZE) {
      Serial.println("Buffer full, stopping recording");
      updateTFTStatus("Buffer full!", ST77XX_YELLOW);
      stopRecording();
   }

   /* ── TTS playback (blocks until done) ────────────────────────────────── */
   if (ttsPlaying && !isRecording) {
      playTTSStream();
   }

   /* ── Keep-alive ping ─────────────────────────────────────────────────── */
   if (registered && millis() - lastPingTime >= 10000) {
      webSocket.sendTXT("{\"type\":\"satellite_ping\"}");
      lastPingTime = millis();
   }

   lastButtonState = reading;
}
