/*
 * DAWN Satellite - NeoPixel/WS2812 Support via SPI
 *
 * WS2812 Protocol:
 * - 0 bit: 0.4us high, 0.85us low  (T0H=400ns, T0L=850ns)
 * - 1 bit: 0.8us high, 0.45us low  (T1H=800ns, T1L=450ns)
 * - Total bit time: ~1.25us = 800kHz
 *
 * SPI Encoding (at 2.4MHz = 417ns per bit):
 * - WS2812 "0" = 0b100 (high-low-low)   = ~417ns high, ~834ns low
 * - WS2812 "1" = 0b110 (high-high-low)  = ~834ns high, ~417ns low
 *
 * Each WS2812 byte (8 bits) becomes 24 SPI bits (3 bytes).
 * Each LED needs 3 color bytes = 9 SPI bytes per LED.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "neopixel.h"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define LOG_INFO(fmt, ...) fprintf(stdout, "[NEOPIXEL] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[NEOPIXEL ERROR] " fmt "\n", ##__VA_ARGS__)

/* SPI configuration for WS2812 */
#define SPI_SPEED_HZ 2400000 /* 2.4 MHz for WS2812 timing */
#define SPI_BITS 8
#define SPI_MODE SPI_MODE_0

/* Bytes per LED in SPI buffer (3 color bytes * 3 SPI bytes each) */
#define SPI_BYTES_PER_LED 9

/* Reset time (50us minimum, we use 80us = 192 zero bytes at 2.4MHz) */
#define RESET_BYTES 64

/* WS2812 bit patterns (3 SPI bits per WS2812 bit) */
#define WS_BIT_0 0b100 /* 0.4us high, 0.8us low */
#define WS_BIT_1 0b110 /* 0.8us high, 0.4us low */

/* Get current time in milliseconds */
static uint32_t get_time_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Encode a single byte for WS2812 SPI transmission */
static void encode_byte(uint8_t byte, uint8_t *out) {
   /* Each input bit becomes 3 output bits
    * 8 input bits -> 24 output bits -> 3 output bytes
    *
    * out[0] = bits 7,6,5 (high bits) + bit 4 partial
    * out[1] = bit 4 partial + bits 3,2 + bit 1 partial
    * out[2] = bit 1 partial + bit 0
    */
   uint32_t encoded = 0;

   for (int i = 7; i >= 0; i--) {
      encoded <<= 3;
      encoded |= (byte & (1 << i)) ? WS_BIT_1 : WS_BIT_0;
   }

   /* Extract 3 bytes from 24-bit encoded value */
   out[0] = (encoded >> 16) & 0xFF;
   out[1] = (encoded >> 8) & 0xFF;
   out[2] = encoded & 0xFF;
}

void neopixel_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
   if (s == 0) {
      *r = *g = *b = v;
      return;
   }

   uint8_t region = h / 43;
   uint8_t remainder = (h - (region * 43)) * 6;

   uint8_t p = (v * (255 - s)) >> 8;
   uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
   uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

   switch (region) {
      case 0:
         *r = v;
         *g = t;
         *b = p;
         break;
      case 1:
         *r = q;
         *g = v;
         *b = p;
         break;
      case 2:
         *r = p;
         *g = v;
         *b = t;
         break;
      case 3:
         *r = p;
         *g = q;
         *b = v;
         break;
      case 4:
         *r = t;
         *g = p;
         *b = v;
         break;
      default:
         *r = v;
         *g = p;
         *b = q;
         break;
   }
}

