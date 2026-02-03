/*
 * DAWN Satellite - SPI Display Support (Framebuffer)
 *
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
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stddef.h>
#include <stdint.h>

/* Default framebuffer device for SPI display */
#define DISPLAY_DEFAULT_FB "/dev/fb1"

/* Color definitions (RGB565) */
#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_RED 0xF800
#define COLOR_GREEN 0x07E0
#define COLOR_BLUE 0x001F
#define COLOR_YELLOW 0xFFE0
#define COLOR_CYAN 0x07FF
#define COLOR_MAGENTA 0xF81F
#define COLOR_ORANGE 0xFD20

/**
 * Display context
 */
typedef struct {
   int fd;                /* Framebuffer file descriptor */
   uint16_t *framebuffer; /* Mapped framebuffer memory */
   uint32_t width;        /* Display width in pixels */
   uint32_t height;       /* Display height in pixels */
   uint32_t line_length;  /* Bytes per line */
   size_t fb_size;        /* Total framebuffer size */
   int initialized;       /* Initialization state */
} display_t;

/**
 * Initialize display
 *
 * @param ctx Pointer to display context
 * @param fb_device Framebuffer device path (NULL for default)
 * @return 0 on success, -1 on error
 */
int display_init(display_t *ctx, const char *fb_device);

/**
 * Clean up display
 *
 * @param ctx Pointer to display context
 */
void display_cleanup(display_t *ctx);

/**
 * Clear display to a color
 *
 * @param ctx Pointer to display context
 * @param color RGB565 color value
 */
void display_clear(display_t *ctx, uint16_t color);

/**
 * Draw a single pixel
 *
 * @param ctx Pointer to display context
 * @param x X coordinate
 * @param y Y coordinate
 * @param color RGB565 color value
 */
void display_pixel(display_t *ctx, int x, int y, uint16_t color);

/**
 * Draw a filled rectangle
 *
 * @param ctx Pointer to display context
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @param color RGB565 color value
 */
void display_fill_rect(display_t *ctx, int x, int y, int w, int h, uint16_t color);

/**
 * Draw a horizontal line
 *
 * @param ctx Pointer to display context
 * @param x Start X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param color RGB565 color value
 */
void display_hline(display_t *ctx, int x, int y, int w, uint16_t color);

/**
 * Draw a vertical line
 *
 * @param ctx Pointer to display context
 * @param x X coordinate
 * @param y Start Y coordinate
 * @param h Height
 * @param color RGB565 color value
 */
void display_vline(display_t *ctx, int x, int y, int h, uint16_t color);

/**
 * Draw text (simple 8x8 font)
 *
 * @param ctx Pointer to display context
 * @param x X coordinate
 * @param y Y coordinate
 * @param text Text string
 * @param color Text color (RGB565)
 * @param bg Background color (RGB565)
 * @param scale Font scale (1 = 8x8, 2 = 16x16, etc.)
 */
void display_text(display_t *ctx,
                  int x,
                  int y,
                  const char *text,
                  uint16_t color,
                  uint16_t bg,
                  int scale);

/**
 * Draw a progress bar
 *
 * @param ctx Pointer to display context
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @param percent Percentage (0-100)
 * @param fg Foreground color
 * @param bg Background color
 */
void display_progress_bar(display_t *ctx,
                          int x,
                          int y,
                          int w,
                          int h,
                          int percent,
                          uint16_t fg,
                          uint16_t bg);

/**
 * Draw a filled circle
 *
 * @param ctx Pointer to display context
 * @param cx Center X coordinate
 * @param cy Center Y coordinate
 * @param r Radius
 * @param color RGB565 color value
 */
void display_fill_circle(display_t *ctx, int cx, int cy, int r, uint16_t color);

/**
 * Convert RGB values to RGB565
 *
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 * @return RGB565 color value
 */
static inline uint16_t display_rgb565(uint8_t r, uint8_t g, uint8_t b) {
   return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/**
 * Get display width
 *
 * @param ctx Pointer to display context
 * @return Width in pixels
 */
static inline uint32_t display_get_width(display_t *ctx) {
   return ctx ? ctx->width : 0;
}

/**
 * Get display height
 *
 * @param ctx Pointer to display context
 * @return Height in pixels
 */
static inline uint32_t display_get_height(display_t *ctx) {
   return ctx ? ctx->height : 0;
}

#endif /* DISPLAY_H */
