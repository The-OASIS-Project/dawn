#include <WiFi.h>
#include <driver/i2s_std.h>
#include <driver/i2s_types.h>
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_NeoPixel.h>
#include <SPI.h>

// WiFi credentials
const char* ssid = "TORTUGA";
const char* password = "Large Hadron Collider";

// Server settings
const char* SERVER_IP = "192.168.1.159";
const uint16_t SERVER_PORT = 5000;
const int MAX_CONNECTION_ATTEMPTS = 3;    // Number of connection retry attempts
const int CONNECTION_TIMEOUT_MS = 3000;   // Connection timeout in milliseconds

// Pin definitions based on the ESP32-S3 TFT Feather pinout
#define BUTTON_PIN         18     // GPIO18 - BUTTON
#define I2S_BCLK           5      // GPIO5 - I2S bit clock
#define I2S_LRCLK          6      // GPIO6 - I2S word select
#define I2S_DOUT           9      // GPIO9 - I2S data output
#define MIC_ADC_PIN        1      // GPIO1 (ADC1_CH0) - Analog microphone input

// NeoPixel configuration
#define NEOPIXEL_PIN       17     // GPIO17 - NeoPixel data pin
#define NEOPIXEL_COUNT     3      // Number of NeoPixels
#define NEOPIXEL_TYPE      NEO_GRB + NEO_KHZ800  // Most common NeoPixel type

// TFT Display pins - Use built-in pins for ESP32-S3 TFT Feather
#define TFT_BACKLIGHT      45     // GPIO45 - TFT Backlight

// Audio configuration
#define SAMPLE_RATE        16000  // Sample rate in Hz
#define BITS_PER_SAMPLE    16     // 16-bit samples

// Audio buffer configuration
#define RECORD_TIME        30     // Max recording time in seconds
#define BUFFER_SIZE        (SAMPLE_RATE * RECORD_TIME)  // Buffer in samples

// Debounce settings
#define DEBOUNCE_TIME      50     // Debounce time in milliseconds
unsigned long lastDebounceTime = 0;
int buttonState = HIGH;          // Current state of the button
int lastButtonState = HIGH;      // Previous state of the button

// Buffer for audio recording - store as actual int16_t samples
int16_t* audioBuffer = NULL;
size_t sampleCount = 0;  // Count in samples
bool isRecording = false;

// Buffer for response data
uint8_t* responseBuffer = NULL;
size_t responseLength = 0;

uint32_t recStartUs = 0, recEndUs = 0, nextSampleTime = 0;
double   recFs = SAMPLE_RATE;
const uint32_t sampleInterval_us = (uint32_t)((1000000ULL + SAMPLE_RATE/2) / SAMPLE_RATE); // rounded

// Initialize TFT display - Use TFT_CS, TFT_DC, and TFT_RST from the pins_arduino.h
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// Initialize NeoPixel strip
Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEOPIXEL_TYPE);

// NeoPixel states
enum NeoPixelMode {
   NEO_IDLE_CYCLING,    // Normal color cycling
   NEO_RECORDING,       // Blue - recording audio
   NEO_PLAYING,         // Green - playing back audio
   NEO_WAITING,         // Yellow - waiting for server response
   NEO_ERROR,           // Red - error state
   NEO_OFF              // LEDs off to conserve power
};

// NeoPixel color cycling variables - single state for all pixels
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
const uint32_t IDLE_TIMEOUT_MS = 60000; // 1 minute
const uint32_t ERROR_DISPLAY_MS = 3000;  // 3 seconds

// NeoPixel task handle
TaskHandle_t neoPixelTaskHandle = NULL;

// TFT Display settings
#define TEXT_SIZE 2             // Slightly larger than the smallest font
#define STATUS_LINE_HEIGHT 20   // Height of each status line (with TEXT_SIZE 2)
#define MAX_STATUS_LINES 6      // Max number of status lines to show
#define PROGRESS_BAR_Y 120      // Y position for progress bar
#define PROGRESS_BAR_HEIGHT 10  // Height of progress bar

// Protocol configuration
#define PROTOCOL_VERSION       0x01    // Protocol version identifier
#define PACKET_HEADER_SIZE     8       // Bytes: [4:length][1:version][1:type][2:checksum]
#define PACKET_MAX_SIZE        8192    // Maximum size for data packets
#define PACKET_TYPE_HANDSHAKE  0x01    // Handshake packet
#define PACKET_TYPE_DATA       0x02    // Data packet
#define PACKET_TYPE_DATA_END   0x03    // Final data packet
#define PACKET_TYPE_ACK        0x04    // Acknowledgment packet
#define PACKET_TYPE_NACK       0x05    // Negative acknowledgment
#define PACKET_TYPE_RETRY      0x06    // Retry request

// Retry configuration
#define MAX_RETRIES            5       // Maximum number of retries per chunk
#define RETRY_BASE_DELAY       100     // Base delay for exponential backoff (ms)
#define MAX_RETRY_DELAY        2000    // Maximum retry delay (ms)

#define FIRST_PACKET_SERVER_RESPONSE_TIMEOUT 30000

// For storing sequence numbers
uint16_t sendSequence = 0;
uint16_t receiveSequence = 0;

// WAV header structure
struct __attribute__((packed)) WavHeader {
  char     riff[4] = {'R','I','F','F'};
  uint32_t chunkSize = 0;
  char     wave[4] = {'W','A','V','E'};
  char     fmt[4]  = {'f','m','t',' '};
  uint32_t subchunk1Size = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = 1;  // Mono
  uint32_t sampleRate  = SAMPLE_RATE;
  uint32_t byteRate    = SAMPLE_RATE * 1 * (BITS_PER_SAMPLE/8);
  uint16_t blockAlign  = 1 * (BITS_PER_SAMPLE/8);
  uint16_t bitsPerSample = BITS_PER_SAMPLE;
  char     data[4] = {'d','a','t','a'};
  uint32_t subchunk2Size = 0;
};

// NeoPixel state management functions
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