int neopixel_init(neopixel_t *ctx, const char *spi_device, int num_leds) {
   if (!ctx || num_leds <= 0 || num_leds > NEOPIXEL_MAX_LEDS) {
      return -1;
   }

   memset(ctx, 0, sizeof(neopixel_t));
   ctx->num_leds = num_leds;
   ctx->brightness = 128; /* 50% default brightness */
   ctx->mode = NEO_OFF;

   const char *dev = spi_device ? spi_device : NEOPIXEL_DEFAULT_SPI;

   /* Open SPI device */
   ctx->spi_fd = open(dev, O_RDWR);
   if (ctx->spi_fd < 0) {
      LOG_ERROR("Cannot open SPI device '%s'", dev);
      return -1;
   }

   /* Configure SPI */
   uint8_t mode = SPI_MODE;
   uint8_t bits = SPI_BITS;
   uint32_t speed = SPI_SPEED_HZ;

   if (ioctl(ctx->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
       ioctl(ctx->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
       ioctl(ctx->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
      LOG_ERROR("Cannot configure SPI");
      close(ctx->spi_fd);
      return -1;
   }

   /* Allocate pixel data buffer (GRB format, 3 bytes per LED) */
   ctx->pixel_data = calloc(num_leds * 3, 1);
   if (!ctx->pixel_data) {
      LOG_ERROR("Cannot allocate pixel buffer");
      close(ctx->spi_fd);
      return -1;
   }

   /* Allocate SPI buffer (9 bytes per LED + reset bytes) */
   ctx->spi_buffer_size = (num_leds * SPI_BYTES_PER_LED) + RESET_BYTES;
   ctx->spi_buffer = calloc(ctx->spi_buffer_size, 1);
   if (!ctx->spi_buffer) {
      LOG_ERROR("Cannot allocate SPI buffer");
      free(ctx->pixel_data);
      close(ctx->spi_fd);
      return -1;
   }

   ctx->initialized = 1;
   ctx->last_update = get_time_ms();

   LOG_INFO("NeoPixel initialized: %d LEDs on %s", num_leds, dev);

   /* Clear all LEDs */
   neopixel_clear(ctx);
   neopixel_show(ctx);

   return 0;
}

void neopixel_cleanup(neopixel_t *ctx) {
   if (!ctx)
      return;

   if (ctx->initialized) {
      neopixel_clear(ctx);
      neopixel_show(ctx);
   }

   if (ctx->pixel_data) {
      free(ctx->pixel_data);
      ctx->pixel_data = NULL;
   }

   if (ctx->spi_buffer) {
      free(ctx->spi_buffer);
      ctx->spi_buffer = NULL;
   }

   if (ctx->spi_fd >= 0) {
      close(ctx->spi_fd);
      ctx->spi_fd = -1;
   }

   ctx->initialized = 0;
   LOG_INFO("NeoPixel cleaned up");
}

void neopixel_set_pixel(neopixel_t *ctx, int index, uint8_t r, uint8_t g, uint8_t b) {
   if (!ctx || !ctx->initialized || index < 0 || index >= ctx->num_leds) {
      return;
   }

   /* Apply brightness */
   r = (r * ctx->brightness) >> 8;
   g = (g * ctx->brightness) >> 8;
   b = (b * ctx->brightness) >> 8;

   /* WS2812 uses GRB order */
   uint8_t *p = &ctx->pixel_data[index * 3];
   p[0] = g;
   p[1] = r;
   p[2] = b;
}

void neopixel_set_all(neopixel_t *ctx, uint8_t r, uint8_t g, uint8_t b) {
   if (!ctx || !ctx->initialized)
      return;

   for (int i = 0; i < ctx->num_leds; i++) {
      neopixel_set_pixel(ctx, i, r, g, b);
   }
}

void neopixel_clear(neopixel_t *ctx) {
   if (!ctx || !ctx->pixel_data)
      return;
   memset(ctx->pixel_data, 0, ctx->num_leds * 3);
}

int neopixel_show(neopixel_t *ctx) {
   if (!ctx || !ctx->initialized)
      return -1;

   /* Clear SPI buffer */
   memset(ctx->spi_buffer, 0, ctx->spi_buffer_size);

   /* Encode each LED's color data */
   uint8_t *spi_ptr = ctx->spi_buffer;

   for (int led = 0; led < ctx->num_leds; led++) {
      uint8_t *color = &ctx->pixel_data[led * 3];

      /* Encode G, R, B bytes (WS2812 order) */
      for (int c = 0; c < 3; c++) {
         encode_byte(color[c], spi_ptr);
         spi_ptr += 3;
      }
   }

   /* Reset bytes are already zero from memset */

   /* Send via SPI */
   struct spi_ioc_transfer tr = {
      .tx_buf = (unsigned long)ctx->spi_buffer,
      .rx_buf = 0,
      .len = ctx->spi_buffer_size,
      .speed_hz = SPI_SPEED_HZ,
      .bits_per_word = SPI_BITS,
   };

   if (ioctl(ctx->spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
      LOG_ERROR("SPI transfer failed");
      return -1;
   }

   return 0;
}

void neopixel_set_brightness(neopixel_t *ctx, uint8_t brightness) {
   if (ctx)
      ctx->brightness = brightness;
}

void neopixel_set_mode(neopixel_t *ctx, neopixel_mode_t mode) {
   if (!ctx)
      return;

   ctx->mode = mode;

   /* Immediate color update for static modes */
   switch (mode) {
      case NEO_OFF:
         neopixel_clear(ctx);
         break;
      case NEO_RECORDING:
         neopixel_set_all(ctx, 0, 0, 255); /* Blue */
         break;
      case NEO_PLAYING:
         neopixel_set_all(ctx, 0, 255, 0); /* Green */
         break;
      case NEO_WAITING:
         neopixel_set_all(ctx, 255, 255, 0); /* Yellow */
         break;
      case NEO_ERROR:
         neopixel_set_all(ctx, 255, 0, 0); /* Red */
         break;
      case NEO_IDLE_CYCLING:
         /* Handled in neopixel_update() */
         break;
   }

   neopixel_show(ctx);
}

void neopixel_update(neopixel_t *ctx) {
   if (!ctx || !ctx->initialized)
      return;

   uint32_t now = get_time_ms();
   uint32_t elapsed = now - ctx->last_update;

   /* Only update animations, not static modes */
   if (ctx->mode != NEO_IDLE_CYCLING) {
      return;
   }

   /* Update every 50ms for smooth animation */
   if (elapsed < 50) {
      return;
   }

   ctx->last_update = now;

   /* Slow hue cycling */
   ctx->current_hue += 1; /* Full cycle every ~13 seconds */

   uint8_t r, g, b;
   neopixel_hsv_to_rgb(ctx->current_hue, 255, 200, &r, &g, &b);
   neopixel_set_all(ctx, r, g, b);
   neopixel_show(ctx);
}
