/*
 * DAWN Satellite - NeoPixel/WS2812 Support via SPI
 *
 * Uses SPI to bit-bang the WS2812 protocol. At 2.4MHz SPI clock,
 * each WS2812 bit takes 3 SPI bits, giving ~800kHz effective rate.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef NEOPIXEL_H
#define NEOPIXEL_H

#include <stddef.h>
#include <stdint.h>

/* Default SPI device for NeoPixels */
#define NEOPIXEL_DEFAULT_SPI "/dev/spidev0.0"

/* Maximum number of LEDs supported */
#define NEOPIXEL_MAX_LEDS 16

/* LED state modes (matches your ESP32 implementation) */
typedef enum {
   NEO_OFF,
   NEO_IDLE_CYCLING, /* Slow color cycling */
   NEO_RECORDING,    /* Blue - recording audio */
   NEO_PLAYING,      /* Green - playing response */
   NEO_WAITING,      /* Yellow - waiting for server */
   NEO_ERROR         /* Red - error state */
} neopixel_mode_t;

/**
 * NeoPixel context
 */
typedef struct {
   int spi_fd;             /* SPI device file descriptor */
   int num_leds;           /* Number of LEDs in strip */
   uint8_t *pixel_data;    /* RGB data for each pixel (3 bytes per LED) */
   uint8_t *spi_buffer;    /* SPI output buffer */
   size_t spi_buffer_size; /* Size of SPI buffer */
   neopixel_mode_t mode;   /* Current display mode */
   uint8_t brightness;     /* Global brightness (0-255) */
   int initialized;        /* Initialization state */

   /* Animation state */
   uint8_t current_hue;  /* Current hue for cycling */
   uint32_t last_update; /* Last update timestamp (ms) */
} neopixel_t;

/**
 * Initialize NeoPixel strip
 *
 * @param ctx Pointer to NeoPixel context
 * @param spi_device SPI device path (NULL for default)
 * @param num_leds Number of LEDs in strip
 * @return 0 on success, -1 on error
 */
int neopixel_init(neopixel_t *ctx, const char *spi_device, int num_leds);

/**
 * Clean up NeoPixel resources
 *
 * @param ctx Pointer to NeoPixel context
 */
void neopixel_cleanup(neopixel_t *ctx);

/**
 * Set a single pixel color
 *
 * @param ctx Pointer to NeoPixel context
 * @param index Pixel index (0-based)
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 */
void neopixel_set_pixel(neopixel_t *ctx, int index, uint8_t r, uint8_t g, uint8_t b);

/**
 * Set all pixels to the same color
 *
 * @param ctx Pointer to NeoPixel context
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 */
void neopixel_set_all(neopixel_t *ctx, uint8_t r, uint8_t g, uint8_t b);

/**
 * Clear all pixels (turn off)
 *
 * @param ctx Pointer to NeoPixel context
 */
void neopixel_clear(neopixel_t *ctx);

/**
 * Update the LED strip with current pixel data
 *
 * @param ctx Pointer to NeoPixel context
 * @return 0 on success, -1 on error
 */
int neopixel_show(neopixel_t *ctx);

/**
 * Set display mode (for state machine integration)
 *
 * @param ctx Pointer to NeoPixel context
 * @param mode Display mode
 */
void neopixel_set_mode(neopixel_t *ctx, neopixel_mode_t mode);

/**
 * Update animation (call periodically from main loop)
 *
 * @param ctx Pointer to NeoPixel context
 */
void neopixel_update(neopixel_t *ctx);

/**
 * Set global brightness
 *
 * @param ctx Pointer to NeoPixel context
 * @param brightness Brightness level (0-255)
 */
void neopixel_set_brightness(neopixel_t *ctx, uint8_t brightness);

/**
 * Convert HSV to RGB
 *
 * @param h Hue (0-255)
 * @param s Saturation (0-255)
 * @param v Value/Brightness (0-255)
 * @param r Pointer to red output
 * @param g Pointer to green output
 * @param b Pointer to blue output
 */
void neopixel_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b);

#endif /* NEOPIXEL_H */