// NeoPixel color cycling task
void neoPixelTask(void *pvParameters) {
   const TickType_t xDelay = pdMS_TO_TICKS(50); // Update every 50ms for smooth transitions
   const float TRANSITION_SPEED = 0.015f; // Speed of color transitions (0.0 to 1.0 per update)
   const uint32_t NEW_COLOR_INTERVAL = 3000; // Change target color every 3 seconds

   // Initialize global color state with random starting color
   globalColorState.current_hue = random(0, 256);
   globalColorState.target_hue = random(0, 256);
   globalColorState.current_sat = 255; // Full saturation
   globalColorState.target_sat = 255;
   globalColorState.current_val = 200; // Moderate brightness
   globalColorState.target_val = 200;
   globalColorState.transition_progress = 0.0f;
   globalColorState.transitioning = true;

   uint32_t lastColorChange = millis();
   lastIdleActivity = millis();

   while (1) {
      uint32_t currentTime = millis();

      // Handle different NeoPixel modes
      switch (currentNeoMode) {
         case NEO_IDLE_CYCLING: {
            // Check if idle timeout has been reached
            if (currentTime - lastIdleActivity >= IDLE_TIMEOUT_MS) {
               currentNeoMode = NEO_OFF;
               turnOffNeoPixels();
               break;
            }

            // Check if it's time to select a new target color
            if (currentTime - lastColorChange >= NEW_COLOR_INTERVAL) {
               // Only set new target if not currently transitioning or transition is nearly complete
               if (!globalColorState.transitioning || globalColorState.transition_progress > 0.9f) {
                  globalColorState.target_hue = random(0, 256);
                  globalColorState.target_sat = random(200, 256); // High saturation with some variation
                  globalColorState.target_val = random(150, 255); // Varied brightness
                  globalColorState.transition_progress = 0.0f;
                  globalColorState.transitioning = true;
                  lastColorChange = currentTime; // Only update when we actually start a new transition
               }
            }

            // Update color state
            if (globalColorState.transitioning) {
               // Smooth interpolation between current and target colors
               int16_t hue_diff = globalColorState.target_hue - globalColorState.current_hue;
               // Handle hue wrap-around (shortest path around color wheel)
               if (hue_diff > 128) hue_diff -= 256;
               if (hue_diff < -128) hue_diff += 256;

               globalColorState.current_hue = (uint8_t)(globalColorState.current_hue + hue_diff * TRANSITION_SPEED);
               globalColorState.current_sat = (uint8_t)(globalColorState.current_sat + 
                  (globalColorState.target_sat - globalColorState.current_sat) * TRANSITION_SPEED);
               globalColorState.current_val = (uint8_t)(globalColorState.current_val + 
                  (globalColorState.target_val - globalColorState.current_val) * TRANSITION_SPEED);

               // Update progress
               globalColorState.transition_progress += TRANSITION_SPEED;

               // Check if transition is complete
               if (globalColorState.transition_progress >= 1.0f) {
                  globalColorState.current_hue = globalColorState.target_hue;
                  globalColorState.current_sat = globalColorState.target_sat;
                  globalColorState.current_val = globalColorState.target_val;
                  globalColorState.transitioning = false;
                  globalColorState.transition_progress = 0.0f;
               }
            }

            // Convert HSV to RGB and set all pixels to the same color
            uint32_t color = strip.ColorHSV(
               globalColorState.current_hue * 256, // Convert 8-bit hue to 16-bit
               globalColorState.current_sat,
               globalColorState.current_val
            );

            // Set all pixels to the same color
            for (int i = 0; i < NEOPIXEL_COUNT; i++) {
               strip.setPixelColor(i, color);
            }
            strip.show();
            break;
         }

         case NEO_RECORDING:
            setNeoPixelColor(0, 0, 255);  // Blue
            break;

         case NEO_PLAYING:
            setNeoPixelColor(0, 255, 0);  // Green
            break;

         case NEO_WAITING:
            setNeoPixelColor(255, 255, 0);  // Yellow
            break;

         case NEO_ERROR:
            setNeoPixelColor(255, 0, 0);  // Red
            // Check if error display time has elapsed
            if (currentTime - errorStartTime >= ERROR_DISPLAY_MS) {
               currentNeoMode = NEO_IDLE_CYCLING;
               lastIdleActivity = millis();  // Reset idle timer
            }
            break;

         case NEO_OFF:
            // LEDs are already off, do nothing
            break;
      }

      // Wait before next update
      vTaskDelay(xDelay);
   }
}

// Function to update TFT display with status message
void updateTFTStatus(const String& message, uint16_t color, bool clearScreen = false) {
  static int statusLine = 0;

  if (clearScreen) {
    tft.fillScreen(ST77XX_BLACK);
    statusLine = 0;
  }

  // If we've filled the screen, clear it and start again
  if (statusLine >= MAX_STATUS_LINES) {
    tft.fillScreen(ST77XX_BLACK);
    statusLine = 0;
  }
  
  // Set cursor position for this status line
  tft.setCursor(0, statusLine * STATUS_LINE_HEIGHT);
  tft.setTextColor(color);
  
  // Print the message
  tft.println(message);
  
  // Move to next line
  statusLine++;
}

// Function to update progress bar
void updateProgressBar(int percentage, uint16_t color) {
  // Clear the progress bar area
  tft.fillRect(0, PROGRESS_BAR_Y, tft.width(), PROGRESS_BAR_HEIGHT, ST77XX_BLACK);
  
  // Draw the progress bar
  int barWidth = (percentage * tft.width()) / 100;
  tft.fillRect(0, PROGRESS_BAR_Y, barWidth, PROGRESS_BAR_HEIGHT, color);
}

// Function to draw recording indicator
void drawRecordingIndicator(bool isActive) {
  static bool lastState = false;
  
  if (isActive != lastState) {
    uint16_t color = isActive ? ST77XX_RED : ST77XX_BLACK;
    tft.fillCircle(tft.width() - 15, 15, 10, color);
    lastState = isActive;
  }
}

