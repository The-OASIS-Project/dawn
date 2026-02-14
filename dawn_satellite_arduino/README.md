# DAWN Satellite — ESP32-S3 (DAP2 Tier 2)

Push-to-talk voice satellite for the DAWN assistant. Streams raw audio to the
daemon over WebSocket; the server handles ASR, LLM, and TTS. Audio responses
are played back through an I2S speaker.

## Hardware

| Component | Part | Notes |
|-----------|------|-------|
| MCU | [Adafruit ESP32-S3 TFT Feather](https://www.adafruit.com/product/5483) | Built-in TFT + NeoPixels, PSRAM required |
| Speaker amp | MAX98357 I2S breakout | 3.3V logic, mono |
| Microphone | Analog electret mic module | Connected to ADC pin |
| Button | Momentary push button | Push-to-talk trigger |

### Pin Assignments

| Function | GPIO |
|----------|------|
| Push-to-talk button | 18 |
| I2S BCLK | 5 |
| I2S LRCLK (WS) | 6 |
| I2S DOUT | 9 |
| Microphone ADC | 1 |
| NeoPixel (on-board) | 17 |
| TFT backlight (on-board) | 45 |

## Arduino IDE Setup

### Board

1. **File** > **Preferences** > **Additional Board Manager URLs** — add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. **Tools** > **Board** > **Boards Manager** — search **esp32** by Espressif, install
3. Select board: **Adafruit Feather ESP32-S3 TFT**
4. Under **Tools**, set:
   - **PSRAM**: `OPI PSRAM`
   - **Flash Size**: `4MB (32Mb)`
   - **Partition Scheme**: `Default 4MB with spiffs`
   - **Upload Speed**: `921600`

### Libraries

Install all via **Sketch** > **Include Library** > **Manage Libraries...**:

| Library | Author | Search term |
|---------|--------|-------------|
| WebSockets | Markus Sattler (Links2004) | `WebSockets` |
| ArduinoJson | Benoit Blanchon | `ArduinoJson` |
| Adafruit GFX Library | Adafruit | `Adafruit GFX` |
| Adafruit ST7735 and ST7789 | Adafruit | `Adafruit ST7789` |
| Adafruit NeoPixel | Adafruit | `Adafruit NeoPixel` |

## Configuration

Copy the example secrets header and edit it with your credentials:

```bash
cp arduino_secrets.h.example arduino_secrets.h
```

Then edit `arduino_secrets.h`:

```cpp
#define SECRET_SSID     "YOUR_SSID"
#define SECRET_PASSWORD "YOUR_PASSWORD"

#define SECRET_SERVER_IP   "192.168.1.159"
#define SECRET_SERVER_PORT 3000

#define SECRET_SATELLITE_NAME     "Office Speaker"
#define SECRET_SATELLITE_LOCATION "office"
```

**Note:** `arduino_secrets.h` is gitignored to keep credentials out of version control.

## Upload & Run

1. Connect the ESP32-S3 via USB
2. Select the correct port under **Tools** > **Port**
3. **Sketch** > **Upload**
4. Open **Serial Monitor** at 115200 baud to see status output

## Usage

- **Press and hold** the button to record
- **Release** to send audio to the daemon
- NeoPixels and TFT show current state:
  - Cycling colors = idle/ready
  - Blue = recording
  - Yellow = waiting for response
  - Green = playing TTS response
  - Red = error (clears after 3 seconds)

## How It Works

This is a DAP2 **Tier 2** satellite — it has no local ASR or TTS. The full
pipeline is:

1. ESP32 samples the analog mic at 16 kHz and streams 100 ms PCM chunks over
   WebSocket (binary type `0x01`)
2. On button release, sends end-of-utterance marker (`0x02`)
3. Daemon runs ASR (Whisper), sends text to LLM, synthesizes TTS
4. Daemon streams 48 kHz PCM audio back (binary type `0x11` / `0x12`)
5. ESP32 plays audio through I2S, calling `webSocket.loop()` between writes to
   keep receiving data

See `docs/DAP2_DESIGN.md` in the main repository for the full protocol spec.

## Memory

All large buffers are allocated in PSRAM:

| Buffer | Size | Purpose |
|--------|------|---------|
| Audio recording | 480 KB | 15 sec at 16 kHz, 16-bit |
| TTS ring buffer | 1.0 MB | ~23.8 sec at 22050 Hz, 16-bit (power-of-two for fast modulo) |
| WS send buffer | 3.2 KB | One 100 ms audio chunk |
