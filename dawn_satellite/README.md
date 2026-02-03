# DAWN Satellite

Push-to-talk voice satellite client for DAWN voice assistant server.
Designed for Raspberry Pi Zero 2 W with I2S audio hardware.

## Overview

The DAWN Satellite is a lightweight client that:
- Captures audio from an I2S MEMS microphone
- Sends audio to the DAWN server via DAP (DAWN Audio Protocol)
- Receives AI-generated TTS responses
- Plays responses through an I2S amplifier
- Provides visual feedback via SPI display and RGB LEDs

## Hardware Requirements

### Recommended Hardware

| Component | Model | Notes |
|-----------|-------|-------|
| SBC | Raspberry Pi Zero 2 W | 512MB RAM, WiFi |
| Microphone | INMP441 or SPH0645 | I2S MEMS microphone |
| Amplifier | MAX98357A | I2S Class D amplifier |
| Speaker | 3W 4Ω or 8Ω | Small speaker |
| Display | ST7789 or ILI9341 | SPI TFT display (optional) |
| Button | Momentary pushbutton | PTT trigger |
| LEDs | WS2812/NeoPixel | Status indicator via SPI (optional) |

### GPIO Pinout (BCM)

```
                 Pi Zero 2 W
                 +---------+
        3V3  [1] | o     o | [2]  5V
      GPIO2  [3] | o     o | [4]  5V
      GPIO3  [5] | o     o | [6]  GND
      GPIO4  [7] | o     o | [8]  GPIO14
        GND  [9] | o     o | [10] GPIO15
     GPIO17 [11] | o     o | [12] GPIO18 (I2S BCLK)
     GPIO27 [13] | o     o | [14] GND
     GPIO22 [15] | o     o | [16] GPIO23
        3V3 [17] | o     o | [18] GPIO24
     GPIO10 [19] | o     o | [20] GND
      GPIO9 [21] | o     o | [22] GPIO25
     GPIO11 [23] | o     o | [24] GPIO8
        GND [25] | o     o | [26] GPIO7
      GPIO0 [27] | o     o | [28] GPIO1
      GPIO5 [29] | o     o | [30] GND
      GPIO6 [31] | o     o | [32] GPIO12
     GPIO13 [33] | o     o | [34] GND
     GPIO19 [35] | o     o | [36] GPIO16
     GPIO26 [37] | o     o | [38] GPIO20
        GND [39] | o     o | [40] GPIO21 (I2S DOUT)
                 +---------+
```

### Wiring

#### I2S Microphone (INMP441)
| Mic Pin | Pi GPIO | Notes |
|---------|---------|-------|
| VDD | 3V3 | Power |
| GND | GND | Ground |
| SD | GPIO20 | I2S Data In |
| WS | GPIO19 | I2S Word Select (LRCLK) |
| SCK | GPIO18 | I2S Bit Clock (BCLK) |
| L/R | GND | Left channel (or 3V3 for right) |

#### I2S Amplifier (MAX98357A)
| Amp Pin | Pi GPIO | Notes |
|---------|---------|-------|
| VIN | 5V | Power (5V for more volume) |
| GND | GND | Ground |
| DIN | GPIO21 | I2S Data Out |
| BCLK | GPIO18 | I2S Bit Clock |
| LRC | GPIO19 | I2S Word Select |
| GAIN | - | Leave floating for 9dB |
| SD | 3V3 | Enable (or GPIO for mute control) |

#### Button
| Component | Pi GPIO | Notes |
|-----------|---------|-------|
| Button | GPIO17 | Active low (internal pull-up) |

#### NeoPixel LEDs (WS2812)
| NeoPixel Pin | Pi GPIO | Notes |
|--------------|---------|-------|
| VCC | 5V | Power (5V recommended) |
| GND | GND | Ground |
| DIN | GPIO10 (MOSI) | SPI data output |

Note: NeoPixels use SPI0 MOSI for data. If using both NeoPixels and SPI display,
connect NeoPixels to a separate SPI bus or use GPIO bit-banging for the display.