void setup() {
   Serial.begin(115200);
   delay(1000);  // Give serial monitor time to start up
   
   Serial.println("ESP32-S3 Dawn Audio Client with NeoPixels");
   
   // Initialize TFT display
   pinMode(TFT_BACKLIGHT, OUTPUT);
   digitalWrite(TFT_BACKLIGHT, HIGH);  // Turn on backlight
   
   tft.init(135, 240);  // Initialize ST7789 240x135
   tft.setRotation(3);  // Landscape mode
   tft.fillScreen(ST77XX_BLACK);
   tft.setTextSize(TEXT_SIZE);
   tft.setTextWrap(true);
   
   updateTFTStatus("Dawn Audio Client", ST77XX_BLUE, true);
   
   // Initialize NeoPixel strip
   strip.begin();
   strip.clear();
   strip.show();
   updateTFTStatus("NeoPixels: OK", ST77XX_GREEN);
   Serial.println("NeoPixel strip initialized");
   
   // Initialize PSRAM
   Serial.println("Checking PSRAM...");
   if (psramFound()) {
      Serial.println("PSRAM found and initialized");
      updateTFTStatus("PSRAM: OK", ST77XX_GREEN);
   } else {
      Serial.println("PSRAM not found! Please enable PSRAM in the board config");
      updateTFTStatus("PSRAM: ERROR", ST77XX_RED);
      while(1);
   }
   
   // Allocate all buffers at startup - never reallocate during operation
   Serial.println("Allocating static buffers...");
   
   // Audio recording buffer (in samples, not bytes)
   audioBuffer = (int16_t*)ps_malloc(BUFFER_SIZE * sizeof(int16_t));
   if (audioBuffer == NULL) {
      Serial.println("Failed to allocate PSRAM audio buffer!");
      updateTFTStatus("Audio buffer failed!", ST77XX_RED);
      while(1);
   }
   Serial.printf("Allocated %d bytes in PSRAM for audio buffer\n", BUFFER_SIZE * sizeof(int16_t));
   
   // Dual-purpose buffer: WAV creation when sending, server response when receiving
   // Size: large enough for max audio data + WAV header + padding
   size_t responseBufferSize = BUFFER_SIZE * sizeof(int16_t) + sizeof(WavHeader) + 1024;
   responseBuffer = (uint8_t*)ps_malloc(responseBufferSize);
   if (responseBuffer == NULL) {
      Serial.println("Failed to allocate PSRAM response buffer!");
      updateTFTStatus("Response buffer failed!", ST77XX_RED);
      while(1);
   }
   Serial.printf("Allocated %d bytes in PSRAM for dual-purpose buffer\n", responseBufferSize);
   
   // Clear both buffers
   memset(audioBuffer, 0, BUFFER_SIZE * sizeof(int16_t));
   memset(responseBuffer, 0, responseBufferSize);
   
   Serial.println("Two-buffer system allocated successfully");
   updateTFTStatus("Buffers allocated", ST77XX_GREEN);
   
   // Initialize button pin
   pinMode(BUTTON_PIN, INPUT_PULLUP);
   
   // Set pin attenuation for mic
   analogSetPinAttenuation(MIC_ADC_PIN, ADC_11db);
   
   // Set ADC resolution (default is 12-bit on ESP32-S3)
   analogReadResolution(12);
   
   // Connect to WiFi
   WiFi.begin(ssid, password);
   updateTFTStatus("WiFi connecting...", ST77XX_YELLOW);
   Serial.print("Connecting to WiFi");
   int attempts = 0;
   while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
   }
   
   if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      updateTFTStatus("WiFi: " + WiFi.localIP().toString(), ST77XX_GREEN);
   } else {
      Serial.println("\nWiFi connection failed! Continuing without WiFi...");
      updateTFTStatus("WiFi: FAILED", ST77XX_RED);
      updateTFTStatus("Local mode only", ST77XX_YELLOW);
   }
   
   // Create NeoPixel task on Core 0 (Core 1 is used by Arduino main loop)
   xTaskCreatePinnedToCore(
      neoPixelTask,           // Task function
      "NeoPixel Task",        // Task name
      4096,                   // Stack size (4KB should be sufficient)
      NULL,                   // Task parameters
      1,                      // Priority (1 = low priority, higher numbers = higher priority)
      &neoPixelTaskHandle,    // Task handle
      0                       // Core to run on (0 or 1)
   );
   
   if (neoPixelTaskHandle != NULL) {
      Serial.println("NeoPixel task created successfully");
      updateTFTStatus("NeoPixel task: OK", ST77XX_GREEN);
   } else {
      Serial.println("Failed to create NeoPixel task!");
      updateTFTStatus("NeoPixel task: FAILED", ST77XX_RED);
   }
   
   reportMemoryUsage();

   updateTFTStatus("Press button to record", ST77XX_WHITE);
   Serial.println("Setup complete. Press the button to start recording.");
   
   // Start in idle cycling mode
   setNeoPixelMode(NEO_IDLE_CYCLING);
}

void loop() {
   // Read the button state with debounce
   int reading = digitalRead(BUTTON_PIN);
   
   // Check if the button state has changed
   if (reading != lastButtonState) {
      lastDebounceTime = millis();
   }
   
   // If the button state has been stable for longer than the debounce time
   if ((millis() - lastDebounceTime) > DEBOUNCE_TIME) {
      // If the button state has changed
      if (reading != buttonState) {
         buttonState = reading;
         
         // Button is pressed (LOW when pressed as it's connected to ground)
         if (buttonState == LOW && !isRecording) {
            startRecording();
         }
         // Button is released
         else if (buttonState == HIGH && isRecording) {
            stopRecording();
            processRecording();
         }
      }
   }
   
   // Handle audio sampling
   while (isRecording && sampleCount < BUFFER_SIZE &&
          (int32_t)(micros() - nextSampleTime) >= 0) {
      int adcVal = analogRead(MIC_ADC_PIN);
      audioBuffer[sampleCount++] = (int16_t)((adcVal - 2048) << 4); // 12b -> signed 16b
      nextSampleTime += sampleInterval_us;                           // accumulate, no drift
   }
   
   // Show recording progress every half second
   static unsigned long lastProgressUpdate = 0;
   if (isRecording && millis() - lastProgressUpdate >= 500) {
      // Calculate percentage of buffer filled
      int percentage = (sampleCount * 100) / BUFFER_SIZE;
      updateProgressBar(percentage, ST77XX_RED);
      
      // Update seconds display
      float seconds = (float)sampleCount / SAMPLE_RATE;
      Serial.printf("Recording: %.1f seconds, %d samples\n", seconds, sampleCount);
      
      lastProgressUpdate = millis();
      
      // Toggle recording indicator
      drawRecordingIndicator(true);
   }
   
   // Flash recording indicator
   static unsigned long lastBlinkTime = 0;
   if (isRecording && millis() - lastBlinkTime >= 250) {
      drawRecordingIndicator((millis() / 250) % 2 == 0);
      lastBlinkTime = millis();
   }
   
   // Stop recording if buffer is full
   if (isRecording && sampleCount >= BUFFER_SIZE) {
      Serial.println("Buffer full, stopping recording");
      updateTFTStatus("Buffer full!", ST77XX_YELLOW);
      stopRecording();
      processRecording();
   }
   
   lastButtonState = reading;
}

void startRecording() {
   Serial.println("Starting recording...");
   updateTFTStatus("Recording...", ST77XX_RED, true);
   setNeoPixelMode(NEO_RECORDING);
   sampleCount    = 0;
   isRecording    = true;
   recStartUs     = micros();
   nextSampleTime = recStartUs;
   drawRecordingIndicator(true);
}

void stopRecording() {
   Serial.println("Stopping recording...");
   isRecording = false;
   recEndUs = micros();
   recFs = (double)sampleCount * 1e6 / (double)(recEndUs - recStartUs);
   Serial.printf("Effective sample rate: %.2f Hz\n", recFs);
   
   Serial.printf("Recorded %d samples (%.1f seconds)\n", 
      sampleCount, 
      (float)sampleCount / SAMPLE_RATE);
   
   updateTFTStatus("Recording complete", ST77XX_GREEN);
   updateTFTStatus(String(sampleCount) + " samples", ST77XX_WHITE);
   drawRecordingIndicator(false);
   setNeoPixelMode(NEO_IDLE_CYCLING);  // Return to idle state
}

void processRecording() {
   bool useLocalPlayback = true;
   bool hasError = false;
   
   // Only try server connection if WiFi is connected
   if (WiFi.status() == WL_CONNECTED) {
      updateTFTStatus("Connecting to server...", ST77XX_YELLOW);
      setNeoPixelMode(NEO_WAITING);
      Serial.println("Trying to connect to Dawn server...");
      if (sendToServerAndGetResponse()) {
         Serial.println("Successfully communicated with server");
         updateTFTStatus("Server OK", ST77XX_GREEN);
         useLocalPlayback = false;
      } else {
         Serial.println("Server communication failed, falling back to local playback");
         updateTFTStatus("Server failed", ST77XX_RED);
         updateTFTStatus("Using local playback", ST77XX_YELLOW);
         setNeoPixelMode(NEO_ERROR);
         useLocalPlayback = true;
         hasError = true;
      }
   } else {
      Serial.println("No WiFi connection, using local playback");
      updateTFTStatus("No WiFi, local mode", ST77XX_YELLOW);
   }
   
   if (useLocalPlayback && !hasError) {
      // Use local playback with our recorded data
      playRecording(true);
   } else if (!hasError) {
      // Play the response from the server
      playServerResponse();
   }
   // If hasError is true, we just display the error and wait for next recording
   
   // Return to idle state after processing (unless there was an error)
   if (!hasError) {
      setNeoPixelMode(NEO_IDLE_CYCLING);
   }
}

// Create a WAV file in memory from the audio buffer
size_t createWavFileInResponseBuffer() {
   // Calculate WAV size
   size_t dataSize = sampleCount * sizeof(int16_t);
   size_t totalSize = sizeof(WavHeader) + dataSize;
   
   // responseBuffer should be large enough (we allocated it with extra space)
   size_t responseBufferSize = BUFFER_SIZE * sizeof(int16_t) + sizeof(WavHeader) + 1024;
   if (totalSize > responseBufferSize) {
      Serial.printf("WAV file too large: %d bytes (buffer: %d bytes)\n", totalSize, responseBufferSize);
      updateTFTStatus("WAV too large!", ST77XX_RED);
      return 0;
   }
   
   // Create WAV header
   WavHeader header;
   header.subchunk2Size = dataSize;
   header.chunkSize = 36 + dataSize;
   
   // Copy header to responseBuffer (reusing it for transmission)
   memcpy(responseBuffer, &header, sizeof(WavHeader));
   
   // Copy audio data to responseBuffer after the header
   memcpy(responseBuffer + sizeof(WavHeader), audioBuffer, dataSize);
   
   Serial.printf("Created WAV file in response buffer: %d bytes\n", totalSize);
   return totalSize;
}

bool connectToServer(WiFiClient &client) {
  // Set TCP buffer sizes for better performance
  client.setNoDelay(true);             // Disable Nagle's algorithm

   for (int attempt = 1; attempt <= MAX_CONNECTION_ATTEMPTS; attempt++) {
      updateTFTStatus("Attempt " + String(attempt) + "/" + String(MAX_CONNECTION_ATTEMPTS), ST77XX_YELLOW);
      Serial.printf("Connection attempt %d/%d to %s:%d\n", 
                   attempt, MAX_CONNECTION_ATTEMPTS, SERVER_IP, SERVER_PORT);
      
      client.setTimeout(CONNECTION_TIMEOUT_MS / 1000); // Convert to seconds
      
      if (client.connect(SERVER_IP, SERVER_PORT)) {
         Serial.println("Connected to server");
         updateTFTStatus("Connected to server", ST77XX_GREEN);
         return true;
      }
      
      Serial.printf("Connection failed (attempt %d)\n", attempt);
      
      // Wait before next attempt
      delay(500);
   }
   
   Serial.println("All connection attempts failed");
   updateTFTStatus("Connection failed!", ST77XX_RED);
   return false;
}