#### SPI Display (ST7789)
| Display Pin | Pi GPIO | Notes |
|-------------|---------|-------|
| VCC | 3V3 | Power |
| GND | GND | Ground |
| SCL | GPIO11 (SCLK) | SPI Clock |
| SDA | GPIO10 (MOSI) | SPI Data |
| RES | GPIO25 | Reset |
| DC | GPIO24 | Data/Command |
| CS | GPIO8 (CE0) | Chip Select |
| BLK | 3V3 or GPIO | Backlight |

## Software Setup

### 1. Enable I2S on Raspberry Pi

Edit `/boot/config.txt`:
```bash
# Enable I2S
dtparam=i2s=on

# I2S microphone overlay
dtoverlay=i2s-mmap
dtoverlay=googlevoicehat-soundcard

# SPI display (optional - depends on your display)
dtoverlay=spi0-1cs
```

Reboot after changes.

### 2. Install Dependencies

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    libasound2-dev \
    libgpiod-dev
```

### 3. Build the Satellite

```bash
cd dawn_satellite
mkdir build && cd build
cmake ..
make -j4
```

Build options:
- `-DENABLE_NEOPIXEL=ON` (default) - Enable NeoPixel LED support via SPI
- `-DENABLE_DISPLAY=OFF` (default) - Enable SPI display support

Example with display enabled:
```bash
cmake -DENABLE_DISPLAY=ON ..
```

### 4. Configure

Copy and edit the configuration file:
```bash
sudo mkdir -p /etc/dawn
sudo cp config/satellite.conf /etc/dawn/
sudo nano /etc/dawn/satellite.conf
```

Update `server_ip` to match your DAWN server.

### 5. Run

```bash
./dawn_satellite --server 192.168.1.100
```

Or with keyboard control (for testing without GPIO):
```bash
./dawn_satellite --server 192.168.1.100 --keyboard
```

## Usage

### Push-to-Talk Operation

1. Press and hold the button (or SPACE key)
2. Speak your command
3. Release the button
4. Wait for the AI response
5. Response plays through the speaker

### LED Status Indicators (NeoPixel)

| Color | State |
|-------|-------|
| Rainbow cycling | Idle, ready |
| Blue | Recording audio |
| Yellow | Connecting/Sending/Waiting |
| Green | Playing response |
| Red | Error |

### Command Line Options

```
-s, --server IP      DAWN server IP address
-p, --port PORT      DAWN server port (default: 5000)
-c, --capture DEV    ALSA capture device
-o, --playback DEV   ALSA playback device
-k, --keyboard       Use keyboard instead of GPIO button
-d, --no-display     Disable display output
-n, --num-leds N     Number of NeoPixel LEDs (default: 3, max: 16)
-v, --verbose        Enable verbose logging
-h, --help           Show help message
```

## Troubleshooting

### No Audio Input

1. Check I2S is enabled: `arecord -l`
2. Verify microphone wiring
3. Test with: `arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 test.wav`

### No Audio Output

1. Check amplifier connections
2. Test with: `aplay -D plughw:0,0 test.wav`
3. Try `speaker-test -D plughw:0,0 -c 2`

### Connection Failed

1. Verify DAWN server is running
2. Check network connectivity: `ping <server_ip>`
3. Ensure firewall allows port 5000

### GPIO Not Working

1. Check `libgpiod` is installed
2. Verify permissions: add user to `gpio` group
3. Test with: `gpioinfo`

### NeoPixels Not Working

1. Ensure SPI is enabled: `ls /dev/spidev*`
2. Check wiring: DIN connects to GPIO10 (MOSI)
3. Verify 5V power supply can handle LED current (~60mA per LED at full white)
4. Test SPI permissions: add user to `spi` group
5. Try fewer LEDs with `--num-leds 1` to rule out power issues

## Cross-Compilation

To build on a different machine for Pi Zero 2 W:

```bash
# Install cross-compiler
sudo apt install gcc-aarch64-linux-gnu

# Build
mkdir build-cross && cd build-cross
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-aarch64.cmake ..
make -j$(nproc)
```

## License

GPLv3 - See LICENSE file for details.

## Contributing

Contributions welcome! See the main DAWN repository for guidelines.