// === Checksum Implementation ===
// Simple Fletcher-16 checksum algorithm
uint16_t calculateChecksum(const uint8_t* data, size_t length) {
  uint16_t sum1 = 0;
  uint16_t sum2 = 0;
  
  for (size_t i = 0; i < length; i++) {
    sum1 = (sum1 + data[i]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }
  
  return (sum2 << 8) | sum1;
}

// === Packet Building and Parsing ===
// Create a packet header
void buildPacketHeader(uint8_t* header, uint32_t dataLength, uint8_t packetType, uint16_t checksum) {
  // 4 bytes: length (big endian)
  header[0] = (dataLength >> 24) & 0xFF;
  header[1] = (dataLength >> 16) & 0xFF;
  header[2] = (dataLength >> 8) & 0xFF;
  header[3] = dataLength & 0xFF;
  
  // 1 byte: protocol version
  header[4] = PROTOCOL_VERSION;
  
  // 1 byte: packet type
  header[5] = packetType;
  
  // 2 bytes: checksum (big endian)
  header[6] = (checksum >> 8) & 0xFF;
  header[7] = checksum & 0xFF;
}

// Parse a packet header
bool parsePacketHeader(const uint8_t* header, uint32_t* dataLength, uint8_t* packetType, uint16_t* checksum) {
  // Extract length (big endian)
  *dataLength = ((uint32_t)header[0] << 24) |
                ((uint32_t)header[1] << 16) |
                ((uint32_t)header[2] << 8) |
                header[3];
  
  // Check protocol version
  if (header[4] != PROTOCOL_VERSION) {
    return false;
  }
  
  // Extract packet type
  *packetType = header[5];
  
  // Extract checksum
  *checksum = ((uint16_t)header[6] << 8) | header[7];
  
  return true;
}

// === Improved Handshake Implementation ===
bool performHandshake(WiFiClient &client) {
  uint8_t handshakeData[4] = {0xA5, 0x5A, 0xB2, 0x2B}; // Magic bytes
  uint8_t header[PACKET_HEADER_SIZE];
  uint16_t checksum = calculateChecksum(handshakeData, sizeof(handshakeData));
  
  // Build handshake packet
  buildPacketHeader(header, sizeof(handshakeData), PACKET_TYPE_HANDSHAKE, checksum);
  
  // Send header
  if (!client.write(header, PACKET_HEADER_SIZE)) {
    Serial.println("Failed to send handshake header");
    return false;
  }
  
  // Send handshake data
  if (!client.write(handshakeData, sizeof(handshakeData))) {
    Serial.println("Failed to send handshake data");
    return false;
  }
  
  client.clear(); // Make sure data is sent immediately
  
  // Wait for acknowledgment with timeout
  unsigned long startTime = millis();
  unsigned long timeout = 3000; // 3 second timeout
  uint8_t responseHeader[PACKET_HEADER_SIZE];
  size_t bytesRead = 0;
  
  Serial.println("Waiting for handshake response...");
  
  while (bytesRead < PACKET_HEADER_SIZE && (millis() - startTime < timeout)) {
    if (client.available()) {
      int got = client.read(responseHeader + bytesRead, PACKET_HEADER_SIZE - bytesRead);
      if (got > 0) {
        bytesRead += got;
        Serial.printf("Read %d response bytes, total %d of %d\n", got, bytesRead, PACKET_HEADER_SIZE);
      }
    }
    else {
      delay(10); // Short delay if no data available
    }
  }
  
  if (bytesRead != PACKET_HEADER_SIZE) {
    Serial.printf("Failed to receive complete handshake response: got %d of %d bytes\n", 
                 bytesRead, PACKET_HEADER_SIZE);
    return false;
  }
  
  // Log the raw header for debugging
  Serial.println("Received handshake response header:");
  for (int i = 0; i < PACKET_HEADER_SIZE; i++) {
    Serial.printf("0x%02X ", responseHeader[i]);
  }
  Serial.println();
  
  uint32_t respLength;
  uint8_t respType;
  uint16_t respChecksum;
  
  if (!parsePacketHeader(responseHeader, &respLength, &respType, &respChecksum)) {
    Serial.println("Invalid handshake response header");
    return false;
  }
  
  Serial.printf("Response header: length=%lu, type=0x%02X, checksum=0x%04X\n", 
               (unsigned long)respLength, respType, respChecksum);
  
  if (respType != PACKET_TYPE_ACK) {
    Serial.printf("Handshake not acknowledged, got type 0x%02X instead of 0x%02X\n", 
                 respType, PACKET_TYPE_ACK);
    return false;
  }
  
  Serial.println("Handshake successful");
  return true;
}

// === Chunked Data Transfer with Retries ===
bool sendDataWithRetries(WiFiClient &client, const uint8_t* data, size_t dataSize) {
  uint8_t header[PACKET_HEADER_SIZE];
  const size_t chunkSize = PACKET_MAX_SIZE;  // Use the defined constant
  size_t totalSent = 0;
  sendSequence = 0;
  
  Serial.printf("Sending %zu bytes with chunk size %d bytes\n", dataSize, PACKET_MAX_SIZE);
  
  while (totalSent < dataSize) {
    size_t remainingBytes = dataSize - totalSent;
    size_t currentChunkSize = (remainingBytes > chunkSize) ? chunkSize : remainingBytes;
    bool isLastChunk = (totalSent + currentChunkSize >= dataSize);
    
    // Packet type: last chunk or regular data
    uint8_t packetType = isLastChunk ? PACKET_TYPE_DATA_END : PACKET_TYPE_DATA;
    
    // Calculate checksum for this chunk
    uint16_t checksum = calculateChecksum(data + totalSent, currentChunkSize);
    
    // Build header
    buildPacketHeader(header, currentChunkSize, packetType, checksum);
    
    // Add sequence number
    uint8_t sequenceBytes[2];
    sequenceBytes[0] = (sendSequence >> 8) & 0xFF;
    sequenceBytes[1] = sendSequence & 0xFF;
    
    // Retry loop
    bool chunkSent = false;
    for (int retryCount = 0; retryCount < MAX_RETRIES && !chunkSent; retryCount++) {
      // If retry, add delay with exponential backoff
      if (retryCount > 0) {
        int delayTime = min(RETRY_BASE_DELAY * (1 << retryCount), MAX_RETRY_DELAY);
        Serial.printf("Retry %d after %d ms\n", retryCount, delayTime);
        delay(delayTime);
      }
      
      // Send header
      if (!client.write(header, PACKET_HEADER_SIZE)) {
        Serial.println("Failed to send chunk header");
        continue;
      }
      
      // Send sequence number
      if (!client.write(sequenceBytes, 2)) {
        Serial.println("Failed to send sequence number");
        continue;
      }
      
      // Send chunk data
      if (!client.write(data + totalSent, currentChunkSize)) {
        Serial.println("Failed to send chunk data");
        continue;
      }
      
      // Wait for acknowledgment with timeout
      unsigned long ackTimeout = millis() + 1000; // 1 second timeout
      bool ackReceived = false;
      
      while (millis() < ackTimeout && !ackReceived) {
        if (client.available() >= PACKET_HEADER_SIZE) {
          uint8_t ackHeader[PACKET_HEADER_SIZE];
          if (client.read(ackHeader, PACKET_HEADER_SIZE) == PACKET_HEADER_SIZE) {
            uint32_t ackLength;
            uint8_t ackType;
            uint16_t ackChecksum;
            
            if (parsePacketHeader(ackHeader, &ackLength, &ackType, &ackChecksum)) {
              if (ackType == PACKET_TYPE_ACK) {
                chunkSent = true;
                ackReceived = true;
                break;
              } else if (ackType == PACKET_TYPE_NACK) {
                Serial.println("Received NACK, will retry");
                break;
              }
            }
          }
        }
        delay(10);
      }
      
      if (!ackReceived) {
        Serial.println("ACK timeout, will retry");
      }
    }
    
    if (!chunkSent) {
      Serial.println("Failed to send chunk after all retries");
      return false;
    }
    
    totalSent += currentChunkSize;
    sendSequence++;
    
    // Update progress bar
    int percentage = (totalSent * 100) / dataSize;
    updateProgressBar(percentage, ST77XX_BLUE);
  }
  
  return true;
}

// === Receiving Data with Verification ===
bool receiveDataWithVerification(WiFiClient &client, uint8_t* buffer, size_t bufferSize, size_t* receivedSize) {
  *receivedSize = 0;
  receiveSequence = 0;
  
  Serial.printf("Ready to receive data (max packet: %d bytes)\n", PACKET_MAX_SIZE);
  
  // Add a small delay before trying to read the response
  delay(200);
  
  Serial.println("Ready to receive server response...");
  
  // Set a longer timeout for the first packet
  unsigned long firstPacketTimeout = millis() + FIRST_PACKET_SERVER_RESPONSE_TIMEOUT;
  
   // Wait for the first packet with the longer timeout
   while (client.available() < PACKET_HEADER_SIZE) {
   if (millis() > firstPacketTimeout) {
      Serial.println("Timeout waiting for AI response");
      return false;
   }
   
   // Periodically update the user about the wait
   if (millis() % 5000 == 0) {  // Every 5 seconds
      updateTFTStatus("Waiting for AI...", ST77XX_CYAN);
   }
   
   delay(50);
   }
  
  Serial.println("First response packet detected");
  
  // Now proceed with normal packet reception
  while (true) {
    // Read packet header
    uint8_t header[PACKET_HEADER_SIZE];
    size_t headerBytesRead = 0;
    unsigned long headerTimeout = millis() + 2000; // 2 second timeout for header
    
    while (headerBytesRead < PACKET_HEADER_SIZE) {
      if (millis() > headerTimeout) {
        Serial.printf("Timeout reading header: got %d of %d bytes\n", headerBytesRead, PACKET_HEADER_SIZE);
        return false;
      }
      
      if (client.available()) {
        int got = client.read(header + headerBytesRead, PACKET_HEADER_SIZE - headerBytesRead);
        if (got > 0) {
          headerBytesRead += got;
        } else if (got < 0) {
          Serial.println("Error reading from socket");
          return false;
        }
      } else {
        delay(10);
      }
    }
    
    // Log header bytes for debugging
    Serial.print("Received header: ");
    for (int i = 0; i < PACKET_HEADER_SIZE; i++) {
      Serial.printf("0x%02X ", header[i]);
    }
    Serial.println();
    
    uint32_t dataLength;
    uint8_t packetType;
    uint16_t expectedChecksum;
    
    if (!parsePacketHeader(header, &dataLength, &packetType, &expectedChecksum)) {
      Serial.println("Invalid packet header");
      sendNack(client);
      return false;
    }
    
    Serial.printf("Packet: type=0x%02X, length=%lu, checksum=0x%04X\n", 
                  packetType, (unsigned long)dataLength, expectedChecksum);
    
    // Check if data length is valid
   if (dataLength > PACKET_MAX_SIZE) {
     Serial.printf("Data length too large: %lu (max: %d)\n", (unsigned long)dataLength, PACKET_MAX_SIZE);
     sendNack(client);
     return false;
   }

   if (*receivedSize + dataLength > bufferSize) {
     Serial.printf("Receive buffer overflow: %lu (max: %d)\n", *receivedSize + dataLength, bufferSize);
     sendNack(client);
     return false;
   }

    // Read sequence number (first 2 bytes)
    uint8_t seqBytes[2];
    unsigned long seqTimeout = millis() + 1000;
    
    while (client.available() < 2) {
      if (millis() > seqTimeout) {
        Serial.println("Timeout waiting for sequence number");
        return false;
      }
      delay(10);
    }
    
    if (client.read(seqBytes, 2) != 2) {
      Serial.println("Failed to read sequence number");
      sendNack(client);
      return false;
    }
    
    uint16_t packetSequence = ((uint16_t)seqBytes[0] << 8) | seqBytes[1];
    
    // Verify sequence number
    if (packetSequence != receiveSequence) {
      Serial.printf("Sequence mismatch: expected %u, got %u\n", receiveSequence, packetSequence);
      sendNack(client);
      continue;
    }
    
    // Read chunk data with better timeout handling
    size_t bytesRead = 0;
    unsigned long dataTimeout = millis() + 5000; // 5 second timeout for data
    
    Serial.printf("Reading %lu bytes of data...\n", (unsigned long)dataLength);
    
    // Create a temporary buffer for the chunk data
    uint8_t* chunkBuffer = (uint8_t*)ps_malloc(dataLength);
    if (!chunkBuffer) {
      Serial.println("Failed to allocate chunk buffer");
      sendNack(client);
      return false;
    }
    
    while (bytesRead < dataLength) {
      if (millis() > dataTimeout) {
        Serial.printf("Timeout reading data: got %lu of %lu bytes\n", 
                     (unsigned long)bytesRead, (unsigned long)dataLength);
        free(chunkBuffer);
        return false;
      }
      
      int available = client.available();
      if (available <= 0) {
        delay(10);
        continue;
      }
      
      size_t toRead = min((size_t)available, (size_t)(dataLength - bytesRead));
      int got = client.read(chunkBuffer + bytesRead, toRead);
      
      if (got <= 0) {
        Serial.println("Error reading chunk data");
        free(chunkBuffer);
        sendNack(client);
        return false;
      }
      
      bytesRead += got;
      
      // Update timeout based on progress
      dataTimeout = millis() + 2000; // Reset timeout after successful read
    }
    
    // Verify checksum
    uint16_t actualChecksum = calculateChecksum(chunkBuffer, dataLength);
    if (actualChecksum != expectedChecksum) {
      Serial.printf("Checksum mismatch: expected 0x%04X, got 0x%04X\n", expectedChecksum, actualChecksum);
      free(chunkBuffer);
      sendNack(client);
      continue;
    }
    
    // Copy chunk data to the output buffer - WITHOUT the sequence number
    // This is the critical fix - we only copy the actual data
    memcpy(buffer + *receivedSize, chunkBuffer, dataLength);
    free(chunkBuffer);
    
    // Send acknowledgment with multiple attempts
    for (int ackAttempt = 0; ackAttempt < 3; ackAttempt++) {
      if (sendAck(client)) {
        break;
      }
      
      Serial.printf("ACK send attempt %d failed, retrying...\n", ackAttempt + 1);
      delay(50);
    }
    
    // Update received size
    *receivedSize += dataLength;
    receiveSequence++;
    
    // Update progress bar
    int percentage = (*receivedSize * 100) / bufferSize; // This is approximate
    updateProgressBar(percentage, ST77XX_GREEN);
    
    // Check if this was the last packet
    if (packetType == PACKET_TYPE_DATA_END) {
      Serial.println("Received final data packet");
      break;
    }
  }
  
  Serial.printf("Successfully received %zu bytes\n", *receivedSize);
  return true;
}

// === Acknowledgment Functions ===
bool sendAck(WiFiClient &client) {
  uint8_t header[PACKET_HEADER_SIZE];
  buildPacketHeader(header, 0, PACKET_TYPE_ACK, 0);
  
  // Log what we're sending
  Serial.print("Sending ACK: ");
  for (int i = 0; i < PACKET_HEADER_SIZE; i++) {
    Serial.printf("0x%02X ", header[i]);
  }
  Serial.println();
  
  // Send with verification
  size_t sent = client.write(header, PACKET_HEADER_SIZE);
  client.clear(); // Ensure data is sent immediately
  
  return (sent == PACKET_HEADER_SIZE);
}

void sendNack(WiFiClient &client) {
  uint8_t header[PACKET_HEADER_SIZE];
  buildPacketHeader(header, 0, PACKET_TYPE_NACK, 0);
  client.write(header, PACKET_HEADER_SIZE);
}

// Add this helper function to print WAV header contents
void dumpWavHeader(const WavHeader* header) {
  Serial.println("WAV Header Contents:");
  
  // Print RIFF identifier
  Serial.print("RIFF ID: ");
  for (int i = 0; i < 4; i++) {
    Serial.print((char)header->riff[i]);
  }
  Serial.println();
  
  // Print chunk size
  Serial.printf("Chunk Size: %lu\n", (unsigned long)header->chunkSize);
  
  // Print WAVE identifier
  Serial.print("WAVE ID: ");
  for (int i = 0; i < 4; i++) {
    Serial.print((char)header->wave[i]);
  }
  Serial.println();
  
  // Print format section
  Serial.print("Format: ");
  for (int i = 0; i < 4; i++) {
    Serial.print((char)header->fmt[i]);
  }
  Serial.println();
  
  // Print audio format details
  Serial.printf("Audio Format: %u\n", header->audioFormat);
  Serial.printf("Channels: %u\n", header->numChannels);
  Serial.printf("Sample Rate: %lu\n", (unsigned long)header->sampleRate);
  Serial.printf("Byte Rate: %lu\n", (unsigned long)header->byteRate);
  Serial.printf("Block Align: %u\n", header->blockAlign);
  Serial.printf("Bits Per Sample: %u\n", header->bitsPerSample);
  
  // Print data section
  Serial.print("Data ID: ");
  for (int i = 0; i < 4; i++) {
    Serial.print((char)header->data[i]);
  }
  Serial.println();
  
  // Print data size
  Serial.printf("Data Size: %lu\n", (unsigned long)header->subchunk2Size);
}

// === Improved WAV Validation Function ===
bool isValidWavFormat(const uint8_t* buffer, size_t size) {
  if (size < 44) { // Minimum WAV header size
    Serial.printf("Buffer too small for WAV header: %zu bytes (need at least 44)\n", size);
    return false;
  }
  
  // Print first 44 bytes (WAV header) for debugging
  Serial.println("First 44 bytes of received data:");
  for (int i = 0; i < 44 && i < size; i++) {
    Serial.printf("%02X ", buffer[i]);
    if ((i + 1) % 16 == 0) Serial.println();
  }
  Serial.println();
  
  // Attempt to fix common WAV header corruption issues
  // If we detect binary data that's not a valid WAV header, try to reconstruct it
  if (buffer[0] != 'R' || buffer[1] != 'I' || buffer[2] != 'F' || buffer[3] != 'F') {
    Serial.println("Invalid RIFF header");
    char header_chars[5] = {0};
    header_chars[0] = (char)buffer[0];
    header_chars[1] = (char)buffer[1];
    header_chars[2] = (char)buffer[2];
    header_chars[3] = (char)buffer[3];
    Serial.printf("Found: %s\n", header_chars);
    
    // Since the RIFF header is invalid, let's assume the data is raw PCM
    // We'll play it anyway, but with more careful validation
    return false;
  }
  
  // Check WAVE format
  if (buffer[8] != 'W' || buffer[9] != 'A' || buffer[10] != 'V' || buffer[11] != 'E') {
    Serial.println("Invalid WAVE format");
    char format_chars[5] = {0};
    format_chars[0] = (char)buffer[8];
    format_chars[1] = (char)buffer[9];
    format_chars[2] = (char)buffer[10];
    format_chars[3] = (char)buffer[11];
    Serial.printf("Found: %s\n", format_chars);
    return false;
  }
  
  return true;
}

// === Updated Server Communication Function ===
bool sendToServerAndGetResponse() {
   WiFiClient client;
   
   // Try to connect to server
   if (!connectToServer(client)) {
      return false;
   }
   
   // Perform handshake with retry
   int handshakeAttempts = 0;
   bool handshakeSuccess = false;
   
   while (handshakeAttempts < 3 && !handshakeSuccess) {
      if (handshakeAttempts > 0) {
         Serial.printf("Retrying handshake (attempt %d/3)...\n", handshakeAttempts + 1);
         delay(500 * handshakeAttempts);
      }
      
      handshakeSuccess = performHandshake(client);
      handshakeAttempts++;
   }
   
   if (!handshakeSuccess) {
      Serial.println("Handshake failed after multiple attempts");
      client.stop();
      return false;
   }
   
   // Create WAV file in response buffer (reusing for transmission)
   size_t wavSize = createWavFileInResponseBuffer();
   if (wavSize == 0) {
      Serial.println("Failed to create WAV file in response buffer");
      updateTFTStatus("WAV creation failed!", ST77XX_RED);
      client.stop();
      return false;
   }
   
   Serial.printf("Sending %zu bytes of WAV data using dual-purpose buffer\n", wavSize);
   updateTFTStatus("Sending data...", ST77XX_BLUE);
   
   // Send data directly from responseBuffer
   bool sendSuccess = sendDataWithRetries(client, responseBuffer, wavSize);
   
   // No need to free anything - using static buffer!
   
   if (!sendSuccess) {
      Serial.println("Failed to send data to server");
      updateTFTStatus("Send failed!", ST77XX_RED);
      client.stop();
      return false;
   }
   
   Serial.println("Waiting for response...");
   updateTFTStatus("Waiting for response...", ST77XX_CYAN);
   setNeoPixelMode(NEO_WAITING);
   
   // Keep the connection alive while waiting for response
   client.setNoDelay(true);
   
   // Receive response into static buffer
   size_t receivedSize = 0;
   bool receiveSuccess = receiveDataWithVerification(client, responseBuffer, 
                                                   BUFFER_SIZE * sizeof(int16_t) + 1024,
                                                   &receivedSize);
   
   client.stop();
   
   if (!receiveSuccess) {
      Serial.println("Failed to receive response");
      updateTFTStatus("Receive failed!", ST77XX_RED);
      setNeoPixelMode(NEO_ERROR);
      return false;
   }
   
   responseLength = receivedSize;
   Serial.printf("Successfully received %zu bytes into static buffer\n", responseLength);
   
   // Validate WAV format
   if (!isValidWavFormat(responseBuffer, responseLength)) {
      Serial.println("Invalid WAV format in response");
      updateTFTStatus("Invalid WAV format!", ST77XX_RED);
      return false;
   }
   
   // Cast to WavHeader now that we know it's valid
   WavHeader* header = (WavHeader*)responseBuffer;
   
   Serial.printf("Valid WAV response: %lu Hz, %u-bit, %u-ch\n", 
              (unsigned long)header->sampleRate, header->bitsPerSample, header->numChannels);
   
   return true;
}

// === Improved PlayServerResponse Function ===
// This function will handle corrupted WAV data more gracefully
void playServerResponse() {
  Serial.println("Playing server response...");
  updateTFTStatus("Playing server audio...", ST77XX_CYAN, true);
  setNeoPixelMode(NEO_PLAYING);
  
  if (responseLength < 44) {
    Serial.println("Response too small to be audio data");
    updateTFTStatus("Invalid audio data!", ST77XX_RED);
    setNeoPixelMode(NEO_ERROR);
    return; // Don't attempt playback, just show error
  }
  
  // Try to detect if this is a valid WAV file
  bool isValidWav = isValidWavFormat(responseBuffer, responseLength);
  
  if (isValidWav) {
    // Parse WAV header
    WavHeader* header = (WavHeader*)responseBuffer;
    uint32_t dataOffset = sizeof(WavHeader);
    uint32_t dataSize = header->subchunk2Size;
    uint32_t serverSampleRate = header->sampleRate;
    uint16_t serverChannels = header->numChannels;
    uint16_t serverBitDepth = header->bitsPerSample;

    Serial.printf("Server WAV: %lu Hz, %u-bit, %u channels, %lu bytes\n", 
                 (unsigned long)serverSampleRate, 
                 serverBitDepth, 
                 serverChannels, 
                 (unsigned long)dataSize);
    updateTFTStatus(String(serverSampleRate) + "Hz " + 
                   String(serverBitDepth) + "bit " + 
                   String(serverChannels) + "ch", ST77XX_WHITE);
    
    // Extract response samples
    int16_t* responseSamples = (int16_t*)(responseBuffer + dataOffset);
    size_t numResponseSamples = dataSize / sizeof(int16_t);
    
    Serial.printf("Playing %zu samples at %lu Hz\n", numResponseSamples, (unsigned long)serverSampleRate);
    updateTFTStatus(String(numResponseSamples) + " samples", ST77XX_WHITE);
    updateTFTStatus(String(serverSampleRate) + " Hz", ST77XX_WHITE);
    
    // Play using our playback function with resampling
    playAudioSamples(responseSamples, numResponseSamples, serverSampleRate);
  } else {
    // Not a valid WAV - don't attempt playback, show error
    Serial.println("Not a valid WAV - cannot play invalid audio data");
    updateTFTStatus("Invalid WAV format!", ST77XX_RED);
    setNeoPixelMode(NEO_ERROR);
    return;
  }
}

void playRecording(bool useLocalBuffer) {
   Serial.println("Playing back recording...");
   updateTFTStatus("Playing local audio...", ST77XX_YELLOW, true);
   setNeoPixelMode(NEO_PLAYING);
   updateTFTStatus(String(sampleCount) + " samples", ST77XX_WHITE);
   updateTFTStatus(String((int)recFs) + " Hz", ST77XX_WHITE);
   
   // If using local buffer, play directly from audioBuffer
   int16_t* samples = useLocalBuffer ? audioBuffer : NULL;
   size_t numSamples = useLocalBuffer ? sampleCount : 0;
   
   playAudioSamples(samples, numSamples, recFs);
}

void playAudioSamples(int16_t* samples, size_t numSamples, double sourceSampleRate) {
   // Configure I2S for the speaker
   i2s_chan_handle_t tx_handle;
   
   // Create and configure I2S channel
   i2s_chan_config_t chan_cfg = {
      .id = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 16,
      .dma_frame_num = 512,
      .auto_clear = true,
      .auto_clear_before_cb = false,
      .allow_pd = false,
      .intr_priority = 0
   };
   
   ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));
   
   // Standard mode configuration
   i2s_std_config_t std_cfg = {};
   
   // Initialize all configuration fields to ensure proper playback
   std_cfg.clk_cfg.sample_rate_hz = 48000;
   std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
   std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
   std_cfg.clk_cfg.ext_clk_freq_hz = 0;

   std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, 
      I2S_SLOT_MODE_STEREO);   // bit_shift=true, Philips IÃ‚Â²S
   
   std_cfg.gpio_cfg.mclk = GPIO_NUM_NC;
   std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
   std_cfg.gpio_cfg.ws = (gpio_num_t)I2S_LRCLK;
   std_cfg.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
   std_cfg.gpio_cfg.din = GPIO_NUM_NC;
   std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
   std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
   std_cfg.gpio_cfg.invert_flags.ws_inv = false;
   
   ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
   ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
   delay(3);   // let LRCLK/BCLK settle
   
   Serial.println("I2S configured for playback");
   updateTFTStatus("I2S configured", ST77XX_CYAN);
   
   // Play audio
   size_t bytesWritten = 0;
   Serial.println("Playback started");
   updateTFTStatus("Playback started", ST77XX_CYAN);

   // Set output sample rate and prepare for resampling
   const double outFs = 48000.0;
   const size_t CHUNK = 512;              // matches dma_frame_num nicely
   int16_t out[CHUNK * 2];                // 16-bit stereo (L,R)

   // Calculate total output frames
   size_t outFrames = (size_t)((double)numSamples * (outFs / sourceSampleRate) + 0.5);
   double step = sourceSampleRate / outFs;  // input index advance per output frame
   double pos = 0.0;

   for (size_t produced = 0; produced < outFrames; ) {
      size_t n = min(CHUNK, outFrames - produced);
      for (size_t j = 0; j < n; ++j) {
         size_t i0 = (size_t)pos;
         double frac = pos - (double)i0;
         int16_t a = samples[(i0 < numSamples) ? i0 : (numSamples - 1)];
         int16_t b = samples[(i0 + 1 < numSamples) ? (i0 + 1) : (numSamples - 1)];
         int16_t s = (int16_t)((1.0 - frac) * a + frac * b);  // linear interp

         out[2*j + 0] = s;  // Left
         out[2*j + 1] = s;  // Right
         pos += step;
      }
      size_t bytes = n * 2 * sizeof(int16_t);
      ESP_ERROR_CHECK(i2s_channel_write(tx_handle, out, bytes, &bytesWritten, portMAX_DELAY));
      produced += n;
      
      // Update progress bar every chunk
      int percentage = (produced * 100) / outFrames;
      updateProgressBar(percentage, ST77XX_CYAN);
      
      // Show progress every 1 second (approximately)
      if (produced % (size_t)(outFs / CHUNK) == 0) {
         float seconds = (float)produced / outFs;
         Serial.printf("Played %0.1f seconds\n", seconds);
      }
   }
    
   Serial.println("Playback finished");
   updateTFTStatus("Playback complete", ST77XX_GREEN);
   setNeoPixelMode(NEO_IDLE_CYCLING);  // Return to idle after playback
   
   // Stop I2S driver
   ESP_ERROR_CHECK(i2s_channel_disable(tx_handle));
   ESP_ERROR_CHECK(i2s_del_channel(tx_handle));
   
   // Clear progress bar
   updateProgressBar(0, ST77XX_BLACK);
   
   // Ready for next recording
   updateTFTStatus("Ready for recording", ST77XX_WHITE);
   Serial.println("Ready for next recording. Press the button to start.");
}

// Memory usage reporting function - updated for two-buffer system
void reportMemoryUsage() {
   Serial.println("=== Memory Usage Report ===");
   Serial.printf("Total PSRAM: %d bytes\n", ESP.getPsramSize());
   Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
   Serial.printf("Used PSRAM: %d bytes\n", ESP.getPsramSize() - ESP.getFreePsram());
   
   size_t audioSize = BUFFER_SIZE * sizeof(int16_t);
   size_t responseSize = BUFFER_SIZE * sizeof(int16_t) + sizeof(WavHeader) + 1024;
   size_t totalStaticAllocation = audioSize + responseSize;
   
   Serial.printf("Audio buffer: %d bytes\n", audioSize);
   Serial.printf("Response buffer: %d bytes\n", responseSize);
   Serial.printf("Total static allocation: %d bytes\n", totalStaticAllocation);
   
   Serial.printf("Heap free: %d bytes\n", ESP.getFreeHeap());
   Serial.printf("Heap total: %d bytes\n", ESP.getHeapSize());
   Serial.println("=============================");
}